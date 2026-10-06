#include "attachments.h"
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QSet>
#include <QStringDecoder>
#include <QUuid>
#include <algorithm>

namespace openghost
{
namespace
{
constexpr qint64 TextBytes = 256 * 1024;
constexpr qint64 PictureBytes = 32 * 1024 * 1024; // a picture file, before preparation
// attachment-reader.js IMAGE: a picture larger than this is re-encoded smaller.
constexpr int PictureSide = 2560;
constexpr qint64 PictureKept = 6000000;
constexpr qint64 DraftBytes = 48 * 1024 * 1024;

// The picture types Pi takes as prompt images (Pi's detectSupportedImageMimeType).
QString piPicture(const QByteArray &head)
{
    if (head.startsWith("\xff\xd8\xff") && head.size() > 3 && uchar(head[3]) != 0xf7)
        return QStringLiteral("image/jpeg");
    if (head.startsWith("\x89PNG\r\n\x1a\n"))
        return QStringLiteral("image/png");
    if (head.startsWith("GIF87a") || head.startsWith("GIF89a"))
        return QStringLiteral("image/gif");
    if (head.startsWith("RIFF") && head.mid(8, 4) == "WEBP")
        return QStringLiteral("image/webp");
    if (head.startsWith("BM") && head.size() >= 18)
        return QStringLiteral("image/bmp");
    return {};
}
// Other pictures Qt reads by their signature; they are converted for Pi.
bool otherPicture(QIODevice *device)
{
    static const QSet<QByteArray> known{"tif", "tiff", "heif", "heic", "avif", "jxl", "jp2",
                                        "ico", "icns", "qoi", "psd"};
    return known.contains(QImageReader::imageFormat(device).toLower());
}
QString refusal(const QString &name, const QString &why)
{
    return QStringLiteral("%1 %2 Nothing was added.").arg(name, why);
}
QString unreadable(const QString &name)
{
    return refusal(name, QStringLiteral("isn't a text file or a picture. OpenGhost sends text "
                                        "files and pictures; PDFs, documents, audio and video "
                                        "can't be read yet."));
}
QByteArray encoded(const QImage &image, const char *format, int quality)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, format, quality) ? bytes : QByteArray();
}
FileRead picture(QFile &file, const QString &name, const QString &mime)
{
    FileRead read;
    if (file.size() > PictureBytes) {
        read.error = refusal(name, QStringLiteral("is larger than 32 MB."));
        return read;
    }
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || bytes.size() != file.size()) {
        read.error = QStringLiteral("Cannot read the selected file. Nothing was added.");
        return read;
    }
    QBuffer source;
    source.setData(bytes);
    source.open(QIODevice::ReadOnly);
    QImageReader reader(&source);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) {
        read.error = refusal(name, QStringLiteral("is a picture OpenGhost can't decode."));
        return read;
    }
    auto &a = read.attachment;
    a.name = name;
    a.size = bytes.size();
    a.kind = Attachment::Kind::Image;
    a.width = image.width();
    a.height = image.height();
    // A picture Pi takes as it is stays exact; Pi fits it to the model itself.
    QByteArray data = bytes;
    a.mime = mime;
    if (mime.isEmpty() || bytes.size() > PictureKept ||
        std::max(image.width(), image.height()) > PictureSide) {
        const QImage fitted =
            std::max(image.width(), image.height()) > PictureSide
                ? image.scaled(PictureSide, PictureSide, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation)
                : image;
        data = fitted.hasAlphaChannel() ? encoded(fitted, "PNG", -1) : QByteArray();
        a.mime = QStringLiteral("image/png");
        if (data.isEmpty() || data.size() > PictureKept) {
            QImage flat(fitted.size(), QImage::Format_RGB32);
            flat.fill(Qt::white);
            flat.setDevicePixelRatio(fitted.devicePixelRatio());
            {
                QPainter painter(&flat);
                painter.drawImage(0, 0, fitted);
            }
            data = encoded(flat, "JPEG", 90);
            a.mime = QStringLiteral("image/jpeg");
        }
        if (data.isEmpty()) {
            read.error = refusal(name, QStringLiteral("could not be prepared."));
            return read;
        }
    }
    a.dataUrl = QStringLiteral("data:%1;base64,%2").arg(a.mime, QString::fromLatin1(data.toBase64()));
    return read;
}
} // namespace

QString textOnlyRefusal(const QString &name)
{
    return refusal(name, QStringLiteral("is a picture. Files kept for every chat can be text "
                                        "only; attach pictures to a message."));
}

FileRead readLocalFile(const QUrl &url, bool pictures)
{
    FileRead read;
    const QFileInfo info(url.toLocalFile());
    if (!url.isLocalFile() || !info.isFile() || info.isSymLink()) {
        read.error = QStringLiteral("Choose regular local files. Nothing was added.");
        return read;
    }
    const auto name = info.fileName();
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        read.error = QStringLiteral("Cannot read the selected file. Nothing was added.");
        return read;
    }
    const auto head = file.peek(32);
    if (head.startsWith("%PDF-")) {
        read.error = refusal(name, QStringLiteral("is a PDF. OpenGhost can't read PDFs yet; "
                                                  "it sends text files and pictures."));
        return read;
    }
    const auto mime = piPicture(head);
    if (!mime.isEmpty() || otherPicture(&file)) {
        if (!pictures) {
            read.error = textOnlyRefusal(name);
            return read;
        }
        return picture(file, name, mime);
    }
    const auto bytes = file.read(TextBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        read.error = QStringLiteral("Cannot read the selected file. Nothing was added.");
        return read;
    }
    // This is the entire file, not a streaming chunk: an incomplete final UTF-8
    // sequence must fail rather than sit in the decoder's buffer and disappear.
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString text = decoder(bytes.left(TextBytes));
    // Reference AttachmentReader.looksBinary: UTF-8 validity alone does not make
    // control-heavy binary data a text file. Keep normal text whitespace/ESC.
    const auto sniff = bytes.first(std::min<qsizetype>(bytes.size(), 8192));
    const auto controls = std::count_if(sniff.cbegin(), sniff.cend(), [](unsigned char b) {
        return b < 9 || (b > 13 && b < 32 && b != 27);
    });
    if (decoder.hasError() || bytes.contains('\0') || controls * 100 > sniff.size()) {
        read.error = unreadable(name);
        return read;
    }
    if (bytes.size() > TextBytes || !file.atEnd()) {
        read.error = QStringLiteral("Text files are limited to 256 KiB each. Nothing was added.");
        return read;
    }
    auto &a = read.attachment;
    a.name = name;
    a.size = bytes.size();
    a.mime = QStringLiteral("text/plain");
    a.kind = Attachment::Kind::Text;
    a.text = text;
    return read;
}

namespace
{
qint64 weight(const Attachment &a)
{
    return a.text ? a.text->toUtf8().size() : a.dataUrl ? a.dataUrl->size() : 0;
}
} // namespace

QString AttachmentStore::prepare(const QList<QUrl> &urls, int remaining, QVector<Prepared> &out)
{
    out.clear();
    if (urls.isEmpty() || remaining < 0 || urls.size() > std::min(20, remaining))
        return QStringLiteral("Attach at most 20 files to one message. Nothing was added.");
    if (m_payloads.size() + urls.size() > 64)
        return QStringLiteral(
            "Too many retained draft attachments. Remove some before adding more.");
    QVector<Attachment> prepared;
    qint64 total = 0;
    for (const auto &a : m_payloads)
        total += weight(a);
    for (const auto &url : urls) {
        auto read = readLocalFile(url, true);
        if (!read.error.isEmpty())
            return read.error;
        total += weight(read.attachment);
        if (total > DraftBytes)
            return QStringLiteral("Draft attachment storage is full. Nothing was added.");
        read.attachment.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        prepared.append(read.attachment);
    }
    // Publish the whole selection only after every read succeeds.
    for (const auto &a : prepared) {
        m_payloads.insert(a.id, a);
        out.append({a.id, a.name, a.size, a.kind == Attachment::Kind::Image});
    }
    return {};
}
std::optional<QVector<Attachment>> AttachmentStore::resolve(const QStringList &tokens) const
{
    if (tokens.size() > 20)
        return {};
    QVector<Attachment> result;
    QSet<QString> seen;
    for (const auto &token : tokens) {
        const auto a = m_payloads.constFind(token);
        if (a == m_payloads.cend() || seen.contains(token))
            return {};
        seen.insert(token);
        result.append(*a);
    }
    return result;
}
void AttachmentStore::release(const QStringList &tokens)
{
    for (const auto &token : tokens)
        m_payloads.remove(token);
}
} // namespace openghost
