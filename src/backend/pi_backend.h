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
// credential. Each sign-in is a flow of its own: a cancelled or superseded one's
// steps and outcome never reach the newer one.
//
// OpenGhost owns no permission policy: whether a tool call may run is Pi's and its
// permission plugin's. Ask / Auto / Full is relayed as the chat's mode (the
// bridge's `mode`, said to Pi's extensions), set on the chat's child before every
// run and as soon as the chat changes mode (idle or not); a later update always
// wins. A permission request is Pi's own extension UI confirm titled
// "openghost:approval", shown as an approval card (a reverse request) and answered
// once, only to the child and turn that asked; Stop, the turn's end or Pi taking it
// back withdraws the card. Any other extension dialog is cancelled at once and
// said, as are extension errors: nothing waits on a question OpenGhost cannot show.
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
    void answer(RequestId id, const ReverseResult &result) override;
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
        QString client;
        std::optional<QString> queued; // exact queued text, including an empty transform
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
        PermissionMode mode = PermissionMode::Ask;
        ModelSelection chosen;
        QString message, stopReason, errorMessage, failedTurn;
        QVector<Steer> steers;
        QString steering; // client input awaiting Pi's steer reply
        std::optional<TurnCompleted> settled; // wait for an in-flight steer before ending
        bool clearing = false;
        QStringList queue; // Pi's steering queue, as last reported
        QHash<QString, QString> calls; // tool calls shown this run: id -> name
        int thought = -1; // the streaming message's latest thinking block (content index)
    };
    // One OpenGhost session and the Pi child running its session file.
    struct Chat {
        PiProcess *pi = nullptr;
        PiProcess *retiring = nullptr; // still owns the file until process exit
        bool loaded = false, loading = false, deleting = false, stopping = false;
        QVector<Then> waiting; // until loaded
        QString version;       // its incarnation; empty: Pi has no such session
        Sequence seq = 0;
        std::optional<Run> run;
        QString last;                        // journal key of Pi's latest accepted turn
        QHash<QString, QPair<QString, bool>> recorded; // client -> turn, retry: taken before
        std::optional<QJsonObject> context; // what this child's bridge holds
        std::optional<PermissionMode> mode; // the access mode this child's bridge holds
        quint64 modeSent = 0;                // the latest mode update sent to this child
        int auth = 0;                       // the configuration generation its Pi has read
        QVector<RequestId> deletes;          // DeleteSession calls awaiting erasure
    };
    // A tool call waiting on the user: Pi's confirm dialog, shown as a card.
    struct Approval {
        QString session, turn, dialog, approvalId;
        PiProcess *pi = nullptr; // the child whose dialog this is: only it is answered
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
    bool recoverJournal(const QString &session, const QString &client, const QJsonArray &entries);
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
    bool dialog(const QString &session, PiProcess *pi, const QJsonObject &request);
    void approvalEnded(const QString &session, const QString &approvalId, const QJsonObject &value);
    void withdrawApprovals(const QString &session, bool answer);
    void report(const QString &level, const QString &message);
    void extensionRecord(const QString &type, const QJsonObject &record);
    void setMode(const QString &session, PermissionMode mode,
                 std::function<void(std::optional<Error>)> done);
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
    QHash<QString, QString> m_flows;   // provider -> its current sign-in's token
    QHash<RequestId, QString> m_flowOf; // Login/SetKey/CancelLogin request -> its flow
    QHash<QString, LoginStep> m_steps; // provider -> its current sign-in step
    // provider -> the prompt its current sign-in waits on, as Pi asked it. It stays
    // open through Pi's notifications until answered, withdrawn, replaced, cancelled
    // or ended.
    QHash<QString, LoginStep> m_prompts;
    QSet<RequestId> m_pendingStarts, m_withdrawn; // cancellation before dispatch/load
    int m_tokens = 0;
    // Pi's configuration generation: bumped whenever the control child rereads
    // models.json and credentials, or a sign-in/logout changes them. A chat's Pi
    // that read an older one rereads before its next run.
    int m_auth = 1;
    QHash<RequestId, Approval> m_approvals;   // reverse request -> its Pi dialog
    RequestId m_reverse = RequestId(1) << 48; // reverse request IDs, apart from callers'
    quint64 m_modeSeq = 0; // access mode updates, in the order they were asked for
};
} // namespace openghost
