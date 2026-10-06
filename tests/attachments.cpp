// Local files as OpenGhost sends them: the reader (text, pictures, refusals),
// General's pinned files, and the composer path end to end through the window
// facade, the real ChatService and PiBackend, to tests/pi/pi (a scripted stand-in
// for `pi --mode rpc`; no model, network or credentials).
#include "backend/pi_backend.h"
#include "window.h"
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace openghost;

class AttachmentsTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString write(const QString &name, const QByteArray &bytes)
    {
        const auto path = m_dir.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            return {};
        return path;
    }
    QString picture(const QString &name, const QSize &size, const char *format, bool alpha = false)
    {
        QImage image(size, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
        image.fill(alpha ? QColor(10, 20, 30, 128) : QColor(200, 100, 50));
        // Detail, so a large picture is large on disk too.
        for (int y = 0; y < size.height(); y += 7)
            for (int x = 0; x < size.width(); x += 5)
                image.setPixel(x, y, qRgba((x * 13) % 256, (y * 7) % 256, (x + y) % 256, 255));
        const auto path = m_dir.filePath(name);
        return image.save(path, format) ? path : QString();
    }
    static QUrl url(const QString &path) { return QUrl::fromLocalFile(path); }
    QVector<QJsonObject> records() const
    {
        QFile file(qEnvironmentVariable("FAKE_PI_LOG"));
        QVector<QJsonObject> list;
        if (file.open(QIODevice::ReadOnly))
            for (const auto &line : file.readAll().split('\n'))
                if (!line.isEmpty())
                    list.append(QJsonDocument::fromJson(line).object());
        return list;
    }
    QVector<QJsonObject> prompts() const
    {
        QVector<QJsonObject> list;
        for (const auto &record : records())
            if (record.value("type").toString() == QStringLiteral("prompt") &&
                !record.value("message").toString().startsWith(QStringLiteral("/openghost ")))
                list.append(record);
        return list;
    }

  private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("PATH", QByteArray(OPENGHOST_FAKE_PI_DIR ":") + qgetenv("PATH"));
        qputenv("FAKE_PI_LOG", m_dir.filePath(QStringLiteral("pi.log")).toUtf8());
    }

    void textIsReadWhole()
    {
        const auto path = write(QStringLiteral("notes.md"), "# Notes\ncafé 👻\n");
        const auto read = readLocalFile(url(path), true);
        QVERIFY(read.error.isEmpty());
        QCOMPARE(read.attachment.kind, openghost::Attachment::Kind::Text);
        QCOMPARE(read.attachment.text.value(), QString::fromUtf8("# Notes\ncafé 👻\n"));
        QCOMPARE(read.attachment.name, QStringLiteral("notes.md"));
        QCOMPARE(read.attachment.size, qint64(QByteArray("# Notes\ncafé 👻\n").size()));
        QVERIFY(!read.attachment.path); // No local path leaves the reader.
        // Extensionless and source files are text too.
        QVERIFY(readLocalFile(url(write(QStringLiteral("Makefile"), "all:\n")), true).error.isEmpty());
        QVERIFY(!readLocalFile(url(write(QStringLiteral("big.txt"), QByteArray(256 * 1024 + 1, 'x'))),
                               true)
                     .error.isEmpty());
        QVERIFY(readLocalFile(url(write(QStringLiteral("edge.txt"), QByteArray(256 * 1024, 'x'))),
                              true)
                    .error.isEmpty());
    }

    void malformedTextIsNeverSilentlyTruncated()
    {
        for (const auto &tail : {QByteArray("\xc3", 1), QByteArray("\xe2\x82", 2),
                                 QByteArray("\xf0\x9f\x91", 3)}) {
            const auto path = write(QStringLiteral("incomplete.txt"), "valid prefix " + tail);
            const auto read = readLocalFile(url(path), true);
            QVERIFY2(!read.error.isEmpty(), "An unfinished UTF-8 sequence must not be dropped");
            PreferencesStore prefs{QString()};
            GeneralPreview general(&prefs);
            QVERIFY(!general.add({url(path)}).isEmpty());
            QVERIFY(general.files().isEmpty());
        }
    }

    void controlHeavyBinaryIsNotText()
    {
        const auto binary = write(QStringLiteral("controls.bin"), QByteArray(100, '\x01'));
        QVERIFY(!readLocalFile(url(binary), true).error.isEmpty());
        QVERIFY(readLocalFile(url(write(QStringLiteral("whitespace.txt"), "a\t\r\nb\f\n")), true)
                    .error.isEmpty());
    }

    void thinPicturesKeepAtLeastOnePixel()
    {
        for (const auto &size : {QSize(3000, 1), QSize(1, 3000)}) {
            const auto path = picture(QStringLiteral("thin.png"), size, "PNG");
            const auto read = readLocalFile(url(path), true);
            QVERIFY2(read.error.isEmpty(), qPrintable(read.error));
            const auto data = read.attachment.dataUrl.value();
            const auto image = QImage::fromData(QByteArray::fromBase64(
                data.mid(data.indexOf(QLatin1Char(',')) + 1).toLatin1()));
            QCOMPARE(image.size(), size.width() > size.height() ? QSize(2560, 1) : QSize(1, 2560));
        }
    }

    void picturesArePreparedForPi()
    {
        // A picture Pi takes as it is stays byte for byte.
        const auto small = picture(QStringLiteral("small.png"), {40, 30}, "PNG");
        QFile original(small);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto bytes = original.readAll();
        auto read = readLocalFile(url(small), true);
        QVERIFY(read.error.isEmpty());
        QCOMPARE(read.attachment.kind, openghost::Attachment::Kind::Image);
        QCOMPARE(read.attachment.mime, QStringLiteral("image/png"));
        QCOMPARE(read.attachment.dataUrl.value(),
                 QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64()));
        QCOMPARE(read.attachment.width.value(), 40);
        QCOMPARE(read.attachment.height.value(), 30);
        QCOMPARE(read.attachment.size, bytes.size());
        QVERIFY(!read.attachment.text);
        QCOMPARE(readLocalFile(url(picture(QStringLiteral("p.jpg"), {8, 8}, "JPEG")), true)
                     .attachment.mime,
                 QStringLiteral("image/jpeg"));
        // GIF and BMP stay in their original formats and are previewable too.
        const auto gif = QByteArray::fromBase64(
            "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7");
        read = readLocalFile(url(write(QStringLiteral("dot.gif"), gif)), true);
        QVERIFY2(read.error.isEmpty(), qPrintable(read.error));
        QCOMPARE(read.attachment.mime, QStringLiteral("image/gif"));
        QCOMPARE(read.attachment.dataUrl.value(), QStringLiteral("data:image/gif;base64,") +
                                                     QString::fromLatin1(gif.toBase64()));
        read = readLocalFile(url(picture(QStringLiteral("small.bmp"), {8, 8}, "BMP")), true);
        QVERIFY(read.error.isEmpty());
        QCOMPARE(read.attachment.mime, QStringLiteral("image/bmp"));
        // Larger than 2560 px: fitted and re-encoded; the original size is reported.
        read = readLocalFile(url(picture(QStringLiteral("wide.png"), {3000, 600}, "PNG")), true);
        QVERIFY(read.error.isEmpty());
        QCOMPARE(read.attachment.width.value(), 3000);
        QCOMPARE(read.attachment.mime, QStringLiteral("image/jpeg"));
        const auto comma = read.attachment.dataUrl->indexOf(QLatin1Char(','));
        const auto fitted = QImage::fromData(
            QByteArray::fromBase64(read.attachment.dataUrl->mid(comma + 1).toLatin1()));
        QCOMPARE(fitted.size(), QSize(2560, 512));
        // Transparency is kept as PNG.
        read = readLocalFile(url(picture(QStringLiteral("glass.png"), {2600, 10}, "PNG", true)), true);
        QCOMPARE(read.attachment.mime, QStringLiteral("image/png"));
        // A picture type Pi does not take is converted, when Qt reads it.
        if (QImageWriter::supportedImageFormats().contains("tiff")) {
            read = readLocalFile(url(picture(QStringLiteral("scan.tiff"), {16, 16}, "TIFF")), true);
            QVERIFY(read.error.isEmpty());
            QCOMPARE(read.attachment.kind, openghost::Attachment::Kind::Image);
            QVERIFY(read.attachment.mime == QStringLiteral("image/png") ||
                    read.attachment.mime == QStringLiteral("image/jpeg"));
        }
        // Pinned files are text only: a picture is refused by name.
        read = readLocalFile(url(small), false);
        QVERIFY(read.error.contains(QStringLiteral("small.png")));
        QVERIFY(read.error.contains(QStringLiteral("text only")));
    }

    void unreadableFilesAreRefusedByName()
    {
        auto read = readLocalFile(url(write(QStringLiteral("report.pdf"), "%PDF-1.7\n...")), true);
        QVERIFY(read.error.contains(QStringLiteral("report.pdf is a PDF")));
        read = readLocalFile(url(write(QStringLiteral("song.mp3"), QByteArray("ID3\x04\0\0", 6))),
                             true);
        QVERIFY(read.error.contains(QStringLiteral("song.mp3 isn't a text file or a picture")));
        read = readLocalFile(url(write(QStringLiteral("latin1.txt"), "caf\xe9\n")), true);
        QVERIFY(!read.error.isEmpty());
        // A file that claims a picture's signature but does not decode.
        read = readLocalFile(url(write(QStringLiteral("broken.png"), "\x89PNG\r\n\x1a\nnope")), true);
        QVERIFY(read.error.contains(QStringLiteral("can't decode")));
        QVERIFY(!readLocalFile(QUrl(QStringLiteral("https://example.com/a.txt")), true)
                     .error.isEmpty());
        QVERIFY(!readLocalFile(url(m_dir.path()), true).error.isEmpty()); // a directory
        // One refusal refuses the whole selection.
        AttachmentStore store;
        QVector<AttachmentStore::Prepared> prepared;
        QVERIFY(!store.prepare({url(write(QStringLiteral("ok.txt"), "ok")),
                                url(m_dir.filePath(QStringLiteral("report.pdf")))},
                               20, prepared)
                     .isEmpty());
        QVERIFY(prepared.isEmpty());
        QVERIFY(store.prepare({url(m_dir.filePath(QStringLiteral("ok.txt"))),
                               url(m_dir.filePath(QStringLiteral("small.png")))},
                              20, prepared)
                    .isEmpty());
        QCOMPARE(prepared.size(), 2);
        QVERIFY(!prepared[0].picture && prepared[1].picture);
        const auto payload = store.resolve({prepared[0].token, prepared[1].token});
        QVERIFY(payload && payload->at(1).dataUrl);
        QVERIFY(!store.resolve({prepared[0].token, prepared[0].token}));
        store.release({prepared[0].token});
        QVERIFY(!store.resolve({prepared[0].token}));
    }

    void generalKeepsPinnedTextFiles()
    {
        const auto path = m_dir.filePath(QStringLiteral("prefs.json"));
        const auto style = write(QStringLiteral("style.md"), "Use short sentences.");
        {
            PreferencesStore prefs(path);
            GeneralPreview general(&prefs);
            QSignalSpy changed(&general, &GeneralPreview::filesChanged);
            QVERIFY(general.add({url(style)}).isEmpty());
            QCOMPARE(changed.count(), 1);
            auto files = general.files();
            QCOMPARE(files.size(), 1);
            const auto row = files.first().toMap();
            QCOMPARE(row.value("name").toString(), QStringLiteral("style.md"));
            QCOMPARE(row.value("kind").toString(), QStringLiteral("text"));
            QCOMPARE(row.value("chars").toInt(), 20);
            QCOMPARE(general.tip(row.value("id").toString()), QFileInfo(style).absoluteFilePath());
            // The same file again replaces its copy: how a changed file is updated.
            write(QStringLiteral("style.md"), "Use very short sentences.");
            QVERIFY(general.add({url(style)}).isEmpty());
            QCOMPARE(general.files().size(), 1);
            QCOMPARE(prefs.value().userContext.files.first().text.value(),
                     QStringLiteral("Use very short sentences."));
            // Pictures, PDFs and other files are refused; nothing is added.
            const auto refusal = general.add({url(m_dir.filePath(QStringLiteral("small.png")))});
            QVERIFY(refusal.contains(QStringLiteral("text only")));
            QVERIFY(general.add({url(m_dir.filePath(QStringLiteral("report.pdf")))})
                        .contains(QStringLiteral("PDF")));
            QCOMPARE(general.files().size(), 1);
            QVERIFY(general.add({url(write(QStringLiteral("long.txt"), QByteArray(200001 - 25, 'x')))})
                        .contains(QStringLiteral("200,000")));
            QCOMPARE(general.files().size(), 1);
        }
        // Kept across restarts, with the instructions.
        PreferencesStore prefs(path);
        GeneralPreview general(&prefs);
        QCOMPARE(general.files().size(), 1);
        general.remove(general.files().first().toMap().value("id").toString());
        QCOMPARE(general.files().size(), 0);
        QCOMPARE(PreferencesStore(path).value().userContext.files.size(), 0);
    }

    void generalFailuresKeepThePreviousContents()
    {
        QTemporaryDir dir;
        const auto prefsPath = dir.filePath(QStringLiteral("prefs.json"));
        PreferencesStore prefs(prefsPath);
        GeneralPreview general(&prefs);
        QList<QUrl> urls;
        for (int i = 0; i < 21; ++i)
            urls.append(url(write(QStringLiteral("pinned-%1.txt").arg(i), "original")));
        QVERIFY(general.add(urls.first(20)).isEmpty());
        QCOMPARE(general.files().size(), 20);
        QVERIFY(general.add({urls.last()}).contains(QStringLiteral("20 files")));
        QCOMPARE(general.files().size(), 20);
        // A count-full store can still replace the same path, even spelled with /./.
        write(QStringLiteral("pinned-0.txt"), "replacement");
        const auto same = url(m_dir.path() + QStringLiteral("/./pinned-0.txt"));
        QVERIFY(general.add({same}).isEmpty());
        QCOMPARE(general.files().size(), 20);
        QCOMPARE(prefs.value().userContext.files.first().text.value(), QStringLiteral("replacement"));
        // A failed selection cannot partly replace an existing copy.
        write(QStringLiteral("pinned-0.txt"), "must not be kept");
        const auto bad = url(write(QStringLiteral("invalid.pdf"), "%PDF-1.7"));
        QVERIFY(!general.add({urls.first(), bad}).isEmpty());
        QCOMPARE(prefs.value().userContext.files.first().text.value(), QStringLiteral("replacement"));
        // Deterministic save failure: a directory occupies the preferences filename.
        QVERIFY(QFile::remove(prefsPath));
        QVERIFY(QDir().mkdir(prefsPath));
        QSignalSpy failed(&general, &GeneralPreview::saveFailed);
        QSignalSpy changed(&general, &GeneralPreview::filesChanged);
        general.add({urls.first()});
        QCOMPARE(failed.count(), 1);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(prefs.value().userContext.files.first().text.value(), QStringLiteral("replacement"));
        general.remove(general.files().first().toMap().value("id").toString());
        QCOMPARE(failed.count(), 2);
        QCOMPARE(general.files().size(), 20);
    }

    // The composer's real path: pick → send through the facade → Pi's prompt.
    void composerFilesReachPi()
    {
        PiBackend backend;
        WindowController window(&backend, QString());
        QTRY_VERIFY(window.ready());
        QSignalSpy picked(&window, &WindowController::filesPicked);
        QSignalSpy accepted(&window, &WindowController::accepted);
        const auto token = [&picked](int i) {
            return picked.last().at(0).toList().at(i).toMap().value("token");
        };
        // An attachment-only message with a text file.
        QVERIFY(window.pick({url(m_dir.filePath(QStringLiteral("ok.txt")))}, 20).isEmpty());
        QCOMPARE(picked.last().at(0).toList().first().toMap().value("picture").toBool(), false);
        QVERIFY(window.send(QString(), {token(0)}));
        QTRY_COMPARE(accepted.count(), 1);
        QTRY_VERIFY(!window.busy());
        QCOMPARE(prompts().last().value("message").toString(),
                 QStringLiteral("<file name=\"ok.txt\">\nok\n</file>\n"));
        auto *transcript = window.transcript();
        const auto user = transcript->index(transcript->rowCount() - 2);
        const auto cards = transcript->data(user, TranscriptModel::AttachmentsRole).toList();
        QCOMPARE(cards.size(), 1);
        QCOMPARE(cards.first().toMap().value("mime").toString(), QStringLiteral("text/plain"));
        // A text card has no preview, so never a preview error.
        const auto key = transcript->data(user, TranscriptModel::KeyRole).toString();
        QCOMPARE(window.previewState(key, 0), QString());

        // A picture: refused for a model that can't see, with the draft kept.
        QVERIFY(window.pick({url(m_dir.filePath(QStringLiteral("small.png")))}, 20).isEmpty());
        QVERIFY(picked.last().at(0).toList().first().toMap().value("picture").toBool());
        const auto photo = token(0);
        const auto before = prompts().size();
        QCOMPARE(window.send(QStringLiteral("What is this?"), {photo}), 0ULL);
        QVERIFY(window.status().contains(QStringLiteral("can't see pictures")));
        QCOMPARE(prompts().size(), before);
        // With a model that sees, Pi gets the picture as its prompt image.
        window.settings()->choose(QStringLiteral("p"), QStringLiteral("v"));
        QTRY_COMPARE(window.settings()->property("model").toString(), QStringLiteral("v"));
        QTRY_VERIFY(!window.admitting() && window.ready());
        QVERIFY(window.send(QStringLiteral("What is this?"), {photo}));
        QTRY_COMPARE(accepted.count(), 2);
        QTRY_VERIFY(!window.busy());
        const auto sent = prompts().last();
        QCOMPARE(sent.value("message").toString(),
                 QStringLiteral("<file name=\"small.png\"></file>\nWhat is this?"));
        QCOMPARE(sent.value("images").toArray().first().toObject().value("mimeType").toString(),
                 QStringLiteral("image/png"));
        // Its card previews the picture that was sent, on request.
        const auto row = transcript->index(transcript->rowCount() - 2);
        const auto photoKey = transcript->data(row, TranscriptModel::KeyRole).toString();
        QCOMPARE(transcript->data(row, TranscriptModel::AttachmentsRole)
                     .toList()
                     .first()
                     .toMap()
                     .value("mime")
                     .toString(),
                 QStringLiteral("image/png"));
        QCOMPARE(window.previewState(photoKey, 0), QString());
        QVERIFY(window.previewImage(photoKey, 0).isNull());
        QSignalSpy previewed(&window, &WindowController::previewChanged);
        window.preview(photoKey, 0);
        QCOMPARE(previewed.count(), 1);
        QCOMPARE(window.previewState(photoKey, 0), QStringLiteral("ready"));
        QCOMPARE(window.previewImage(photoKey, 0).size(), QSize(40, 30));
        window.release({photo});

        // Original JPEG bytes retain EXIF orientation; preview must honor it too.
        const auto jpeg = picture(QStringLiteral("rotated.jpg"), {40, 30}, "JPEG");
        QFile original(jpeg);
        QVERIFY(original.open(QIODevice::ReadOnly));
        auto bytes = original.readAll();
        original.close();
        // APP1 Exif, little-endian TIFF, one SHORT Orientation entry = 6 (90° CW).
        bytes.insert(2, QByteArray::fromHex(
            "ffe1002245786966000049492a0008000000010012010300010000000600000000000000"));
        QVERIFY(!write(QStringLiteral("rotated.jpg"), bytes).isEmpty());
        QVERIFY(window.pick({url(jpeg)}, 20).isEmpty());
        QVERIFY(window.send(QStringLiteral("Rotated"), {token(0)}));
        QTRY_COMPARE(accepted.count(), 3);
        QTRY_VERIFY(!window.busy());
        const auto rotatedRow = transcript->index(transcript->rowCount() - 2);
        const auto rotatedKey = transcript->data(rotatedRow, TranscriptModel::KeyRole).toString();
        window.preview(rotatedKey, 0);
        QCOMPARE(window.previewImage(rotatedKey, 0).size(), QSize(30, 40));
    }
};

QTEST_MAIN(AttachmentsTest)
#include "attachments.moc"
