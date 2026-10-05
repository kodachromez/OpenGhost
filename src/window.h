#pragma once

#include "frontend/attachments.h"
#include "frontend/chat_service.h"
#include "frontend/store.h"
#include "model.h"
#include "offline_services.h"
#include "settings.h"
#include <memory>

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
    // The desktop build's browser panel (openghost::Browser); null without one.
    Q_PROPERTY(QObject *browser READ browser CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool routine READ routine NOTIFY changed)
    Q_PROPERTY(QString activity READ activity NOTIFY changed)
    Q_PROPERTY(QVariantMap liveMetrics READ liveMetrics NOTIFY changed)
    Q_PROPERTY(QString session READ session NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY changed)
    Q_PROPERTY(bool canSteer READ canSteer NOTIFY changed)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY changed)
    Q_PROPERTY(QString retryRow READ retryRow NOTIFY changed)
    Q_PROPERTY(bool canSwitch READ canSwitch NOTIFY changed)
    Q_PROPERTY(bool admitting READ admitting NOTIFY changed)
    Q_PROPERTY(bool picking READ off CONSTANT)
    Q_PROPERTY(bool switching READ off CONSTANT)
    Q_PROPERTY(QVariantMap uploads READ emptyMap CONSTANT)
    Q_PROPERTY(QVariantMap modes READ modes NOTIFY changed)
    Q_PROPERTY(QVariantList approvals READ approvals NOTIFY approvalsChanged)
    // Its own NOTIFY: the Settings pages (and tab delegates) change only with support.
    Q_PROPERTY(bool runtimePlugins READ runtimePlugins NOTIFY runtimePluginsChanged)
    Q_PROPERTY(bool pluginsLoading READ pluginsLoading NOTIFY pluginsChanged)
    Q_PROPERTY(bool pluginsLoaded READ pluginsLoaded NOTIFY pluginsChanged)
    Q_PROPERTY(QString pluginsError READ pluginsError NOTIFY pluginsChanged)
    Q_PROPERTY(int pluginCount READ pluginCount NOTIFY pluginsChanged)
    Q_PROPERTY(QObject *plugins READ plugins CONSTANT)
  public:
    explicit WindowController(QObject *parent = nullptr);
    // `dataPath`: the frontend's local chat/usage store; empty keeps it in memory.
    // `host`: the desktop's host services (the browser panel), else none.
    WindowController(openghost::Backend *backend, QString preferencesPath, QString dataPath = {},
                     openghost::HostServices *host = nullptr, QObject *parent = nullptr);
    TranscriptModel *transcript() { return &m_transcript; }
    SessionModel *sessions() { return &m_sessions; }
    Settings *settings() { return &m_settings; }
    GeneralPreview *general() { return &m_general; }
    UsagePreview *usage() { return &m_usage; }
    QObject *browser() const { return m_browser; }
    QString status() const { return m_notice.isEmpty() ? m_chat.status() : m_notice; }
    QString session() const { return m_chat.current().id; }
    bool ready() const { return m_chat.ready(); }
    bool busy() const { return m_chat.busy(); }
    bool canCancel() const { return m_chat.canCancel(); }
    bool canSteer() const { return m_chat.canSteer(); }
    bool canRetry() const { return m_chat.canRetry(); }
    QString retryRow() const
    {
        const auto &chat = m_chat.current();
        return canRetry() && chat.reconciled && !chat.rows.isEmpty() &&
                       chat.rows.last().role == openghost::DisplayRow::Role::Note
                   ? chat.rows.last().key
                   : QString();
    }
    bool routine() const { return status().isEmpty(); }
    QString activity() const { return m_chat.current().turn.activity; }
    QVariantMap liveMetrics() const;
    bool canSwitch() const { return m_chat.canSwitch(); }
    bool admitting() const { return m_chat.pending(); }
    bool off() const { return false; }
    bool on() const { return true; }
    QString emptyText() const { return {}; }
    QVariantMap emptyMap() const { return {}; }
    QVariantMap modes() const;
    QVariantList approvals() const;
    bool runtimePlugins() const { return m_chat.plugins()->supported(); }
    bool pluginsLoading() const { return m_chat.plugins()->loading(); }
    bool pluginsLoaded() const { return m_chat.plugins()->loaded(); }
    QString pluginsError() const { return m_chat.plugins()->error(); }
    int pluginCount() const { return m_plugins.rowCount(); }
    PluginModel *plugins() { return &m_plugins; }
    Q_INVOKABLE void refreshPlugins() { m_chat.plugins()->refresh(); }
    Q_INVOKABLE void setPluginEnabled(const QString &id, bool enabled)
    {
        m_chat.plugins()->setEnabled(id, enabled);
    }

    // The folder New Chat's draft (or the open chat) belongs to; "" for none.
    Q_INVOKABLE bool isWorkspace(const QString &folder) const
    {
        return m_chat.current().folder == folder;
    }
    Q_INVOKABLE QString newChatIn(const QString &folder)
    {
        if (!folder.isEmpty() && !m_library.folder(folder))
            return QStringLiteral("This folder is not in the chat list.");
        m_chat.newChat(folder);
        return {};
    }
    Q_INVOKABLE quint64 send(const QString &text, const QVariantList &files = {});
    Q_INVOKABLE QString pick(const QList<QUrl> &urls, int remaining, int pictures = 0);
    Q_INVOKABLE void release(const QVariantList &tokens);
    Q_INVOKABLE bool pasteRefused() const;
    Q_INVOKABLE void cancel() { m_chat.stop(); }
    Q_INVOKABLE void retry()
    {
        m_notice.clear();
        m_chat.retry();
    }
    Q_INVOKABLE void close();
    Q_INVOKABLE void newChat(); // Local draft; first start creates the backend session.
    Q_INVOKABLE void open(const QString &id) { m_chat.open(id); }
    Q_INVOKABLE void rename(const QString &id, const QString &title) { m_chat.rename(id, title); }
    Q_INVOKABLE void remove(const QString &id) { m_chat.remove(id); }
    Q_INVOKABLE void removeFolder(const QString &folder) { m_chat.removeFolder(folder); }
    Q_INVOKABLE int folderChats(const QString &folder) const
    {
        return int(m_library.inFolder(folder).size());
    }
    // Saved folder collapse (library.js): "home:" for the chats without one.
    Q_INVOKABLE QVariantMap collapsedFolders() const;
    Q_INVOKABLE void toggleFolder(const QString &key)
    {
        m_chat.toggleFolder(key == QStringLiteral("home:") ? std::nullopt
                                                           : std::optional<QString>(key));
    }
    Q_INVOKABLE void setPermissionMode(const QString &name);
    Q_INVOKABLE void approve(const QString &id, bool allow)
    {
        m_chat.approve(id.toULongLong(), allow);
    }
    Q_INVOKABLE void copy(const QString &text);
    Q_INVOKABLE QString selectedText(QQuickTextDocument *document, int start, int end) const;
    Q_INVOKABLE void copySelection(QQuickTextDocument *document, int start, int end);
    Q_INVOKABLE void copyEntry(const QString &key);
    Q_INVOKABLE bool openExternal(const QString &url);
    Q_INVOKABLE bool openLink(const QString &url);
    Q_INVOKABLE void refreshProviders() { m_chat.refresh(); }
    Q_INVOKABLE void retryModels() { m_chat.refresh(); }
    Q_INVOKABLE void saveDefaults();
    Q_INVOKABLE void login(const QString &provider, const QString &method);
    Q_INVOKABLE void answerLogin(const QString &id, const QString &prompt, const QString &answer);
    Q_INVOKABLE void cancelLogin(const QString &id);
    Q_INVOKABLE void logout(const QString &provider)
    {
        m_chat.authenticate(openghost::Logout{provider});
    }
    Q_INVOKABLE void preview(const QString &key, int card) { emit previewChanged(key, card); }
    Q_INVOKABLE QString previewState(const QString &, int) const { return backendUnavailable(); }
    Q_INVOKABLE QImage previewImage(const QString &, int) const { return {}; }
  signals:
    void changed();
    void approvalsChanged();
    void pluginsChanged();
    void runtimePluginsChanged();
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
    void syncPlugins();
    void catalog();
    QObject *m_browser = nullptr;
    openghost::PreferencesStore m_preferences;
    std::unique_ptr<openghost::KeyStore> m_store;
    openghost::Library m_library;
    openghost::ChatService m_chat;
    QString m_notice;
    QVariantMap m_login;
    openghost::AttachmentStore m_attachments;
    QHash<QString, Entry> m_rendered;
    TranscriptModel m_transcript;
    SessionModel m_sessions;
    PluginModel m_plugins;
    Settings m_settings;
    GeneralPreview m_general;
    UsagePreview m_usage;
};
