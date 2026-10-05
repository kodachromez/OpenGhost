#include "medialoader.h"

#include "media.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

MediaLoader::MediaLoader(QObject *parent) : QObject(parent) {}

MediaLoader *MediaLoader::instance()
{
    static QPointer<MediaLoader> loader;
    if (!loader)
        loader = new MediaLoader(QCoreApplication::instance());
    return loader;
}

MediaLoader *MediaLoader::create(QQmlEngine *, QJSEngine *)
{
    MediaLoader *loader = instance();
    QJSEngine::setObjectOwnership(loader, QJSEngine::CppOwnership);
    return loader;
}

QString MediaLoader::state(const QString &url) const { return m_entries.value(url).state; }

bool MediaLoader::load(const QString &url, bool consented)
{
    if (!media::fetchable(url) || (!consented && !media::trusted(url)))
        return false;
    {
        QMutexLocker locker(&m_lock);
        Entry &entry = m_entries[url];
        entry.used = ++m_clock;
        if (!entry.state.isEmpty())
            return true;
        entry.state = QStringLiteral("loading");
    }
    emit changed();
    const quint64 generation = m_generation;
    QPointer<MediaLoader> self(this);
    // A load settles on a later turn of the event loop, never inside the
    // call that started it (a fetch may answer at once): whoever asked sees
    // "loading" first.
    const Done done = [self, url, generation](const QByteArray &bytes) {
        if (!self)
            return;
        if (bytes.isEmpty()) {
            QMetaObject::invokeMethod(
                self.data(),
                [self, url, generation] {
                    if (self)
                        self->finish(url, generation, {});
                },
                Qt::QueuedConnection);
            return;
        }
        // Decoding a large picture takes a while: off the GUI thread. The
        // application object (living as long as any loader) brings it back.
        QThreadPool::globalInstance()->start([self, url, generation, bytes] {
            const QImage image = decode(bytes);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, url, generation, image] {
                    if (self)
                        self->finish(url, generation, image);
                },
                Qt::QueuedConnection);
        });
    };
    if (m_fetch)
        m_fetch(url, consented, done);
    else
        fetchNetwork(url, consented, done);
    return true;
}

void MediaLoader::fetchNetwork(const QString &url, bool consented, const Done &done)
{
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
        m_network->setAutoDeleteReplies(true);
    }
    QNetworkRequest request{QUrl(url)};
    // Each hop is checked against the same rule as the first address.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(3);
    // Nothing of the person goes with it, and nothing comes back to stay.
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Accept", "image/webp,image/png,image/jpeg,image/gif;q=0.9,*/*;q=0.5");
    request.setTransferTimeout(media::LoadTimeout);
    QNetworkReply *reply = m_network->get(request);
    // The whole load, not just a stall, is bounded.
    QTimer::singleShot(media::LoadTimeout, reply, [reply] { reply->abort(); });
    connect(reply, &QNetworkReply::redirected, reply, [reply, consented](const QUrl &to) {
        if (media::redirectAllowed(to.toString(), consented))
            emit reply->redirectAllowed();
        else
            reply->abort();
    });
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
        if (received > MaxBytes || total > MaxBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray bytes;
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300)
            bytes = reply->read(MaxBytes + 1);
        done(bytes.size() > MaxBytes ? QByteArray() : bytes);
    });
}

void MediaLoader::finish(const QString &url, quint64 generation, const QImage &image)
{
    if (generation != m_generation)
        return;
    auto found = m_entries.find(url);
    if (found == m_entries.end())
        return;
    // media-embed.js probe(): a picture one pixel wide is no picture.
    const bool ok = !image.isNull() && image.width() > 1;
    {
        QMutexLocker locker(&m_lock);
        found->state = ok ? QStringLiteral("ready") : QStringLiteral("failed");
        if (ok) {
            found->image = image;
            found->size = image.text(QStringLiteral("openghost-natural")).isEmpty()
                              ? image.size()
                              : QSize(image.text(QStringLiteral("openghost-natural-w")).toInt(),
                                      image.text(QStringLiteral("openghost-natural-h")).toInt());
            found->id = QString::number(++m_nextId);
            m_ids.insert(found->id, url);
        }
        trim();
    }
    emit changed();
    emit settled(url, ok);
}

void MediaLoader::trim()
{
    for (;;) {
        qint64 bytes = 0;
        int kept = 0;
        auto oldest = m_entries.end();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->state == QLatin1String("loading"))
                continue;
            ++kept;
            bytes += it->image.sizeInBytes();
            if (oldest == m_entries.end() || it->used < oldest->used)
                oldest = it;
        }
        if (oldest == m_entries.end() || (kept <= MaxKept && bytes <= MaxKeptBytes))
            return;
        m_ids.remove(oldest->id);
        m_entries.erase(oldest);
    }
}

QSize MediaLoader::size(const QString &url) const { return m_entries.value(url).size; }

QString MediaLoader::source(const QString &url) const
{
    const Entry entry = m_entries.value(url);
    return entry.state == QLatin1String("ready")
               ? QStringLiteral("image://openghost-media/") + entry.id
               : QString();
}

bool MediaLoader::trusted(const QString &url) const { return media::trusted(url); }

double MediaLoader::stackRatio(double raw, bool many) const { return media::stackRatio(raw, many); }

int MediaLoader::stackWidth(double ratio) const { return media::stackWidth(ratio); }

bool MediaLoader::needsBackdrop(int width, int height, double ratio) const
{
    return media::needsBackdrop(QSize(width, height), ratio);
}

QStringList MediaLoader::thumbnails(const QString &id) const { return media::thumbnails(id); }

QImage MediaLoader::image(const QString &id)
{
    QMutexLocker locker(&m_lock);
    const QString url = m_ids.value(id);
    auto found = m_entries.find(url);
    if (url.isEmpty() || found == m_entries.end())
        return {};
    found->used = ++m_clock;
    return found->image;
}

void MediaLoader::setFetch(Fetch fetch)
{
    m_fetch = std::move(fetch);
    forget();
}

void MediaLoader::forget()
{
    ++m_generation;
    {
        QMutexLocker locker(&m_lock);
        m_entries.clear();
        m_ids.clear();
    }
    emit changed();
}

QImage MediaLoader::decode(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    if (bytes.isEmpty() || bytes.size() > MaxBytes || !buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    reader.setAllocationLimit(256);
    static const QList<QByteArray> formats{"png", "jpeg", "jpg", "webp", "gif"};
    if (!formats.contains(reader.format().toLower()))
        return {};
    const QSize natural = reader.size();
    if (!natural.isValid() || natural.isEmpty() || natural.width() > MaxEdge ||
        natural.height() > MaxEdge)
        return {};
    if (natural.width() > ShownEdge || natural.height() > ShownEdge)
        reader.setScaledSize(natural.scaled(ShownEdge, ShownEdge, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull())
        return {};
    // Its own shape is what the stack measures, however it was scaled.
    image.setText(QStringLiteral("openghost-natural"), QStringLiteral("1"));
    image.setText(QStringLiteral("openghost-natural-w"), QString::number(natural.width()));
    image.setText(QStringLiteral("openghost-natural-h"), QString::number(natural.height()));
    return image;
}

QImage MediaImages::requestImage(const QString &id, QSize *size, const QSize &)
{
    const QImage image = MediaLoader::instance()->image(id);
    if (size)
        *size = image.size();
    return image;
}
