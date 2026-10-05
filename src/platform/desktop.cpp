#include "platform.h"
#include <QDesktopServices>
#include <QUrl>

bool platform::reducedMotion()
{
    const QByteArray forced = qgetenv("OPENGHOST_REDUCED_MOTION");
    return forced.isEmpty() ? systemReducedMotion() : forced != "0";
}

bool platform::openLink(const QString &url, bool allowMailto)
{
    const QUrl target(url, QUrl::StrictMode);
    if (!target.isValid())
        return false;
    const QString scheme = target.scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https") &&
        !(allowMailto && scheme == QLatin1String("mailto")))
        return false;
    return QDesktopServices::openUrl(target);
}
