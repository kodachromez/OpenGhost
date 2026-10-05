#include "media.h"

#include <QRegularExpression>
#include <QUrl>

#include <algorithm>
#include <cmath>

// A port of the address rules in openghost/media-embed.js and the frame
// arithmetic in media-slider.js.
namespace media
{
namespace
{
using Rx = QRegularExpression;

const Rx &youtube()
{
    thread_local const Rx rx(
        QStringLiteral(
            R"(^https?:\/\/(?:www\.|m\.|music\.)?(?:youtube\.com\/(?:watch\?(?:[^#\s]*&)?v=|shorts\/|live\/|embed\/)|youtu\.be\/)([\w-]{11})(?![\w-]))"),
        Rx::CaseInsensitiveOption);
    return rx;
}

// media-embed.js TRUSTED: places that hand out what they hold and fetch
// nothing on request.
const QVector<Rx> &trustedPlaces()
{
    thread_local const QVector<Rx> places{
        Rx(QStringLiteral(R"(^https:\/\/[a-z0-9]+\.mm\.bing\.net\/th[?/])"),
           Rx::CaseInsensitiveOption),
        Rx(QStringLiteral(R"(^https:\/\/th\.bing\.com\/th[?/])"), Rx::CaseInsensitiveOption),
        Rx(QStringLiteral(R"(^https:\/\/i\d?\.ytimg\.com\/vi(?:_webp)?\/)"),
           Rx::CaseInsensitiveOption),
        Rx(QStringLiteral(R"(^https:\/\/upload\.wikimedia\.org\/wikipedia\/)"),
           Rx::CaseInsensitiveOption),
        Rx(QStringLiteral(R"(^https:\/\/encrypted-tbn\d\.gstatic\.com\/images\?)"),
           Rx::CaseInsensitiveOption),
    };
    return places;
}
} // namespace

QString videoId(const QString &url) { return youtube().match(url).captured(1); }

bool trusted(const QString &url)
{
    return std::any_of(trustedPlaces().begin(), trustedPlaces().end(),
                       [&url](const Rx &place) { return place.match(url).hasMatch(); });
}

bool fetchable(const QString &url)
{
    const QUrl parsed(url, QUrl::StrictMode);
    const QString scheme = parsed.scheme().toLower();
    return parsed.isValid() && !parsed.host().isEmpty() && parsed.userInfo().isEmpty() &&
           (scheme == QLatin1String("https") || scheme == QLatin1String("http"));
}

bool redirectAllowed(const QString &to, bool consented)
{
    return consented ? fetchable(to) : trusted(to);
}

QString hostOf(const QString &url)
{
    QString host = QUrl(url).host();
    if (host.startsWith(QLatin1String("www.")))
        host.remove(0, 4);
    return host;
}

QStringList thumbnails(const QString &id)
{
    if (id.isEmpty())
        return {};
    return {QStringLiteral("https://i.ytimg.com/vi/%1/hq720.jpg").arg(id),
            QStringLiteral("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(id)};
}

VideoWords videoWords(const QString &text, const QString &url)
{
    VideoWords out;
    const QString words = text.trimmed();
    static const Rx scheme(QStringLiteral("^https?://"), Rx::CaseInsensitiveOption);
    static const Rx prefix(QStringLiteral("^https?://(www\\.)?"));
    if (words.isEmpty() || scheme.match(words).hasMatch() ||
        QString(words).remove(QRegularExpression(QStringLiteral("^www\\."))) ==
            QString(url).remove(prefix))
        return out;
    QStringList bits =
        words.split(QRegularExpression(QStringLiteral("\\s+·\\s+")), Qt::SkipEmptyParts);
    static const Rx time(QStringLiteral(R"(^\d{1,2}(?::\d{2}){1,2}$)"));
    if (bits.size() > 1 && time.match(bits.last()).hasMatch())
        out.time = bits.takeLast();
    if (bits.size() > 1)
        out.by = bits.takeLast();
    out.title = bits.join(QStringLiteral(" · "));
    return out;
}

double stackRatio(double raw, bool many)
{
    if (!std::isfinite(raw) || raw <= 0)
        raw = FallbackRatio;
    const double *range = many ? ManyRatio : SingleRatio;
    return std::clamp(raw, range[0], range[1]);
}

int stackWidth(double ratio)
{
    return int(std::lround(std::min<double>(FrameWidth, FrameHeight * ratio)));
}

bool needsBackdrop(QSize size, double ratio)
{
    if (size.width() <= 0 || size.height() <= 0)
        return true;
    return std::abs(double(size.width()) / size.height() - ratio) > RatioMatch;
}
} // namespace media
