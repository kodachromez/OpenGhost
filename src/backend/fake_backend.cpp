#include "fake_backend.h"
#include <QUuid>

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
bool supported(const StartTurn &s)
{
    return s.input.attachments.isEmpty() && s.params.userContext.files.isEmpty() &&
           !s.params.host.browser && !s.params.side;
}
bool same(const StartTurn &a, const StartTurn &b)
{
    // All other input forms are refused before acceptance; compare every supported field.
    const auto &x = a.params;
    const auto &y = b.params;
    return a.sessionVersion == b.sessionVersion && a.input.text == b.input.text &&
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
        return Reply{catalog()};
    if (std::holds_alternative<ProvidersList>(command)) {
        Provider provider;
        provider.id = QStringLiteral("fake");
        provider.name = QStringLiteral("Fake backend (no provider access)");
        provider.status.connected = true;
        return Reply{QVector<Provider>{provider}};
    }
    if (const auto *start = std::get_if<StartTurn>(&command)) {
        if (start->sessionId.isEmpty() || start->clientTurnId.isEmpty() ||
            start->input.text.trimmed().isEmpty())
            return failure(QStringLiteral("invalid_request"),
                           QStringLiteral("Session, client turn and text required."));
        if (!supported(*start))
            return failure(
                QStringLiteral("unsupported"),
                QStringLiteral(
                    "Fake backend supports text input only; no files, browser or mini chats."));
        if (!valid(start->params.selection))
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
        turn.recovery.input = DisplayInput{start->input.text, {}};
        turn.messageId = uuid();
        const bool brief = start->params.selection.model == QStringLiteral("brief");
        turn.response =
            brief
                ? QStringLiteral("**Fake Brief** — received your message. No model was contacted.")
                : QStringLiteral("**Fake Echo** — this is a simulated response, not a "
                                 "model.\n\nYou said:\n\n") +
                      start->input.text;
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
        m_sessions.remove(remove->sessionId);
        return Reply{Null{}};
    }
    return failure(QStringLiteral("unsupported"),
                   QStringLiteral("Not implemented by the text-only fake backend."));
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
        if (turn.offset == 0)
            publish(it.key(), *it, turn, MessageStarted{}, true);
        const QString chunk = turn.response.mid(turn.offset, 5);
        turn.offset += chunk.size();
        publish(it.key(), *it, turn, MessageDelta{chunk}, true);
        if (turn.offset == turn.response.size()) {
            publish(it.key(), *it, turn, MessageCompleted{turn.response, {}}, true);
            turn.terminal = true;
            it->active.clear();
            publish(it.key(), *it, turn, TurnCompleted{});
        }
    }
}
} // namespace openghost
