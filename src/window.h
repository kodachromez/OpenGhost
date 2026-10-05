#pragma once

#include "frontend/chat_service.h"
#include "model.h"
#include "offline_services.h"
#include "settings.h"

class QQuickTextDocument;
class QQmlNetworkAccessManagerFactory;
void selectControlsStyle();
QQmlNetworkAccessManagerFactory *denyNetwork();

// QML presentation adapter only. Semantic frontend state lives in ChatService;
// the injected Backend has no dependency on this window or on transport.
class WindowController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *transcript READ transcript CONSTANT)
    Q_PROPERTY(QObject *sessions READ sessions CONSTANT)
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
    Q_PROPERTY(QObject *general READ general CONSTANT)
    Q_PROPERTY(QObject *usage READ usage CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool routine READ off CONSTANT)
    Q_PROPERTY(QString activity READ emptyText CONSTANT)
    Q_PROPERTY(QVariantMap liveMetrics READ emptyMap CONSTANT)
    Q_PROPERTY(QString session READ session NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY changed)
    Q_PROPERTY(bool canSteer READ off CONSTANT)
    Q_PROPERTY(bool canSwitch READ canSwitch NOTIFY changed)
    Q_PROPERTY(bool admitting READ admitting NOTIFY changed)
    Q_PROPERTY(bool picking READ off CONSTANT)
    Q_PROPERTY(bool switching READ off CONSTANT)
    Q_PROPERTY(QVariantMap uploads READ emptyMap CONSTANT)
    Q_PROPERTY(QVariantMap modes READ modes NOTIFY changed)
    Q_PROPERTY(QVariantList approvals READ approvals CONSTANT)
  public:
    explicit WindowController(QObject *parent = nullptr);
    WindowController(openghost::Backend *backend, QString preferencesPath,
                     QObject *parent = nullptr);
    TranscriptModel *transcript() { return &m_transcript; }
    SessionModel *sessions() { return &m_sessions; }
    Settings *settings() { return &m_settings; }
    GeneralPreview *general() { return &m_general; }
    UsagePreview *usage() { return &m_usage; }
    QString status() const { return m_notice.isEmpty() ? m_chat.status() : m_notice; }
    QString session() const { return m_chat.current().id; }
    bool ready() const { return m_chat.ready(); }
    bool busy() const { return m_chat.busy(); }
    bool canCancel() const { return m_chat.canCancel(); }
    bool canSwitch() const { return m_chat.canSwitch(); }
    bool admitting() const { return m_chat.pending(); }
    bool off() const { return false; }
    bool on() const { return true; }
    QString emptyText() const { return {}; }
    QVariantMap emptyMap() const { return {}; }
    QVariantMap modes() const;
    QVariantList approvals() const { return {}; }

    Q_INVOKABLE bool isWorkspace(const QString &folder) const { return folder.isEmpty(); }
    Q_INVOKABLE QString newChatIn(const QString &folder)
    {
        if (!folder.isEmpty())
            return backendUnavailable();
        newChat();
        return {};
    }
    Q_INVOKABLE quint64 send(const QString &text, const QVariantList &files = {});
    Q_INVOKABLE QString pick(const QList<QUrl> &, int, int = 0) { return backendUnavailable(); }
    Q_INVOKABLE void release(const QVariantList &) {} // No tokens are ever issued.
    Q_INVOKABLE bool pasteRefused() const;
    Q_INVOKABLE void cancel() { m_chat.stop(); }
    Q_INVOKABLE void close();
    Q_INVOKABLE void newChat(); // Local draft; first start creates the backend session.
    Q_INVOKABLE void open(const QString &id) { m_chat.open(id); }
    Q_INVOKABLE void rename(const QString &id, const QString &title) { m_chat.rename(id, title); }
    Q_INVOKABLE void remove(const QString &id)
    {
        unavailable();
        emit sessionRemoved(id, false);
    }
    Q_INVOKABLE void removeFolder(const QString &folder)
    {
        unavailable();
        emit folderRemoved(folder, false, backendUnavailable());
    }
    Q_INVOKABLE int folderChats(const QString &) const { return 0; }
    Q_INVOKABLE void setPermissionMode(const QString &name);
    Q_INVOKABLE void approve(const QString &, bool) { unavailable(); }
    Q_INVOKABLE void copy(const QString &text);
    Q_INVOKABLE QString selectedText(QQuickTextDocument *document, int start, int end) const;
    Q_INVOKABLE void copySelection(QQuickTextDocument *document, int start, int end);
    Q_INVOKABLE void copyEntry(const QString &key);
    Q_INVOKABLE bool openExternal(const QString &url);
    Q_INVOKABLE bool openLink(const QString &url);
    Q_INVOKABLE void refreshProviders() { unavailable(); }
    Q_INVOKABLE void retryModels() { unavailable(); }
    Q_INVOKABLE void saveDefaults() { unavailable(); }
    Q_INVOKABLE void login(const QString &, const QString &) { unavailable(); }
    Q_INVOKABLE void answerLogin(const QString &, const QString &, const QString &)
    {
        unavailable();
    }
    Q_INVOKABLE void cancelLogin(const QString &) { unavailable(); }
    Q_INVOKABLE void logout(const QString &) { unavailable(); }
    Q_INVOKABLE void preview(const QString &key, int card) { emit previewChanged(key, card); }
    Q_INVOKABLE QString previewState(const QString &, int) const { return backendUnavailable(); }
    Q_INVOKABLE QImage previewImage(const QString &, int) const { return {}; }
  signals:
    void changed();
    void approvalsChanged();
    void accepted(quint64 submission); // Validated acceptance, never completion.
    void filesPicked(QVariantList files, QString error);
    void conversationReplaced(QString left);
    void sessionRemoved(QString sessionId, bool deleted);
    void folderRemoved(QString folder, bool removed, QString notice);
    void previewChanged(QString key, int card);
    void answered();
    void worked();
    void closeRequested();

  private:
    void unavailable()
    {
        m_notice = QStringLiteral("This operation is not connected in the native frontend yet.");
        emit changed();
    }
    void sync();
    void catalog();
    openghost::PreferencesStore m_preferences;
    openghost::ChatService m_chat;
    QString m_notice;
    QHash<QString, Entry> m_rendered;
    TranscriptModel m_transcript;
    SessionModel m_sessions;
    Settings m_settings;
    GeneralPreview m_general;
    UsagePreview m_usage;
};
