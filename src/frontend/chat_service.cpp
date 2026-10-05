#include "chat_service.h"
#include <QDateTime>
#include <QUuid>
#include <algorithm>
#include <cmath>

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
            m_approvals.clear();
            m_earlyApprovals.clear();
            m_cancelling = false;
            auto calls = std::move(m_calls);
            m_calls.clear();
            for (auto &complete : calls)
                complete(Result{error});
            m_pending = false;
            problem(error);
        });
        connect(m_backend, &Backend::reverseRequest, this, &ChatService::reverse);
        connect(m_backend, &Backend::reverseCancelled, this, [this](RequestId id) {
            m_approvals.removeIf([id](const auto &p) { return p.request == id; });
            m_earlyApprovals.removeIf([id](const auto &p) { return p.request == id; });
            emit changed();
        });
        connect(m_backend, &Backend::globalEvent, this, [this](const GlobalEvent &event) {
            if (!std::holds_alternative<Log>(event))
                refresh();
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
    if (!m_backend || m_ready || pending())
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
                Q_UNUSED(name);
                m_status.clear();
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
    if (pending())
        return;
    const auto left = m_current;
    m_current.clear();
    if (m_ready)
        m_status.clear();
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
    if (pending() || !m_chats.contains(id))
        return;
    const auto left = m_current;
    m_current = id;
    if (m_ready)
        m_status.clear();
    emit changed();
    emit replaced(left);
    if (!m_ready)
        return;
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
    if (m_current.isEmpty()) {
        ChatRecord chat = m_draft;
        chat.id = uuid();
        chat.title = titleFrom(text);
        chat.created = chat.updated = QDateTime::currentMSecsSinceEpoch();
        m_current = chat.id;
        m_chats.insert(chat.id, chat);
    }
    auto &chat = editable();
    if (!chat.turn.remoteId.isEmpty())
        chat.pastTurns.insert(chat.turn.remoteId, chat.turn);
    chat.turn = {};
    chat.turn.firstRow = chat.rows.size();
    chat.turn.clientId = uuid();
    chat.turn.replyKey = uuid();
    chat.updated = QDateTime::currentMSecsSinceEpoch();
    const QString userKey = uuid();
    DisplayRow user{
        DisplayRow::Role::User, userKey, text, QStringLiteral("sending"), {}, {}, {}, -1, -1};
    for (const auto &a : attachments)
        user.attachments.append({a.name,
                                 a.size,
                                 a.kind == Attachment::Kind::Image,
                                 {},
                                 a.note,
                                 a.width,
                                 a.height,
                                 {},
                                 {}});
    chat.rows.append(user);
    StartTurn start;
    start.sessionId = chat.id;
    start.sessionVersion = chat.version;
    start.clientTurnId = chat.turn.clientId;
    start.input = {text, attachments};
    start.params = params(chat);
    chat.turn.prepared = start;
    const auto submission = ++m_submission;
    chat.turn.submission = submission;
    dispatchStart(start, submission, userKey);
    emit changed();
    return submission;
}
SessionParams ChatService::params(const ChatRecord &chat) const
{
    SessionParams p;
    p.selection = chat.selection;
    p.permissionMode = chat.mode;
    p.title = chat.title;
    p.userContext = m_preferences->value().userContext;
    return p;
}
void ChatService::dispatchStart(const StartTurn &start, quint64 submission, const QString &userKey)
{
    m_status.clear();
    m_pending = true;
    call(start, [this, start, submission, userKey](const Result &result) {
        auto &chat = m_chats[start.sessionId];
        const auto *ack = value<StartAccepted>(result);
        m_pending = false;
        if (!ack || ack->turnId.isEmpty() || ack->sessionVersion.isEmpty() ||
            (start.sessionVersion && *start.sessionVersion != ack->sessionVersion) ||
            (!chat.turn.remoteId.isEmpty() && chat.turn.remoteId != ack->turnId)) {
            chat.reconciled = false;
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
        chat.turn.remoteId = ack->turnId;
        if (chat.turn.started < 0)
            chat.turn.started = QDateTime::currentMSecsSinceEpoch();
        drainEarly(chat);
        if (chat.turn.stopped && !m_cancelling)
            cancelRemote(chat);
        for (auto &row : chat.rows)
            if (row.key == userKey)
                row.state = QStringLiteral("done");
        emit accepted(submission);
        emit changed();
    });
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
        if (owner == &turn && !turn.terminal && usage->context)
            turn.context = usage->context;
        updateMetrics(chat, *owner);
        if (!replay)
            emit usageRecorded(*usage);
        emit changed();
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
            if (turn.started < 0)
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
        if (std::none_of(chat.rows.cbegin(), chat.rows.cend(),
                         [&](const auto &r) { return r.key == turn.replyKey; }))
            chat.rows.append({DisplayRow::Role::Assistant,
                              turn.replyKey,
                              {},
                              QStringLiteral("live"),
                              {},
                              {},
                              {},
                              -1,
                              -1});
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
    } else if (const auto *done = std::get_if<TurnCompleted>(&event.payload)) {
        endTurn(chat, *done, replay);
    } else if (const auto *tool = std::get_if<ToolStarted>(&event.payload)) {
        if (tool->toolCallId.isEmpty() || turn.tools.contains(tool->toolCallId))
            return;
        turn.tools.insert(tool->toolCallId, {tool->name, "started", {}, {}});
        turn.activity = tool->title.value_or(tool->name);
        emit worked();
    } else if (const auto *progress = std::get_if<ToolProgress>(&event.payload)) {
        auto tool = turn.tools.find(progress->toolCallId);
        if (tool != turn.tools.end() && tool->state == "started")
            tool->progress = progress->detail;
    } else if (const auto *done = std::get_if<ToolCompleted>(&event.payload)) {
        auto tool = turn.tools.find(done->toolCallId);
        if (tool != turn.tools.end() && tool->state == "started") {
            tool->state = "completed";
            tool->result = done->detail;
            turn.activity.clear();
        }
    } else if (const auto *accepted = std::get_if<InputAccepted>(&event.payload)) {
        if (replay && !turn.steering.contains(accepted->clientInputId) && accepted->input) {
            const auto key = uuid();
            chat.rows.append({DisplayRow::Role::User,
                              key,
                              accepted->input->text,
                              "queued",
                              accepted->input->attachments,
                              {},
                              {},
                              -1,
                              -1});
            turn.steering.insert(accepted->clientInputId, key);
        }
        const auto key = turn.steering.value(accepted->clientInputId);
        for (auto &row : chat.rows)
            if (!key.isEmpty() && row.key == key && row.state != "applied") {
                row.state = "applied";
                turn.replyKey = uuid(); // The next assistant message is a new reply part.
            }
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
    endTurn(chat, {TurnStatus::Cancelled, {}, {}});
    if (!chat.turn.remoteId.isEmpty()) {
        cancelRemote(chat);
    } else {
        chat.turn.retryable = true;
        chat.reconciled = false; // unknown acceptance; never permit automatic resend
        const auto pending = m_calls.keys();
        for (const auto request : pending)
            m_backend->cancelRequest(request);
    }
    emit changed();
}
void ChatService::cancelRemote(ChatRecord &chat)
{
    m_cancelling = true;
    const auto id = chat.id;
    call(CancelTurn{id, chat.turn.remoteId}, [this, id](const Result &result) {
        m_cancelling = false;
        if (!value<Null>(result)) {
            m_chats[id].reconciled = false;
            m_chats[id].turn.retryable = true; // Reconciliation only, never an automatic resend.
            problem(errorOf(result));
        } else
            emit changed();
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
    for (auto it = chat.rows.rbegin(); it != chat.rows.rend(); ++it) {
        if (std::any_of(turn.messages.cbegin(), turn.messages.cend(),
                        [&](const auto &m) { return m.rowKey == it->key; })) {
            it->metrics = turn.terminal ? metrics(turn) : QString();
            it->tip = QStringLiteral("Input %1 · Output %2").arg(turn.input).arg(turn.output);
            it->started = turn.started;
            it->completed = turn.completed;
            break;
        }
    }
}
void ChatService::endTurn(ChatRecord &chat, const TurnCompleted &done, bool replay)
{
    auto &turn = chat.turn;
    turn.terminal = true;
    turn.completed = replay ? -1 : QDateTime::currentMSecsSinceEpoch();
    const auto finish = done.finishReason.value_or(turn.finishReason);
    turn.activity.clear();
    for (auto &tool : turn.tools)
        if (tool.state == "started")
            tool.state = "interrupted";
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
    else if (std::none_of(turn.messages.cbegin(), turn.messages.cend(),
                          [](const auto &m) { return !m.text.trimmed().isEmpty(); }))
        note = QStringLiteral("The model returned no text.");
    if (!note.isEmpty())
        chat.rows.append({DisplayRow::Role::Note, uuid(), note, state, {}, {}, {}, -1, -1});
    dismissApprovals(chat.id, QStringLiteral("cancelled"));
    updateMetrics(chat, turn);
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
        row.attachments.append({a.name,
                                a.size,
                                a.kind == Attachment::Kind::Image,
                                {},
                                a.note,
                                a.width,
                                a.height,
                                {},
                                {}});
    chat.rows.append(row);
    chat.turn.steering.insert(client, key);
    dismissApprovals(session, QStringLiteral("superseded"));
    const auto submission = ++m_submission;
    m_pending = true;
    call(SteerTurn{session, turnId, client, input, {}}, [this, session, key,
                                                         submission](const Result &result) {
        m_pending = false;
        auto &chat = m_chats[session];
        const auto *ack = value<SteerAccepted>(result);
        for (auto &row : chat.rows)
            if (row.key == key && row.state != "applied")
                row.state = ack ? (ack->accepted ? (chat.turn.terminal ? "unconfirmed" : "queued")
                                                 : "notApplied")
                                : "unconfirmed";
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
    m_pending = true;
    m_status.clear();
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
             chat.turn.remoteId = ack->turnId;
             if (chat.turn.started < 0)
                 chat.turn.started = QDateTime::currentMSecsSinceEpoch();
             drainEarly(chat);
             emit changed();
         });
    emit changed();
}
void ChatService::remove(const QString &id)
{
    if (!m_ready || pending() || !m_chats.contains(id)) {
        emit removed(id, false);
        return;
    }
    m_pending = true;
    call(DeleteSession{id}, [this, id](const Result &result) {
        m_pending = false;
        if (!value<Null>(result)) {
            problem(errorOf(result));
            emit removed(id, false);
            return;
        }
        dismissApprovals(id, QStringLiteral("cancelled"));
        m_chats.remove(id);
        emit removed(id, true); // Forget the composer's cached draft before switching.
        if (id == m_current)
            newChat();
        else
            emit changed();
    });
    emit changed();
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
void ChatService::authenticate(const Command &command)
{
    if (!m_ready) {
        emit authFinished();
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
        emit authFinished();
        return;
    }
    call(command, [this, provider](const Result &result) {
        if (const auto *error = std::get_if<Error>(&result)) {
            m_authErrors.insert(provider, *error);
            problem(*error);
        } else {
            m_authErrors.remove(provider);
            m_status.clear();
        }
        refresh();
        emit authFinished();
    });
}
void ChatService::reverse(RequestId id, const ReverseRequest &request)
{
    const auto *approval = std::get_if<ApprovalRequest>(&request);
    if (!approval) {
        m_backend->answer(id, Error{"unsupported",
                                    "Native browser/media host tools are not available.",
                                    {},
                                    {},
                                    false,
                                    {}});
        return;
    }
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
} // namespace openghost
