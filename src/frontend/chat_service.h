#pragma once
#include "backend/backend.h"
#include "host.h"
#include "library.h"
#include "plugins.h"
#include "preferences.h"
#include <QHash>
#include <QSet>
#include <functional>
#include <memory>

namespace openghost
{
struct DisplayRow {
    // Moved: a mini chat's "caught up" line. Preserved: a saved entry this port
    // does not draw (compaction/stats), kept unchanged for the next save.
    // Tool: one tool call's card (its text is the call's output).
    enum class Role { User, Assistant, Note, Moved, Preserved, Tool } role = Role::Note;
    DisplayRow() = default;
    DisplayRow(Role role, QString key, QString text, QString state,
               QVector<DisplayAttachment> attachments, QString metrics, QString tip, qint64 started,
               qint64 completed)
        : role(role), key(std::move(key)), text(std::move(text)), state(std::move(state)),
          attachments(std::move(attachments)), metrics(std::move(metrics)), tip(std::move(tip)),
          started(started), completed(completed)
    {
    }
    QString key, text, state;
    QVector<DisplayAttachment> attachments;
    QString metrics, tip;
    qint64 started = -1, completed = -1;
    // Frontend recovery checkpoint fields (library.js displayMessages), never
    // model input: the client turn ID, steering ID, and whether the turn's
    // outcome is not yet settled.
    QString backendTurn, clientInputId, remoteTurn, model;
    bool pendingTurn = false, hidden = false, uncounted = false;
    struct Spent {
        double input = 0, cached = 0, written = 0, output = 0, requests = 0;
    };
    std::optional<Spent> usage;
    QJsonObject raw;
    // Assistant: a part of its turn's reply that follows tool calls.
    bool continued = false;
    // Tool: the call (state is running, done, error, cancelled, missing or
    // unconfirmed; never done without the backend's result saying so).
    struct Tool {
        QString callId, name;
        QString arguments; // The model's JSON text as shown, when known.
        bool known = false;
        QString ending; // How it ended, a line each, when the backend said more.
        // Live output dropped from its front: whole lines, then the cut start
        // of the first line kept; `trimmed` counts UTF-16 units (not saved).
        double omittedLines = 0, omittedCharacters = 0, trimmed = 0;
        qsizetype removed = 0; // Units of the latest output snapshot not kept.
        QString after;         // The reply part (row key) whose message made the call.
        bool saved = false;    // Settled by the backend's saved result.
    } tool;
};
struct ChatRecord {
    QString id, title;
    bool renamed = false;
    qint64 created = 0, updated = 0;
    QString folder;      // project folder; empty for a chat without one
    QString parent;      // set: the mini chat of that chat (session `<parent>:mini`)
    bool pinned = false; // library index state
    bool restored = false, loaded = true, locked = false, recovering = false;
    double tokens = 0;           // context used, as last reported
    qint64 seen = 0;             // mini: the parent's `updated` at its latest question
    QVector<SessionEvent> raced; // live events that raced session.get
    std::optional<QString> version;
    ModelSelection selection;
    PermissionMode mode = PermissionMode::Ask;
    QVector<DisplayRow> rows;
    Sequence sequence = -1;
    bool reconciled = true;
    struct Turn {
        QString clientId, remoteId, replyKey;
        bool terminal = false, stopped = false, retryable = false;
        qint64 started = -1, completed = -1;
        double input = 0, output = 0, cached = 0, written = 0, requests = 0;
        bool dispatched = false, acknowledged = false, invalidStart = false;
        RequestId request = 0;               // The start/retry call; Stop cancels only this one.
        QHash<QString, RequestId> hostCalls; // toolCallId -> reverse request
        QSet<QString> hostSeen; // completed/cancelled IDs remain spent for this turn
        std::optional<Usage::Context> context;
        QString activity, finishReason;
        QHash<QString, QString> tools; // toolCallId -> its latest card's row key
        int toolCount = 0;
        bool continued = false; // The next reply part follows tool calls.
        QHash<QString, QString> steering;          // clientInputId -> display row
        QHash<QString, DisplayRow> recoveryInputs; // saved queued inputs during replay
        QVector<SessionEvent> early;
        std::optional<StartTurn> prepared; // exact input; never reconstructed from previews
        int firstRow = 0;
        quint64 submission = 0;
        struct Message {
            QString id, text, rowKey;
            bool sealed = false;
        };
        QVector<Message> messages;
    } turn;
    QHash<QString, Turn> pastTurns; // Correlated late usage only.
};
struct PendingApproval {
    RequestId request = 0;
    ApprovalRequest data;
};

// Frontend state, separate from both rendering and transport. The Library holds
// display caches, recovery markers and the chat index: never model history.
// Every start/retry/steer first commits a required checkpoint; reopening reads
// session.get by the saved pending client turn and never resends input.
class ChatService final : public QObject
{
    Q_OBJECT
  public:
    ChatService(Backend *backend, PreferencesStore *preferences, Library *library,
                HostServices *host = nullptr, QObject *parent = nullptr);
    // Ephemeral: an owned in-memory library (tests; nothing survives exit).
    ChatService(Backend *backend, PreferencesStore *preferences, QObject *parent = nullptr)
        : ChatService(backend, preferences, nullptr, nullptr, parent)
    {
    }
    void initialize();
    Plugins *plugins() { return &m_plugins; }
    const Plugins *plugins() const { return &m_plugins; }
    const QVector<Model> &models() const { return m_models; }
    const QVector<Provider> &providers() const { return m_providers; }
    QVector<ChatRecord> chats() const;
    const ChatRecord &current() const;
    QString status() const { return m_status; }
    bool connected() const { return m_ready; }
    bool ready() const { return m_ready && current().reconciled; }
    bool busy() const { return !current().turn.clientId.isEmpty() && !current().turn.terminal; }
    bool pending() const { return m_pending || m_cancelling; }
    bool canSwitch() const { return !pending(); }
    bool canCancel() const { return busy() && !current().turn.stopped; }
    bool canSteer() const
    {
        return ready() && busy() && !pending() && !current().turn.remoteId.isEmpty();
    }
    bool canRetry() const { return m_ready && !busy() && !pending() && current().turn.retryable; }
    const QVector<PendingApproval> &approvals() const { return m_approvals; }
    void newChat(const QString &folder = {});
    void open(const QString &id);
    Library *library() const { return m_library; }
    QString folderOf(const ChatRecord &chat) const;
    bool setPinned(const QString &id, bool pinned);
    bool toggleFolder(const std::optional<QString> &folder);
    QString addFolder(const QString &path, const QString &name = {});
    void removeFolder(const QString &folder);
    // Chat locks: state only; the cipher is the injected host sealer.
    QString protect(const QString &id, const QString &password);
    QString unlock(const QString &id, const QString &password);
    void lock(const QString &id);
    QString unprotect(const QString &id);
    // The mini chat of the open chat (mini-chat.js / SideChat). Closing keeps it.
    QString openMini();
    void closeMini();
    const ChatRecord *mini() const;
    bool miniBehind() const;
    quint64 sendMini(const QString &text, const QVector<Attachment> &attachments = {});
    void stopMini();
    void retryMini();
    void approveMini(RequestId id, bool allow);
    void clearMini();
    quint64 send(const QString &text, const QVector<Attachment> &attachments = {});
    void retry();
    void remove(const QString &id);
    void refresh();
    // `flow` names the sign-in form it belongs to; authFinished hands it back, so a
    // cancelled or replaced sign-in's late end never touches a newer form. Each call
    // is the provider's latest attempt until the next: only the latest attempt's
    // result (or an answer to it) sets or clears the provider's error and status.
    // An older one still settles (authFinished) and rereads the catalog.
    void authenticate(const Command &command, const QString &flow = {});
    void answerLogin(const AnswerLogin &answer); // Not an auth mutation: no authFinished.
    void approve(RequestId id, bool allow);
    // A card action the request offered (ApprovalRequest::actions): it is the
    // answer (approve… allows, deny… denies) with how wide an Allow is or why a
    // Deny was given. `note`: a deny reason; `scope`: "subagent" or "session".
    // Anything the request did not offer is ignored. OpenGhost decides nothing
    // here: the asker (Pi's plugin-permissions) applies it.
    void decide(RequestId id, const QString &action, const QString &note, const QString &scope);
    // The Permissions switch, told to the backend: off, it declines and withdraws
    // a waiting request (the card goes through reverseCancelled) and asks no more.
    void setPermissionsEnabled(bool enabled)
    {
        if (m_backend)
            m_backend->setPermissionsEnabled(enabled);
    }
    static QString metrics(const ChatRecord::Turn &turn);
    static QJsonArray entries(const QVector<DisplayRow> &rows);
    static QVector<DisplayRow> rowsOf(const QJsonArray &saved);
    void stop();
    void choose(const ModelSelection &selection, bool thinkingPreference);
    void setMode(PermissionMode mode);
    void rename(const QString &id, const QString &title);
  signals:
    void changed();
    void catalogChanged();
    void accepted(quint64 submission);
    void submissionFailed(quint64 submission);
    void replaced(const QString &left);
    void answered();
    void worked();
    void removed(QString id, bool ok);
    void folderRemoved(QString folder, bool ok, QString notice);
    void miniCleared(bool ok);
    void usageRecorded(openghost::Usage usage);
    void authFinished(const QString &flow);
    void loginStep(const openghost::LoginStep &step);

  private:
    struct Focus; // Directs the public turn operations at the mini chat.
    using Completion = std::function<void(const Result &)>;
    ChatRecord restoredRecord(const Library::Chat &chat) const;
    void syncIndex();
    bool load(ChatRecord &chat);
    void reconcile(const QString &id);
    bool recoverTurn(ChatRecord &chat, const RecoveredTurn &snapshot);
    bool checkpoint(ChatRecord &chat);
    void save(ChatRecord &chat);
    void settlePending(ChatRecord &chat, const QString &clientTurn);
    void cancelHost(ChatRecord::Turn &turn);
    void deleteNext(QStringList sessions, const QString &id, std::function<void(bool, Error)> done);
    RequestId call(const Command &command, Completion completion);
    ChatRecord &editable();
    void event(const SessionEvent &event);
    void applyEvent(ChatRecord &chat, const SessionEvent &event, bool replay = false);
    void drainEarly(ChatRecord &chat);
    void reverse(RequestId id, const ReverseRequest &request);
    void dismissApprovals(const QString &session, const QString &reason);
    void endTurn(ChatRecord &chat, const TurnCompleted &done, bool replay = false,
                 bool local = false);
    void cancelRemote(ChatRecord &chat);
    void updateMetrics(ChatRecord &chat, ChatRecord::Turn &turn);
    SessionParams params(const ChatRecord &chat) const;
    quint64 steer(const Input &input);
    bool dispatchStart(const StartTurn &start, quint64 submission, const QString &userKey);
    void checkpointFailed(ChatRecord &chat);
    void finishRows(ChatRecord &chat, const QString &state);
    void publishReply(ChatRecord &chat);
    DisplayRow *toolRow(ChatRecord &chat, const QString &callId);
    DisplayRow *addTool(ChatRecord &chat, const QString &callId, const QString &name);
    void chooseSaved(const ModelSelection &selection, bool thinkingPreference);
    ModelSelection preferredModel() const;
    void problem(const Error &error);
    Backend *m_backend;
    Plugins m_plugins;
    PreferencesStore *m_preferences;
    std::unique_ptr<MemoryKeyStore> m_ownStore;
    std::unique_ptr<Library> m_ownLibrary;
    Library *m_library;
    HostServices *m_host;
    NoHost m_noHost;
    QString m_mini;
    bool m_miniMoved = false;
    QHash<RequestId, QPair<QString, QString>> m_hostRequests; // -> session, tool call
    QHash<RequestId, Completion> m_calls;
    QHash<QString, ChatRecord> m_chats;
    ChatRecord m_draft;
    QString m_current, m_status;
    QVector<Model> m_models;
    QVector<Provider> m_providers;
    QHash<QString, Error> m_authErrors;
    QHash<QString, quint64> m_authAttempt; // provider -> its latest auth attempt
    quint64 m_authAttempts = 0;
    QVector<PendingApproval> m_approvals;
    QVector<PendingApproval> m_earlyApprovals;
    QSet<QString> m_reverseSeen;
    bool m_refreshing = false, m_refreshAgain = false;
    RequestId m_next = 0;
    quint64 m_submission = 0;
    bool m_ready = false, m_pending = false, m_cancelling = false;
};
} // namespace openghost
