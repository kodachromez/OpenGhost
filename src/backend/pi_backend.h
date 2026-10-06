#pragma once
#include "backend.h"
#include <QHash>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <functional>

namespace openghost
{
// Proof-of-concept: one `pi --mode rpc --no-session` child. A turn is admitted only
// by Pi's own prompt reply, streams Pi's messages, usage and steering receipts, and
// ends with Pi's agent_settled (or a confirmed abort). Models come from Pi's
// get_available_models and selection uses set_model. Providers, sign-in, logout and
// failed-turn Retry go through Pi's own runtime via the OpenGhost bridge extension
// (src/backend/pi/openghost-bridge.js); Pi stores every credential. One Pi context
// serves every chat, and turn journals live only as long as this process.
class PiBackend final : public Backend
{
    Q_OBJECT
  public:
    explicit PiBackend(QObject *parent = nullptr);
    ~PiBackend() override;
    void request(RequestId id, const Command &command) override;
    void cancelRequest(RequestId id) override;
    void answer(RequestId, const ReverseResult &) override {}
    void browserChanged(const BrowserState &) override {}

  private:
    using Done = std::function<void(const QJsonObject &)>;
    // One accepted turn's display events, addressable by its client turn ID.
    struct Journal {
        QString session, client, turn;
        std::optional<DisplayInput> input; // absent for a retry
        QVector<SessionEvent> events;
        bool terminal = false;
        TurnStatus status = TurnStatus::Done;
    };
    struct Steer {
        QString client, queued; // queued: the text Pi put in its steering queue
        DisplayInput input;
        bool admitted = false, applied = false;
    };
    // The one turn Pi is running, from its start/retry request to agent_settled.
    struct Run {
        QString session, client, turn;
        RequestId request = 0;
        std::optional<DisplayInput> input;
        bool retry = false, sent = false, accepted = false, active = false;
        bool cancelled = false, aborting = false; // Stop asked / abort sent
        bool abandoned = false;                    // its request already failed
        bool prompted = false;                     // the prompt's own user message seen
        QString message, stopReason, errorMessage;
        QVector<Steer> steers;
        QString steering; // client input awaiting Pi's steer reply
        QStringList queue; // Pi's steering queue, as last reported
    };
    struct Session {
        QString version;
        Sequence seq = 0;
    };
    void dispatch(RequestId id, const Command &command);
    Result execute(const Command &command);
    void begin(RequestId id, Run run, const ModelSelection &chosen, const QString &text);
    void submit(const QString &text);
    void admitted(const QString &turn, const QJsonObject &reply);
    void accept(const QString &disposition);
    void abort();
    void finish(TurnCompleted done);
    void steer(RequestId id, const SteerTurn &steer);
    void cancelTurn(RequestId id, const CancelTurn &cancel);
    std::optional<Error> admissible(const QString &session, const std::optional<QString> &version);
    bool send(const QJsonObject &command);
    void rpc(QJsonObject command, Done done, int deadline = 30000);
    void bridge(QJsonObject request, Done done);
    void auth(RequestId id, const QString &provider, QJsonObject request);
    void setModel(const QString &provider, const QString &model,
                  std::function<void(std::optional<Error>)> done);
    void step(const QString &token, const QString &kind, const QJsonObject &value);
    void readStdout();
    void line(const QByteArray &bytes);
    void runEvent(const QString &type, const QJsonObject &object);
    void publish(EventPayload payload, bool message = false);
    QProcess m_pi;
    QByteArray m_buffer;
    std::optional<Run> m_run;
    QHash<QString, Session> m_sessions;  // OpenGhost chat -> its incarnation, once Pi accepted
    QHash<QString, Journal> m_journal;   // session + '\n' + client turn
    QStringList m_order;                 // journal keys, oldest first
    QString m_last;                      // the latest accepted turn's journal key
    QHash<QString, QVector<RequestId>> m_cancels; // turn -> CancelTurn waiting on abort
    int m_rpc = 0;
    QHash<QString, Done> m_waiting; // RPC id, or "bridge:" + bridge token
    enum class Bridge { Unknown, Ready, Missing } m_bridge = Bridge::Unknown;
    QVector<QPair<QJsonObject, Done>> m_bridgeQueue; // until get_commands answers
    QTemporaryDir m_bridgeDir;
    QString m_bridgePath;
    QHash<QString, QString> m_logins; // bridge token -> provider signing in
    QHash<QString, LoginStep> m_steps; // provider -> its current sign-in step
    QSet<QString> m_cancelled;         // providers whose sign-in was cancelled
    QString m_modelProvider, m_model;  // what set_model last made Pi use
};
} // namespace openghost
