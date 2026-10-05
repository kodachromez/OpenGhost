#pragma once
#include "backend/types.h"
#include <QObject>

namespace openghost
{
// UI-facing host services a desktop frontend supplies to the backend
// (desktop/preload.js, host-tools.js, browser-host-tools.md): the built-in
// browser's state and the host.tool calls run against it. A build without the
// browser (OPENGHOST_BROWSER=OFF) uses NoHost: no browser panel (null state).
// The desktop Browser owns its optional BrowserAutomation implementation.
// An OFF build has no engine linkage or advertised browser tools.
class HostServices : public QObject
{
    Q_OBJECT
  public:
    using QObject::QObject;
    // nullopt: no browser panel (host.browser = null), never an invented one.
    virtual std::optional<BrowserState> browser() const = 0;
    // Published at initialize (host.tools); only these names may be called.
    virtual QVector<HostToolSchema> tools() const = 0;
    // Must settle with finished() exactly once, never inline, unless cancelled.
    virtual void run(RequestId id, const HostToolRequest &request) = 0;
    // The call was cancelled by the backend or its turn ended: release it.
    virtual void cancel(RequestId id) = 0;
    // The chat's turn ended (chat.js end): whatever the browser held for it
    // (its driving, a wait for the user's hand-back) ends with it.
    virtual void turnEnded(const QString &sessionId) { Q_UNUSED(sessionId) }
    // A new queued user message releases that session's hand-back wait, never
    // replays the interrupted browser action.
    virtual void inputQueued(const QString &sessionId) { Q_UNUSED(sessionId) }
  signals:
    void finished(openghost::RequestId id, const openghost::HostToolResult &result);
    void browserChanged(const openghost::BrowserState &state);
};

class NoHost final : public HostServices
{
    Q_OBJECT
  public:
    using HostServices::HostServices;
    std::optional<BrowserState> browser() const override { return std::nullopt; }
    QVector<HostToolSchema> tools() const override { return {}; }
    void run(RequestId, const HostToolRequest &) override {}
    void cancel(RequestId) override {}
};
} // namespace openghost
