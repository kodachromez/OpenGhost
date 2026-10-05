#pragma once
#include "backend/backend.h"
#include "preferences.h"
#include <QHash>
#include <QSet>
#include <functional>

namespace openghost
{
struct DisplayRow {
    enum class Role { User, Assistant, Note } role = Role::Note;
    QString key, text, state;
    QVector<DisplayAttachment> attachments;
    QString metrics, tip;
    qint64 started = -1, completed = -1;
};
struct ChatRecord {
    QString id, title;
    bool renamed = false;
    qint64 created = 0, updated = 0;
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
        double input = 0, output = 0, cached = 0;
        std::optional<Usage::Context> context;
        QString activity, finishReason;
        struct ToolState {
            QString name, state;
            QJsonObject progress, result;
        };
        QHash<QString, ToolState> tools;
        QHash<QString, QString> steering; // clientInputId -> display row
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

// Frontend state, separate from both rendering and transport. Cache is display
// only, deliberately in-memory for this extraction; it is never model history.
// This exercised subset is NOT yet a production recovery/checkpoint client.
class ChatService final : public QObject
{
    Q_OBJECT
  public:
    ChatService(Backend *backend, PreferencesStore *preferences, QObject *parent = nullptr);
    void initialize();
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
    void newChat();
    void open(const QString &id);
    quint64 send(const QString &text, const QVector<Attachment> &attachments = {});
    void retry();
    void remove(const QString &id);
    void refresh();
    void authenticate(const Command &command);
    void approve(RequestId id, bool allow);
    static QString metrics(const ChatRecord::Turn &turn);
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
    void usageRecorded(openghost::Usage usage);
    void authFinished();

  private:
    using Completion = std::function<void(const Result &)>;
    RequestId call(const Command &command, Completion completion);
    ChatRecord &editable();
    void event(const SessionEvent &event);
    void applyEvent(ChatRecord &chat, const SessionEvent &event, bool replay = false);
    void drainEarly(ChatRecord &chat);
    void reverse(RequestId id, const ReverseRequest &request);
    void dismissApprovals(const QString &session, const QString &reason);
    void endTurn(ChatRecord &chat, const TurnCompleted &done, bool replay = false);
    void cancelRemote(ChatRecord &chat);
    void updateMetrics(ChatRecord &chat, ChatRecord::Turn &turn);
    SessionParams params(const ChatRecord &chat) const;
    quint64 steer(const Input &input);
    void dispatchStart(const StartTurn &start, quint64 submission, const QString &userKey);
    void finishRows(ChatRecord &chat, const QString &state);
    void publishReply(ChatRecord &chat);
    void chooseSaved(const ModelSelection &selection, bool thinkingPreference);
    ModelSelection preferredModel() const;
    void problem(const Error &error);
    Backend *m_backend;
    PreferencesStore *m_preferences;
    QHash<RequestId, Completion> m_calls;
    QHash<QString, ChatRecord> m_chats;
    ChatRecord m_draft;
    QString m_current, m_status;
    QVector<Model> m_models;
    QVector<Provider> m_providers;
    QHash<QString, Error> m_authErrors;
    QVector<PendingApproval> m_approvals;
    QVector<PendingApproval> m_earlyApprovals;
    QSet<QString> m_reverseSeen;
    bool m_refreshing = false, m_refreshAgain = false;
    RequestId m_next = 0;
    quint64 m_submission = 0;
    bool m_ready = false, m_pending = false, m_cancelling = false;
};
} // namespace openghost
