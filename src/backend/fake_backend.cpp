#include "fake_backend.h"
#include <QUuid>
#include <algorithm>

namespace openghost
{
namespace
{
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
Error failure(const QString &code, const QString &message)
{
    return {code, message, {}, QStringLiteral("none"), false, {}};
}
QVector<Model> catalog()
{
    return {{QStringLiteral("echo"),
             QStringLiteral("fake"),
             QStringLiteral("Fake Echo"),
             {},
             false,
             {},
             {}},
            {QStringLiteral("brief"),
             QStringLiteral("fake"),
             QStringLiteral("Fake Brief"),
             {},
             false,
             {QStringLiteral("low"), QStringLiteral("high")},
             QStringLiteral("low")}};
}
bool valid(const ModelSelection &selection)
{
    for (const auto &model : catalog())
        if (selection.provider == model.provider && selection.model == model.id)
            return !selection.thinking || selection.thinking->isEmpty() ||
                   model.thinkingLevels.contains(*selection.thinking);
    return false;
}
bool supported(const Input &input)
{
    if (input.attachments.size() > 20)
        return false;
    for (const auto &a : input.attachments)
        if (a.id.isEmpty() || a.name.isEmpty() || a.kind != Attachment::Kind::Text || !a.text ||
            a.text->toUtf8().size() > 256 * 1024 || a.dataUrl || a.path || a.video)
            return false;
    return true;
}
bool sameInput(const Input &a, const Input &b)
{
    if (a.text != b.text || a.attachments.size() != b.attachments.size())
        return false;
    for (qsizetype i = 0; i < a.attachments.size(); ++i) {
        const auto &x = a.attachments[i], &y = b.attachments[i];
        if (x.id != y.id || x.name != y.name || x.mime != y.mime || x.size != y.size ||
            x.kind != y.kind || x.text != y.text || x.note != y.note || x.width != y.width ||
            x.height != y.height || x.truncated != y.truncated)
            return false;
    }
    return true;
}
DisplayInput display(const Input &input)
{
    DisplayInput out{input.text, {}};
    for (const auto &a : input.attachments)
        out.attachments.append({a.name, a.size, false, {}, a.note, {}, {}, {}, {}});
    return out;
}
bool same(const StartTurn &a, const StartTurn &b)
{
    // All other input forms are refused before acceptance; compare every supported field.
    const auto &x = a.params;
    const auto &y = b.params;
    return a.sessionVersion == b.sessionVersion && sameInput(a.input, b.input) &&
           x.selection.provider == y.selection.provider && x.selection.model == y.selection.model &&
           x.selection.thinking == y.selection.thinking && x.permissionMode == y.permissionMode &&
           x.cwd == y.cwd && x.title == y.title &&
           x.userContext.instructions == y.userContext.instructions;
}
} // namespace

FakeBackend::FakeBackend(QObject *parent, int intervalMs) : Backend(parent)
{
    connect(&m_timer, &QTimer::timeout, this, &FakeBackend::advance);
    if (intervalMs > 0)
        m_timer.start(intervalMs);
}
void FakeBackend::request(RequestId id, const Command &command)
{
    Q_ASSERT(!m_pending.contains(id));
    m_pending.insert(id);
    QTimer::singleShot(0, this, [this, id, command] {
        if (!m_pending.remove(id))
            return;
        emit replied(id, execute(command));
    });
}
void FakeBackend::cancelRequest(RequestId id)
{
    if (m_pending.remove(id))
        QTimer::singleShot(0, this, [this, id] {
            emit replied(id, failure(QStringLiteral("cancelled"),
                                     QStringLiteral("Fake request cancelled.")));
        });
}
Result FakeBackend::execute(const Command &command)
{
    if (const auto *hello = std::get_if<Initialize>(&command)) {
        if (hello->protocolVersion != QStringLiteral("0.1"))
            return failure(QStringLiteral("unsupported"), QStringLiteral("Expected protocol 0.1."));
        m_initialized = true;
        Initialized result;
        result.backend =
            BackendInfo{QStringLiteral("OpenGhost fake (in-memory)"), QStringLiteral("1"), {}};
        result.capabilities.authProviders = true;
        result.capabilities.sessionDelete = true;
        result.capabilities.sessionRecovery = true; // This process only, never crash recovery.
        return Reply{result};
    }
    if (std::holds_alternative<Shutdown>(command)) {
        m_timer.stop();
        m_initialized = false;
        return Reply{Null{}};
    }
    if (!m_initialized)
        return failure(QStringLiteral("backend_unavailable"),
                       QStringLiteral("Fake backend not initialized."));
    if (std::holds_alternative<ModelsList>(command))
        return Reply{m_connected ? catalog() : QVector<Model>{}};
    if (std::holds_alternative<ProvidersList>(command)) {
        Provider provider;
        provider.id = QStringLiteral("fake");
        provider.name = QStringLiteral("Fake backend (no provider access)");
        provider.status.connected = m_connected;
        provider.status.waiting = m_waiting;
        provider.methods = {{AuthMethod::Kind::ApiKey,
                             "Fixture key",
                             QStringLiteral("Use only the word fixture; never a real key."),
                             {},
                             QStringLiteral("fixture"),
                             {}},
                            {AuthMethod::Kind::OAuth, "Simulated sign-in", {}, {}, {}, {}}};
        return Reply{QVector<Provider>{provider}};
    }
    const auto auth = [&](const QString &provider) -> Result {
        if (provider != "fake")
            return failure("unsupported", "Only the fake provider exists.");
        emit globalEvent(
            AuthChanged{provider, ProviderStatus{m_connected, {}, m_waiting, {}, {}, {}}});
        emit globalEvent(ModelsChanged{provider});
        return Reply{ProviderStatus{m_connected, {}, m_waiting, {}, {}, {}}};
    };
    if (const auto *key = std::get_if<SetKey>(&command)) {
        if (key->provider != "fake" || key->key != std::optional<QString>("fixture"))
            return failure("invalid_key",
                           "This demo accepts only the word fixture. No credential was saved.");
        m_connected = true;
        return auth(key->provider);
    }
    if (const auto *login = std::get_if<Login>(&command)) {
        if (login->provider != "fake")
            return failure("unsupported", "Only fake sign-in is available.");
        m_waiting = true;
        const auto generation = ++m_loginGeneration;
        QTimer::singleShot(250, this, [this, generation] {
            if (generation != m_loginGeneration)
                return;
            m_waiting = false;
            m_connected = true;
            emit globalEvent(AuthChanged{"fake", ProviderStatus{true, {}, false, {}, {}, {}}});
            emit globalEvent(ModelsChanged{QStringLiteral("fake")});
        });
        return auth(login->provider);
    }
    if (const auto *cancel = std::get_if<CancelLogin>(&command)) {
        if (cancel->provider != "fake")
            return failure("unsupported", "Unknown provider.");
        ++m_loginGeneration;
        m_waiting = false;
        return auth(cancel->provider);
    }
    if (const auto *logout = std::get_if<Logout>(&command)) {
        if (logout->provider != "fake")
            return failure("unsupported", "Unknown provider.");
        ++m_loginGeneration;
        m_waiting = false;
        m_connected = false;
        return auth(logout->provider);
    }
    if (const auto *retry = std::get_if<RetryTurn>(&command)) {
        auto session = m_sessions.find(retry->sessionId);
        if (session == m_sessions.end() || session->version != retry->sessionVersion ||
            retry->clientTurnId.isEmpty())
            return failure("session_conflict", "Retry requires the existing incarnation.");
        for (const auto &prior : session->turns) {
            if (prior.recovery.clientTurnId != retry->clientTurnId)
                continue;
            auto proposed = prior.start;
            proposed.params = retry->params;
            proposed.sessionVersion = retry->sessionVersion;
            if (prior.failedFrom == retry->failedTurnId && same(prior.start, proposed))
                return Reply{RetryAccepted{prior.recovery.turnId}};
            return failure("duplicate_request", "Conflicting retry identity.");
        }
        if (!session->active.isEmpty())
            return failure("busy", "A turn is running.");
        const auto old =
            std::find_if(session->turns.cbegin(), session->turns.cend(),
                         [&](const auto &t) { return t.recovery.turnId == retry->failedTurnId; });
        if (old == session->turns.cend() || !old->terminal || !old->failed)
            return failure("invalid_request", "Retry must name an exact failed turn.");
        if (!m_connected || !valid(retry->params.selection))
            return failure("model_unavailable", "Fake model unavailable.");
        Turn next;
        next.start = old->start;
        next.start.clientTurnId = retry->clientTurnId;
        next.start.sessionVersion = retry->sessionVersion;
        next.failedFrom = retry->failedTurnId;
        next.start.params = retry->params;
        next.recovery.clientTurnId = retry->clientTurnId;
        next.recovery.turnId = uuid();
        next.retried = true;
        next.messageId = uuid();
        next.response = QStringLiteral(
            "**Fake retry** completed. No input was resubmitted and no model was contacted.");
        session->active = retry->clientTurnId;
        auto &turn = session->turns.insert(retry->clientTurnId, next).value();
        publish(retry->sessionId, *session, turn, TurnStarted{});
        return Reply{RetryAccepted{turn.recovery.turnId}};
    }
    if (const auto *steer = std::get_if<SteerTurn>(&command)) {
        auto session = m_sessions.find(steer->sessionId);
        if (session == m_sessions.end() || session->active.isEmpty())
            return failure("turn_missing", "No running turn.");
        auto &turn = session->turns[session->active];
        if (steer->turnId != turn.recovery.turnId || steer->clientInputId.isEmpty())
            return failure("stale_turn", "Wrong steering identity.");
        if (!supported(steer->input) || steer->host.browser ||
            (steer->input.text.trimmed().isEmpty() && steer->input.attachments.isEmpty()))
            return failure("unsupported", "Unsupported steering input.");
        if (turn.steering.contains(steer->clientInputId)) {
            if (!sameInput(turn.steering[steer->clientInputId], steer->input))
                return failure("duplicate_request", "Conflicting steering input.");
            return Reply{SteerAccepted{true}};
        }
        turn.steering.insert(steer->clientInputId, steer->input);
        turn.queue.append(steer->clientInputId);
        return Reply{SteerAccepted{true}};
    }
    if (const auto *start = std::get_if<StartTurn>(&command)) {
        if (start->sessionId.isEmpty() || start->clientTurnId.isEmpty() ||
            (start->input.text.trimmed().isEmpty() && start->input.attachments.isEmpty()))
            return failure(QStringLiteral("invalid_request"),
                           QStringLiteral("Session, client turn and text required."));
        if (!supported(start->input) || !start->params.userContext.files.isEmpty() ||
            start->params.host.browser)
            return failure(QStringLiteral("unsupported"),
                           QStringLiteral("Fake supports prepared text attachments only; browser, "
                                          "media and pinned files remain mocked."));
        if (!m_connected || !valid(start->params.selection))
            return failure(QStringLiteral("model_unavailable"),
                           QStringLiteral("Choose a fake catalog model."));
        auto it = m_sessions.find(start->sessionId);
        if (it != m_sessions.end()) {
            const auto previous = it->turns.constFind(start->clientTurnId);
            if (previous != it->turns.cend()) {
                if (!same(previous->start, *start))
                    return failure(QStringLiteral("duplicate_request"),
                                   QStringLiteral("Conflicting client turn ID."));
                return Reply{StartAccepted{previous->recovery.turnId, it->version}};
            }
            if (!start->sessionVersion || *start->sessionVersion != it->version)
                return failure(QStringLiteral("session_conflict"),
                               QStringLiteral("Session incarnation differs."));
            if (!it->active.isEmpty())
                return failure(QStringLiteral("busy"),
                               QStringLiteral("A turn is already running."));
        } else {
            if (start->sessionVersion)
                return failure(QStringLiteral("session_missing"),
                               QStringLiteral("Session does not exist."));
            Session session;
            session.version = uuid();
            it = m_sessions.insert(start->sessionId, session);
        }
        it->params = start->params;
        Turn turn;
        turn.start = *start;
        turn.recovery.clientTurnId = start->clientTurnId;
        turn.recovery.turnId = uuid();
        turn.recovery.input = display(start->input);
        turn.messageId = uuid();
        const bool brief = start->params.selection.model == QStringLiteral("brief");
        turn.response =
            brief
                ? QStringLiteral("**Fake Brief** — received your message. No model was contacted.")
                : QStringLiteral("**Fake Echo** — this is a simulated response, not a "
                                 "model.\n\nYou said:\n\n") +
                      start->input.text;
        if (!start->input.attachments.isEmpty()) {
            turn.response += QStringLiteral("\n\nPrepared text attachments (simulated receipt):");
            for (const auto &a : start->input.attachments)
                turn.response += QStringLiteral("\n- ") + a.name;
        }
        if (start->input.text == "/fake empty")
            turn.response.clear();
        it->active = start->clientTurnId;
        auto &saved = it->turns.insert(it->active, turn).value();
        // Exercise the permitted events-before-ack order. The journal is updated first.
        publish(start->sessionId, *it, saved, TurnStarted{});
        return Reply{StartAccepted{saved.recovery.turnId, it->version}};
    }
    if (const auto *get = std::get_if<GetSession>(&command)) {
        const auto it = m_sessions.constFind(get->sessionId);
        if (it == m_sessions.cend())
            return Reply{SessionRecovery{MissingSession{}}};
        ExistingSession snapshot{it->version, it->seq, {}};
        const auto turn = it->turns.constFind(get->clientTurnId.value_or(it->active));
        if (turn != it->turns.cend())
            snapshot.turn = turn->recovery;
        return Reply{SessionRecovery{snapshot}};
    }
    if (const auto *cancel = std::get_if<CancelTurn>(&command)) {
        auto it = m_sessions.find(cancel->sessionId);
        if (it == m_sessions.end())
            return failure(QStringLiteral("session_missing"),
                           QStringLiteral("Session does not exist."));
        for (auto &turn : it->turns) {
            if (turn.recovery.turnId != cancel->turnId)
                continue;
            if (!turn.terminal) {
                turn.terminal = true;
                if (turn.approval) {
                    m_approvals.remove(turn.approval);
                    emit reverseCancelled(turn.approval);
                    turn.approval = 0;
                }
                it->active.clear();
                publish(cancel->sessionId, *it, turn, TurnCompleted{TurnStatus::Cancelled, {}, {}});
            }
            return Reply{Null{}};
        }
        return failure(QStringLiteral("turn_missing"), QStringLiteral("Turn does not exist."));
    }
    if (const auto *config = std::get_if<ConfigureSession>(&command)) {
        auto it = m_sessions.find(config->sessionId);
        if (it == m_sessions.end())
            return failure(QStringLiteral("session_missing"),
                           QStringLiteral("Session does not exist."));
        if (config->sessionVersion != it->version)
            return failure(QStringLiteral("session_conflict"),
                           QStringLiteral("Session incarnation differs."));
        if (!it->active.isEmpty() && (config->model || config->provider || config->thinking))
            return failure(QStringLiteral("busy"),
                           QStringLiteral("Stop the response before switching models."));
        auto selection = it->params.selection;
        if (config->model)
            selection.model = *config->model;
        if (config->provider)
            selection.provider = *config->provider;
        if (config->thinking)
            selection.thinking = config->thinking->isEmpty() ? std::nullopt : config->thinking;
        if (!valid(selection))
            return failure(QStringLiteral("model_unavailable"),
                           QStringLiteral("Unknown fake model or thinking level."));
        it->params.selection = selection;
        if (config->permissionMode)
            it->params.permissionMode = *config->permissionMode;
        SessionConfigured canonical;
        canonical.model = selection.model;
        canonical.provider = selection.provider;
        canonical.thinking.emplace(selection.thinking);
        canonical.permissionMode = it->params.permissionMode;
        return Reply{canonical};
    }
    if (const auto *remove = std::get_if<DeleteSession>(&command)) {
        const auto session = m_sessions.constFind(remove->sessionId);
        if (session != m_sessions.cend())
            for (const auto &turn : session->turns)
                if (turn.approval) {
                    m_approvals.remove(turn.approval);
                    emit reverseCancelled(turn.approval);
                }
        m_sessions.remove(remove->sessionId);
        return Reply{Null{}};
    }
    return failure(QStringLiteral("unsupported"),
                   QStringLiteral("Not implemented by the fake backend."));
}
void FakeBackend::publish(const QString &sessionId, Session &session, Turn &turn,
                          EventPayload payload, bool message)
{
    SessionEvent event{{sessionId, ++session.seq, turn.recovery.turnId,
                        message ? std::optional<QString>(turn.messageId) : std::nullopt,
                        turn.recovery.clientTurnId},
                       std::move(payload)};
    turn.recovery.events.append(event);
    emit sessionEvent(event);
}
void FakeBackend::advance()
{
    if (!m_initialized)
        return;
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (it->active.isEmpty())
            continue;
        auto &turn = it->turns[it->active];
        const auto scenario = turn.start.input.text;
        if ((scenario == "/fake tools" || scenario == "/fake approval") && !turn.toolStarted) {
            turn.toolStarted = true;
            publish(
                it.key(), *it, turn,
                ToolStarted{"fixture-tool", "demo", QStringLiteral("Simulated tool (no effects)")});
            if (scenario == "/fake approval" &&
                turn.start.params.permissionMode != PermissionMode::Full) {
                turn.approval = ++m_reverse;
                m_approvals.insert(turn.approval, it.key());
                ApprovalRequest request{
                    it.key(), turn.recovery.turnId, "fixture-approval", "fixture-tool", "demo", {},
                    {}};
                ApprovalPresentation presentation;
                presentation.kind = "command";
                presentation.title = "Run a simulated tool";
                presentation.effect = "run";
                presentation.badge = true;
                presentation.code = "No command runs. This is an in-memory approval fixture.";
                presentation.reveal = "command";
                request.presentation = presentation;
                emit reverseRequest(turn.approval, request);
            }
            return;
        }
        if (turn.approval)
            continue;
        if (turn.toolStarted && !turn.approvalDone) {
            turn.approvalDone = true;
            publish(it.key(), *it, turn,
                    ToolProgress{"fixture-tool", {{"text", "Simulated progress"}}});
            publish(
                it.key(), *it, turn,
                ToolCompleted{"fixture-tool",
                              {{"text", "Simulated result; no host action"}, {"isError", false}}});
        }
        if (!turn.queue.isEmpty()) {
            if (turn.offset)
                publish(it.key(), *it, turn, MessageCompleted{turn.response.left(turn.offset), {}},
                        true);
            const auto client = turn.queue.takeFirst();
            publish(it.key(), *it, turn, InputAccepted{client, display(turn.steering[client])});
            turn.messageId = uuid();
            turn.offset = 0;
            turn.response =
                QStringLiteral("**Fake steering** applied: ") + turn.steering[client].text;
        }
        if (turn.offset == 0)
            publish(it.key(), *it, turn, MessageStarted{}, true);
        const QString chunk = turn.response.mid(turn.offset, 5);
        turn.offset += chunk.size();
        publish(it.key(), *it, turn, MessageDelta{chunk}, true);
        if (turn.offset == turn.response.size()) {
            publish(it.key(), *it, turn, MessageCompleted{turn.response, {}}, true);
            Usage usage;
            usage.provider = "fake";
            usage.model = turn.start.params.selection.model;
            usage.modelName = QStringLiteral("Fake ") + usage.model;
            usage.input = 100;
            usage.cached = 20;
            usage.output = 30;
            usage.requests = 1;
            usage.context = Usage::Context{130, 4096};
            publish(it.key(), *it, turn, usage,
                    true); // Fixed fixture counts, not estimates/billing.
            turn.terminal = true;
            turn.failed = scenario == "/fake error" && !turn.retried;
            it->active.clear();
            TurnCompleted done;
            if (turn.failed) {
                done.status = TurnStatus::Error;
                done.error = Error{
                    "fixture_error",
                    "Simulated failure. Retry targets this failed turn without resending input.",
                    {},
                    QStringLiteral("retry"),
                    true,
                    {}};
            } else if (scenario == "/fake length")
                done.finishReason = "length";
            publish(it.key(), *it, turn, done);
        }
    }
}
void FakeBackend::answer(RequestId id, const ReverseResult &result)
{
    const auto pending = m_approvals.find(id);
    if (pending == m_approvals.end())
        return;
    const auto sessionId = pending.value();
    m_approvals.erase(pending);
    auto session = m_sessions.find(sessionId);
    if (session == m_sessions.end() || session->active.isEmpty())
        return;
    auto &turn = session->turns[session->active];
    if (turn.approval != id)
        return;
    turn.approval = 0;
    const auto *answer = std::get_if<ApprovalAnswer>(&result);
    const bool allow = answer && answer->decision == Decision::Allow;
    publish(sessionId, *session, turn,
            ApprovalResolved{"fixture-approval", allow ? Decision::Allow : Decision::Deny});
    if (!allow) {
        turn.approvalDone = true;
        publish(
            sessionId, *session, turn,
            ToolCompleted{"fixture-tool", {{"text", "Denied; nothing ran"}, {"isError", true}}});
        turn.response = QStringLiteral("Simulated tool denied. Nothing ran.");
    }
}
} // namespace openghost
