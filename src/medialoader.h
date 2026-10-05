#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QSize>

#include <functional>

class QNetworkAccessManager;

// The pictures a reply shows (media-embed.js probe(), and the previews of
// its video cards). The window's QML engine is denied every network request
// (denyNetwork()); this is the one way bytes reach a reply, under the
// reference's rules: a picture from a trusted preview place loads by itself,
// one from anywhere else only once the person asked for it (`consented`),
// and only over http(s). A load sent on elsewhere must stay within the same
// rule. A load is bounded in time (media::LoadTimeout) and size (MaxBytes);
// it sends no cookies or credentials and keeps none. Decoded pictures are
// shown through the "openghost-media" image provider (MediaImages).
//
// A load that fails, is refused or decodes to nothing is "failed": the
// picture stays a link. Results are kept for the process, bounded by
// MaxKept pictures and MaxKeptBytes of pixels, oldest first.
class MediaLoader final : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

  public:
    static constexpr qint64 MaxBytes = 16 * 1024 * 1024;
    static constexpr int MaxEdge = 8192;   // Pictures larger than this are refused.
    static constexpr int ShownEdge = 1600; // Decoded pictures are scaled to fit this.
    static constexpr int MaxKept = 256;
    static constexpr qint64 MaxKeptBytes = 192ll * 1024 * 1024;

    // Bytes for an address, or an empty array when they did not come.
    using Done = std::function<void(const QByteArray &bytes)>;
    using Fetch = std::function<void(const QString &url, bool consented, const Done &done)>;

    static MediaLoader *instance();
    static MediaLoader *create(QQmlEngine *, QJSEngine *);

    // "" (never asked), "loading", "ready" or "failed".
    Q_INVOKABLE QString state(const QString &url) const;
    // Starts a load when there is none; false when the rules refuse it (an
    // untrusted picture without consent, or not http(s)).
    Q_INVOKABLE bool load(const QString &url, bool consented);
    Q_INVOKABLE QSize size(const QString &url) const;
    // The image provider's address for a ready picture, else empty.
    Q_INVOKABLE QString source(const QString &url) const;
    Q_INVOKABLE bool trusted(const QString &url) const;

    // media-slider.js arithmetic, for MediaStack.qml.
    Q_INVOKABLE double stackRatio(double raw, bool many) const;
    Q_INVOKABLE int stackWidth(double ratio) const;
    Q_INVOKABLE bool needsBackdrop(int width, int height, double ratio) const;
    Q_INVOKABLE QStringList thumbnails(const QString &id) const;

    // The provider's picture for an id from source().
    QImage image(const QString &id);

    // Replaces how bytes are fetched (tests and fixtures; null restores the
    // network) and forgets every kept result.
    void setFetch(Fetch fetch);
    // Forgets every kept result (a later load starts again).
    void forget();

    // Decodes bytes as a reply picture: PNG, JPEG, WebP or GIF (its first
    // frame), at most MaxEdge on a side, scaled to fit ShownEdge. Null when
    // they are none of these. Exposed for tests.
    static QImage decode(const QByteArray &bytes);

  signals:
    // A load started or settled, or everything was forgotten.
    void changed();
    void settled(const QString &url, bool ok);

  private:
    explicit MediaLoader(QObject *parent = nullptr);
    void fetchNetwork(const QString &url, bool consented, const Done &done);
    void finish(const QString &url, quint64 generation, const QImage &image);
    void trim();

    struct Entry {
        QString state;
        QImage image;
        QSize size;
        QString id;
        quint64 used = 0;
    };
    QHash<QString, Entry> m_entries;
    QHash<QString, QString> m_ids; // Provider id → address.
    mutable QMutex m_lock;         // Guards the pictures the provider reads.
    Fetch m_fetch;
    QNetworkAccessManager *m_network = nullptr;
    quint64 m_generation = 0, m_clock = 0, m_nextId = 0;
};

// image://openghost-media/<id>: a picture MediaLoader decoded. Never loads.
class MediaImages final : public QQuickImageProvider
{
  public:
    MediaImages() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requested) override;
};
