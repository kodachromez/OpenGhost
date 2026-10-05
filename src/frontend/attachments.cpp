#include "attachments.h"
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringDecoder>
#include <QUuid>
#include <algorithm>

namespace openghost
{
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
        total += a.text->toUtf8().size();
    for (const auto &url : urls) {
        const QFileInfo info(url.toLocalFile());
        if (!url.isLocalFile() || !info.isFile() || info.isSymLink())
            return QStringLiteral("Choose regular local text files. Nothing was added.");
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            return QStringLiteral("Cannot read the selected file. Nothing was added.");
        const auto bytes = file.read(256 * 1024 + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() > 256 * 1024 || !file.atEnd())
            return QStringLiteral(
                "Text attachments are limited to 256 KiB each. Nothing was added.");
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString text = decoder(bytes);
        if (decoder.hasError() || bytes.contains('\0') || bytes.startsWith("%PDF-"))
            return QStringLiteral("Only UTF-8 text preparation is connected. Images, PDF and media "
                                  "remain mocked. Nothing was added.");
        total += bytes.size();
        if (total > 8 * 1024 * 1024)
            return QStringLiteral("Draft attachment storage is full. Nothing was added.");
        Attachment a;
        a.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        a.name = info.fileName();
        a.size = bytes.size();
        a.mime = "text/plain";
        a.kind = Attachment::Kind::Text;
        a.text = text;
        prepared.append(a);
    }
    // Publish the whole selection only after every read succeeds.
    for (const auto &a : prepared) {
        m_payloads.insert(a.id, a);
        out.append({a.id, a.name, a.size});
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
