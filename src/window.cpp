#include "window.h"
#include "platform/platform.h"
#include "rich.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQmlNetworkAccessManagerFactory>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <algorithm>

// OpenGhost's native window allows no QML image/font/style networking.
namespace
{
class DenyAll final : public QQmlNetworkAccessManagerFactory
{
    class Manager final : public QNetworkAccessManager
    {
      public:
        using QNetworkAccessManager::QNetworkAccessManager;

      protected:
        QNetworkReply *createRequest(Operation operation, const QNetworkRequest &,
                                     QIODevice *data) override
        {
            return QNetworkAccessManager::createRequest(operation, QNetworkRequest(), data);
        }
    };

  public:
    QNetworkAccessManager *create(QObject *parent) override
    {
        auto *manager = new Manager(parent);
        manager->setAutoDeleteReplies(true);
        return manager;
    }
};
} // namespace

QQmlNetworkAccessManagerFactory *denyNetwork()
{
    static DenyAll factory;
    return &factory;
}

void selectControlsStyle()
{
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        QQuickStyle::setStyle(QStringLiteral("Fusion"));
}

WindowController::WindowController(QObject *parent) : WindowController(nullptr, {}, parent) {}

WindowController::WindowController(openghost::Backend *backend, QString preferencesPath,
                                   QObject *parent)
    : QObject(parent), m_preferences(std::move(preferencesPath)), m_chat(backend, &m_preferences),
      m_general(&m_preferences)
{
    registerNativeTypes();
    connect(&m_chat, &openghost::ChatService::changed, this, &WindowController::sync);
    connect(&m_chat, &openghost::ChatService::catalogChanged, this, &WindowController::catalog);
    connect(&m_chat, &openghost::ChatService::accepted, this, &WindowController::accepted);
    connect(&m_chat, &openghost::ChatService::answered, this, &WindowController::answered);
    connect(&m_chat, &openghost::ChatService::worked, this, &WindowController::worked);
    connect(&m_chat, &openghost::ChatService::replaced, this, [this](const QString &left) {
        m_transcript.reset({});
        m_rendered.clear();
        sync();
        emit conversationReplaced(left);
    });
    connect(&m_settings, &Settings::chosen, this, [this](const Selection &selection) {
        const auto &old = m_chat.current().selection;
        const bool effort = old.model == selection.model && old.provider == selection.provider;
        m_chat.choose({selection.provider, selection.model,
                       selection.thinking.isEmpty() ? std::nullopt
                                                    : std::optional<QString>(selection.thinking)},
                      effort);
    });
    connect(&m_preferences, &openghost::PreferencesStore::saveFailed, this,
            [this](const QString &error) {
                m_notice = error;
                emit changed();
            });
    connect(&m_preferences, &openghost::PreferencesStore::changed, this,
            &WindowController::catalog);
    connect(&m_sessions, &SessionModel::queryChanged, this, &WindowController::sync);
    m_notice = m_preferences.error();
    catalog();
    sync();
    m_chat.initialize();
}

void WindowController::catalog()
{
    Account account;
    for (const auto &model : m_chat.models())
        account.models.append({model.provider, model.id, model.name, model.thinkingLevels, true,
                               model.vision.value_or(false),
                               model.defaultThinking.value_or(QString())});
    for (const auto &provider : m_chat.providers())
        account.providers.append(QVariantMap{
            {"id", provider.id},
            {"name", provider.name},
            {"hint", QStringLiteral("Development fixture only. No credentials or network.")},
            {"connected", provider.status.connected},
            {"logout", false},
            {"oauth", false},
            {"apiKey", false},
            {"note", QString()},
            {"error", false}});
    account.providersLoaded = true;
    if (account.models.isEmpty())
        account.catalogError = account.providersError = m_chat.status();
    const auto &prefs = m_preferences.value();
    account.defaults = {prefs.model.provider, prefs.model.model,
                        prefs.preferredThinking.value_or(QString())};
    m_settings.apply(account);
}

void WindowController::sync()
{
    const auto &chat = m_chat.current();
    m_settings.use({chat.selection.provider, chat.selection.model,
                    chat.selection.thinking.value_or(QString())});
    QVector<Entry> rows;
    QHash<QString, Entry> rendered;
    for (const auto &row : chat.rows) {
        Entry entry;
        entry.kind = row.role == openghost::DisplayRow::Role::User        ? Entry::User
                     : row.role == openghost::DisplayRow::Role::Assistant ? Entry::Assistant
                                                                          : Entry::Note;
        entry.key = row.key;
        entry.text = row.text;
        entry.state = row.state;
        entry.copyable = entry.kind == Entry::Assistant && !row.text.isEmpty();
        const auto previous = m_rendered.constFind(entry.key);
        entry.revision =
            previous == m_rendered.cend()
                ? 1
                : previous->revision +
                      (previous->text != entry.text || previous->state != entry.state ||
                       previous->kind != entry.kind || previous->copyable != entry.copyable);
        rows.append(entry);
        rendered.insert(entry.key, entry);
    }
    m_rendered = std::move(rendered);
    m_transcript.apply(rows);
    QVector<Session> sessions;
    for (const auto &record : m_chat.chats())
        if (m_sessions.query().isEmpty() ||
            record.title.contains(m_sessions.query(), Qt::CaseInsensitive))
            sessions.append({record.id, record.title, {}, record.updated, record.created});
    m_sessions.apply(sessions);
    emit changed();
}

QVariantMap WindowController::modes() const
{
    return {{"known", true},
            {"permissions",
             QStringList{QStringLiteral("ask"), QStringLiteral("auto"), QStringLiteral("full")}},
            {"permission", openghost::modeName(m_chat.current().mode)}};
}
void WindowController::setPermissionMode(const QString &name)
{
    if (const auto mode = openghost::parseMode(name))
        m_chat.setMode(*mode);
}
quint64 WindowController::send(const QString &text, const QVariantList &files)
{
    if (!files.isEmpty()) {
        unavailable();
        return 0;
    }
    m_notice.clear();
    return m_chat.send(text);
}
void WindowController::copyEntry(const QString &key)
{
    for (const auto &row : m_chat.current().rows)
        if (row.key == key) {
            copy(row.text);
            return;
        }
}
void WindowController::close() { emit closeRequested(); }
void WindowController::newChat() { m_chat.newChat(); }

// These clipboard and selection helpers are retained from window.cpp.
bool WindowController::pasteRefused() const
{
    const QMimeData *data = QGuiApplication::clipboard()->mimeData(QClipboard::Clipboard);
    if (!data)
        return false;
    if (data->hasImage())
        return true;
    const auto urls = data->urls();
    return std::any_of(urls.cbegin(), urls.cend(),
                       [](const QUrl &url) { return url.isLocalFile(); });
}

void WindowController::copy(const QString &text)
{
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
}

QString WindowController::selectedText(QQuickTextDocument *document, int start, int end) const
{
    return document ? InlineFormat::selected(document->textDocument(), start, end) : QString();
}

void WindowController::copySelection(QQuickTextDocument *document, int start, int end)
{
    copy(selectedText(document, start, end));
}

bool WindowController::openExternal(const QString &url) { return platform::openLink(url, false); }

bool WindowController::openLink(const QString &url) { return platform::openLink(url, true); }
