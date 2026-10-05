// Reply pictures and videos: markdown's media blocks, the address rules of
// media-embed.js and MediaLoader's policy. Bytes come from an injected
// fetch; transport tests use an owned loopback server, never the Internet.
#include "media.h"
#include "markdown.h"
#include "mediafetch.h"
#include "medialoader.h"
#include "video_fixture.h"
#include "window.h"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQmlNetworkAccessManagerFactory>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
QByteArray png(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(56, 101, 148));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

const QString Picture = QStringLiteral("https://upload.wikimedia.org/wikipedia/commons/a/a.png");
const QString Elsewhere = QStringLiteral("https://example.com/picture.png");
const QString Video = QStringLiteral("https://www.youtube.com/watch?v=abcdefghijk");

markdown::Document parsed(const QString &text, bool live = false)
{
    markdown::Options options;
    options.live = live;
    return markdown::parse(text, options);
}

// A fetch that answers from a table, now or when told, and counts asks.
struct Fixture {
    QHash<QString, QByteArray> bytes;
    QStringList asked;
    QList<std::pair<QString, MediaLoader::Done>> held;
    bool hold = false;
    MediaLoader::Fetch fetch()
    {
        return [this](const QString &url, bool, const MediaLoader::Done &done) {
            asked << url;
            if (hold)
                held << std::make_pair(url, done);
            else
                done(bytes.value(url));
        };
    }
};

struct LocalHttp {
    QTcpServer server;
    QList<QByteArray> requests;
    std::function<void(QTcpSocket *, const QByteArray &)> respond;
    LocalHttp()
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            auto *socket = server.nextPendingConnection();
            auto input = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, input] {
                input->append(socket->readAll());
                if (!input->contains("\r\n\r\n"))
                    return;
                requests << *input;
                socket->disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);
                if (respond)
                    respond(socket, *input);
            });
        });
    }
    QString url(const QString &path = "/") const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path);
    }
    static void answer(QTcpSocket *socket, const QByteArray &body, const QByteArray &extra = {})
    {
        socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n" + extra + "\r\n" + body);
        socket->disconnectFromHost();
    }
};

bool settle(MediaLoader *loader, const QString &url)
{
    return QTest::qWaitFor([&] { return loader->state(url) != QLatin1String("loading"); }, 3000);
}
} // namespace

class MediaTest : public QObject
{
    Q_OBJECT

  private slots:
    void cleanup() { MediaLoader::instance()->setFetch({}); }

    void videoIds()
    {
        QCOMPARE(media::videoId(Video), QStringLiteral("abcdefghijk"));
        QCOMPARE(media::videoId(QStringLiteral("https://youtu.be/abcdefghijk?t=4")),
                 QStringLiteral("abcdefghijk"));
        QCOMPARE(media::videoId(QStringLiteral("https://m.youtube.com/shorts/abc-efg_ijk")),
                 QStringLiteral("abc-efg_ijk"));
        QCOMPARE(
            media::videoId(QStringLiteral("https://music.youtube.com/watch?list=x&v=abcdefghijk")),
            QStringLiteral("abcdefghijk"));
        QCOMPARE(media::videoId(QStringLiteral("HTTPS://YOUTUBE.COM/embed/abcdefghijk")),
                 QStringLiteral("abcdefghijk"));
        QVERIFY(
            media::videoId(QStringLiteral("https://www.youtube.com/watch?v=abcdefghij")).isEmpty());
        QVERIFY(media::videoId(QStringLiteral("https://www.youtube.com/watch?v=abcdefghijkl"))
                    .isEmpty());
        QVERIFY(media::videoId(QStringLiteral("https://vimeo.com/123456789")).isEmpty());
        QVERIFY(media::videoId(QStringLiteral("https://youtube.com.evil.com/watch?v=abcdefghijk"))
                    .isEmpty());
    }

    void trustedPlaces()
    {
        QVERIFY(media::trusted(Picture));
        QVERIFY(media::trusted(QStringLiteral("https://tse1.mm.bing.net/th?id=OIP.x")));
        QVERIFY(media::trusted(QStringLiteral("https://th.bing.com/th/id/OIP.x")));
        QVERIFY(media::trusted(QStringLiteral("https://i.ytimg.com/vi/abcdefghijk/hq720.jpg")));
        QVERIFY(media::trusted(QStringLiteral("https://i9.ytimg.com/vi_webp/abcdefghijk/a.webp")));
        QVERIFY(media::trusted(QStringLiteral("https://encrypted-tbn0.gstatic.com/images?q=tbn")));
        QVERIFY(!media::trusted(Elsewhere));
        QVERIFY(!media::trusted(QStringLiteral("http://upload.wikimedia.org/wikipedia/a.png")));
        QVERIFY(!media::trusted(
            QStringLiteral("https://upload.wikimedia.org.example.com/wikipedia/a")));
        QVERIFY(!media::trusted(QStringLiteral("https://th.bing.com/search?q=x")));
    }

    void redirects()
    {
        QVERIFY(media::redirectAllowed(QStringLiteral("https://i.ytimg.com/vi/x/a.jpg"), false));
        QVERIFY(!media::redirectAllowed(Elsewhere, false));
        QVERIFY(media::redirectAllowed(Elsewhere, true));
        QVERIFY(media::redirectAllowed(QStringLiteral("http://example.com/a.png"), true));
        QVERIFY(!media::redirectAllowed(QStringLiteral("https://user:password@example.com/a.png"),
                                        true));
        QVERIFY(!media::redirectAllowed(QStringLiteral("http://i.ytimg.com/vi/x/a.jpg"), false));
        QVERIFY(!media::redirectAllowed(QStringLiteral("file:///etc/passwd"), true));
        QVERIFY(!media::redirectAllowed(QStringLiteral("ftp://example.com/a.png"), true));
        QVERIFY(!media::redirectAllowed(QStringLiteral("data:image/png;base64,AAAA"), true));
    }

    void videoWords()
    {
        auto words = media::videoWords(QStringLiteral("A talk · Some channel · 4:40"), Video);
        QCOMPARE(words.title, QStringLiteral("A talk"));
        QCOMPARE(words.by, QStringLiteral("Some channel"));
        QCOMPARE(words.time, QStringLiteral("4:40"));
        words = media::videoWords(QStringLiteral("One · Two · Three"), Video);
        QCOMPARE(words.title, QStringLiteral("One · Two"));
        QCOMPARE(words.by, QStringLiteral("Three"));
        QVERIFY(words.time.isEmpty());
        words = media::videoWords(QStringLiteral("Just a title"), Video);
        QCOMPARE(words.title, QStringLiteral("Just a title"));
        QVERIFY(words.by.isEmpty());
        QVERIFY(media::videoWords(Video, Video).title.isEmpty());
        QVERIFY(media::videoWords(QStringLiteral("youtube.com/watch?v=abcdefghijk"), Video)
                    .title.isEmpty());
        QVERIFY(media::videoWords(QStringLiteral("  "), Video).title.isEmpty());
    }

    void stackShape()
    {
        QCOMPARE(media::stackRatio(10, false), 2.2);
        QCOMPARE(media::stackRatio(10, true), 1.6);
        QCOMPARE(media::stackRatio(0.1, true), 0.75);
        QCOMPARE(media::stackRatio(-1, false), 4.0 / 3.0);
        QCOMPARE(media::stackWidth(2), 380);
        QCOMPARE(media::stackWidth(0.75), 255);
        QVERIFY(!media::needsBackdrop(QSize(400, 200), 2.0));
        QVERIFY(media::needsBackdrop(QSize(200, 400), 2.0));
        QVERIFY(media::needsBackdrop(QSize(), 2.0));
        QCOMPARE(media::thumbnails(QStringLiteral("abcdefghijk")).size(), 2);
        QVERIFY(media::thumbnails({}).isEmpty());
        QCOMPARE(media::hostOf(QStringLiteral("https://www.example.com/a")),
                 QStringLiteral("example.com"));
    }

    void mediaParagraphs()
    {
        markdown::MediaList list;
        QVERIFY(markdown::mediaItems(QStringLiteral("![A cat](%1)").arg(Picture), false, list));
        QCOMPARE(list.items.size(), 1);
        QCOMPARE(list.items[0].caption, QStringLiteral("A cat"));
        QCOMPARE(list.items[0].src, Picture);
        QVERIFY(!list.items[0].video && list.items[0].href.isEmpty());

        QVERIFY(markdown::mediaItems(
            QStringLiteral(
                "[![A cat](%1 \"title\")](https://example.com/page) [Talk · Chan](%2)\n%2.")
                .arg(Picture, Video),
            false, list));
        QCOMPARE(list.items.size(), 3);
        QCOMPARE(list.items[0].href, QStringLiteral("https://example.com/page"));
        QVERIFY(list.items[1].video && list.items[1].caption == QStringLiteral("Talk · Chan"));
        QCOMPARE(list.items[2].src, Video); // The bare link's full stop is not the address.

        // An unsafe page address is dropped; the picture stays.
        QVERIFY(markdown::mediaItems(QStringLiteral("[![x](%1)](javascript:alert(1))").arg(Picture),
                                     false, list));
        QVERIFY(list.items[0].href.isEmpty());

        // Anything else makes it a paragraph like any other.
        QVERIFY(!markdown::mediaItems(QStringLiteral("See ![x](%1)").arg(Picture), false, list));
        QVERIFY(
            !markdown::mediaItems(QStringLiteral("[a page](https://example.com)"), false, list));
        QVERIFY(
            !markdown::mediaItems(QStringLiteral("![x](data:image/png;base64,AAAA)"), false, list));
        QVERIFY(
            !markdown::mediaItems(QStringLiteral("![x](%1) and words").arg(Picture), false, list));
        QVERIFY(list.items.isEmpty());

        // While the reply is writing, a half-written picture waits.
        QVERIFY(markdown::mediaItems(QStringLiteral("![A](%1)\n![B](https://x").arg(Picture), true,
                                     list));
        QVERIFY(list.open);
        QCOMPARE(list.items.size(), 1);
        QVERIFY(!markdown::mediaItems(QStringLiteral("![A](%1)\n![B](https://x").arg(Picture),
                                      false, list));
        QVERIFY(markdown::mediaItems(QStringLiteral("[!["), true, list) && list.items.isEmpty());
    }

    void mediaBlocks()
    {
        auto doc = parsed(QStringLiteral("Look at this:\n![A cat](%1)\n\nAfter.").arg(Picture));
        QCOMPARE(doc.blocks.size(), 3);
        QCOMPARE(doc.blocks[0]->kind, markdown::Kind::Paragraph);
        QCOMPARE(doc.blocks[1]->kind, markdown::Kind::Media);
        QCOMPARE(doc.blocks[1]->media.size(), 1);
        QVERIFY(!doc.blocks[1]->open);
        QCOMPARE(doc.blocks[2]->kind, markdown::Kind::Paragraph);

        doc =
            parsed(QStringLiteral("[A talk · A channel · 4:40](%1) [https://youtu.be/bbbbbbbbbbb]"
                                  "(https://youtu.be/bbbbbbbbbbb)\n\nhttps://youtu.be/ccccccccccc")
                       .arg(Video));
        QCOMPARE(doc.blocks.size(), 2);
        QCOMPARE(doc.blocks[0]->kind, markdown::Kind::Media);
        QCOMPARE(doc.blocks[0]->media.size(), 2);
        QCOMPARE(doc.blocks[1]->kind, markdown::Kind::Media);
        QVERIFY(doc.blocks[1]->media[0].video);

        // A list of nothing but pictures and videos is them.
        doc = parsed(QStringLiteral("- ![A](%1)\n- [Talk](%2)").arg(Picture, Video));
        QCOMPARE(doc.blocks.size(), 1);
        QCOMPARE(doc.blocks[0]->kind, markdown::Kind::Media);
        QCOMPARE(doc.blocks[0]->media.size(), 2);
        QVERIFY(doc.blocks[0]->media[1].video);
        doc = parsed(QStringLiteral("- ![A](%1)\n- words").arg(Picture));
        QCOMPARE(doc.blocks[0]->kind, markdown::Kind::List);
        doc = parsed(QStringLiteral("- [ ] ![A](%1)").arg(Picture));
        QCOMPARE(doc.blocks[0]->kind, markdown::Kind::List);

        // Inside a quote a link stays a link.
        doc = parsed(QStringLiteral("> ![A](%1)").arg(Picture));
        QCOMPARE(doc.blocks[0]->kind, markdown::Kind::Quote);
        QCOMPARE(doc.blocks[0]->children[0]->kind, markdown::Kind::Paragraph);

        // What it shows is part of its identity.
        const auto one = parsed(QStringLiteral("![A](%1)").arg(Picture));
        const auto two = parsed(QStringLiteral("![B](%1)").arg(Picture));
        QVERIFY(one.blocks[0]->hash != two.blocks[0]->hash);

        // Live: the last list of pictures is still being written.
        doc = parsed(QStringLiteral("![A](%1)\n![B](https://x").arg(Picture), true);
        QCOMPARE(doc.blocks.size(), 1);
        QCOMPARE(doc.blocks[0]->kind, markdown::Kind::Media);
        QVERIFY(doc.blocks[0]->open);
        doc = parsed(QStringLiteral("![A](%1)\n\nMore").arg(Picture), true);
        QVERIFY(!doc.blocks[0]->open);
    }

    void refusesWithoutAsking()
    {
        Fixture fixture;
        auto *loader = MediaLoader::instance();
        loader->setFetch(fixture.fetch());
        QVERIFY(!loader->load(Elsewhere, false));
        QVERIFY(!loader->load(QStringLiteral("ftp://example.com/a.png"), true));
        QVERIFY(!loader->load(QStringLiteral("file:///etc/hostname"), true));
        QVERIFY(!loader->load(QStringLiteral("data:image/png;base64,AAAA"), true));
        QVERIFY(fixture.asked.isEmpty());
        QVERIFY(loader->state(Elsewhere).isEmpty());
        // Asked for, it loads.
        fixture.bytes.insert(Elsewhere, png(40, 20));
        QVERIFY(loader->load(Elsewhere, true));
        QVERIFY(settle(loader, Elsewhere));
        QCOMPARE(loader->state(Elsewhere), QStringLiteral("ready"));
        QCOMPARE(fixture.asked, QStringList{Elsewhere});
    }

    void loadsAndShows()
    {
        Fixture fixture;
        fixture.bytes.insert(Picture, png(240, 120));
        auto *loader = MediaLoader::instance();
        loader->setFetch(fixture.fetch());
        QSignalSpy settled(loader, &MediaLoader::settled);
        QVERIFY(loader->load(Picture, false));
        QVERIFY(loader->load(Picture, false)); // One load per address.
        QVERIFY(settle(loader, Picture));
        QCOMPARE(fixture.asked.size(), 1);
        QCOMPARE(settled.size(), 1);
        QCOMPARE(settled[0][1].toBool(), true);
        QCOMPARE(loader->size(Picture), QSize(240, 120));
        const QString source = loader->source(Picture);
        QVERIFY(source.startsWith(QStringLiteral("image://openghost-media/")));
        MediaImages provider;
        QSize size;
        const QImage image = provider.requestImage(
            source.mid(QStringLiteral("image://openghost-media/").size()), &size, {});
        QCOMPARE(image.size(), QSize(240, 120));
        QCOMPARE(size, QSize(240, 120));
        QVERIFY(provider.requestImage(QStringLiteral("999999"), &size, {}).isNull());
    }

    void failures()
    {
        Fixture fixture;
        const QString empty = QStringLiteral("https://i.ytimg.com/vi/abcdefghijk/hq720.jpg");
        const QString dot = QStringLiteral("https://i.ytimg.com/vi/abcdefghijk/mqdefault.jpg");
        const QString junk = QStringLiteral("https://th.bing.com/th/id/junk");
        fixture.bytes.insert(dot, png(1, 1));
        fixture.bytes.insert(junk, QByteArray("<html>not a picture</html>"));
        auto *loader = MediaLoader::instance();
        loader->setFetch(fixture.fetch());
        for (const QString &url : {empty, dot, junk}) {
            QVERIFY(loader->load(url, false));
            QVERIFY(settle(loader, url));
            QCOMPARE(loader->state(url), QStringLiteral("failed"));
            QVERIFY(loader->source(url).isEmpty());
        }
    }

    void staleLoadsAreDropped()
    {
        Fixture fixture;
        fixture.hold = true;
        auto *loader = MediaLoader::instance();
        loader->setFetch(fixture.fetch());
        QVERIFY(loader->load(Picture, false));
        QCOMPARE(loader->state(Picture), QStringLiteral("loading"));
        QCOMPARE(fixture.held.size(), 1);
        const auto late = fixture.held.takeFirst();
        loader->forget();
        late.second(png(20, 20));
        QTest::qWait(100);
        QVERIFY(loader->state(Picture).isEmpty());
    }

    void metadataServiceAndCache()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("media-info.json");
        auto host = std::make_shared<FixtureVideoInfo>();
        host->hold = true;
        VideoTitles titles(host, path);
        titles.request("not-an-id");
        titles.request("abcdefghij\n");
        QVERIFY(host->asked.isEmpty());
        titles.request(QString(11, QChar(0x00e9)));
        QVERIFY(host->asked.isEmpty());
        titles.request("abcdefghijk");
        titles.request("abcdefghijk");
        QCOMPARE(host->asked.size(), 1);
        QVERIFY(titles.info("abcdefghijk")["title"].toString().isEmpty());
        host->pending.takeFirst()({"Actual title", "Actual author"});
        QTRY_COMPARE(titles.info("abcdefghijk")["title"].toString(), QString("Actual title"));
        VideoTitles reopened(host, path);
        reopened.request("abcdefghijk");
        QCOMPARE(host->asked.size(), 1);
        QCOMPARE(reopened.info("abcdefghijk")["by"].toString(), QString("Actual author"));
        titles.request("bbbbbbbbbbb");
        host->pending.takeFirst()({"", "Not a valid title result"});
        QTest::qWait(20);
        QVERIFY(titles.info("bbbbbbbbbbb")["by"].toString().isEmpty());
        titles.request("bbbbbbbbbbb");
        QCOMPARE(host->asked.size(), 2); // Failure is shared for the process.
        VideoTitles again(host, path);
        again.request("bbbbbbbbbbb");
        QCOMPARE(host->asked.size(), 3); // But not on disk.
        const auto stale = host->pending.takeFirst();
        again.setService({});
        stale({"Too late", ""});
        QTest::qWait(20);
        QVERIFY(again.info("bbbbbbbbbbb")["title"].toString().isEmpty());
        // Cache failure must not discard genuine host metadata.
        VideoTitles unwritable(host, dir.path());
        unwritable.request("ccccccccccc");
        host->pending.takeFirst()({"Still visible", ""});
        QTRY_COMPARE(unwritable.info("ccccccccccc")["title"].toString(), QString("Still visible"));
    }

    void metadataFifoAndMalformedCache()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("info.json");
        auto host = std::make_shared<FixtureVideoInfo>();
        VideoTitles titles(host, path);
        for (int i = 0; i < 302; ++i) {
            const QString id = QString::number(i).rightJustified(11, 'a');
            host->values[id] = {QString::number(i), "Author"};
            titles.request(id);
        }
        QTRY_COMPARE(titles.info("aaaaaaaa301")["title"].toString(), QString("301"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).array().size(), 300);
        file.close();
        VideoTitles reopened(host, path);
        QVERIFY(reopened.info("aaaaaaaaaa0")["title"].toString().isEmpty());
        QCOMPARE(reopened.info("aaaaaaaaaa2")["title"].toString(), QString("2"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("not JSON");
        file.close();
        reopened.setService(host, path);
        QVERIFY(reopened.info("aaaaaaaaaa2")["title"].toString().isEmpty());
    }

    void metadataWirePolicy()
    {
        const QString id = "abc-efg_ijk";
        const QUrl url = NetworkVideoInfo::address(id);
        QCOMPARE(
            url.toEncoded(),
            QByteArray(
                "https://www.youtube.com/"
                "oembed?url=https%3A%2F%2Fwww.youtube.com%2Fwatch%3Fv%3Dabc-efg_ijk&format=json"));
        QVERIFY(NetworkVideoInfo::address("abcdefghij/").isEmpty());
        QVERIFY(NetworkVideoInfo::redirectAllowed(url, id));
        for (const auto &bad :
             {QString("http://www.youtube.com/oembed"), QString("https://evil.example/oembed"),
              QString("https://www.youtube.com/watch?v=abc-efg_ijk"),
              QString("file:///etc/passwd")})
            QVERIFY(!NetworkVideoInfo::redirectAllowed(QUrl(bad), id));
        auto changed = url;
        changed.setUserInfo("secret:password");
        QVERIFY(!NetworkVideoInfo::redirectAllowed(changed, id));
        changed = url;
        changed.setPort(444);
        QVERIFY(!NetworkVideoInfo::redirectAllowed(changed, id));
        QVERIFY(!NetworkVideoInfo::redirectAllowed(NetworkVideoInfo::address("aaaaaaaaaaa"), id));
        QCOMPARE(
            NetworkVideoInfo::parse(R"({"title":"A <b>title</b>","author_name":"Channel"})").title,
            QString("A <b>title</b>"));
        QVERIFY(NetworkVideoInfo::parse("not json").title.isEmpty());
        QVERIFY(NetworkVideoInfo::parse(R"({"title":{},"author_name":[]})").title.isEmpty());
        QVERIFY(NetworkVideoInfo::parse(QByteArray(NetworkVideoInfo::MaxBytes + 1, ' '))
                    .title.isEmpty());
        QCOMPARE(NetworkVideoInfo::Timeout, 10000);
        NetworkVideoInfo service;
        bool refused = false;
        service.lookup("invalid", [&](VideoInfo info) { refused = info.title.isEmpty(); });
        QVERIFY(refused); // No request can be constructed for an arbitrary URL.
    }

    void boundedNetwork()
    {
        LocalHttp http;
        QVERIFY(http.server.isListening());
        QNetworkAccessManager network;
        auto get = [&](const QString &path, qint64 cap, bool redirects = true) {
            bool completed = false;
            QByteArray result;
            int calls = 0;
            media::get(
                network, QNetworkRequest(QUrl(http.url(path))), cap, 150,
                [redirects](const QUrl &to) { return redirects && to.scheme() == "http"; },
                [&](const QByteArray &bytes) {
                    result = bytes;
                    completed = true;
                    ++calls;
                });
            if (!QTest::qWaitFor([&] { return completed; }, 2000))
                qFatal("Bounded frontend request did not complete");
            QTest::qWait(10);
            if (calls != 1)
                qFatal("Frontend GET completed more than once");
            return result;
        };
        http.respond = [](QTcpSocket *s, const QByteArray &request) {
            if (request.startsWith("GET /redirect ")) {
                s->write("HTTP/1.1 302 Found\r\nLocation: /ok\r\nContent-Length: 0\r\n\r\n");
                s->disconnectFromHost();
            } else if (request.startsWith("GET /loop ")) {
                s->write("HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n");
                s->disconnectFromHost();
            } else if (request.startsWith("GET /long "))
                LocalHttp::answer(s, "x", "Content-Length: 99999\r\n");
            else if (request.startsWith("GET /unknown "))
                LocalHttp::answer(s, QByteArray(1025, 'x'));
            else if (request.startsWith("GET /hang ")) { /* deadline */
            } else if (request.startsWith("GET /error ")) {
                s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
                s->disconnectFromHost();
            } else
                LocalHttp::answer(s, "okay", "Set-Cookie: secret=value\r\n");
        };
        QCOMPARE(get("/ok", 4), QByteArray("okay")); // Exact cap allowed.
        QCOMPARE(get("/redirect", 4), QByteArray("okay"));
        int before = http.requests.size();
        QVERIFY(get("/redirect", 4, false).isEmpty());
        QCOMPARE(http.requests.size(), before + 1); // Refused before the next hop.
        before = http.requests.size();
        QVERIFY(get("/loop", 4).isEmpty());
        QVERIFY(http.requests.size() - before <= 4);
        QVERIFY(get("/long", 1024).isEmpty());
        QVERIFY(get("/unknown", 1024).isEmpty());
        QVERIFY(get("/hang", 1024).isEmpty());
        QVERIFY(get("/error", 1024).isEmpty());
        for (const auto &request : http.requests) {
            QVERIFY(!request.toLower().contains("cookie:"));
            QVERIFY(!request.toLower().contains("authorization:"));
        }
    }

    void productionImageCapAndGlobalDenial()
    {
        LocalHttp http;
        auto *loader = MediaLoader::instance();
        loader->setFetch({});
        QCOMPARE(MediaLoader::MaxBytes, 16 * 1024 * 1024);
        QCOMPARE(media::LoadTimeout, 9000);
        http.respond = [](QTcpSocket *s, const QByteArray &request) {
            if (request.startsWith("GET /large "))
                LocalHttp::answer(s, QByteArray(MediaLoader::MaxBytes + 1, 'x'));
            else
                LocalHttp::answer(s, png(240, 120));
        };
        QVERIFY(!loader->load(http.url(), false));
        QVERIFY(http.requests.isEmpty());
        QVERIFY(loader->load(http.url(), true));
        QVERIFY(settle(loader, http.url()));
        QCOMPARE(loader->state(http.url()), QString("ready"));
        QVERIFY(loader->load(http.url("/large"), true));
        QVERIFY(settle(loader, http.url("/large")));
        QCOMPARE(loader->state(http.url("/large")), QString("failed"));
        const auto before = http.requests.size();
        QQmlEngine engine;
        engine.setNetworkAccessManagerFactory(denyNetwork());
        auto *reply = engine.networkAccessManager()->get(QNetworkRequest(QUrl(http.url("/qml"))));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QVERIFY(finished.wait(1000));
        QCOMPARE(http.requests.size(), before); // Media did not open QML networking.
    }

    void decodeBounds()
    {
        QImage big = MediaLoader::decode(png(2000, 1000));
        QCOMPARE(big.size(), QSize(1600, 800));
        QVERIFY(MediaLoader::decode(png(MediaLoader::MaxEdge + 1, 2)).isNull());
        QVERIFY(MediaLoader::decode(QByteArray()).isNull());
        QByteArray bmp;
        QBuffer buffer(&bmp);
        buffer.open(QIODevice::WriteOnly);
        QImage(8, 8, QImage::Format_RGB32).save(&buffer, "BMP");
        QVERIFY(MediaLoader::decode(bmp).isNull()); // Not a format a reply shows.

        Fixture fixture;
        fixture.bytes.insert(Picture, png(2000, 1000));
        auto *loader = MediaLoader::instance();
        loader->setFetch(fixture.fetch());
        QVERIFY(loader->load(Picture, false));
        QVERIFY(settle(loader, Picture));
        QCOMPARE(loader->size(Picture), QSize(2000, 1000)); // Its own shape, not the shown one.
    }
};

QTEST_MAIN(MediaTest)
#include "media.moc"
