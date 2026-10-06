#pragma once
#include "backend.h"
#include "pi_process.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <map>

namespace openghost
{
// Pi through `pi --mode rpc`. Every OpenGhost session (a chat, or its `<id>:mini`)
// is its own Pi session file under the session directory, run by its own Pi child
// in the chat's folder: chats never share history, and each runs its own turn. A
// `--no-session` child serves the model catalog, providers and sign-in.
//
// A turn is admitted only by Pi's own prompt reply, streams Pi's messages, usage
// and steering receipts, and ends with Pi's agent_settled (or a confirmed abort).
// Its start and end are recorded in Pi's session (the bridge's `mark`), so after a
// restart session.get rebuilds exactly that turn from Pi's own entries. Standing
// instructions and General's pinned text files travel as named sections of Pi's
// system prompt (the bridge's `context`), never as history. A message's text files
// precede its text as Pi's own `pi @file` puts them; its pictures are Pi's prompt
// images, sent only to a model Pi says sees images. Delete stops the chat's child
// and removes its Pi session file. Providers, sign-in, logout and failed-turn Retry go through Pi's
// own runtime via the bridge (src/backend/pi/openghost-bridge.js); Pi stores every
// credential.
class PiBackend final : public Backend
{
    Q_OBJECT
  public:
    // `sessionDir` keeps Pi's session files. Empty: a temporary directory, so no
    // chat outlives this backend.
    explicit PiBackend(QString sessionDir = {}, QObject *parent = nullptr);
    ~PiBackend() override;
    void request(RequestId id, const Command &command) override;
    void cancelRequest(RequestId id) override;
    void answer(RequestId, const ReverseResult &) override {}
    void browserChanged(const BrowserState &) override {}
    QString sessionFile(const QString &session) const;

  private:
    using Done = PiProcess::Done;
    using Then = std::function<void(std::optional<Error>)>;
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
    // The turn a chat's Pi is running, from its start/retry request to agent_settled.
    struct Run {
        QString session, client, turn, version;
        RequestId request = 0;
        std::optional<DisplayInput> input;
        bool retry = false, sent = false, accepted = false, active = false;
        bool cancelled = false, aborting = false; // Stop asked / abort sent
        bool abandoned = false;                    // its request already failed
        bool prompted = false;                     // the prompt's own user message seen
        int stage = 0;                             // preparation before the prompt
        QString text;          // Pi's prompt message: the files' text, then the input's
        QJsonArray images;     // Pi's prompt images, in the input's order
        QJsonObject context;   // instructions and pinned files, for the bridge
        ModelSelection chosen;
        QString message, stopReason, errorMessage;
        QVector<Steer> steers;
        QString steering; // client input awaiting Pi's steer reply
        QStringList queue; // Pi's steering queue, as last reported
    };
    // One OpenGhost session and the Pi child running its session file.
    struct Chat {
        PiProcess *pi = nullptr;
        bool loaded = false, loading = false, deleting = false;
        QVector<Then> waiting; // until loaded
        QString version;       // its incarnation; empty: Pi has no such session
        Sequence seq = 0;
        std::optional<Run> run;
        QString last;                        // journal key of Pi's latest accepted turn
        QHash<QString, QPair<QString, bool>> recorded; // client -> turn, retry: taken before
        std::optional<QJsonObject> context; // what this child's bridge holds
        QVector<RequestId> deletes;          // DeleteSession calls awaiting erasure
    };
    void dispatch(RequestId id, const Command &command);
    Result execute(const Command &command);
    PiProcess *spawn(const QStringList &args, const QString &cwd, QString *error);
    PiProcess *child(const QString &session, const QString &cwd, QString *error);
    void retire(Chat &chat, std::function<void()> gone = {});
    void exited(const QString &session);
    void reap();
    void load(const QString &session, Then then);
    void loaded(const QString &session, const QJsonArray &entries);
    void entries(const QString &session, std::function<void(std::optional<QJsonArray>, Error)> done);
    void getSession(RequestId id, const GetSession &get);
    void configure(RequestId id, const ConfigureSession &configure);
    void remove(RequestId id, const QString &session);
    void erase(const QString &session);
    void start(RequestId id, Run run, const SessionParams &params);
    void advance(const QString &session, const QString &turn);
    Run *running(const QString &session, const QString &turn);
    void refuse(const QString &session, const Error &error);
    void submit(const QString &session);
    void admitted(const QString &session, const QString &turn, const QJsonObject &reply);
    void accept(const QString &session, const QString &disposition);
    void abort(const QString &session, std::function<void(const Result &)> stopped = {});
    void finish(const QString &session, TurnCompleted done);
    void steer(RequestId id, const SteerTurn &steer);
    void cancelTurn(RequestId id, const CancelTurn &cancel);
    std::optional<Error> admissible(const QString &session, const std::optional<QString> &version);
    void auth(RequestId id, const QString &provider, QJsonObject request);
    void setModel(PiProcess *pi, const QString &provider, const QString &model,
                  std::function<void(std::optional<Error>, ModelSelection)> done);
    void step(const QString &token, const QString &kind, const QJsonObject &value);
    void runEvent(const QString &session, const QString &type, const QJsonObject &object);
    Sequence next(Chat &chat);
    void publish(const QString &session, EventPayload payload, bool message = false);
    void remember(Journal journal, bool latest);
    QTemporaryDir m_bridgeDir, m_ownSessions;
    QString m_bridgePath, m_sessionDir;
    PiProcess *m_control = nullptr; // catalog, providers and sign-in
    std::map<QString, Chat> m_chats; // node-based: references survive insertion
    QHash<QString, Journal> m_journal; // session + '\n' + client turn
    QStringList m_order;               // journal keys, oldest first
    QHash<QString, QVector<std::function<void(const Result &)>>> m_stops; // turn -> awaiting abort
    QTimer m_reaper;
    QHash<QString, QString> m_logins;  // bridge token -> provider signing in
    QHash<QString, LoginStep> m_steps; // provider -> its current sign-in step
    QSet<QString> m_cancelled;         // providers whose sign-in was cancelled
    int m_tokens = 0;
};
} // namespace openghost
