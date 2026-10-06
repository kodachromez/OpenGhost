#include "window.h"
#include "frontend/browser.h"
#include "platform/platform.h"
#include "rich.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQmlNetworkAccessManagerFactory>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QUuid>
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

WindowController::WindowController(QObject *parent)
    : WindowController(nullptr, {}, {}, nullptr, parent)
{
}

namespace
{
std::unique_ptr<openghost::KeyStore> keyStore(const QString &path)
{
    if (path.isEmpty())
        return std::make_unique<openghost::MemoryKeyStore>();
    return std::make_unique<openghost::FileKeyStore>(path);
}
} // namespace

WindowController::WindowController(openghost::Backend *backend, QString preferencesPath,
                                   QString dataPath, openghost::HostServices *host,
                                   QObject *parent)
    : QObject(parent), m_browser(qobject_cast<openghost::Browser *>(host)),
      m_preferences(std::move(preferencesPath)), m_store(keyStore(dataPath)),
      m_library(m_store.get()), m_chat(backend, &m_preferences, &m_library, host),
      m_general(&m_preferences), m_usage(m_store.get())
{
    registerNativeTypes();
    connect(m_chat.plugins(), &openghost::Plugins::entriesChanged, this,
            &WindowController::syncPlugins);
    connect(m_chat.plugins(), &openghost::Plugins::stateChanged, this,
            &WindowController::pluginsChanged);
    connect(m_chat.plugins(), &openghost::Plugins::supportChanged, this,
            &WindowController::runtimePluginsChanged);
    connect(&m_chat, &openghost::ChatService::changed, this, &WindowController::sync);
    connect(&m_chat, &openghost::ChatService::catalogChanged, this, &WindowController::catalog);
    connect(&m_chat, &openghost::ChatService::accepted, this, &WindowController::accepted);
    connect(&m_chat, &openghost::ChatService::removed, this, &WindowController::sessionRemoved);
    connect(&m_chat, &openghost::ChatService::usageRecorded, &m_usage, &UsagePreview::record);
    connect(&m_chat, &openghost::ChatService::authFinished, this, [this] {
        if (m_login.value("type") == "waiting") {
            m_login.insert("begun", true);
            return;
        }
        m_login.clear();
        catalog();
    });
    // The backend's sign-in step for the provider being signed in (the reference's
    // login step: prompt, select, device code or links); never another provider's.
    connect(&m_chat, &openghost::ChatService::loginStep, this,
            [this](const openghost::LoginStep &step) {
                if (m_login.value("providerId").toString() != step.provider)
                    return;
                QVariantMap login{{"id", m_login.value("id")},
                                  {"providerId", step.provider},
                                  {"type", step.type},
                                  {"message", step.message}};
                if (m_login.contains("begun"))
                    login.insert("begun", m_login.value("begun"));
                if (step.promptId) {
                    login.insert("promptId", *step.promptId);
                    login.insert("input", step.type == "prompt");
                    login.insert("secret", step.secret);
                    login.insert("placeholder", step.placeholder.value_or(QString()));
                }
                if (step.url)
                    login.insert("url", *step.url);
                if (step.userCode)
                    login.insert("userCode", *step.userCode);
                QVariantList options, links;
                for (const auto &option : step.options)
                    options.append(QVariantMap{{"id", option.id}, {"label", option.label}});
                for (const auto &link : step.links)
                    links.append(QVariantMap{{"url", link.id}, {"label", link.label}});
                login.insert("options", options);
                login.insert("links", links);
                m_login = login;
                catalog();
            });
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
    connect(&m_sessions, &SessionModel::pinToggled, this, [this](const QString &id, bool pinned) {
        if (!m_chat.setPinned(id, pinned))
            sync(); // The saved pin stays authoritative.
    });
    connect(&m_chat, &openghost::ChatService::folderRemoved, this,
            &WindowController::folderRemoved);
    m_notice = m_preferences.error();
    catalog();
    sync();
    m_chat.initialize();
}

void WindowController::syncPlugins()
{
    const int count = m_plugins.rowCount();
    m_plugins.sync(m_chat.plugins()->entries());
    if (m_plugins.rowCount() != count)
        emit pluginsChanged(); // pluginCount; row content is the model's own signal.
}

void WindowController::catalog()
{
    Account account;
    for (const auto &model : m_chat.models())
        account.models.append({model.provider, model.id, model.name, model.thinkingLevels, true,
                               model.vision.value_or(false),
                               model.defaultThinking.value_or(QString())});
    for (const auto &provider : m_chat.providers()) {
        if (m_login.value("providerId") == provider.id && m_login.value("begun").toBool() &&
            !provider.status.waiting.value_or(false))
            m_login.clear();
        bool key = false, oauth = false;
        QStringList hints;
        for (const auto &method : provider.methods) {
            key |= method.kind == openghost::AuthMethod::Kind::ApiKey;
            oauth |= method.kind == openghost::AuthMethod::Kind::OAuth;
            if (method.hint)
                hints.append(*method.hint);
        }
        QString note = provider.status.waiting.value_or(false)
                           ? QStringLiteral("Waiting for sign-in…")
                       : provider.status.connected ? QStringLiteral("Connected")
                                                   : QStringLiteral("Not connected");
        if (provider.status.error)
            std::visit(
                [&](const auto &error) {
                    using T = std::decay_t<decltype(error)>;
                    if constexpr (std::is_same_v<T, openghost::Error>)
                        note = error.message;
                    else
                        note = error;
                },
                *provider.status.error);
        account.providers.append(QVariantMap{{"id", provider.id},
                                             {"name", provider.name},
                                             {"hint", hints.join(' ')},
                                             {"connected", provider.status.connected},
                                             // Log out removes a stored credential; one
                                             // from the environment is not the backend's.
                                             {"logout", provider.status.connected &&
                                                            provider.status.keySaved.value_or(true)},
                                             {"oauth", oauth},
                                             {"apiKey", key},
                                             {"note", note},
                                             {"error", provider.status.error.has_value()}});
    }
    account.login = m_login;
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
    if (!m_chat.connected() && !m_login.isEmpty()) {
        m_login.clear();
        catalog();
    }
    const auto &chat = m_chat.current();
    m_settings.use({chat.selection.provider, chat.selection.model,
                    chat.selection.thinking.value_or(QString())});
    QVector<Entry> rows;
    QHash<QString, Entry> rendered;
    for (const auto &row : chat.rows) {
        if (row.hidden || row.role == openghost::DisplayRow::Role::Preserved ||
            row.role == openghost::DisplayRow::Role::Moved)
            continue; // Pending parts and undrawn saved entries; moved is mini-only.
        Entry entry;
        entry.kind = row.role == openghost::DisplayRow::Role::User        ? Entry::User
                     : row.role == openghost::DisplayRow::Role::Assistant ? Entry::Assistant
                                                                          : Entry::Note;
        entry.key = row.key;
        entry.text = row.text;
        entry.state = row.state;
        entry.metrics = row.metrics;
        entry.preview = row.tip;
        if (entry.kind == Entry::User) {
            static const QHash<QString, QString> receipts{
                {"sending", "Sending…"},
                {"queued", "Queued for this reply"},
                {"applied", "Applied to this reply"},
                {"notApplied", "Not applied. Nothing was resent."},
                {"unconfirmed", "Input not confirmed. Nothing was resent."}};
            entry.preview = receipts.value(row.state);
        }
        entry.started = row.started;
        entry.completed = row.completed;
        for (const auto &a : row.attachments)
            entry.attachments.append({a.name, QStringLiteral("text/plain"), a.size.value_or(-1)});
        entry.copyable = entry.kind == Entry::Assistant && !row.text.isEmpty();
        const auto previous = m_rendered.constFind(entry.key);
        entry.revision =
            previous == m_rendered.cend()
                ? 1
                : previous->revision +
                      (previous->text != entry.text || previous->state != entry.state ||
                       previous->kind != entry.kind || previous->copyable != entry.copyable ||
                       previous->attachments != entry.attachments ||
                       previous->metrics != entry.metrics || previous->preview != entry.preview ||
                       previous->started != entry.started ||
                       previous->completed != entry.completed);
        rows.append(entry);
        rendered.insert(entry.key, entry);
    }
    m_rendered = std::move(rendered);
    m_transcript.apply(rows);
    QVector<Session> sessions;
    QSet<QString> listed;
    for (const auto &record : m_chat.chats()) {
        listed.insert(record.folder);
        if (m_sessions.query().isEmpty() ||
            (!record.locked && record.title.contains(m_sessions.query(), Qt::CaseInsensitive)))
            sessions.append({record.id, record.title, record.folder, record.updated, record.created,
                             record.pinned});
    }
    m_sessions.apply(sessions);
    // Kept folders without a chat, by their latest activity.
    auto folders = m_library.folders();
    std::sort(folders.begin(), folders.end(), [this](const auto &a, const auto &b) {
        return m_library.activity(a) > m_library.activity(b);
    });
    QStringList empty, order;
    for (const auto &folder : folders) {
        order.append(folder.path);
        if (!listed.contains(folder.path))
            empty.append(folder.path);
    }
    m_sessions.setEmptyFolders(empty, order);
    emit approvalsChanged();
    emit changed();
}

QVariantMap WindowController::collapsedFolders() const
{
    QVariantMap result;
    if (m_library.homeCollapsed())
        result.insert(QStringLiteral("home:"), true);
    for (const auto &folder : m_library.folders())
        if (folder.collapsed)
            result.insert(folder.path, true);
    return result;
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
    QStringList tokens;
    for (const auto &token : files)
        tokens.append(token.toString());
    const auto payload = m_attachments.resolve(tokens);
    if (!payload) {
        m_notice =
            QStringLiteral("An attachment payload is no longer available. Choose the file again.");
        emit changed();
        return 0;
    }
    m_notice.clear();
    return m_chat.send(text, *payload);
}
QString WindowController::pick(const QList<QUrl> &urls, int remaining, int pictures)
{
    Q_UNUSED(pictures);
    if (!ready())
        return backendUnavailable();
    QVector<openghost::AttachmentStore::Prepared> prepared;
    const auto error = m_attachments.prepare(urls, remaining, prepared);
    if (!error.isEmpty())
        return error;
    QVariantList files;
    for (const auto &a : prepared)
        files.append(QVariantMap{
            {"token", a.token}, {"name", a.name}, {"size", a.size}, {"picture", false}});
    // This bounded preparation is synchronous: publish to the still-owning
    // composer before returning, never to a draft switched on the next event.
    emit filesPicked(files, {});
    return {};
}
void WindowController::release(const QVariantList &tokens)
{
    QStringList list;
    for (const auto &token : tokens)
        list.append(token.toString());
    m_attachments.release(list);
}
QVariantMap WindowController::liveMetrics() const
{
    const auto &turn = m_chat.current().turn;
    if (!busy() || turn.started < 0)
        return {};
    return {{"text", openghost::ChatService::metrics(turn)},
            {"started", turn.started},
            {"live", true},
            {"tip", QStringLiteral("Input %1 · Output %2").arg(turn.input).arg(turn.output)}};
}
QVariantList WindowController::approvals() const
{
    QVariantList result;
    for (const auto &pending : m_chat.approvals()) {
        if (pending.data.sessionId != session())
            continue;
        const auto &p = pending.data;
        QVariantMap card;
        if (p.presentation) {
            const auto &d = *p.presentation;
            QVariantList places;
            for (const auto &place : d.places)
                places.append(QVariantMap{
                    {"kind", place.kind}, {"label", place.label}, {"title", place.title}});
            card = {{"kind", d.kind},
                    {"title", d.title},
                    {"effect", d.effect.value_or(QString())},
                    {"badge", d.badge.value_or(false)},
                    {"places", places},
                    {"code", d.code.value_or(QString())},
                    {"removed", d.removed.value_or(QString())},
                    {"added", d.added.value_or(QString())},
                    {"quote", d.quote.value_or(QString())},
                    {"reveal", d.reveal.value_or(QString())}};
        } else
            card = {{"kind", "command"},
                    {"title", p.tool},
                    {"code", QString::fromUtf8(QJsonDocument(p.args).toJson())},
                    {"reveal", "command"}};
        result.append(QVariantMap{
            {"requestId", QString::number(pending.request)}, {"card", card}, {"answered", false}});
    }
    return result;
}
void WindowController::saveDefaults()
{
    auto prefs = m_preferences.value();
    prefs.model = m_chat.current().selection;
    prefs.preferredThinking = prefs.model.thinking;
    prefs.model.thinking.reset();
    m_preferences.save(prefs);
}
void WindowController::login(const QString &provider, const QString &method)
{
    if (!m_chat.connected() || !m_login.isEmpty() || (method != "api_key" && method != "oauth"))
        return;
    const auto &providers = m_chat.providers();
    const auto p = std::find_if(providers.cbegin(), providers.cend(),
                                [&](const auto &p) { return p.id == provider; });
    if (p == providers.cend())
        return;
    const auto kind = method == "api_key" ? openghost::AuthMethod::Kind::ApiKey
                                          : openghost::AuthMethod::Kind::OAuth;
    const auto advertised = std::find_if(p->methods.cbegin(), p->methods.cend(),
                                         [&](const auto &m) { return m.kind == kind; });
    if (advertised == p->methods.cend())
        return;
    m_login = {{"id", QUuid::createUuid().toString()},
               {"providerId", provider},
               {"type", "prompt"},
               {"message", advertised->hint.value_or(QString())}};
    if (kind == openghost::AuthMethod::Kind::ApiKey) {
        m_login.insert("promptId", "key");
        m_login.insert("input", true);
        m_login.insert("secret", true);
        m_login.insert("placeholder", advertised->placeholder.value_or(QString()));
    } else {
        m_login.insert("type", "waiting");
        m_chat.authenticate(openghost::Login{provider});
    }
    catalog();
}
void WindowController::answerLogin(const QString &id, const QString &prompt, const QString &answer)
{
    if (m_login.value("id").toString() != id || m_login.value("promptId").toString() != prompt ||
        prompt.isEmpty())
        return;
    const auto provider = m_login.value("providerId").toString();
    if (prompt != "key") { // The backend's own step; the answer is not retained here either.
        m_login = {{"id", id}, {"providerId", provider}, {"type", "waiting"},
                   {"message", QString()}};
        catalog();
        m_chat.answerLogin(openghost::AnswerLogin{provider, prompt, answer});
        return;
    }
    m_login.remove("promptId");
    m_login.remove("input");
    catalog();
    m_chat.authenticate(openghost::SetKey{provider, answer}); // Never retained in display/settings.
}
void WindowController::cancelLogin(const QString &id)
{
    if (m_login.value("id").toString() != id)
        return;
    const auto provider = m_login.value("providerId").toString();
    m_login.clear();
    catalog();
    m_chat.authenticate(openghost::CancelLogin{provider});
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
