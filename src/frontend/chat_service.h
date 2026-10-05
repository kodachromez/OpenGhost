#pragma once
#include "backend/backend.h"
#include "preferences.h"
#include <QHash>
#include <functional>

namespace openghost
{
struct DisplayRow {
    enum class Role { User, Assistant, Note } role = Role::Note;
    QString key, text, state;
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
        bool terminal = false, stopped = false;
        struct Message {
            QString id, text;
            bool sealed = false;
        };
        QVector<Message> messages;
    } turn;
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
    bool ready() const { return m_ready && current().reconciled; }
    bool busy() const { return !current().turn.clientId.isEmpty() && !current().turn.terminal; }
    bool pending() const { return m_pending; }
    bool canSwitch() const { return !m_pending; }
    bool canCancel() const { return busy() && !current().turn.stopped; }
    void newChat();
    void open(const QString &id);
    quint64 send(const QString &text);
    void stop();
    void choose(const ModelSelection &selection, bool thinkingPreference);
    void setMode(PermissionMode mode);
    void rename(const QString &id, const QString &title);
  signals:
    void changed();
    void catalogChanged();
    void accepted(quint64 submission);
    void replaced(const QString &left);
    void answered();
    void worked();

  private:
    using Completion = std::function<void(const Result &)>;
    RequestId call(const Command &command, Completion completion);
    ChatRecord &editable();
    void event(const SessionEvent &event);
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
    RequestId m_next = 0;
    quint64 m_submission = 0;
    bool m_ready = false, m_pending = false;
};
} // namespace openghost
