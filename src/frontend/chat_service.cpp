#include "chat_service.h"
#include <QDateTime>
#include <QSignalBlocker>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <memory>

namespace openghost
{
namespace
{
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
template <class T> const T *value(const Result &result)
{
    const auto *reply = std::get_if<Reply>(&result);
    return reply ? std::get_if<T>(reply) : nullptr;
}
QString titleFrom(const QString &text)
{
    QStringList lines;
    for (const auto &line : text.split(QLatin1Char('\n')))
        if (!line.trimmed().isEmpty())
            lines.append(line.trimmed());
    QString title;
    for (const auto &line : lines)
        if (!line.startsWith(QLatin1Char('>'))) {
            title = line;
            break;
        }
    if (title.isEmpty() && !lines.isEmpty())
        title = lines.first().mid(1).trimmed();
    if (title.size() > 60)
        title = title.left(59).trimmed() + QChar(0x2026);
    return title.isEmpty() ? QStringLiteral("New chat") : title;
}
Error errorOf(const Result &result)
{
    if (const auto *error = std::get_if<Error>(&result))
        return *error;
    return {QStringLiteral("protocol_error"),
            QStringLiteral("Unexpected backend reply."),
            {},
            {},
            {},
            {}};
}
const QString Locked = QStringLiteral("Locked local view");
QString modelKey(const ModelSelection &selection)
{
    return selection.provider.isEmpty() ? selection.model
                                        : selection.provider + QLatin1Char('/') + selection.model;
}
QJsonObject modelObject(const ModelSelection &selection)
{
    QJsonObject o{{"provider", selection.provider}, {"model", selection.model}};
    if (selection.thinking)
        o.insert("thinking", *selection.thinking);
    return o;
}
QJsonObject attachmentObject(const DisplayAttachment &a)
{
    QJsonObject o{{"name", a.name}, {"image", a.image.value_or(false)}};
    if (a.size)
        o.insert("size", double(*a.size));
    if (a.url)
        o.insert("url", *a.url);
    if (a.note)
        o.insert("note", *a.note);
    if (a.width)
        o.insert("width", *a.width);
    if (a.height)
        o.insert("height", *a.height);
    if (a.pasted) {
        QJsonObject p;
        if (a.pasted->preview)
            p.insert("preview", *a.pasted->preview);
        if (a.pasted->lines)
            p.insert("lines", *a.pasted->lines);
        o.insert("pasted", p);
    }
    if (a.video) {
        QJsonObject v;
        if (a.video->poster)
            v.insert("poster", *a.video->poster);
        if (a.video->duration)
            v.insert("duration", *a.video->duration);
        o.insert("video", v);
    }
    return o;
}
DisplayAttachment attachmentOf(const QJsonObject &o)
{
    DisplayAttachment a;
    a.name = o.value("name").toString();
    if (o.contains("size"))
        a.size = qint64(o.value("size").toDouble());
    a.image = o.value("image").toBool();
    if (o.value("url").isString())
        a.url = o.value("url").toString();
    if (o.value("note").isString())
        a.note = o.value("note").toString();
    if (o.contains("width"))
        a.width = o.value("width").toInt();
    if (o.contains("height"))
        a.height = o.value("height").toInt();
    if (o.value("pasted").isObject()) {
        const auto p = o.value("pasted").toObject();
        a.pasted = DisplayAttachment::Pasted{};
        if (p.value("preview").isString())
            a.pasted->preview = p.value("preview").toString();
        if (p.contains("lines"))
            a.pasted->lines = p.value("lines").toInt();
    }
    if (o.value("video").isObject()) {
        const auto v = o.value("video").toObject();
        a.video = DisplayAttachment::Video{};
        if (v.value("poster").isString())
            a.video->poster = v.value("poster").toString();
        if (v.contains("duration"))
            a.video->duration = v.value("duration").toDouble();
    }
    return a;
}
DisplayAttachment displayOf(const Attachment &a)
{
    return {a.name, a.size, a.kind == Attachment::Kind::Image, {}, a.note, a.width, a.height,
            {},     {}};
}
const QStringList Receipts{"queued", "applied", "notApplied", "unconfirmed"};

// A tool card's output, bounded as Ghosty's transcript bounds it: live output
// keeps its newest 32 Ki units, a result its first 32 Ki and a notice.
constexpr qsizetype ToolOutput = 32 * 1024;
constexpr int ToolCards = 256; // Per turn.
const QString ToolClipped =
    QStringLiteral("\n\n[Display limit reached; additional text is omitted here.]");
qsizetype codePoints(QStringView text)
{
    qsizetype count = text.size();
    for (const QChar c : text)
        count -= c.isLowSurrogate();
    return count;
}
// Where the kept tail of `all` starts: past the bound, at a line start when
// one follows, never inside a surrogate pair.
qsizetype tailStart(const QString &all)
{
    if (all.size() <= ToolOutput)
        return 0;
    qsizetype start = all.size() - ToolOutput;
    if (all.at(start - 1) != QLatin1Char('\n')) {
        const auto newline = all.indexOf(QLatin1Char('\n'), start);
        if (newline >= 0 && newline < all.size() - 1)
            start = newline + 1;
        else if (all.at(start).isLowSurrogate())
            ++start;
    }
    return start;
}
// Ended lines dropped count as lines; a cut inside one counts its dropped start.
void countDropped(DisplayRow::Tool &tool, QStringView removed)
{
    const auto newline = removed.lastIndexOf(QLatin1Char('\n'));
    if (newline >= 0) {
        tool.omittedLines += removed.count(QLatin1Char('\n'));
        tool.omittedCharacters = 0;
    }
    tool.omittedCharacters += codePoints(removed.mid(newline + 1));
}
void liveOutput(DisplayRow &row, const QString &output, bool append)
{
    auto &tool = row.tool;
    if (append) {
        QString all = row.text + output;
        const auto start = tailStart(all);
        if (start > 0) {
            countDropped(tool, QStringView(all).left(start));
            tool.trimmed += start; // The view removes just this prefix from its document.
            all.remove(0, start);
        }
        row.text = all;
        return;
    }
    // A snapshot of everything so far. When it only grew, the view trims its
    // front; otherwise it shows the new text whole.
    const auto start = tailStart(output);
    const bool grew = start >= tool.removed &&
                      QStringView(output).mid(tool.removed).startsWith(row.text);
    if (grew)
        tool.trimmed += start - tool.removed;
    tool.removed = start;
    tool.omittedLines = tool.omittedCharacters = 0;
    if (start > 0)
        countDropped(tool, QStringView(output).left(start));
    row.text = output.mid(start);
}
QString resultOutput(const QString &output)
{
    if (output.size() <= ToolOutput)
        return output;
    qsizetype cut = ToolOutput;
    if (output.at(cut - 1).isHighSurrogate())
        --cut;
    return output.left(cut) + ToolClipped;
}
} // namespace

// The display cache. Notes (errors/Stopped) are view-only, as in the reference.
QJsonArray ChatService::entries(const QVector<DisplayRow> &rows)
{
    QJsonArray out;
    QString reply; // The latest assistant row: a tool call made by its message joins it.
    for (const auto &row : rows) {
        QJsonObject o;
        if (row.role == DisplayRow::Role::Assistant)
            reply = row.key;
        switch (row.role) {
        case DisplayRow::Role::Note:
            continue;
        case DisplayRow::Role::Preserved:
            out.append(row.raw);
            continue;
        case DisplayRow::Role::Moved:
            out.append(QJsonObject{{"role", "moved"}});
            continue;
        case DisplayRow::Role::User: {
            o = {{"role", "user"}, {"text", row.text}, {"content", row.text}};
            QJsonArray attachments;
            for (const auto &a : row.attachments)
                attachments.append(attachmentObject(a));
            o.insert("attachments", attachments);
            if (!row.clientInputId.isEmpty())
                o.insert("clientInputId", row.clientInputId);
            if (Receipts.contains(row.state))
                o.insert("receipt", row.state);
            break;
        }
        case DisplayRow::Role::Tool:
            o = {{"role", "tool"},
                 {"callId", row.tool.callId},
                 {"name", row.tool.name},
                 {"state", row.state},
                 {"output", row.text}};
            if (row.tool.known)
                o.insert("arguments", row.tool.arguments);
            if (!row.tool.ending.isEmpty())
                o.insert("ending", row.tool.ending);
            if (row.tool.omittedLines > 0)
                o.insert("omittedLines", row.tool.omittedLines);
            if (row.tool.omittedCharacters > 0)
                o.insert("omittedCharacters", row.tool.omittedCharacters);
            if (!row.remoteTurn.isEmpty())
                o.insert("turn", row.remoteTurn);
            if (!reply.isEmpty() && row.tool.after == reply)
                o.insert("joined", true);
            break;
        case DisplayRow::Role::Assistant:
            o = {{"role", "assistant"}, {"content", row.text}};
            if (row.continued)
                o.insert("continued", true);
            if (!row.remoteTurn.isEmpty())
                o.insert("turn", row.remoteTurn);
            if (!row.model.isEmpty())
                o.insert("model", row.model);
            if (row.usage)
                o.insert("usage", QJsonObject{{"input", row.usage->input},
                                              {"cached", row.usage->cached},
                                              {"written", row.usage->written},
                                              {"output", row.usage->output},
                                              {"requests", row.usage->requests}});
            else if (row.uncounted)
                o.insert("uncounted", true);
            break;
        }
        if (!row.backendTurn.isEmpty())
            o.insert("backendTurn", row.backendTurn);
        if (row.pendingTurn)
            o.insert("pendingTurn", true);
        out.append(o);
    }
    return Library::displayMessages(out);
}
QVector<DisplayRow> ChatService::rowsOf(const QJsonArray &saved)
{
    QVector<DisplayRow> rows;
    QString reply;
    for (const auto &value : Library::displayMessages(saved)) {
        const auto o = value.toObject();
        const auto role = o.value("role").toString();
        DisplayRow row;
        row.key = uuid();
        row.state = QStringLiteral("done");
        row.backendTurn = o.value("backendTurn").toString();
        row.clientInputId = o.value("clientInputId").toString();
        row.pendingTurn = o.value("pendingTurn").toBool();
        if (role == "user") {
            row.role = DisplayRow::Role::User;
            row.text = o.value("text").toString();
            if (Receipts.contains(o.value("receipt").toString()))
                row.state = o.value("receipt").toString();
            for (const auto &a : o.value("attachments").toArray())
                row.attachments.append(attachmentOf(a.toObject()));
        } else if (role == "assistant") {
            row.role = DisplayRow::Role::Assistant;
            row.text = o.value("content").toString();
            row.hidden = row.text.trimmed().isEmpty(); // a pending part, never drawn empty
            row.remoteTurn = o.value("turn").isString() ? o.value("turn").toString() : QString();
            row.model = o.value("model").toString();
            row.uncounted = o.value("uncounted").toBool();
            row.continued = o.value("continued").toBool();
            if (o.value("usage").isObject()) {
                const auto u = o.value("usage").toObject();
                row.usage =
                    DisplayRow::Spent{u.value("input").toDouble(), u.value("cached").toDouble(),
                                      u.value("written").toDouble(), u.value("output").toDouble(),
                                      u.value("requests").toDouble()};
            }
        } else if (role == "tool") {
            row.role = DisplayRow::Role::Tool;
            row.tool.callId = o.value("callId").toString();
            row.tool.name = o.value("name").toString();
            row.tool.known = o.value("arguments").isString();
            row.tool.arguments = o.value("arguments").toString();
            row.tool.ending = o.value("ending").toString();
            row.tool.omittedLines = o.value("omittedLines").toDouble();
            row.tool.omittedCharacters = o.value("omittedCharacters").toDouble();
            row.text = o.value("output").toString();
            row.remoteTurn = o.value("turn").toString();
            if (o.value("joined").toBool())
                row.tool.after = reply;
            // A call saved while it ran: its result never arrived here.
            row.state = o.value("state").toString();
            if (row.state == QStringLiteral("running"))
                row.state = QStringLiteral("unconfirmed");
        } else if (role == "moved") {
            row.role = DisplayRow::Role::Moved;
            row.text = QStringLiteral("Caught up with the main chat");
        } else {
            row.role = DisplayRow::Role::Preserved;
            row.hidden = true;
            row.raw = o;
        }
        if (row.role == DisplayRow::Role::Assistant)
            reply = row.key;
        rows.append(row);
    }
    return rows;
}
ChatService::ChatService(Backend *backend, PreferencesStore *preferences, Library *library,
                         HostServices *host, QObject *parent)
    : QObject(parent), m_backend(backend),
      m_plugins([this](const Command &command, Plugins::Completion done) {
          call(command, std::move(done));
      }),
      m_preferences(preferences), m_library(library), m_host(host ? host : &m_noHost)
{
    if (!m_library) {
        m_ownStore = std::make_unique<MemoryKeyStore>();
        m_ownLibrary = std::make_unique<Library>(m_ownStore.get());
        m_library = m_ownLibrary.get();
    }
    m_status = QStringLiteral("Backend not connected. Use --fake-backend for the in-memory demo.");
    if (m_backend) {
        connect(m_backend, &Backend::replied, this, [this](RequestId id, const Result &result) {
            auto it = m_calls.find(id);
            if (it == m_calls.end())
                return;
            auto complete = std::move(it.value());
            m_calls.erase(it);
            complete(result);
        });
        connect(m_backend, &Backend::sessionEvent, this, &ChatService::event);
        connect(m_backend, &Backend::closed, this, [this](const Error &error) {
            m_ready = false;
            m_plugins.initialize(false);
            for (auto &chat : m_chats) {
                chat.reconciled = false;
                chat.turn.terminal = true;
                finishRows(chat, QStringLiteral("unconfirmed"));
                m_host->turnEnded(chat.id);
            }
            m_approvals.clear();
            m_earlyApprovals.clear();
            for (auto it = m_hostRequests.cbegin(); it != m_hostRequests.cend(); ++it)
                m_host->cancel(it.key());
            m_hostRequests.clear();
            m_cancelling = false;
            auto calls = std::move(m_calls);
            m_calls.clear();
            for (auto &complete : calls)
                complete(Result{error});
            m_pending = false;
            problem(error);
        });
        connect(m_backend, &Backend::reverseRequest, this, &ChatService::reverse);
        connect(m_host, &HostServices::finished, this,
                [this](RequestId id, const HostToolResult &result) {
                    const auto owner = m_hostRequests.take(id);
                    if (owner.first.isEmpty())
                        return; // cancelled or already settled: exactly once
                    if (auto chat = m_chats.find(owner.first); chat != m_chats.end())
                        chat->turn.hostCalls.remove(owner.second);
                    m_backend->answer(id, result);
                });
        connect(m_host, &HostServices::browserChanged, this,
                [this](const BrowserState &state) { m_backend->browserChanged(state); });
        connect(m_backend, &Backend::reverseCancelled, this, [this](RequestId id) {
            const auto owner = m_hostRequests.take(id);
            if (!owner.first.isEmpty()) {
                if (auto chat = m_chats.find(owner.first); chat != m_chats.end())
                    chat->turn.hostCalls.remove(owner.second);
                m_host->cancel(id);
                HostToolResult result;
                result.status = HostToolResult::Status::Cancelled;
                m_backend->answer(id, result);
            }
            m_approvals.removeIf([id](const auto &p) { return p.request == id; });
            m_earlyApprovals.removeIf([id](const auto &p) { return p.request == id; });
            emit changed();
        });
        connect(m_backend, &Backend::globalEvent, this, [this](const GlobalEvent &event) {
            if (const auto *plugin = std::get_if<PluginChanged>(&event))
                m_plugins.observe(*plugin);
            else if (const auto *step = std::get_if<LoginStep>(&event))
                emit loginStep(*step);
            else if (const auto *log = std::get_if<Log>(&event);
                     log && (log->level == QStringLiteral("error") ||
                             log->level == QStringLiteral("warning")))
                problem({QStringLiteral("backend_log"), log->message, {}, {}, {}, {}});
            else if (std::holds_alternative<AuthChanged>(event) ||
                     std::holds_alternative<ModelsChanged>(event))
                refresh();
        });
    }
    for (const auto &saved : m_library->chats())
        m_chats.insert(saved.id, restoredRecord(saved));
    if (!m_library->error().isEmpty())
        m_status = m_library->error();
    newChat();
}
ChatRecord ChatService::restoredRecord(const Library::Chat &saved) const
{
    ChatRecord chat;
    chat.id = saved.id;
    chat.title = m_library->titleOf(saved.id).value_or(Locked);
    chat.renamed = saved.named;
    chat.created = saved.created;
    chat.updated = saved.updated;
    chat.folder = m_library->isHome(saved) ? QString() : saved.folder;
    chat.pinned = saved.pinned;
    chat.selection.provider = saved.model.value("provider").toString();
    chat.selection.model = saved.model.value("model").toString();
    if (saved.model.value("thinking").isString())
        chat.selection.thinking = saved.model.value("thinking").toString();
    chat.mode = m_preferences->value().mode;
    // Display only until session.get confirms the backend session and version.
    chat.restored = true;
    chat.loaded = false;
    chat.reconciled = false;
    chat.locked = m_library->isLocked(saved.id);
    return chat;
}
QString ChatService::folderOf(const ChatRecord &chat) const { return chat.folder; }
RequestId ChatService::call(const Command &command, Completion completion)
{
    const auto id = ++m_next;
    m_calls.insert(id, std::move(completion));
    m_backend->request(id, command);
    return id;
}
void ChatService::initialize()
{
    if (!m_backend || m_ready || pending())
        return;
    m_plugins.initialize(false);
    m_pending = true;
    Initialize hello;
    hello.connectionId = uuid();
    hello.client = {QStringLiteral("OpenGhost"), QStringLiteral("1.3.0"), {}, {}};
    hello.tools = m_host->tools(); // none without a native browser host
    call(hello, [this, connection = hello.connectionId](const Result &result) {
        const auto *hello = value<Initialized>(result);
        if (!hello || hello->protocolVersion != QStringLiteral("0.1") ||
            !hello->capabilities.sessionRecovery) {
            m_pending = false;
            problem(hello ? Error{QStringLiteral("unsupported"),
                                  QStringLiteral("Session recovery is required."),
                                  {},
                                  {},
                                  {},
                                  {}}
                          : errorOf(result));
            return;
        }
        m_plugins.initialize(hello->capabilities.runtimePlugins, connection);
        const QString name = hello->backend ? hello->backend->name : QStringLiteral("Backend");
        call(ModelsList{}, [this, name](const Result &models) {
            const auto *list = value<QVector<Model>>(models);
            if (!list) {
                m_pending = false;
                problem(errorOf(models));
                return;
            }
            m_models = *list;
            call(ProvidersList{}, [this, name](const Result &providers) {
                m_pending = false;
                const auto *list = value<QVector<Provider>>(providers);
                if (!list) {
                    problem(errorOf(providers));
                    return;
                }
                m_providers = *list;
                m_ready = true;
                Q_UNUSED(name);
                m_status.clear();
                m_draft.selection = preferredModel();
                emit catalogChanged();
                emit changed();
                if (const auto it = m_chats.constFind(m_current);
                    it != m_chats.cend() && it->restored && it->loaded && !it->reconciled)
                    reconcile(m_current);
            });
        });
    });
}
QVector<ChatRecord> ChatService::chats() const
{
    QVector<ChatRecord> result;
    for (const auto &chat : m_chats)
        if (chat.parent.isEmpty())
            result.append(chat);
    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b) { return a.updated > b.updated; });
    return result;
}
const ChatRecord &ChatService::current() const
{
    const auto it = m_chats.constFind(m_current);
    return it == m_chats.cend() ? m_draft : it.value();
}
ChatRecord &ChatService::editable() { return m_current.isEmpty() ? m_draft : m_chats[m_current]; }
void ChatService::newChat(const QString &folder)
{
    if (pending() || (!folder.isEmpty() && !m_library->folder(folder)))
        return;
    closeMini();
    const auto left = m_current;
    if (!left.isEmpty() && m_library->isProtected(left))
        lock(left); // A protected chat locks again as soon as it is left.
    m_current.clear();
    if (m_ready)
        m_status.clear();
    m_draft = {};
    m_draft.folder = folder.isEmpty() ? QString() : m_library->folder(folder)->path;
    const auto &prefs = m_preferences->value();
    m_draft.selection = preferredModel();
    m_draft.mode = prefs.mode;
    emit changed();
    emit replaced(left);
}
ModelSelection ChatService::preferredModel() const
{
    auto selection = m_preferences->value().model;
    if (selection.model.isEmpty() && selection.provider.isEmpty() && !m_models.isEmpty())
        selection = {m_models.first().provider, m_models.first().id, {}};
    selection.thinking = m_preferences->value().preferredThinking;
    for (const auto &model : m_models) {
        if (model.id != selection.model || model.provider != selection.provider)
            continue;
        if (!selection.thinking || !model.thinkingLevels.contains(*selection.thinking))
            selection.thinking =
                model.defaultThinking && model.thinkingLevels.contains(*model.defaultThinking)
                    ? model.defaultThinking
                    : std::nullopt;
    }
    return selection;
}
void ChatService::open(const QString &id)
{
    if (pending() || !m_chats.contains(id) || !m_chats[id].parent.isEmpty() || id == m_current)
        return;
    closeMini();
    const auto left = m_current;
    if (!left.isEmpty() && m_library->isProtected(left))
        lock(left);
    m_current = id;
    if (m_ready)
        m_status.clear();
    auto &opened = m_chats[id];
    if (m_library->isLocked(id)) {
        // Its lock screen: messages are read only once the password is in.
        opened.locked = true;
        m_status = QStringLiteral("This chat is locked.");
        emit changed();
        emit replaced(left);
        return;
    }
    const bool loaded = opened.loaded || load(opened);
    emit changed();
    emit replaced(left);
    if (!m_ready || !loaded)
        return;
    if (opened.restored && !opened.reconciled) {
        reconcile(id);
        return;
    }
    if (!m_chats[id].reconciled) {
        problem(
            {QStringLiteral("invalid_recovery"),
             m_chats[id].turn.retryable
                 ? QStringLiteral("Acceptance is uncertain. Use Retry to reconcile before sending.")
                 : QStringLiteral("Only cached display is available. Start a new chat."),
             {},
             {},
             false,
             {}});
        return;
    }
    m_pending = true;
    const auto expected = m_chats[id].version;
    // Known local chats retain their display rows, never submit them as input.
    call(GetSession{id, {}}, [this, id, expected](const Result &result) {
        m_pending = false;
        auto &chat = m_chats[id];
        const auto *recovery = value<SessionRecovery>(result);
        const auto *existing = recovery ? std::get_if<ExistingSession>(recovery) : nullptr;
        if (!existing || !expected || existing->sessionVersion != *expected ||
            existing->revision != chat.sequence) {
            chat.reconciled = false;
            problem(
                recovery
                    ? Error{QStringLiteral("invalid_recovery"),
                            QStringLiteral(
                                "Session identity or display revision differs. Start a new chat."),
                            {},
                            {},
                            {},
                            {}}
                    : errorOf(result));
            return;
        }
        // The in-process fake delivers all events to this owner; no cache/journal
        // reconstruction is claimed. A foreign recovered turn must fail closed.
        if (existing->turn && existing->turn->turnId != chat.turn.remoteId) {
            chat.reconciled = false;
            problem({QStringLiteral("invalid_recovery"),
                     QStringLiteral("Unrecognized recovered turn."),
                     {},
                     {},
                     {},
                     {}});
            return;
        }
        chat.sequence = existing->revision;
        chat.reconciled = true;
        emit changed();
    });
    emit changed();
}
bool ChatService::load(ChatRecord &chat)
{
    QString error;
    const auto body = chat.parent.isEmpty() ? m_library->conversation(chat.id, &error)
                                            : m_library->side(chat.parent, &error);
    if (!body) {
        // Never an empty chat that a later save could write over.
        problem({QStringLiteral("unreadable"), error, {}, {}, false, {}});
        return false;
    }
    chat.rows = rowsOf(body->messages);
    chat.tokens = body->tokens;
    chat.seen = body->seen;
    chat.loaded = true;
    chat.locked = false;
    return true;
}
bool ChatService::checkpoint(ChatRecord &chat)
{
    // The index (session identity) and the display checkpoint, with its pending
    // markers, must reach storage before any start/retry/steer is dispatched.
    if (!chat.loaded || !m_library->persist())
        return false;
    return chat.parent.isEmpty()
               ? m_library->saveMessages(chat.id, entries(chat.rows), chat.tokens)
               : m_library->saveSide(chat.parent, {entries(chat.rows), chat.tokens, chat.seen});
}
void ChatService::save(ChatRecord &chat)
{
    if (!chat.loaded || chat.locked || !m_chats.contains(chat.id))
        return; // Best effort; never writes a cache it has not read.
    if (chat.parent.isEmpty())
        m_library->saveMessages(chat.id, entries(chat.rows), chat.tokens);
    else
        m_library->saveSide(chat.parent, {entries(chat.rows), chat.tokens, chat.seen});
}
void ChatService::settlePending(ChatRecord &chat, const QString &clientTurn)
{
    if (clientTurn.isEmpty())
        return;
    for (auto &row : chat.rows)
        if (row.backendTurn == clientTurn)
            row.pendingTurn = false;
}
void ChatService::reconcile(const QString &id)
{
    auto it = m_chats.find(id);
    if (it == m_chats.end() || !m_ready || it->recovering || pending())
        return;
    if (!it->loaded && !load(*it))
        return;
    // A durable pending marker names an uncertain start even across a restart;
    // reconciliation reads it from the backend and never resends it.
    QString pendingTurn;
    for (const auto &row : it->rows)
        if (row.pendingTurn && !row.backendTurn.isEmpty()) {
            pendingTurn = row.backendTurn;
            break;
        }
    it->recovering = true;
    it->reconciled = false;
    it->raced.clear();
    m_pending = true;
    call(GetSession{id, pendingTurn.isEmpty() ? std::nullopt : std::optional(pendingTurn)},
         [this, id, pendingTurn](const Result &result) {
             m_pending = false;
             auto it = m_chats.find(id);
             if (it == m_chats.end())
                 return;
             auto &chat = it.value();
             chat.recovering = false;
             const auto raced = std::move(chat.raced);
             chat.raced.clear();
             const auto fail = [&](const QString &code, const QString &message) {
                 chat.reconciled = false;
                 problem({code, message, {}, {}, false, {}});
             };
             const auto *recovery = value<SessionRecovery>(result);
             if (!recovery) {
                 chat.reconciled = false;
                 problem(errorOf(result));
                 return;
             }
             if (std::holds_alternative<MissingSession>(*recovery)) {
                 // An unopened mini chat is new, unlike a chat with a saved display.
                 if (!chat.parent.isEmpty() && chat.rows.isEmpty()) {
                     chat.version.reset();
                     chat.sequence = -1;
                     chat.reconciled = true;
                     chat.restored = false;
                     emit changed();
                     return;
                 }
                 fail(QStringLiteral("session_missing"),
                      QStringLiteral("The backend session is missing. This chat is kept for "
                                     "display only. Restore the backend session or start a new "
                                     "chat."));
                 return;
             }
             const auto &saved = std::get<ExistingSession>(*recovery);
             if (saved.sessionVersion.isEmpty() || saved.revision < 0) {
                 fail(QStringLiteral("invalid_recovery"),
                      QStringLiteral("The backend returned an invalid session recovery "
                                     "response."));
                 return;
             }
             if (!pendingTurn.isEmpty() &&
                 (!saved.turn || saved.turn->clientTurnId != pendingTurn)) {
                 fail(QStringLiteral("turn_missing"),
                      QStringLiteral("The backend did not accept the saved turn. Its display copy "
                                     "is preserved; it has not been sent again. Start a new chat "
                                     "or restore the backend session."));
                 return;
             }
             const auto rows = chat.rows;
             const auto turn = chat.turn;
             chat.version = saved.sessionVersion;
             chat.sequence = saved.revision;
             if (saved.turn && !recoverTurn(chat, *saved.turn)) {
                 chat.rows = rows;
                 chat.turn = turn;
                 chat.version.reset();
                 chat.sequence = -1;
                 fail(QStringLiteral("invalid_recovery"),
                      QStringLiteral("The backend returned an invalid turn recovery response."));
                 return;
             }
             chat.reconciled = true;
             chat.restored = false;
             // session.get is the replay/live boundary: raced events apply once.
             for (const auto &event : raced)
                 this->event(event);
             auto &settled = m_chats[id];
             if (!checkpoint(settled)) {
                 settled.reconciled = false;
                 problem({QStringLiteral("checkpoint_failed"),
                          QStringLiteral("Cannot save the session recovery checkpoint."),
                          {},
                          {},
                          false,
                          {}});
                 return;
             }
             m_status.clear();
             emit changed();
         });
    emit changed();
}
bool ChatService::recoverTurn(ChatRecord &chat, const RecoveredTurn &snapshot)
{
    if (snapshot.clientTurnId.isEmpty() || snapshot.turnId.isEmpty())
        return false;
    Sequence sequence = -1;
    for (const auto &event : snapshot.events) {
        const auto &identity = event.identity;
        if (identity.sessionId != chat.id || identity.seq <= sequence ||
            identity.seq > chat.sequence ||
            (identity.turnId && *identity.turnId != snapshot.turnId) ||
            (identity.clientTurnId && *identity.clientTurnId != snapshot.clientTurnId))
            return false;
        sequence = identity.seq;
    }
    const auto client = snapshot.clientTurnId;
    // Rebuild only that turn; completed text and frontend annotations stay put.
    QVector<DisplayRow> queued, prompts;
    for (const auto &row : chat.rows)
        if (row.backendTurn == client && row.role == DisplayRow::Role::User)
            (row.clientInputId.isEmpty() ? prompts : queued).append(row);
    chat.rows.removeIf([&](const DisplayRow &row) { return row.backendTurn == client; });
    if (!chat.turn.remoteId.isEmpty() && chat.turn.remoteId != snapshot.turnId)
        chat.pastTurns.insert(chat.turn.remoteId, chat.turn);
    chat.turn = {};
    chat.turn.clientId = client;
    chat.turn.remoteId = snapshot.turnId;
    chat.turn.replyKey = uuid();
    chat.turn.firstRow = chat.rows.size();
    chat.turn.dispatched = chat.turn.acknowledged = true;
    if (snapshot.input) {
        if (prompts.isEmpty()) {
            DisplayRow prompt{DisplayRow::Role::User,
                              uuid(),
                              snapshot.input->text,
                              "done",
                              snapshot.input->attachments,
                              {},
                              {},
                              -1,
                              -1};
            prompts.append(prompt);
        }
        for (auto prompt : prompts) {
            prompt.backendTurn = client;
            prompt.pendingTurn = true;
            prompt.state = QStringLiteral("done");
            chat.rows.append(prompt);
        }
    } else {
        // A retry carries no input: its open reply part holds the marker.
        DisplayRow part{
            DisplayRow::Role::Assistant, chat.turn.replyKey, {}, "pending", {}, {}, {}, -1, -1};
        part.backendTurn = client;
        part.pendingTurn = part.hidden = true;
        chat.rows.append(part);
    }
    for (const auto &row : queued)
        chat.turn.recoveryInputs.insert(row.clientInputId, row);
    for (const auto &event : snapshot.events)
        applyEvent(chat, event, true);
    // Queued inputs the backend did not accept stay visible, never resent.
    for (auto row : std::as_const(queued)) {
        if (!chat.turn.recoveryInputs.contains(row.clientInputId))
            continue;
        row.state = chat.turn.terminal ? QStringLiteral("unconfirmed") : QStringLiteral("queued");
        chat.rows.append(row);
        chat.turn.steering.insert(row.clientInputId, row.key);
    }
    chat.turn.recoveryInputs.clear();
    return true;
}
quint64 ChatService::send(const QString &text, const QVector<Attachment> &attachments)
{
    if (canSteer())
        return steer({text, attachments});
    if (!ready() || pending() || busy() || (text.trimmed().isEmpty() && attachments.isEmpty()) ||
        current().selection.model.isEmpty())
        return 0;
    const auto &selection = current().selection;
    if (std::none_of(m_models.cbegin(), m_models.cend(), [&](const auto &model) {
            return model.id == selection.model && model.provider == selection.provider;
        })) {
        problem({QStringLiteral("model_unavailable"),
                 QStringLiteral("Choose an available model."),
                 {},
                 {},
                 false,
                 {}});
        return 0;
    }
    if (current().locked || !current().loaded) {
        problem(
            {QStringLiteral("locked"), QStringLiteral("This chat is locked."), {}, {}, false, {}});
        return 0;
    }
    if (m_current.isEmpty()) {
        // A chat is made with its first message: index first, create-only start.
        QString title = titleFrom(text);
        if (text.trimmed().isEmpty() && !attachments.isEmpty()) {
            QStringList names;
            for (const auto &a : attachments)
                names.append(a.name);
            title = titleFrom(names.join(QStringLiteral(", ")));
        }
        const auto *record = m_library->create(m_draft.folder, title);
        if (!record) {
            problem({QStringLiteral("checkpoint_failed"),
                     m_library->error().isEmpty()
                         ? QStringLiteral("Cannot save the session recovery checkpoint.")
                         : m_library->error(),
                     {},
                     {},
                     false,
                     {}});
            return 0;
        }
        ChatRecord chat = m_draft;
        chat.id = record->id;
        chat.title = record->title;
        chat.created = record->created;
        chat.updated = record->updated;
        chat.version.reset(); // explicit create-only precondition, never an upsert
        m_library->update(chat.id,
                          [&](Library::Chat &c) { c.model = modelObject(chat.selection); });
        m_current = chat.id;
        m_chats.insert(chat.id, chat);
    }
    auto &chat = editable();
    if (!chat.turn.remoteId.isEmpty())
        chat.pastTurns.insert(chat.turn.remoteId, chat.turn);
    // Older rows' markers are settled by their own outcome; a new turn starts clean.
    chat.turn = {};
    chat.turn.firstRow = chat.rows.size();
    chat.turn.clientId = uuid();
    chat.turn.replyKey = uuid();
    chat.updated = QDateTime::currentMSecsSinceEpoch();
    if (chat.parent.isEmpty())
        m_library->update(chat.id, [&](Library::Chat &c) { c.updated = chat.updated; });
    const QString userKey = uuid();
    DisplayRow user{
        DisplayRow::Role::User, userKey, text, QStringLiteral("sending"), {}, {}, {}, -1, -1};
    for (const auto &a : attachments)
        user.attachments.append(displayOf(a));
    user.backendTurn = chat.turn.clientId;
    user.pendingTurn = true;
    chat.rows.append(user);
    StartTurn start;
    start.sessionId = chat.id;
    start.sessionVersion = chat.version;
    start.clientTurnId = chat.turn.clientId;
    start.input = {text, attachments};
    start.params = params(chat);
    if (start.params.side) {
        start.params.side->moved = m_miniMoved;
        m_miniMoved = false;
    }
    chat.turn.prepared = start;
    const auto submission = ++m_submission;
    chat.turn.submission = submission;
    dispatchStart(start, submission, userKey);
    emit changed();
    return submission;
}
void ChatService::checkpointFailed(ChatRecord &chat)
{
    // Positively not dispatched: Retry may start this exact prepared input.
    chat.turn.terminal = true;
    chat.turn.retryable = true;
    chat.turn.dispatched = false;
    for (auto &row : chat.rows)
        if (row.state == QStringLiteral("sending") && row.clientInputId.isEmpty())
            row.state = QStringLiteral("error");
    chat.rows.append({DisplayRow::Role::Note,
                      uuid(),
                      QStringLiteral("Cannot save the session recovery checkpoint. Nothing was "
                                     "sent."),
                      QStringLiteral("error"),
                      {},
                      {},
                      {},
                      -1,
                      -1});
    problem({QStringLiteral("checkpoint_failed"),
             QStringLiteral("Cannot save the session recovery checkpoint."),
             {},
             {},
             true,
             {}});
}
SessionParams ChatService::params(const ChatRecord &chat) const
{
    SessionParams p;
    p.selection = chat.selection;
    p.permissionMode = chat.mode;
    p.title = chat.title;
    p.userContext = m_preferences->value().userContext;
    p.host.browser = m_host->browser(); // null: no browser panel
    if (const auto *record = m_library->chat(chat.parent.isEmpty() ? chat.id : chat.parent))
        p.cwd = m_library->cwdOf(*record);
    else if (!chat.folder.isEmpty())
        p.cwd = chat.folder;
    if (!chat.parent.isEmpty()) {
        const auto parent = m_chats.constFind(chat.parent);
        p.side = SideContext{chat.parent,
                             parent != m_chats.cend() && !parent->turn.clientId.isEmpty() &&
                                 !parent->turn.terminal,
                             false};
    }
    return p;
}
bool ChatService::dispatchStart(const StartTurn &start, quint64 submission, const QString &userKey)
{
    m_status.clear();
    auto &owner = m_chats[start.sessionId];
    if (!checkpoint(owner)) {
        checkpointFailed(owner);
        return false;
    }
    owner.turn.dispatched = true; // From here, failure is uncertain until reconciled.
    m_pending = true;
    const auto request = call(start, [this, start, submission, userKey](const Result &result) {
        auto &chat = m_chats[start.sessionId];
        const auto *ack = value<StartAccepted>(result);
        m_pending = false;
        if (!ack || ack->turnId.isEmpty() || ack->sessionVersion.isEmpty() ||
            (start.sessionVersion && *start.sessionVersion != ack->sessionVersion) ||
            (!chat.turn.remoteId.isEmpty() && chat.turn.remoteId != ack->turnId)) {
            chat.reconciled = false;
            chat.turn.invalidStart = true;
            chat.turn.terminal = true;
            chat.turn.retryable = true; // Retry first reconciles; it does not resend.
            finishRows(chat, QStringLiteral("unconfirmed"));
            for (auto &row : chat.rows)
                if (row.key == userKey)
                    row.state = QStringLiteral("unconfirmed");
            dismissApprovals(chat.id, QStringLiteral("cancelled"));
            emit submissionFailed(submission);
            problem(ack ? Error{QStringLiteral("invalid_turn"),
                                QStringLiteral(
                                    "Invalid start acknowledgement; reconcile before continuing."),
                                {},
                                {},
                                {},
                                {}}
                        : errorOf(result));
            return;
        }
        chat.version = ack->sessionVersion;
        if (ack->selection) {
            chat.selection = *ack->selection;
            if (chat.parent.isEmpty())
                m_library->update(chat.id, [&](Library::Chat &c) {
                    c.model = modelObject(chat.selection);
                });
        }
        chat.turn.remoteId = ack->turnId;
        chat.turn.acknowledged = true;
        if (chat.turn.started < 0)
            chat.turn.started = QDateTime::currentMSecsSinceEpoch();
        drainEarly(chat);
        if (chat.turn.stopped && !m_cancelling)
            cancelRemote(chat);
        for (auto &row : chat.rows)
            if (row.key == userKey)
                row.state = QStringLiteral("done");
        // A turn that completed before its acknowledgement settles its markers now.
        if (chat.turn.terminal && !chat.turn.stopped)
            settlePending(chat, chat.turn.clientId);
        save(chat);
        emit accepted(submission);
        emit changed();
    });
    m_chats[start.sessionId].turn.request = request;
    return true;
}
void ChatService::finishRows(ChatRecord &chat, const QString &state)
{
    for (auto &row : chat.rows)
        if (row.state == QStringLiteral("live") || row.state == QStringLiteral("sending"))
            row.state = state;
        else if (row.state == QStringLiteral("queued"))
            row.state =
                QStringLiteral("unconfirmed"); // Queue admission alone does not prove application.
}
void ChatService::publishReply(ChatRecord &chat)
{
    for (auto &row : chat.rows) {
        if (row.role != DisplayRow::Role::Assistant)
            continue;
        QStringList parts;
        for (const auto &message : chat.turn.messages)
            if (message.rowKey == row.key && !message.text.trimmed().isEmpty())
                parts.append(message.text.trimmed());
        if (std::any_of(chat.turn.messages.cbegin(), chat.turn.messages.cend(),
                        [&](const auto &m) { return m.rowKey == row.key; }))
            row.text = parts.join(QStringLiteral("\n\n"));
    }
}
DisplayRow *ChatService::toolRow(ChatRecord &chat, const QString &callId)
{
    const auto key = chat.turn.tools.value(callId);
    if (key.isEmpty())
        return nullptr;
    for (auto &row : chat.rows)
        if (row.key == key)
            return &row;
    return nullptr;
}
// A new card, after everything shown so far: the reply goes on below it.
DisplayRow *ChatService::addTool(ChatRecord &chat, const QString &callId, const QString &name)
{
    auto &turn = chat.turn;
    if (callId.isEmpty() || name.isEmpty() || turn.toolCount >= ToolCards)
        return nullptr;
    ++turn.toolCount;
    DisplayRow row{DisplayRow::Role::Tool, uuid(), {}, QStringLiteral("running"), {}, {}, {}, -1, -1};
    row.backendTurn = turn.clientId;
    row.remoteTurn = turn.remoteId;
    row.tool.callId = callId;
    row.tool.name = name;
    row.tool.after = turn.replyKey;
    chat.rows.append(row);
    turn.tools.insert(callId, row.key);
    turn.replyKey = uuid();
    turn.continued = true;
    return &chat.rows.last();
}
void ChatService::event(const SessionEvent &event)
{
    const auto &id = event.identity;
    auto it = m_chats.find(id.sessionId);
    if (it == m_chats.end() || id.seq < 0)
        return;
    if (it->recovering) {
        // Raced session.get: applied once afterwards, beyond its revision.
        if (it->raced.size() < 256)
            it->raced.append(event);
        return;
    }
    if (id.seq <= it->sequence || it->restored)
        return; // A restored display waits for session.get; never guessed from deltas.
    auto &chat = it.value();
    chat.sequence = id.seq;
    if (const auto *update = std::get_if<SessionUpdated>(&event.payload)) {
        if (update->title && !chat.renamed && !update->title->simplified().isEmpty() &&
            chat.parent.isEmpty()) {
            chat.title = update->title->simplified().left(60);
            m_library->retitle(chat.id, chat.title, false);
        }
        emit changed();
        return;
    }
    applyEvent(chat, event);
}
void ChatService::drainEarly(ChatRecord &chat)
{
    const auto events = std::move(chat.turn.early);
    chat.turn.early.clear();
    for (const auto &event : events)
        applyEvent(chat, event);
    QVector<PendingApproval> approvals;
    for (const auto &p : m_earlyApprovals)
        if (p.data.sessionId == chat.id)
            approvals.append(p);
    m_earlyApprovals.removeIf([&](const auto &p) { return p.data.sessionId == chat.id; });
    for (const auto &p : approvals)
        reverse(p.request, p.data);
}
void ChatService::applyEvent(ChatRecord &chat, const SessionEvent &event, bool replay)
{
    const auto &id = event.identity;
    auto &turn = chat.turn;
    if (turn.remoteId.isEmpty() && !turn.terminal && !turn.stopped &&
        !std::holds_alternative<TurnStarted>(event.payload) &&
        (!id.turnId || !chat.pastTurns.contains(*id.turnId))) {
        if (id.clientTurnId && *id.clientTurnId != turn.clientId)
            return;
        if (turn.early.size() < 256)
            turn.early.append(event);
        else {
            turn.terminal = true;
            turn.retryable = true;
            chat.reconciled = false;
            problem({"invalid_recovery", "Too many uncorrelated early events.", {}, {}, false, {}});
        }
        return;
    }
    if (const auto *usage = std::get_if<Usage>(&event.payload)) {
        auto *owner = &turn;
        if (id.turnId && *id.turnId != turn.remoteId) {
            auto past = chat.pastTurns.find(*id.turnId);
            if (past == chat.pastTurns.end())
                return;
            owner = &past.value();
        }
        if (!id.turnId && id.messageId &&
            std::none_of(turn.messages.cbegin(), turn.messages.cend(),
                         [&](const auto &m) { return m.id == *id.messageId; })) {
            auto past =
                std::find_if(chat.pastTurns.begin(), chat.pastTurns.end(), [&](const auto &t) {
                    return std::any_of(t.messages.cbegin(), t.messages.cend(),
                                       [&](const auto &m) { return m.id == *id.messageId; });
                });
            if (past == chat.pastTurns.end())
                return;
            owner = &past.value();
        }
        if (owner->remoteId.isEmpty() || (id.clientTurnId && *id.clientTurnId != owner->clientId) ||
            (!id.turnId && !id.messageId))
            return;
        if (id.messageId && std::none_of(owner->messages.cbegin(), owner->messages.cend(),
                                         [&](const auto &m) { return m.id == *id.messageId; }))
            return;
        const auto count = [](double n) { return std::isfinite(n) ? std::max(0.0, n) : 0.0; };
        owner->input += count(usage->input);
        owner->output += count(usage->output);
        owner->cached += std::min(count(usage->cached), count(usage->input));
        owner->written += count(usage->written);
        owner->requests += std::max(1.0, count(usage->requests.value_or(1)));
        if (owner == &turn && !turn.terminal && usage->context) {
            turn.context = usage->context;
            chat.tokens = count(usage->context->used);
        }
        updateMetrics(chat, *owner);
        if (!replay) {
            emit usageRecorded(*usage);
            save(chat);
        }
        emit changed();
        return;
    }
    if (turn.stopped && id.turnId == turn.remoteId && id.clientTurnId == turn.clientId &&
        id.messageId && std::holds_alternative<MessageStarted>(event.payload)) {
        // Freeze text, not accounting: Pi may begin a new assistant attempt while
        // Stop is in flight (tools/automatic retry). Retain its identity for usage.
        if (std::none_of(turn.messages.cbegin(), turn.messages.cend(),
                         [&](const auto &m) { return m.id == *id.messageId; }))
            turn.messages.append({*id.messageId, {}, turn.replyKey, true});
        return;
    }
    if (turn.stopped && !turn.remoteId.isEmpty() &&
        std::holds_alternative<TurnCompleted>(event.payload) && id.turnId == turn.remoteId) {
        // The backend's own end of a stopped turn settles its recovery markers.
        turn.completed = replay ? -1 : QDateTime::currentMSecsSinceEpoch();
        updateMetrics(chat, turn);
        settlePending(chat, turn.clientId);
        if (!replay)
            save(chat);
        return;
    }
    if (turn.clientId.isEmpty() || turn.terminal || turn.stopped)
        return;
    if (id.clientTurnId && *id.clientTurnId != turn.clientId)
        return;
    if (std::holds_alternative<TurnStarted>(event.payload)) {
        if (id.clientTurnId == std::optional<QString>(turn.clientId) && id.turnId &&
            !id.turnId->isEmpty() && (turn.remoteId.isEmpty() || turn.remoteId == *id.turnId)) {
            turn.remoteId = *id.turnId;
            if (turn.started < 0 && !replay)
                turn.started = QDateTime::currentMSecsSinceEpoch();
            drainEarly(chat);
        }
        emit changed();
        return;
    }
    if (turn.remoteId.isEmpty() || (id.turnId && *id.turnId != turn.remoteId))
        return;
    auto message = std::find_if(turn.messages.begin(), turn.messages.end(), [&](const auto &m) {
        return id.messageId && m.id == *id.messageId;
    });
    const bool messageOnly = std::holds_alternative<MessageDelta>(event.payload) ||
                             std::holds_alternative<MessageCompleted>(event.payload);
    if (!id.turnId && (!messageOnly || message == turn.messages.end()))
        return;
    if (const auto *start = std::get_if<MessageStarted>(&event.payload)) {
        if (!id.messageId || id.messageId->isEmpty() ||
            start->role != QStringLiteral("assistant") || message != turn.messages.end())
            return;
        auto part = std::find_if(chat.rows.begin(), chat.rows.end(),
                                 [&](const auto &r) { return r.key == turn.replyKey; });
        if (part == chat.rows.end()) {
            chat.rows.append({DisplayRow::Role::Assistant,
                              turn.replyKey,
                              {},
                              QStringLiteral("live"),
                              {},
                              {},
                              {},
                              -1,
                              -1});
            part = chat.rows.end() - 1;
            part->continued = std::exchange(turn.continued, false);
            part->pendingTurn =
                std::any_of(chat.rows.cbegin(), chat.rows.cend(), [&](const auto &r) {
                    return r.backendTurn == turn.clientId && r.pendingTurn;
                });
        }
        part->hidden = false; // A retry's open part becomes its reply.
        part->state = QStringLiteral("live");
        part->backendTurn = turn.clientId;
        part->remoteTurn = turn.remoteId;
        part->model = modelKey(chat.selection);
        turn.messages.append({*id.messageId, {}, turn.replyKey, false});
    } else if (const auto *delta = std::get_if<MessageDelta>(&event.payload)) {
        if (message == turn.messages.end() || message->sealed)
            return;
        message->text += delta->text;
        publishReply(chat);
        emit answered();
    } else if (const auto *done = std::get_if<MessageCompleted>(&event.payload)) {
        if (message == turn.messages.end() || message->sealed)
            return;
        if (done->text)
            message->text = *done->text; // including an authoritative empty string
        message->sealed = true;
        if (done->finishReason)
            turn.finishReason = *done->finishReason;
        publishReply(chat);
        if (!replay)
            save(chat);
    } else if (const auto *done = std::get_if<TurnCompleted>(&event.payload)) {
        endTurn(chat, *done, replay);
    } else if (const auto *tool = std::get_if<ToolStarted>(&event.payload)) {
        // A duplicate (the call's execution after its announcement) only adds
        // arguments that were not known; a reused ID after a result is a new call.
        auto *row = toolRow(chat, tool->toolCallId);
        if (row && row->state == QStringLiteral("running")) {
            if (!row->tool.known && tool->arguments) {
                row->tool.arguments = *tool->arguments;
                row->tool.known = true;
            }
        } else if (!(row = addTool(chat, tool->toolCallId, tool->name))) {
            return;
        } else {
            row->tool.known = tool->arguments.has_value();
            row->tool.arguments = tool->arguments.value_or(QString());
        }
        turn.activity = tool->title.value_or(tool->name);
        emit worked();
    } else if (const auto *progress = std::get_if<ToolProgress>(&event.payload)) {
        // Output only for a running call; orphan or late output is dropped.
        auto *row = toolRow(chat, progress->toolCallId);
        if (!row || row->state != QStringLiteral("running"))
            return;
        const auto legacy = progress->detail.value("text");
        const auto output = progress->output ? progress->output
                            : legacy.isString() ? std::optional(legacy.toString())
                                                : std::nullopt;
        if (!output)
            return;
        liveOutput(*row, *output, progress->append);
        emit worked();
    } else if (const auto *done = std::get_if<ToolCompleted>(&event.payload)) {
        // An outcome the backend did not report is no outcome: the call stays
        // unresolved rather than being shown as done.
        const auto reported = done->isError ? done->isError
                              : done->detail.value("isError").isBool()
                                  ? std::optional(done->detail.value("isError").toBool())
                                  : std::nullopt;
        const auto output = done->output ? *done->output : done->detail.value("text").toString();
        if (!reported || done->toolCallId.isEmpty())
            return;
        auto *row = toolRow(chat, done->toolCallId);
        if (row && done->name && *done->name != row->tool.name)
            return; // Not this call's result.
        if (row && row->state != QStringLiteral("running") && (!done->saved || row->tool.saved))
            return; // A duplicate: the first result stands, settled once by the saved one.
        if (!row && (!done->name || !(row = addTool(chat, done->toolCallId, *done->name))))
            return; // A result for a call never announced: its arguments stay unknown.
        row->state = *reported ? QStringLiteral("error") : QStringLiteral("done");
        row->text = resultOutput(output);
        row->tool.omittedLines = row->tool.omittedCharacters = 0;
        row->tool.removed = 0;
        row->tool.saved = done->saved;
        turn.activity.clear();
    } else if (const auto *accepted = std::get_if<InputAccepted>(&event.payload)) {
        if (replay && !turn.steering.contains(accepted->clientInputId) &&
            (accepted->input || turn.recoveryInputs.contains(accepted->clientInputId))) {
            auto row = turn.recoveryInputs.contains(accepted->clientInputId)
                           ? turn.recoveryInputs.take(accepted->clientInputId)
                           : DisplayRow{DisplayRow::Role::User,
                                        uuid(),
                                        accepted->input->text,
                                        "queued",
                                        accepted->input->attachments,
                                        {},
                                        {},
                                        -1,
                                        -1};
            row.state = QStringLiteral("queued");
            row.backendTurn = turn.clientId;
            row.clientInputId = accepted->clientInputId;
            chat.rows.append(row);
            turn.steering.insert(accepted->clientInputId, row.key);
        }
        const auto key = turn.steering.value(accepted->clientInputId);
        for (auto &row : chat.rows)
            if (!key.isEmpty() && row.key == key && row.state != "applied") {
                row.state = "applied";
                turn.replyKey = uuid(); // The next assistant message is a new reply part.
                turn.continued = false;
            }
        if (!replay)
            save(chat);
    } else if (const auto *resolved = std::get_if<ApprovalResolved>(&event.payload)) {
        m_approvals.removeIf([&](const auto &p) {
            return p.data.sessionId == chat.id && p.data.approvalId == resolved->approvalId;
        });
    } else if (std::holds_alternative<CompactionStarted>(event.payload)) {
        turn.activity = QStringLiteral("Compacting conversation");
        emit worked();
    } else if (std::holds_alternative<CompactionCompleted>(event.payload)) {
        turn.activity.clear();
    }
    emit changed();
}
void ChatService::stop()
{
    if (!canCancel())
        return;
    auto &chat = editable();
    chat.turn.stopped = true; // freeze before any request/response
    endTurn(chat, {TurnStatus::Cancelled, {}, {}}, false, true);
    if (!chat.turn.remoteId.isEmpty()) {
        cancelRemote(chat);
    } else {
        chat.turn.retryable = true;
        chat.reconciled = false; // unknown acceptance; never permit automatic resend
        // Only this turn's own start/retry: other calls (plugin list/enable/
        // disable, catalog) are not part of the chat and may already be applied.
        if (chat.turn.request != 0 && m_calls.contains(chat.turn.request))
            m_backend->cancelRequest(chat.turn.request);
    }
    emit changed();
}
void ChatService::cancelRemote(ChatRecord &chat)
{
    m_cancelling = true;
    const auto id = chat.id;
    const auto client = chat.turn.clientId;
    call(CancelTurn{id, chat.turn.remoteId}, [this, id, client](const Result &result) {
        m_cancelling = false;
        auto it = m_chats.find(id);
        if (it == m_chats.end())
            return;
        if (!value<Null>(result)) {
            it->reconciled = false;
            it->turn.retryable = true; // Reconciliation only, never an automatic resend.
            problem(errorOf(result));
        } else {
            // The backend owns the cancelled turn's outcome now; its display is frozen.
            if (it->turn.clientId == client)
                settlePending(*it, client);
            save(*it);
            emit changed();
        }
    });
}
void ChatService::chooseSaved(const ModelSelection &selection, bool thinkingPreference)
{
    auto preferences = m_preferences->value();
    preferences.model = selection;
    preferences.model.thinking.reset();
    if (thinkingPreference)
        preferences.preferredThinking = selection.thinking;
    if (!m_preferences->save(preferences))
        m_status =
            QStringLiteral("Model selected for this chat, but preferences could not be saved.");
}
void ChatService::choose(const ModelSelection &selection, bool thinkingPreference)
{
    if (pending() || busy()) {
        emit changed();
        return;
    }
    if (!current().version) {
        editable().selection = selection;
        chooseSaved(selection, thinkingPreference);
        emit changed();
        return;
    }
    if (!ready()) {
        emit changed();
        return;
    }
    const auto id = m_current;
    ConfigureSession config;
    config.sessionId = id;
    config.sessionVersion = *current().version;
    config.clientTurnId = uuid();
    config.model = selection.model;
    config.provider = selection.provider;
    config.thinking = selection.thinking.value_or(QString());
    m_pending = true;
    call(config, [this, id, selection, thinkingPreference](const Result &result) {
        m_pending = false;
        const auto *canonical = value<SessionConfigured>(result);
        if (!canonical) {
            problem(errorOf(result));
            return;
        }
        auto &chosen = m_chats[id].selection;
        chosen = selection;
        if (canonical->model && !canonical->model->isEmpty()) {
            chosen.model = *canonical->model;
            chosen.provider = canonical->provider.value_or(selection.provider);
        }
        if (canonical->thinking)
            chosen.thinking = *canonical->thinking;
        if (m_chats[id].parent.isEmpty())
            m_library->update(id, [&](Library::Chat &c) { c.model = modelObject(chosen); });
        auto preference = chosen;
        preference.thinking = selection.thinking; // never persist canonical effort defaults
        chooseSaved(preference, thinkingPreference);
        emit changed();
    });
    emit changed();
}
void ChatService::setMode(PermissionMode mode)
{
    if (pending())
        return;
    const auto save = [this](PermissionMode effective) {
        editable().mode = effective;
        auto prefs = m_preferences->value();
        prefs.mode = effective;
        m_preferences->save(prefs);
        emit changed();
    };
    if (!current().version) {
        save(mode);
        return;
    }
    if (!ready())
        return;
    ConfigureSession config;
    config.sessionId = m_current;
    config.sessionVersion = *current().version;
    config.permissionMode = mode;
    m_pending = true;
    call(config, [this, save, mode](const Result &result) {
        m_pending = false;
        const auto *configured = value<SessionConfigured>(result);
        if (!configured) {
            problem(errorOf(result));
            return;
        }
        save(configured->permissionMode.value_or(mode));
    });
    emit changed();
}
void ChatService::rename(const QString &id, const QString &title)
{
    auto it = m_chats.find(id);
    if (it == m_chats.end() || title.simplified().isEmpty())
        return;
    if (it->locked || !it->parent.isEmpty())
        return; // A protected chat's sealed title changes only while it is open.
    if (!m_library->retitle(id, title.simplified().left(60), true)) {
        problem({QStringLiteral("save_failed"),
                 QStringLiteral("The new name could not be saved."),
                 {},
                 {},
                 false,
                 {}});
        return;
    }
    it->title = title.simplified().left(60);
    it->renamed = true; // local annotation only, never an invented rename RPC
    emit changed();
}
QString ChatService::metrics(const ChatRecord::Turn &turn)
{
    QString text = QStringLiteral("%1");
    if (turn.input || turn.output)
        text = QStringLiteral("%1 tokens · ").arg(turn.input + turn.output, 0, 'f', 0) + text;
    if (turn.input && turn.cached)
        text += QStringLiteral(" · %1% cached").arg(100 * turn.cached / turn.input, 0, 'f', 0);
    return text;
}
void ChatService::updateMetrics(ChatRecord &chat, ChatRecord::Turn &turn)
{
    // A reply with tool calls is in parts: the metrics go under its last text.
    const auto part = [&](const DisplayRow &row) {
        return std::any_of(turn.messages.cbegin(), turn.messages.cend(),
                           [&](const auto &m) { return m.rowKey == row.key; });
    };
    const bool parts = turn.toolCount > 0 && std::any_of(chat.rows.cbegin(), chat.rows.cend(),
                                                         [&](const DisplayRow &row) {
                                                             return part(row) && !row.text.isEmpty();
                                                         });
    for (auto it = chat.rows.rbegin(); it != chat.rows.rend(); ++it) {
        if (part(*it) && (!parts || !it->text.isEmpty())) {
            it->metrics = turn.terminal ? metrics(turn) : QString();
            if (turn.input || turn.output)
                it->usage = DisplayRow::Spent{turn.input, turn.cached, turn.written, turn.output,
                                              turn.requests};
            it->tip = QStringLiteral("Input %1 · Output %2").arg(turn.input).arg(turn.output);
            it->started = turn.started;
            it->completed = turn.completed;
            // The turn's usage is saved once, with the part that shows it.
            if (turn.toolCount > 0)
                for (auto &row : chat.rows)
                    if (&row != &*it && part(row)) {
                        row.metrics.clear();
                        row.tip.clear();
                        row.usage.reset();
                        row.started = row.completed = -1;
                    }
            break;
        }
    }
}
void ChatService::endTurn(ChatRecord &chat, const TurnCompleted &done, bool replay, bool local)
{
    auto &turn = chat.turn;
    turn.terminal = true;
    cancelHost(turn); // A host step still running ends with its turn.
    m_host->turnEnded(chat.id);
    turn.completed = replay ? -1 : QDateTime::currentMSecsSinceEpoch();
    const auto finish = done.finishReason.value_or(turn.finishReason);
    turn.activity.clear();
    // Calls still unresolved: no result was saved (rebuilt history), or none
    // arrived before the turn ended here. Never done, never cancelled.
    for (auto &row : chat.rows)
        if (row.role == DisplayRow::Role::Tool && row.backendTurn == turn.clientId &&
            row.state == QStringLiteral("running"))
            row.state = replay ? QStringLiteral("missing") : QStringLiteral("unconfirmed");
    turn.retryable = done.status == TurnStatus::Error &&
                     (!done.error || (done.error->retryable != std::optional<bool>(false) &&
                                      done.error->action != std::optional<QString>("none")));
    const auto state = done.status == TurnStatus::Cancelled ? QStringLiteral("cancelled")
                       : done.status == TurnStatus::Error   ? QStringLiteral("error")
                                                            : QStringLiteral("done");
    finishRows(chat, state);
    QString note;
    if (done.error)
        note = done.error->message;
    else if (done.status == TurnStatus::Cancelled)
        note = QStringLiteral("Stopped.");
    else if (done.status == TurnStatus::Error)
        note = QStringLiteral("The response failed.");
    else if (finish == "length")
        note = QStringLiteral("The response reached its output limit.");
    else if (finish == "content_filter")
        note = QStringLiteral("The response was filtered.");
    else if (finish == "insufficient_system_resource")
        note = QStringLiteral("The provider ran out of resources and stopped.");
    else if (finish == "handled")
        note = QStringLiteral("Handled as a command; no reply was started.");
    else if (std::none_of(turn.messages.cbegin(), turn.messages.cend(),
                          [](const auto &m) { return !m.text.trimmed().isEmpty(); }))
        note = QStringLiteral("The model returned no text.");
    if (!note.isEmpty())
        chat.rows.append({DisplayRow::Role::Note, uuid(), note, state, {}, {}, {}, -1, -1});
    dismissApprovals(chat.id, QStringLiteral("cancelled"));
    updateMetrics(chat, turn);
    // Settled only by the backend's own completion of an acknowledged start;
    // a local Stop or an unacknowledged/invalid start stays uncertain.
    if (!local && turn.acknowledged && !turn.invalidStart)
        settlePending(chat, turn.clientId);
    for (auto &row : chat.rows)
        if (row.key == turn.replyKey && row.hidden && row.state == QStringLiteral("pending"))
            row.state = QStringLiteral("done");
    if (!replay)
        save(chat);
}
void ChatService::cancelHost(ChatRecord::Turn &turn)
{
    const auto calls = std::move(turn.hostCalls);
    turn.hostCalls.clear();
    for (const auto id : calls) {
        m_hostRequests.remove(id);
        m_host->cancel(id);
        HostToolResult cancelled;
        cancelled.status = HostToolResult::Status::Cancelled;
        m_backend->answer(id, cancelled);
    }
}
quint64 ChatService::steer(const Input &input)
{
    if (input.text.trimmed().isEmpty() && input.attachments.isEmpty())
        return 0;
    auto &chat = editable();
    const auto key = uuid(), client = uuid(), session = chat.id;
    const auto turnId = chat.turn.remoteId;
    DisplayRow row{DisplayRow::Role::User, key, input.text, "sending", {}, {}, {}, -1, -1};
    for (const auto &a : input.attachments)
        row.attachments.append(displayOf(a));
    row.backendTurn = chat.turn.clientId;
    row.clientInputId = client;
    chat.rows.append(row);
    chat.turn.steering.insert(client, key);
    const auto submission = ++m_submission;
    if (!checkpoint(chat)) {
        // Positively not sent; it stays visible and is never sent later by itself.
        chat.rows.last().state = QStringLiteral("notApplied");
        problem({QStringLiteral("checkpoint_failed"),
                 QStringLiteral("Cannot save the session recovery checkpoint. Nothing was sent."),
                 {},
                 {},
                 false,
                 {}});
        return 0;
    }
    dismissApprovals(session, QStringLiteral("superseded"));
    m_host->inputQueued(session);
    m_pending = true;
    call(SteerTurn{session, turnId, client, input, HostContext{m_host->browser()}},
         [this, session, key, submission](const Result &result) {
             m_pending = false;
             auto &chat = m_chats[session];
             const auto *ack = value<SteerAccepted>(result);
             for (auto &row : chat.rows)
                 if (row.key == key && row.state != "applied")
                     row.state =
                         ack ? (ack->accepted ? (chat.turn.terminal ? "unconfirmed" : "queued")
                                              : "notApplied")
                             : "unconfirmed";
             save(chat);
             if (ack && ack->accepted)
                 emit accepted(submission);
             else {
                 emit submissionFailed(submission);
                 problem(ack ? Error{"steering_rejected",
                                     "Input was not accepted. It has not been resent.",
                                     {},
                                     {},
                                     false,
                                     {}}
                             : errorOf(result));
             }
             emit changed();
         });
    emit changed();
    return submission;
}
void ChatService::retry()
{
    if (!canRetry())
        return;
    const auto id = m_current;
    if (current().turn.prepared && !current().turn.dispatched && current().turn.retryable &&
        current().reconciled) {
        // The checkpoint failed before dispatch: nothing was sent, so the exact
        // prepared request (same client turn ID) may start now.
        auto &chat = editable();
        if (!chat.rows.isEmpty() && chat.rows.last().role == DisplayRow::Role::Note)
            chat.rows.removeLast(); // its checkpoint-failure note
        chat.turn.terminal = chat.turn.stopped = chat.turn.retryable = false;
        const auto prepared = *chat.turn.prepared;
        for (auto &row : chat.rows)
            if (row.backendTurn == prepared.clientTurnId && row.role != DisplayRow::Role::Tool &&
                row.state == QStringLiteral("error"))
                row.state = QStringLiteral("sending");
        QString userKey;
        for (const auto &row : chat.rows)
            if (row.backendTurn == prepared.clientTurnId && row.role == DisplayRow::Role::User)
                userKey = row.key;
        dispatchStart(prepared, chat.turn.submission, userKey);
        emit changed();
        return;
    }
    if (!current().reconciled) {
        m_pending = true;
        const auto client = current().turn.clientId;
        call(GetSession{id, client}, [this, id, client](const Result &result) {
            m_pending = false;
            auto &chat = m_chats[id];
            const auto *recovery = value<SessionRecovery>(result);
            if (!recovery) {
                problem(errorOf(result));
                return;
            }
            if (std::holds_alternative<MissingSession>(*recovery)) {
                if (chat.version || !chat.turn.remoteId.isEmpty() || !chat.turn.prepared) {
                    problem({"session_missing",
                             "The previous session is missing; input has not been resent.",
                             {},
                             {},
                             false,
                             {}});
                    return;
                }
                // Positively absent original dispatch: reuse its exact prepared input/identity.
                const auto prepared = *chat.turn.prepared;
                chat.reconciled = true;
                chat.turn.terminal = chat.turn.stopped = false;
                chat.turn.retryable = false;
                dispatchStart(prepared, chat.turn.submission, chat.rows[chat.turn.firstRow].key);
                return;
            }
            const auto &saved = std::get<ExistingSession>(*recovery);
            if (saved.sessionVersion.isEmpty() || saved.revision < chat.sequence ||
                (chat.version && *chat.version != saved.sessionVersion) || !saved.turn ||
                saved.turn->clientTurnId != client || saved.turn->turnId.isEmpty() ||
                (!chat.turn.remoteId.isEmpty() && chat.turn.remoteId != saved.turn->turnId)) {
                problem({"invalid_recovery",
                         "Cannot reconcile this turn. Nothing was resent.",
                         {},
                         {},
                         false,
                         {}});
                return;
            }
            // Validate the entire snapshot before changing any known display state.
            Sequence sequence = -1;
            for (const auto &event : saved.turn->events) {
                const auto &identity = event.identity;
                if (identity.sessionId != id || identity.seq <= sequence ||
                    identity.seq > saved.revision ||
                    (identity.turnId && *identity.turnId != saved.turn->turnId) ||
                    (identity.clientTurnId && *identity.clientTurnId != client)) {
                    problem({"invalid_recovery",
                             "Invalid recovery event identity/order.",
                             {},
                             {},
                             false,
                             {}});
                    return;
                }
                sequence = identity.seq;
            }
            // Rebuild this turn's presentation only. Replay never charges the usage ledger.
            const auto original = chat.turn;
            chat.rows.resize(original.firstRow + 1);
            chat.rows.last().state = "done";
            chat.turn = {};
            chat.turn.clientId = client;
            chat.turn.remoteId = saved.turn->turnId;
            chat.turn.replyKey = uuid();
            chat.turn.firstRow = original.firstRow;
            chat.turn.started = original.started;
            chat.turn.prepared = original.prepared;
            chat.turn.dispatched = chat.turn.acknowledged = true; // its journal exists
            chat.version = saved.sessionVersion;
            for (const auto &event : saved.turn->events)
                applyEvent(chat, event, true);
            if (chat.turn.terminal && original.completed >= 0) {
                chat.turn.completed = original.completed;
                updateMetrics(chat, chat.turn);
            }
            chat.sequence = std::max(chat.sequence, saved.revision);
            chat.reconciled = true;
            m_status.clear();
            emit accepted(original.submission); // Clear only its still-owned composer draft.
            emit changed(); // A second explicit Retry is required for a recovered failure.
        });
        emit changed();
        return;
    }
    auto &chat = editable();
    if (!chat.version || chat.turn.remoteId.isEmpty())
        return;
    const auto failed = chat.turn.remoteId;
    chat.pastTurns.insert(failed, chat.turn);
    chat.turn = {};
    chat.turn.clientId = uuid();
    chat.turn.replyKey = uuid();
    chat.turn.firstRow = chat.rows.size();
    // A retry has no input: its open (hidden) reply part holds the pending marker.
    DisplayRow part{
        DisplayRow::Role::Assistant, chat.turn.replyKey, {}, "pending", {}, {}, {}, -1, -1};
    part.backendTurn = chat.turn.clientId;
    part.pendingTurn = part.hidden = true;
    chat.rows.append(part);
    m_status.clear();
    if (!checkpoint(chat)) {
        chat.rows.removeLast();
        chat.turn = chat.pastTurns.take(failed);
        problem({QStringLiteral("checkpoint_failed"),
                 QStringLiteral("Cannot save the session recovery checkpoint. Nothing was sent."),
                 {},
                 {},
                 true,
                 {}});
        return;
    }
    chat.turn.dispatched = true;
    m_pending = true;
    const auto request =
        call(RetryTurn{id, *chat.version, chat.turn.clientId, failed, params(chat)},
             [this, id](const Result &result) {
                 m_pending = false;
                 auto &chat = m_chats[id];
                 const auto *ack = value<RetryAccepted>(result);
                 if (!ack || ack->turnId.isEmpty() ||
                     (!chat.turn.remoteId.isEmpty() && chat.turn.remoteId != ack->turnId)) {
                     chat.turn.terminal = true;
                     chat.turn.retryable = false; // No safe retry-of-retry inference.
                     chat.reconciled = false;
                     problem(errorOf(result));
                     return;
                 }
                 if (ack->selection) {
                     chat.selection = *ack->selection;
                     if (chat.parent.isEmpty())
                         m_library->update(chat.id, [&](Library::Chat &c) {
                             c.model = modelObject(chat.selection);
                         });
                 }
                 chat.turn.remoteId = ack->turnId;
                 chat.turn.acknowledged = true;
                 if (chat.turn.started < 0)
                     chat.turn.started = QDateTime::currentMSecsSinceEpoch();
                 drainEarly(chat);
                 if (chat.turn.terminal && !chat.turn.stopped)
                     settlePending(chat, chat.turn.clientId);
                 save(chat);
                 emit changed();
             });
    m_chats[id].turn.request = request;
    emit changed();
}
void ChatService::remove(const QString &id)
{
    if (!m_ready || pending() || !m_chats.contains(id)) {
        emit removed(id, false);
        return;
    }
    const QString miniId = id + QStringLiteral(":mini");
    // Delete is idempotent, including a mini session never opened. Refuse an ID
    // collision rather than erase another chat's session.
    if (!m_chats[id].parent.isEmpty() || m_library->chat(miniId) ||
        (id.endsWith(QStringLiteral(":mini")) && m_library->chat(id.left(id.size() - 5)))) {
        problem({QStringLiteral("invalid_request"),
                 QStringLiteral("Cannot delete: the mini session ID belongs to another chat."),
                 {},
                 {},
                 false,
                 {}});
        emit removed(id, false);
        return;
    }
    if (m_mini == miniId)
        closeMini();
    for (const auto &session : {id, miniId})
        if (auto it = m_chats.find(session);
            it != m_chats.end() && it->turn.remoteId.size() && !it->turn.terminal) {
            it->turn.stopped = true;
            endTurn(*it, {TurnStatus::Cancelled, {}, {}}, false, true);
        }
    m_pending = true;
    deleteNext({id, miniId}, id, [this, id, miniId](bool ok, const Error &error) {
        m_pending = false;
        if (!ok) {
            problem(error);
            emit removed(id, false);
            return;
        }
        dismissApprovals(id, QStringLiteral("cancelled"));
        dismissApprovals(miniId, QStringLiteral("cancelled"));
        m_chats.remove(id);
        m_chats.remove(miniId);
        m_library->remove(id);  // index first, then both display caches
        emit removed(id, true); // Forget the composer's cached draft before switching.
        if (id == m_current)
            newChat();
        else
            emit changed();
    });
    emit changed();
}
void ChatService::deleteNext(QStringList sessions, const QString &id,
                             std::function<void(bool, Error)> done)
{
    if (sessions.isEmpty()) {
        done(true, {});
        return;
    }
    const auto session = sessions.takeFirst();
    call(DeleteSession{session}, [this, sessions, id, done](const Result &result) {
        if (!value<Null>(result)) {
            done(false, errorOf(result));
            return;
        }
        deleteNext(sessions, id, done);
    });
}
void ChatService::refresh()
{
    if (!m_ready)
        return;
    if (m_refreshing) {
        m_refreshAgain = true;
        return;
    }
    m_refreshing = true;
    call(ModelsList{}, [this](const Result &result) {
        if (!m_ready) {
            m_refreshing = false;
            return;
        }
        if (const auto *models = value<QVector<Model>>(result))
            m_models = *models;
        else
            problem(errorOf(result));
        call(ProvidersList{}, [this](const Result &result) {
            m_refreshing = false;
            if (!m_ready)
                return;
            if (const auto *providers = value<QVector<Provider>>(result)) {
                m_providers = *providers;
                for (auto &provider : m_providers)
                    if (m_authErrors.contains(provider.id))
                        provider.status.error = m_authErrors[provider.id];
            } else
                problem(errorOf(result));
            emit catalogChanged();
            emit changed();
            if (m_refreshAgain) {
                m_refreshAgain = false;
                refresh();
            }
        });
    });
}
void ChatService::authenticate(const Command &command, const QString &flow)
{
    if (!m_ready) {
        emit authFinished(flow);
        return;
    }
    QString provider;
    std::visit(
        [&](const auto &auth) {
            using T = std::decay_t<decltype(auth)>;
            if constexpr (std::is_same_v<T, SetKey> || std::is_same_v<T, Login> ||
                          std::is_same_v<T, CancelLogin> || std::is_same_v<T, Logout>)
                provider = auth.provider;
        },
        command);
    if (provider.isEmpty()) {
        emit authFinished(flow);
        return;
    }
    const auto attempt = ++m_authAttempts;
    m_authAttempt.insert(provider, attempt);
    call(command, [this, provider, flow, attempt](const Result &result) {
        if (m_authAttempt.value(provider) == attempt) {
            if (const auto *error = std::get_if<Error>(&result)) {
                m_authErrors.insert(provider, *error);
                problem(*error);
            } else {
                m_authErrors.remove(provider);
                m_status.clear();
            }
        }
        refresh();
        emit authFinished(flow);
    });
}
void ChatService::answerLogin(const AnswerLogin &answer)
{
    if (!m_ready || answer.provider.isEmpty())
        return;
    // The answer belongs to the attempt it answered: a failure after a newer one
    // began (a new sign-in, a cancel or a logout) is that attempt's no longer.
    const auto attempt = m_authAttempt.value(answer.provider);
    call(answer, [this, provider = answer.provider, attempt](const Result &result) {
        if (const auto *error = std::get_if<Error>(&result)) {
            if (m_authAttempt.value(provider) == attempt) {
                m_authErrors.insert(provider, *error);
                problem(*error);
            }
            refresh();
        }
    });
}
void ChatService::reverse(RequestId id, const ReverseRequest &request)
{
    const auto *approval = std::get_if<ApprovalRequest>(&request);
    if (const auto *tool = std::get_if<HostToolRequest>(&request)) {
        const auto tools = m_host->tools();
        if (std::none_of(tools.cbegin(), tools.cend(),
                         [&](const auto &t) { return t.name == tool->name; })) {
            m_backend->answer(id, Error{"unsupported",
                                        "Native browser/media host tools are not available.",
                                        {},
                                        {},
                                        false,
                                        {}});
            return;
        }
        auto chat = m_chats.find(tool->sessionId);
        if (chat == m_chats.end() || chat->turn.terminal || chat->turn.stopped ||
            chat->turn.remoteId.isEmpty() || tool->turnId != chat->turn.remoteId ||
            tool->toolCallId.isEmpty() || chat->turn.hostSeen.contains(tool->toolCallId) ||
            m_hostRequests.contains(id)) {
            m_backend->answer(id, Error{"stale_turn",
                                        "Host tool call does not name a new step of this turn.",
                                        {},
                                        {},
                                        false,
                                        {}});
            return;
        }
        chat->turn.hostSeen.insert(tool->toolCallId);
        const auto browser = m_host->browser();
        const bool messageWaiting =
            std::any_of(chat->rows.cbegin(), chat->rows.cend(), [&](const auto &row) {
                return row.role == DisplayRow::Role::User && !row.clientInputId.isEmpty() &&
                       row.backendTurn == chat->turn.clientId &&
                       (row.state == "sending" || row.state == "queued");
            });
        if (browser && browser->control == BrowserState::Control::User && messageWaiting) {
            HostToolResult result;
            result.status = HostToolResult::Status::Cancelled;
            result.reason = "message";
            m_backend->answer(id, result);
            return;
        }
        chat->turn.hostCalls.insert(tool->toolCallId, id);
        m_hostRequests.insert(id, {tool->sessionId, tool->toolCallId});
        m_host->run(id, *tool);
        return;
    }
    if (!approval)
        return;
    auto chat = m_chats.find(approval->sessionId);
    if (chat == m_chats.end() || chat->turn.terminal || chat->turn.stopped) {
        m_backend->answer(id, ApprovalAnswer{Decision::Deny, QStringLiteral("cancelled")});
        return;
    }
    if (chat->turn.remoteId.isEmpty()) {
        if (m_earlyApprovals.size() < 256)
            m_earlyApprovals.append({id, *approval});
        else
            m_backend->answer(id,
                              Error{"stale_turn", "Too many early approvals.", {}, {}, false, {}});
        return;
    }
    const auto key =
        approval->sessionId + QChar(0) + approval->turnId + QChar(0) + approval->approvalId;
    if (approval->turnId != chat->turn.remoteId || approval->approvalId.isEmpty() ||
        m_reverseSeen.contains(key)) {
        m_backend->answer(id, Error{"stale_turn",
                                    "Approval does not name a new step of this turn.",
                                    {},
                                    {},
                                    false,
                                    {}});
        return;
    }
    m_reverseSeen.insert(key);
    m_approvals.append({id, *approval});
    emit changed();
}
void ChatService::approve(RequestId id, bool allow)
{
    const auto it = std::find_if(m_approvals.begin(), m_approvals.end(),
                                 [id](const auto &p) { return p.request == id; });
    if (it == m_approvals.end() || it->data.sessionId != m_current)
        return;
    m_approvals.erase(it); // Exactly once, even if answer() immediately emits events.
    m_backend->answer(id, ApprovalAnswer{allow ? Decision::Allow : Decision::Deny, {}});
    emit changed();
}
void ChatService::decide(RequestId id, const QString &action, const QString &note,
                         const QString &scope)
{
    const auto it = std::find_if(m_approvals.begin(), m_approvals.end(),
                                 [id](const auto &p) { return p.request == id; });
    if (it == m_approvals.end() || it->data.sessionId != m_current)
        return;
    const auto &offered = it->data.actions;
    if (std::none_of(offered.cbegin(), offered.cend(),
                     [&](const ApprovalAction &a) { return a.id == action; }))
        return;
    const bool allow = action.startsWith(QStringLiteral("approve"));
    ApprovalAnswer answer{allow ? Decision::Allow : Decision::Deny, {}, action, {}, {}};
    if (action == QStringLiteral("denyWithReason") && !note.trimmed().isEmpty())
        answer.note = note.trimmed();
    if (allow && it->data.scopes &&
        (scope == QStringLiteral("subagent") || scope == QStringLiteral("session")))
        answer.scope = scope;
    m_approvals.erase(it); // Exactly once, even if answer() immediately emits events.
    m_backend->answer(id, answer);
    emit changed();
}
void ChatService::dismissApprovals(const QString &session, const QString &reason)
{
    QVector<RequestId> ids;
    for (const auto &p : m_approvals)
        if (p.data.sessionId == session)
            ids.append(p.request);
    for (const auto &p : m_earlyApprovals)
        if (p.data.sessionId == session)
            ids.append(p.request);
    m_earlyApprovals.removeIf([&](const auto &p) { return p.data.sessionId == session; });
    m_approvals.removeIf([&](const auto &p) { return p.data.sessionId == session; });
    for (const auto id : ids)
        m_backend->answer(id, ApprovalAnswer{Decision::Deny, reason});
}
void ChatService::problem(const Error &error)
{
    m_status = error.message;
    emit changed();
}

// Library index state: pins, folders and their collapse. Display annotations.
bool ChatService::setPinned(const QString &id, bool pinned)
{
    auto it = m_chats.find(id);
    if (it == m_chats.end() || !it->parent.isEmpty() || !m_library->setPinned(id, pinned))
        return false;
    it->pinned = pinned;
    emit changed();
    return true;
}
bool ChatService::toggleFolder(const std::optional<QString> &folder)
{
    const bool ok = m_library->toggleFolder(folder);
    emit changed();
    return ok;
}
QString ChatService::addFolder(const QString &path, const QString &name)
{
    // The folder picker is a host dialog; this records what it returned.
    if (path.isEmpty() ||
        (!m_library->homePath().isEmpty() && Library::samePath(path, m_library->homePath())))
        return QStringLiteral("This folder cannot be added.");
    if (!m_library->addFolder(path, name) || !m_library->persist())
        return QStringLiteral("The folder could not be saved.");
    emit changed();
    return {};
}
void ChatService::removeFolder(const QString &folder)
{
    if (!m_ready || pending() || folder.isEmpty() || !m_library->folder(folder)) {
        emit folderRemoved(folder, false, m_ready ? QString() : m_status);
        return;
    }
    const auto ids = m_library->inFolder(folder);
    if (ids.isEmpty()) {
        m_library->removeFolder(folder);
        if (m_current.isEmpty() && Library::samePath(m_draft.folder, folder))
            m_draft.folder.clear();
        emit folderRemoved(folder, true, {});
        emit changed();
        return;
    }
    // Each acknowledged deletion is committed alone: a failed child and every
    // unattempted one stay available to retry.
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(this, &ChatService::removed, this, [this, folder, conn](QString, bool ok) {
        disconnect(*conn);
        if (!ok) {
            emit folderRemoved(folder, false, m_status);
            return;
        }
        QTimer::singleShot(0, this, [this, folder] { removeFolder(folder); });
    });
    remove(ids.first());
}

// Locks: frontend view protection. A locked chat's messages leave memory as
// soon as no reply is being written into them.
QString ChatService::protect(const QString &id, const QString &password)
{
    auto it = m_chats.find(id);
    if (it == m_chats.end() || !it->parent.isEmpty() || it->locked ||
        (!it->turn.clientId.isEmpty() && !it->turn.terminal))
        return QStringLiteral("This chat cannot be protected now.");
    std::optional<Library::Body> loaded;
    if (it->loaded)
        loaded = Library::Body{entries(it->rows), it->tokens, 0};
    const auto error = m_library->protect(id, password, loaded);
    if (!error.isEmpty())
        return error;
    lock(id);
    return {};
}
void ChatService::lock(const QString &id)
{
    auto it = m_chats.find(id);
    if (it == m_chats.end() || !m_library->isProtected(id))
        return;
    m_library->relock(id);
    it->locked = true;
    it->title = Locked;
    if (it->turn.clientId.isEmpty() || it->turn.terminal) {
        it->rows.clear();
        it->loaded = false;
    }
    emit changed();
}
QString ChatService::unlock(const QString &id, const QString &password)
{
    auto it = m_chats.find(id);
    if (it == m_chats.end() || !it->locked)
        return {};
    const auto error = m_library->unlock(id, password);
    if (!error.isEmpty())
        return error;
    if (!it->loaded && !load(*it)) {
        // Messages that would not open never pass for an empty chat.
        m_library->relock(id);
        it->locked = true;
        return m_status;
    }
    it->locked = false;
    it->title = m_library->titleOf(id).value_or(Locked);
    if (id == m_current) {
        m_status.clear();
        emit replaced(id);
        if (m_ready && it->restored && !it->reconciled)
            reconcile(id);
    }
    emit changed();
    return {};
}
QString ChatService::unprotect(const QString &id)
{
    auto it = m_chats.find(id);
    if (it == m_chats.end() || it->locked)
        return QStringLiteral("Open the chat with its password first.");
    std::optional<Library::Body> loaded;
    if (it->loaded)
        loaded = Library::Body{entries(it->rows), it->tokens, 0};
    const auto error = m_library->unprotect(id, loaded);
    if (error.isEmpty()) {
        it->title = m_library->titleOf(id).value_or(it->title);
        emit changed();
    }
    return error;
}

// The mini chat (SideChat): session `<id>:mini`, its own cache beside the chat's.
struct ChatService::Focus {
    ChatService &service;
    QString saved;
    QSignalBlocker quiet; // The main view never renders the mini chat's rows.
    explicit Focus(ChatService &s) : service(s), saved(s.m_current), quiet(&s)
    {
        s.m_current = s.m_mini;
    }
    ~Focus()
    {
        service.m_current = saved;
        quiet.unblock();
        emit service.changed();
    }
};
const ChatRecord *ChatService::mini() const
{
    const auto it = m_chats.constFind(m_mini);
    return m_mini.isEmpty() || it == m_chats.cend() ? nullptr : &*it;
}
bool ChatService::miniBehind() const
{
    const auto *side = mini();
    if (!side)
        return false;
    const bool asked = std::any_of(side->rows.cbegin(), side->rows.cend(),
                                   [](const auto &r) { return r.role == DisplayRow::Role::User; });
    const auto parent = m_chats.constFind(side->parent);
    return asked && parent != m_chats.cend() && parent->updated > side->seen;
}
QString ChatService::openMini()
{
    const auto it = m_chats.constFind(m_current);
    if (m_current.isEmpty() || it == m_chats.cend() || !it->parent.isEmpty())
        return QStringLiteral("Send a message first; the mini chat belongs to a saved chat.");
    if (it->locked || m_library->isLocked(m_current))
        return QStringLiteral("This chat is locked.");
    const auto id = m_current + QStringLiteral(":mini");
    if (m_mini == id)
        return {};
    closeMini();
    if (!m_chats.contains(id)) {
        ChatRecord side;
        side.id = id;
        side.parent = m_current;
        side.title = it->title;
        side.folder = it->folder;
        side.selection = it->selection;
        side.mode = it->mode;
        side.restored = true;
        side.loaded = false;
        side.reconciled = false;
        if (!load(side))
            return m_status;
        m_chats.insert(id, side);
    }
    m_mini = id;
    if (m_ready && m_chats[id].restored && !m_chats[id].reconciled)
        reconcile(id);
    emit changed();
    return {};
}
void ChatService::closeMini()
{
    if (m_mini.isEmpty())
        return;
    {
        Focus focus(*this);
        stop(); // Closing stops its reply and keeps its messages with the chat.
    }
    m_miniMoved = false;
    m_mini.clear();
    emit changed();
}
quint64 ChatService::sendMini(const QString &text, const QVector<Attachment> &attachments)
{
    if (!mini())
        return 0;
    const bool behind = miniBehind();
    const bool running = !mini()->turn.clientId.isEmpty() && !mini()->turn.terminal;
    Focus focus(*this);
    auto &side = m_chats[m_mini];
    if (running)
        return send(text, attachments); // steering
    const auto seen = side.seen;
    if (behind)
        side.rows.append({DisplayRow::Role::Moved,
                          uuid(),
                          QStringLiteral("Caught up with the main chat"),
                          "done",
                          {},
                          {},
                          {},
                          -1,
                          -1});
    side.seen = m_chats[side.parent].updated;
    m_miniMoved = behind;
    const auto submission = send(text, attachments);
    if (!submission) {
        auto &back = m_chats[m_mini];
        if (behind && !back.rows.isEmpty() && back.rows.last().role == DisplayRow::Role::Moved)
            back.rows.removeLast();
        back.seen = seen;
        m_miniMoved = false;
    }
    return submission;
}
void ChatService::stopMini()
{
    if (!mini())
        return;
    Focus focus(*this);
    stop();
}
void ChatService::retryMini()
{
    if (!mini())
        return;
    Focus focus(*this);
    retry();
}
void ChatService::approveMini(RequestId id, bool allow)
{
    if (!mini())
        return;
    Focus focus(*this);
    approve(id, allow);
}
void ChatService::clearMini()
{
    if (!mini() || pending()) {
        emit miniCleared(false);
        return;
    }
    const auto id = m_mini;
    {
        Focus focus(*this);
        stop();
    }
    m_pending = true;
    // Delete only the mini session, then reset its view and cache; a failed
    // delete keeps it intact.
    call(DeleteSession{id}, [this, id](const Result &result) {
        m_pending = false;
        auto it = m_chats.find(id);
        if (!value<Null>(result) || it == m_chats.end()) {
            problem(errorOf(result));
            emit miniCleared(false);
            return;
        }
        dismissApprovals(id, QStringLiteral("cancelled"));
        m_library->clearSide(it->parent);
        it->rows.clear();
        it->tokens = 0;
        it->seen = 0;
        it->turn = {};
        it->pastTurns.clear();
        it->version.reset(); // The next turn explicitly creates a new incarnation.
        it->sequence = -1;
        it->reconciled = true;
        it->restored = false;
        emit miniCleared(true);
        emit changed();
    });
    emit changed();
}
} // namespace openghost
