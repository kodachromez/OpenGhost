#pragma once
#include "backend.h"
#include <QHash>
#include <QSet>
#include <QTimer>

namespace openghost
{
// Explicit development fixture. No durability, credentials, files, tools, model
// access or network. It is never silently substituted for an absent backend.
class FakeBackend final : public Backend
{
    Q_OBJECT
  public:
    explicit FakeBackend(QObject *parent = nullptr, int intervalMs = 35);
    void request(RequestId id, const Command &command) override;
    void cancelRequest(RequestId id) override;
    void answer(RequestId, const ReverseResult &) override {}
    void browserChanged(const BrowserState &) override {}
    void advance(); // One deterministic streaming step; intervalMs=0 disables timer.
  private:
    struct Turn {
        StartTurn start;
        RecoveredTurn recovery;
        QString messageId, response;
        qsizetype offset = 0;
        bool terminal = false;
    };
    struct Session {
        QString version, active;
        Sequence seq = 0;
        SessionParams params;
        QHash<QString, Turn> turns; // clientTurnId -> full accepted journal
    };
    Result execute(const Command &command);
    void publish(const QString &sessionId, Session &session, Turn &turn, EventPayload payload,
                 bool message = false);
    QHash<QString, Session> m_sessions;
    QSet<RequestId> m_pending;
    QTimer m_timer;
    bool m_initialized = false;
};
} // namespace openghost
