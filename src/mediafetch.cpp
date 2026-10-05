#include "mediafetch.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <memory>

namespace media
{
void get(QNetworkAccessManager &network, QNetworkRequest request, qint64 cap, int timeout,
         Redirect redirect, BytesDone done)
{
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(3);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(timeout);
    auto *reply = network.get(request);
    reply->setReadBufferSize(std::min<qint64>(cap + 1, 64 * 1024));
    auto bytes = std::make_shared<QByteArray>();
    auto refused = std::make_shared<bool>(false);
    const auto drain = [reply, cap, bytes, refused] {
        if (*refused)
            return;
        const auto length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (reply->isOpen())
            bytes->append(reply->read(std::max<qint64>(0, cap + 1 - bytes->size())));
        if (length > cap || bytes->size() > cap) {
            *refused = true;
            bytes->clear();
            reply->abort();
        }
    };
    // Unlike the transfer timeout, this also bounds slow-but-steady replies
    // and the entire redirect chain.
    QTimer::singleShot(timeout, reply, [reply, refused] {
        *refused = true;
        reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::redirected, reply,
                     [reply, redirect, bytes, refused](const QUrl &to) {
                         bytes->clear();
                         if (redirect(to))
                             emit reply->redirectAllowed();
                         else {
                             *refused = true;
                             reply->abort();
                         }
                     });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, drain);
    QObject::connect(reply, &QIODevice::readyRead, reply, drain);
    QObject::connect(
        reply, &QNetworkReply::finished, &network, [reply, bytes, refused, drain, done] {
            drain();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool ok = !*refused && reply->error() == QNetworkReply::NoError &&
                            status >= 200 && status < 300;
            done(ok ? *bytes : QByteArray());
            reply->deleteLater();
        });
}
} // namespace media
