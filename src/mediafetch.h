#pragma once

#include <QByteArray>
#include <QNetworkRequest>
#include <functional>

class QNetworkAccessManager;

namespace media
{
// Frontend resource GET, not a general QML or backend fetch API. Each caller
// supplies its own initial-address and redirect policy. Entire-exchange deadline,
// bounded incremental capture (including unknown Content-Length), no credentials,
// cookies or cache. Completion runs once on the manager's thread; destruction
// cancels outstanding work. An empty result denotes refusal/failure.
using BytesDone = std::function<void(const QByteArray &)>;
using Redirect = std::function<bool(const QUrl &)>;
void get(QNetworkAccessManager &network, QNetworkRequest request, qint64 cap, int timeout,
         Redirect redirect, BytesDone done);
} // namespace media
