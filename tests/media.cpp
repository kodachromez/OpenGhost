// Reply pictures and videos: markdown's media blocks, the address rules of
// media-embed.js and MediaLoader's policy. Bytes come from an injected
// fetch; nothing here touches the network.
#include "media.h"
#include "markdown.h"
#include "medialoader.h"

#include <QBuffer>
#include <QImage>
#include <QSignalSpy>
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
