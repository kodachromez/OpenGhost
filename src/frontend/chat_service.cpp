#include "chat_service.h"
#include <QDateTime>
#include <QUuid>
#include <algorithm>

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
} // namespace
ChatService::ChatService(Backend *backend, PreferencesStore *preferences, QObject *parent)
    : QObject(parent), m_backend(backend), m_preferences(preferences)
{
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
            for (auto &chat : m_chats) {
                chat.reconciled = false;
                chat.turn.terminal = true;
                finishRows(chat, QStringLiteral("unconfirmed"));
            }
            auto calls = std::move(m_calls);
            m_calls.clear();
            for (auto &complete : calls)
                complete(Result{error});
            m_pending = false;
            problem(error);
        });
        // No approval or browser UI is claimed by this slice. Never leave a
        // reverse call hanging or synthesize consent.
        connect(m_backend, &Backend::reverseRequest, this,
                [this](RequestId id, const ReverseRequest &) {
                    m_backend->answer(
                        id, Error{QStringLiteral("unsupported"),
                                  QStringLiteral("Native reverse requests are not connected yet."),
                                  {},
                                  {},
                                  false,
                                  {}});
                });
    }
    newChat();
}
RequestId ChatService::call(const Command &command, Completion completion)
{
    const auto id = ++m_next;
    m_calls.insert(id, std::move(completion));
    m_backend->request(id, command);
    return id;
}
void ChatService::initialize()
{
    if (!m_backend || m_ready || m_pending)
        return;
    m_pending = true;
    Initialize hello;
    hello.connectionId = uuid();
    hello.client = {QStringLiteral("OpenGhost"), QStringLiteral("1.3.0"), {}, {}};
    call(hello, [this](const Result &result) {
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
                m_status = name + QStringLiteral(" — native preview; chats are in memory only.");
                m_draft.selection = preferredModel();
                emit catalogChanged();
                emit changed();
            });
        });
    });
}
QVector<ChatRecord> ChatService::chats() const
{
    QVector<ChatRecord> result;
    for (const auto &chat : m_chats)
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
void ChatService::newChat()
{
    if (m_pending)
        return;
    const auto left = m_current;
    m_current.clear();
    m_draft = {};
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
    if (m_pending || !m_chats.contains(id))
        return;
    const auto left = m_current;
    m_current = id;
    emit changed();
    emit replaced(left);
    if (!m_ready)
        return;
    if (!m_chats[id].reconciled) {
        problem({QStringLiteral("invalid_recovery"),
                 QStringLiteral("Uncertain chat kept for display. Full recovery is not connected "
                                "yet; start a new chat."),
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
quint64 ChatService::send(const QString &text)
{
    if (!ready() || m_pending || busy() || text.trimmed().isEmpty() ||
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
    if (m_current.isEmpty()) {
        ChatRecord chat = m_draft;
        chat.id = uuid();
        chat.title = titleFrom(text);
        chat.created = chat.updated = QDateTime::currentMSecsSinceEpoch();
        m_current = chat.id;
        m_chats.insert(chat.id, chat);
    }
    auto &chat = editable();
    chat.turn = {};
    chat.turn.clientId = uuid();
    chat.turn.replyKey = uuid();
    chat.updated = QDateTime::currentMSecsSinceEpoch();
    const QString userKey = uuid();
    chat.rows.append({DisplayRow::Role::User, userKey, text, QStringLiteral("sending")});
    StartTurn start;
    start.sessionId = chat.id;
    start.sessionVersion = chat.version;
    start.clientTurnId = chat.turn.clientId;
    start.input.text = text;
    start.params.selection = chat.selection;
    start.params.permissionMode = chat.mode;
    start.params.title = chat.title;
    start.params.userContext = m_preferences->value().userContext;
    m_pending = true;
    const auto submission = ++m_submission;
    call(start, [this, start, submission, userKey](const Result &result) {
        auto &chat = m_chats[start.sessionId];
        const auto *ack = value<StartAccepted>(result);
        m_pending = false;
        if (!ack || ack->turnId.isEmpty() || ack->sessionVersion.isEmpty() ||
            (start.sessionVersion && *start.sessionVersion != ack->sessionVersion) ||
            (!chat.turn.remoteId.isEmpty() && chat.turn.remoteId != ack->turnId)) {
            chat.reconciled = false;
            chat.turn.terminal = true;
            finishRows(chat, QStringLiteral("unconfirmed"));
            for (auto &row : chat.rows)
                if (row.key == userKey)
                    row.state = QStringLiteral("unconfirmed");
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
        chat.turn.remoteId = ack->turnId;
        for (auto &row : chat.rows)
            if (row.key == userKey)
                row.state = QStringLiteral("done");
        emit accepted(submission);
        emit changed();
    });
    emit changed();
    return submission;
}
void ChatService::finishRows(ChatRecord &chat, const QString &state)
{
    for (auto &row : chat.rows)
        if (row.state == QStringLiteral("live") || row.state == QStringLiteral("sending"))
            row.state = state;
}
void ChatService::publishReply(ChatRecord &chat)
{
    QStringList parts;
    for (const auto &message : chat.turn.messages)
        if (!message.text.trimmed().isEmpty())
            parts.append(message.text.trimmed());
    for (auto &row : chat.rows)
        if (row.key == chat.turn.replyKey)
            row.text = parts.join(QStringLiteral("\n\n"));
}
void ChatService::event(const SessionEvent &event)
{
    const auto &id = event.identity;
    auto it = m_chats.find(id.sessionId);
    if (it == m_chats.end() || id.seq < 0 || id.seq <= it->sequence)
        return;
    auto &chat = it.value();
    chat.sequence = id.seq;
    if (const auto *update = std::get_if<SessionUpdated>(&event.payload)) {
        if (update->title && !chat.renamed && !update->title->simplified().isEmpty())
            chat.title = update->title->simplified().left(60);
        emit changed();
        return;
    }
    auto &turn = chat.turn;
    if (turn.clientId.isEmpty() || turn.terminal || turn.stopped)
        return;
    if (id.clientTurnId && *id.clientTurnId != turn.clientId)
        return;
    if (std::holds_alternative<TurnStarted>(event.payload)) {
        if (id.clientTurnId == std::optional<QString>(turn.clientId) && id.turnId &&
            !id.turnId->isEmpty() && (turn.remoteId.isEmpty() || turn.remoteId == *id.turnId))
            turn.remoteId = *id.turnId;
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
        if (turn.messages.isEmpty())
            chat.rows.append(
                {DisplayRow::Role::Assistant, turn.replyKey, {}, QStringLiteral("live")});
        turn.messages.append({*id.messageId, {}, false});
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
        publishReply(chat);
    } else if (const auto *done = std::get_if<TurnCompleted>(&event.payload)) {
        turn.terminal = true;
        finishRows(chat, done->status == TurnStatus::Cancelled ? QStringLiteral("cancelled")
                                                               : QStringLiteral("done"));
        if (done->error)
            chat.rows.append({DisplayRow::Role::Note, uuid(), done->error->message, {}});
    } else if (std::holds_alternative<ToolStarted>(event.payload)) {
        emit worked(); // No tool card, reasoning or progress text.
    }
    emit changed();
}
void ChatService::stop()
{
    if (!canCancel())
        return;
    auto &chat = editable();
    chat.turn.stopped = chat.turn.terminal = true; // freeze before any request/response
    finishRows(chat, QStringLiteral("cancelled"));
    const auto id = chat.id;
    if (!chat.turn.remoteId.isEmpty()) {
        m_pending = true;
        call(CancelTurn{id, chat.turn.remoteId}, [this, id](const Result &result) {
            m_pending = false;
            if (!value<Null>(result)) {
                m_chats[id].reconciled = false;
                problem(errorOf(result));
            } else
                emit changed();
        });
    } else {
        chat.reconciled = false; // unknown acceptance; never permit automatic resend
        const auto pending = m_calls.keys();
        for (const auto request : pending)
            m_backend->cancelRequest(request);
    }
    emit changed();
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
    if (m_pending || busy()) {
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
        auto preference = chosen;
        preference.thinking = selection.thinking; // never persist canonical effort defaults
        chooseSaved(preference, thinkingPreference);
        emit changed();
    });
    emit changed();
}
void ChatService::setMode(PermissionMode mode)
{
    if (m_pending)
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
    it->title = title.simplified().left(60);
    it->renamed = true; // local annotation only, never an invented rename RPC
    emit changed();
}
void ChatService::problem(const Error &error)
{
    m_status = error.message;
    emit changed();
}
} // namespace openghost
