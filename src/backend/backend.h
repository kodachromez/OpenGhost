#pragma once
#include "types.h"
#include <QObject>

namespace openghost
{
// Semantic async port. Implementations and consumers live on the same owner
// thread. request() never emits a reply inline; each admitted call settles once.
// Events may precede its reply. Destroying the port abandons pending callbacks.
// Per connection, events and replies arrive in backend order; PluginChanged
// carries the connectionId of the Initialize it belongs to.
// No UI, QML, process, wire framing or persistence policy belongs in this class.
class Backend : public QObject
{
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual void request(RequestId id, const Command &command) = 0;
    virtual void cancelRequest(RequestId id) = 0; // NOT a rollback or turn.cancel
    virtual void answer(RequestId id, const ReverseResult &result) = 0;
    virtual void browserChanged(const BrowserState &state) = 0;
    // Whether the backend's permission plugin is on (the frontend's Permissions
    // switch). Off: no permission request is left waiting on a card the window
    // no longer shows; one already waiting is declined and withdrawn.
    virtual void setPermissionsEnabled(bool enabled) { Q_UNUSED(enabled) }
  signals:
    void replied(openghost::RequestId id, const openghost::Result &result);
    void sessionEvent(const openghost::SessionEvent &event);
    void globalEvent(const openghost::GlobalEvent &event);
    void reverseRequest(openghost::RequestId id, const openghost::ReverseRequest &request);
    void reverseCancelled(openghost::RequestId id);
    void closed(const openghost::Error &error);
};
} // namespace openghost
