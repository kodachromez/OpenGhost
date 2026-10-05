#include "videoinfo.h"

#include "mediafetch.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>

bool VideoInfoService::validId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    return pattern.match(id).hasMatch() && id.size() == 11;
}

NetworkVideoInfo::NetworkVideoInfo(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this))
{
    // No ambient authenticated proxy, credentials, cookies or browser profile.
    m_network->setProxy(QNetworkProxy::NoProxy);
}

QUrl NetworkVideoInfo::address(const QString &id)
{
    if (!validId(id))
        return {};
    return QUrl(QStringLiteral("https://www.youtube.com/oembed?url=") +
                QString::fromLatin1(QUrl::toPercentEncoding(
                    QStringLiteral("https://www.youtube.com/watch?v=") + id)) +
                QStringLiteral("&format=json"));
}

bool NetworkVideoInfo::redirectAllowed(const QUrl &url, const QString &id)
{
    // No arbitrary redirect fetch, downgrade, port or userinfo. Query order can
    // change, but neither the video nor the endpoint can change.
    const QUrl expected = address(id);
    const QUrlQuery query(url), wanted(expected);
    return !expected.isEmpty() && url.scheme() == QLatin1String("https") &&
           url.host() == expected.host() && url.path() == expected.path() &&
           url.userInfo().isEmpty() && url.port(-1) == -1 && url.fragment().isEmpty() &&
           query.queryItems().size() == 2 &&
           query.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded) ==
               wanted.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded) &&
           query.queryItemValue(QStringLiteral("format")) == QLatin1String("json");
}

VideoInfo NetworkVideoInfo::parse(const QByteArray &bytes)
{
    if (bytes.size() > MaxBytes)
        return {};
    const auto data = QJsonDocument::fromJson(bytes);
    if (!data.isObject())
        return {};
    // oEmbed defines strings. Never display arbitrary JSON/markup as metadata.
    const auto object = data.object();
    return {object.value(QStringLiteral("title")).toString(),
            object.value(QStringLiteral("author_name")).toString()};
}

void NetworkVideoInfo::lookup(const QString &id, Done done)
{
    if (!validId(id)) {
        done({});
        return;
    }
    QNetworkRequest request(address(id));
    request.setRawHeader("Accept", "application/json");
    media::get(
        *m_network, request, MaxBytes, Timeout,
        [id](const QUrl &to) { return redirectAllowed(to, id); },
        [done](const QByteArray &bytes) { done(parse(bytes)); });
}

VideoTitles *VideoTitles::instance()
{
    static QPointer<VideoTitles> titles;
    if (!titles)
        titles = new VideoTitles({}, {}, QCoreApplication::instance());
    return titles;
}

VideoTitles *VideoTitles::create(QQmlEngine *, QJSEngine *)
{
    auto *titles = instance();
    QJSEngine::setObjectOwnership(titles, QJSEngine::CppOwnership);
    return titles;
}

VideoTitles::VideoTitles(std::shared_ptr<VideoInfoService> service, QString cachePath,
                         QObject *parent)
    : QObject(parent)
{
    setService(std::move(service), std::move(cachePath));
}

void VideoTitles::setService(std::shared_ptr<VideoInfoService> service, QString cachePath)
{
    ++m_generation;
    m_service = std::move(service);
    m_path = std::move(cachePath);
    m_asked.clear();
    m_kept.clear();
    m_order.clear();
    restore();
    emit changed();
}

QVariantMap VideoTitles::info(const QString &id) const
{
    const auto value = m_asked.value(id, m_kept.value(id));
    return {{QStringLiteral("title"), value.title}, {QStringLiteral("by"), value.by}};
}

void VideoTitles::request(const QString &id)
{
    if (!VideoInfoService::validId(id) || m_asked.contains(id))
        return;
    m_asked.insert(id, m_kept.value(id));
    if (m_kept.contains(id) || !m_service)
        return;
    const auto generation = m_generation;
    QPointer<VideoTitles> self(this);
    m_service->lookup(id, [self, id, generation](VideoInfo value) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, id, generation, value] {
                if (!self || self->m_generation != generation || value.title.isEmpty())
                    return;
                self->m_asked[id] = value;
                self->m_kept[id] = value;
                self->m_order.append(id);
                while (self->m_order.size() > MaxKept)
                    self->m_kept.remove(self->m_order.takeFirst());
                self->save();
                emit self->changed();
            },
            Qt::QueuedConnection);
    });
}

void VideoTitles::restore()
{
    if (m_path.isEmpty())
        return;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxKept * NetworkVideoInfo::MaxBytes)
        return;
    const auto bytes = file.read(qint64(MaxKept) * NetworkVideoInfo::MaxBytes + 1);
    if (bytes.size() > qint64(MaxKept) * NetworkVideoInfo::MaxBytes)
        return;
    // An ordered array preserves FIFO order (QJsonObject sorts its keys).
    const auto rows = QJsonDocument::fromJson(bytes).array();
    for (const auto &row : rows) {
        const auto object = row.toObject();
        const auto id = object[QStringLiteral("id")].toString();
        VideoInfo info{object[QStringLiteral("title")].toString(),
                       object[QStringLiteral("by")].toString()};
        if (!VideoInfoService::validId(id) || info.title.isEmpty() || m_kept.contains(id) ||
            QJsonDocument(object).toJson().size() > NetworkVideoInfo::MaxBytes)
            continue;
        m_kept.insert(id, info);
        m_order.append(id);
        if (m_order.size() > MaxKept)
            m_kept.remove(m_order.takeFirst());
    }
}

void VideoTitles::save()
{
    if (m_path.isEmpty())
        return;
    QJsonArray rows;
    for (const auto &id : m_order) {
        const auto info = m_kept.value(id);
        rows.append(QJsonObject{{QStringLiteral("id"), id},
                                {QStringLiteral("title"), info.title},
                                {QStringLiteral("by"), info.by}});
    }
    const auto bytes = QJsonDocument(rows).toJson(QJsonDocument::Compact);
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile file(m_path);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size())
        file.commit(); // Advisory presentation cache, like localStorage: best effort.
}
