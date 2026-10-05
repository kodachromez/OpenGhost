#pragma once

#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <functional>
#include <memory>

class QNetworkAccessManager;

struct VideoInfo {
    QString title, by;
};

// Frontend host seam: only an eleven-character YouTube ID, never a model URL.
// Empty info is failure. Implementations complete once on the calling/UI thread,
// or cancel on destruction. No Rust, RPC, FFI, browser or agent contract involved.
class VideoInfoService
{
  public:
    using Done = std::function<void(VideoInfo)>;
    virtual ~VideoInfoService() = default;
    virtual void lookup(const QString &id, Done done) = 0;
    static bool validId(const QString &id);
};

// Production local host implementation of desktop/main.js::videoInfo: HTTPS
// YouTube oEmbed only, 10 seconds, title/author_name or nothing. Added native
// safety bounds: 64 KiB JSON, 3 redirects restricted to the same endpoint.
class NetworkVideoInfo final : public QObject, public VideoInfoService
{
  public:
    static constexpr int Timeout = 10000, MaxBytes = 64 * 1024;
    explicit NetworkVideoInfo(QObject *parent = nullptr);
    void lookup(const QString &id, Done done) override;
    static QUrl address(const QString &id);
    static bool redirectAllowed(const QUrl &url, const QString &id);
    static VideoInfo parse(const QByteArray &bytes);

  private:
    QNetworkAccessManager *m_network;
};

// Presentation/cache layer, matching media-embed.js asked/keptInfo/keep.
// Successful metadata is FIFO-capped at 300 on disk. Failures are shared for
// this process but never persisted. Cache I/O failure does not hide real info.
class VideoTitles final : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
  public:
    static constexpr int MaxKept = 300;
    static VideoTitles *instance();
    static VideoTitles *create(QQmlEngine *, QJSEngine *);
    explicit VideoTitles(std::shared_ptr<VideoInfoService> service, QString cachePath = {},
                         QObject *parent = nullptr);
    Q_INVOKABLE void request(const QString &id);
    Q_INVOKABLE QVariantMap info(const QString &id) const;
    // Injectable host (also used by offline fixtures). Clears runtime entries;
    // cachePath empty disables persistence. Late old-host completions are ignored.
    void setService(std::shared_ptr<VideoInfoService> service, QString cachePath = {});
  signals:
    void changed();

  private:
    void restore();
    void save();
    std::shared_ptr<VideoInfoService> m_service;
    QString m_path;
    QHash<QString, VideoInfo> m_asked, m_kept;
    QStringList m_order;
    quint64 m_generation = 0;
};
