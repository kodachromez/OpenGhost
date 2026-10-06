#include "pi_backend.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cstdio>
#include <utility>

namespace openghost
{
namespace
{
constexpr int StartDeadline = 60000; // Pi's prompt reply, as the reference's turn.start
constexpr int AbortDeadline = 60000; // abort replies once Pi is idle
constexpr qsizetype MaxLine = 64 * 1024 * 1024;
constexpr qsizetype MaxJournals = 64;

QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
Error failure(const QString &code, const QString &message)
{
    return {code, message, {}, QStringLiteral("none"), false, {}};
}
QByteArray jsonLine(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}
QString errorOf(const QJsonObject &reply, const QString &fallback)
{
    return reply.value("error").toString(fallback);
}
QString journalKey(const QString &session, const QString &client) { return session + '\n' + client; }
// Pi's text content: a string, or its text blocks joined as Pi joins them.
QString textOf(const QJsonValue &content)
{
    if (content.isString())
        return content.toString();
    QString text;
    for (const auto &block : content.toArray())
        if (block.toObject().value("type").toString() == QStringLiteral("text"))
            text += block.toObject().value("text").toString();
    return text;
}
std::optional<QString> finishOf(const QString &stopReason)
{
    if (stopReason == QStringLiteral("stop") || stopReason == QStringLiteral("length"))
        return stopReason;
    if (stopReason == QStringLiteral("toolUse"))
        return QStringLiteral("tool_calls");
    return std::nullopt;
}
DisplayInput displayOf(const Input &input)
{
    DisplayInput display{input.text, {}};
    for (const auto &attachment : input.attachments) {
        DisplayAttachment shown;
        shown.name = attachment.name;
        shown.size = attachment.size;
        shown.image = attachment.kind == Attachment::Kind::Image;
        display.attachments.append(shown);
    }
    return display;
}
QVector<Provider> providersOf(const QJsonArray &catalog)
{
    QVector<Provider> list;
    for (const auto &entry : catalog) {
        const auto item = entry.toObject();
        Provider provider;
        provider.id = item.value("id").toString();
        provider.name = item.value("name").toString(provider.id);
        if (item.value("oauth").isString()) // Pi's sign-in flow's own name
            provider.methods.append({AuthMethod::Kind::OAuth, item.value("oauth").toString(),
                                     item.value("oauth").toString(), {}, {}, {}});
        if (item.value("apiKey").isString())
            provider.methods.append({AuthMethod::Kind::ApiKey, item.value("apiKey").toString(),
                                     {}, {}, item.value("apiKey").toString(), {}});
        provider.status.connected = item.value("connected").toBool();
        provider.status.keySaved = item.value("stored").toBool();
        if (!provider.id.isEmpty())
            list.append(provider);
    }
    // Settings → Providers: Pi's whole catalog by display name.
    std::stable_sort(list.begin(), list.end(), [](const Provider &a, const Provider &b) {
        return QString::localeAwareCompare(a.name.toCaseFolded(), b.name.toCaseFolded()) < 0;
    });
    return list;
}
QVector<Model> modelsOf(const QJsonArray &catalog)
{
    QVector<Model> models;
    for (const auto &entry : catalog) {
        const auto model = entry.toObject();
        const auto provider = model.value("provider").toString();
        if (provider.isEmpty())
            continue;
        Model item{model.value("id").toString(), provider, model.value("name").toString(),
                   {}, model.value("input").toArray().contains(QStringLiteral("image")),
                   {}, {}};
        if (model.contains("contextWindow"))
            item.contextWindow = model.value("contextWindow").toDouble();
        models.append(item);
    }
    return models;
}
} // namespace

PiBackend::PiBackend(QObject *parent) : Backend(parent)
{
    // Pi's stderr goes to ours: visible when launched from a terminal, no window.
    m_pi.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(&m_pi, &QProcess::readyReadStandardOutput, this, &PiBackend::readStdout);
    connect(&m_pi, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        fprintf(stderr, "[pi] process error: %s\n", qPrintable(m_pi.errorString()));
    });
    connect(&m_pi, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        fprintf(stderr, "[pi] exited with code %d\n", code);
        const auto gone = failure(QStringLiteral("backend_unavailable"), QStringLiteral("Pi exited."));
        // Its outcome is unknown, never a rejection: the turn ends with the process.
        if (m_run && m_run->accepted) {
            TurnCompleted done{TurnStatus::Error, {}, gone};
            finish(done);
        } else if (m_run) {
            const auto request = m_run->request;
            const bool answered = m_run->abandoned;
            m_run.reset();
            if (!answered)
                emit replied(request, gone);
        }
        const auto cancels = std::exchange(m_cancels, {});
        for (const auto &ids : cancels)
            for (const auto id : ids)
                emit replied(id, gone);
        const QJsonObject exited{{"success", false}, {"ok", false}, {"error", "Pi exited."}};
        const auto waiting = std::exchange(m_waiting, {});
        for (const auto &done : waiting)
            done(exited);
        const auto queued = std::exchange(m_bridgeQueue, {});
        for (const auto &request : queued)
            request.second(exited);
        emit closed(gone);
    });
}
PiBackend::~PiBackend()
{
    if (m_pi.state() != QProcess::NotRunning) {
        m_pi.closeWriteChannel();
        if (!m_pi.waitForFinished(2000)) {
            m_pi.kill();
            m_pi.waitForFinished(1000);
        }
    }
}
void PiBackend::request(RequestId id, const Command &command)
{
    QTimer::singleShot(0, this, [this, id, command] { dispatch(id, command); });
}
// Stop before Pi accepted the start: unsent, it is never sent; sent, Pi's acceptance
// is followed at once by an abort. Nothing else here can be withdrawn.
void PiBackend::cancelRequest(RequestId id)
{
    if (m_run && m_run->request == id && !m_run->accepted)
        m_run->cancelled = true;
}
void PiBackend::dispatch(RequestId id, const Command &command)
{
    const auto settle = [this, id](const QJsonObject &reply, const QString &fallback,
                                   const auto &value) {
        if (reply.value("success").toBool(true) && reply.value("ok").toBool(true))
            emit replied(id, Reply{value(reply)});
        else
            emit replied(id, failure(QStringLiteral("backend_error"), errorOf(reply, fallback)));
    };
    if (std::holds_alternative<ModelsList>(command)) {
        rpc({{"type", "get_available_models"}}, [settle](const QJsonObject &reply) {
            settle(reply, QStringLiteral("Pi could not list models."), [](const QJsonObject &r) {
                return modelsOf(r.value("data").toObject().value("models").toArray());
            });
        });
    } else if (std::holds_alternative<ProvidersList>(command)) {
        bridge({{"op", "providers"}}, [settle](const QJsonObject &reply) {
            settle(reply, QStringLiteral("Pi could not list providers."),
                   [](const QJsonObject &r) { return providersOf(r.value("providers").toArray()); });
        });
    } else if (const auto *key = std::get_if<SetKey>(&command)) {
        auth(id, key->provider,
             key->key ? QJsonObject{{"op", "setKey"}, {"key", *key->key}}
                      : QJsonObject{{"op", "logout"}});
    } else if (const auto *login = std::get_if<Login>(&command)) {
        auth(id, login->provider, {{"op", "login"}});
    } else if (const auto *logout = std::get_if<Logout>(&command)) {
        auth(id, logout->provider, {{"op", "logout"}});
    } else if (const auto *cancel = std::get_if<CancelLogin>(&command)) {
        m_cancelled.insert(cancel->provider);
        auth(id, cancel->provider, {{"op", "cancel"}});
    } else if (const auto *answer = std::get_if<AnswerLogin>(&command)) {
        bridge({{"op", "answer"}, {"promptId", answer->promptId}, {"value", answer->value}},
               [settle](const QJsonObject &reply) {
                   settle(reply, QStringLiteral("Pi did not take the answer."),
                          [](const QJsonObject &) { return Null{}; });
               });
    } else if (const auto *configure = std::get_if<ConfigureSession>(&command)) {
        if (!configure->model || !configure->provider || configure->model->isEmpty()) {
            emit replied(id, Reply{SessionConfigured{}});
            return;
        }
        setModel(*configure->provider, *configure->model,
                 [this, id, configure = *configure](std::optional<Error> error) {
                     if (error)
                         emit replied(id, *error);
                     else {
                         SessionConfigured configured;
                         configured.model = configure.model;
                         configured.provider = configure.provider;
                         emit replied(id, Reply{configured});
                     }
                 });
    } else if (const auto *start = std::get_if<StartTurn>(&command)) {
        const auto known = m_journal.constFind(journalKey(start->sessionId, start->clientTurnId));
        if (known != m_journal.cend()) { // Accepted once already: its identity, never twice.
            if (!known->input)
                emit replied(id, failure(QStringLiteral("duplicate_request"),
                                         QStringLiteral("That turn ID belongs to a retry.")));
            else
                emit replied(id, Reply{StartAccepted{known->turn,
                                                     m_sessions.value(start->sessionId).version}});
            return;
        }
        if (const auto error = admissible(start->sessionId, start->sessionVersion)) {
            emit replied(id, *error);
            return;
        }
        Run run;
        run.session = start->sessionId;
        run.client = start->clientTurnId;
        run.input = displayOf(start->input);
        begin(id, std::move(run), start->params.selection, start->input.text);
    } else if (const auto *retry = std::get_if<RetryTurn>(&command)) {
        const auto known = m_journal.constFind(journalKey(retry->sessionId, retry->clientTurnId));
        if (known != m_journal.cend()) {
            if (known->input)
                emit replied(id, failure(QStringLiteral("duplicate_request"),
                                         QStringLiteral("That turn ID belongs to a new message.")));
            else
                emit replied(id, Reply{RetryAccepted{known->turn}});
            return;
        }
        if (const auto error = admissible(retry->sessionId, retry->sessionVersion)) {
            emit replied(id, *error);
            return;
        }
        // Exactly the chat's failed turn, and only while it is still Pi's latest:
        // Pi continues its own context from there, with no new or repeated input.
        const auto failed =
            std::find_if(m_journal.cbegin(), m_journal.cend(), [&](const Journal &journal) {
                return journal.session == retry->sessionId && journal.turn == retry->failedTurnId;
            });
        if (failed == m_journal.cend() || !failed->terminal ||
            failed->status != TurnStatus::Error) {
            emit replied(id, failure(QStringLiteral("invalid_request"),
                                     QStringLiteral("Pi can retry only a reply that failed.")));
            return;
        }
        if (failed.key() != m_last) {
            emit replied(id, failure(QStringLiteral("stale_turn"),
                                     QStringLiteral("Pi has taken newer input since this reply "
                                                    "failed, so it cannot retry it exactly.")));
            return;
        }
        Run run;
        run.session = retry->sessionId;
        run.client = retry->clientTurnId;
        run.retry = true;
        begin(id, std::move(run), retry->params.selection, {});
    } else if (const auto *steering = std::get_if<SteerTurn>(&command)) {
        steer(id, *steering);
    } else if (const auto *stopping = std::get_if<CancelTurn>(&command)) {
        cancelTurn(id, *stopping);
    } else {
        emit replied(id, execute(command));
    }
}
// A start/retry for this chat, now: one Pi turn at a time, and the incarnation the
// frontend names (none = create-only) must be the one Pi's history belongs to.
std::optional<Error> PiBackend::admissible(const QString &session,
                                           const std::optional<QString> &version)
{
    if (m_run)
        return failure(QStringLiteral("busy"), QStringLiteral("A turn is already running."));
    if (m_pi.state() != QProcess::Running)
        return failure(QStringLiteral("backend_unavailable"), QStringLiteral("Pi is not running."));
    const auto known = m_sessions.constFind(session);
    if (!version && known != m_sessions.cend())
        return failure(QStringLiteral("session_conflict"),
                       QStringLiteral("This chat already exists in Pi."));
    if (version && known == m_sessions.cend())
        return failure(QStringLiteral("session_missing"),
                       QStringLiteral("Pi no longer has this chat."));
    if (version && known->version != *version)
        return failure(QStringLiteral("session_conflict"),
                       QStringLiteral("This chat changed in Pi."));
    return std::nullopt;
}
void PiBackend::begin(RequestId id, Run run, const ModelSelection &chosen, const QString &text)
{
    run.request = id;
    run.turn = uuid();
    const auto turn = run.turn;
    m_run = std::move(run);
    // The chat's chosen model, made Pi's before the prompt (Pi runs RPC lines
    // concurrently, so the prompt waits for set_model's reply).
    if (!chosen.model.isEmpty() &&
        (chosen.provider != m_modelProvider || chosen.model != m_model)) {
        setModel(chosen.provider, chosen.model, [this, turn, text](std::optional<Error> error) {
            if (!m_run || m_run->turn != turn)
                return;
            if (error) {
                const auto request = m_run->request;
                m_run.reset();
                emit replied(request, *error);
            } else {
                submit(text);
            }
        });
        return;
    }
    submit(text);
}
// The prompt (or bridge retry) itself. Its acceptance is Pi's reply, never assumed.
void PiBackend::submit(const QString &text)
{
    auto &run = *m_run;
    const auto turn = run.turn;
    if (run.cancelled) {
        const auto request = run.request;
        m_run.reset();
        emit replied(request, failure(QStringLiteral("cancelled"),
                                      QStringLiteral("Stopped before Pi received it.")));
        return;
    }
    run.sent = true;
    QTimer::singleShot(StartDeadline, this, [this, turn] {
        if (!m_run || m_run->turn != turn || m_run->accepted || m_run->abandoned)
            return;
        // Unknown outcome: should Pi still accept it, it is stopped at once.
        m_run->abandoned = true;
        emit replied(m_run->request,
                     failure(QStringLiteral("timeout"),
                             QStringLiteral("Pi did not answer within 60 seconds. Nothing will "
                                            "be resent; use Retry to check what Pi received.")));
    });
    if (run.retry) {
        bridge({{"op", "retry"}}, [this, turn](const QJsonObject &reply) {
            admitted(turn, {{"success", reply.value("ok").toBool()},
                            {"error", reply.value("error")},
                            {"data", QJsonObject{{"disposition", "started"}}}});
        });
        return;
    }
    const auto id = QStringLiteral("og-%1").arg(++m_rpc);
    m_waiting.insert(id, [this, turn](const QJsonObject &reply) { admitted(turn, reply); });
    if (!send({{"id", id}, {"type", "prompt"}, {"message", text}})) {
        m_waiting.remove(id);
        admitted(turn, {{"success", false}, {"error", "Pi is not accepting input."}});
        return;
    }
    fprintf(stderr, "[pi] prompt sent (%lld chars)\n", static_cast<long long>(text.size()));
}
void PiBackend::admitted(const QString &turn, const QJsonObject &reply)
{
    if (!m_run || m_run->turn != turn)
        return;
    if (!reply.value("success").toBool()) {
        // Refused before acceptance: nothing ran, and the chat's history is unchanged.
        const auto request = m_run->request;
        const bool answered = m_run->abandoned;
        m_run.reset();
        if (!answered)
            emit replied(request,
                         Error{QStringLiteral("rejected"),
                               errorOf(reply, QStringLiteral("Pi did not accept it.")),
                               {}, {}, {}, {}});
        return;
    }
    accept(reply.value("data").toObject().value("disposition").toString());
}
void PiBackend::accept(const QString &disposition)
{
    auto &run = *m_run;
    run.accepted = true;
    auto &session = m_sessions[run.session];
    if (session.version.isEmpty())
        session.version = uuid(); // Created by its first accepted turn.
    const auto journal = journalKey(run.session, run.client);
    m_journal.insert(journal, {run.session, run.client, run.turn, run.input, {}, false,
                               TurnStatus::Done});
    m_order.append(journal);
    for (auto it = m_order.begin(); m_order.size() > MaxJournals && it != m_order.end();) {
        if (m_journal.value(*it).terminal) {
            m_journal.remove(*it);
            it = m_order.erase(it);
        } else {
            ++it;
        }
    }
    m_last = journal;
    if (!run.abandoned) {
        if (run.retry)
            emit replied(run.request, Reply{RetryAccepted{run.turn}});
        else
            emit replied(run.request, Reply{StartAccepted{run.turn, session.version}});
    }
    publish(TurnStarted{});
    if (disposition == QStringLiteral("handled")) {
        // A Pi command consumed it: no run started, so none is awaited.
        TurnCompleted done;
        done.finishReason = QStringLiteral("handled");
        finish(done);
        return;
    }
    if (run.cancelled || run.abandoned)
        abort(); // Late acceptance of a stopped or abandoned start.
}
// Stop: Pi's queued steering is dropped first (abort alone would run it), then
// abort, which replies once Pi is idle.
void PiBackend::abort()
{
    auto &run = *m_run;
    run.cancelled = true;
    if (run.aborting)
        return;
    run.aborting = true;
    const auto turn = run.turn;
    rpc({{"type", "clear_queue"}}, [this, turn](const QJsonObject &) {
        rpc(
            {{"type", "abort"}},
            [this, turn](const QJsonObject &reply) {
                const bool ok = reply.value("success").toBool();
                if (m_run && m_run->turn == turn) {
                    if (ok)
                        finish({TurnStatus::Cancelled, {}, {}});
                    else
                        m_run->aborting = false;
                }
                const auto waiting = m_cancels.take(turn);
                for (const auto id : waiting)
                    emit replied(id, ok ? Result{Reply{Null{}}}
                                        : Result{failure(QStringLiteral("cancel_failed"),
                                                         errorOf(reply, QStringLiteral(
                                                                            "Pi did not stop.")))});
            },
            AbortDeadline);
    });
}
void PiBackend::cancelTurn(RequestId id, const CancelTurn &cancel)
{
    if (m_run && m_run->accepted && m_run->session == cancel.sessionId &&
        m_run->turn == cancel.turnId) {
        m_cancels[cancel.turnId].append(id);
        abort();
        return;
    }
    // Not running: an ended turn has nothing left to stop; an unknown one is refused.
    const bool ended = std::any_of(m_journal.cbegin(), m_journal.cend(), [&](const Journal &j) {
        return j.session == cancel.sessionId && j.turn == cancel.turnId && j.terminal;
    });
    emit replied(id, ended ? Result{Reply{Null{}}}
                           : Result{failure(QStringLiteral("stale_turn"),
                                            QStringLiteral("Pi is not running that reply."))});
}
void PiBackend::finish(TurnCompleted done)
{
    auto &run = *m_run;
    const auto status = done.status;
    publish(std::move(done));
    auto &journal = m_journal[journalKey(run.session, run.client)];
    journal.terminal = true;
    journal.status = status;
    // Steering Pi still holds would otherwise open the next turn.
    if (std::any_of(run.steers.cbegin(), run.steers.cend(),
                    [](const Steer &s) { return s.admitted && !s.applied; }))
        rpc({{"type", "clear_queue"}}, [](const QJsonObject &) {});
    m_run.reset();
}
// Send while busy: Pi's steer queue. Admission is Pi's "queued" reply; application is
// Pi delivering that exact queued text as the next user message, in queue order.
void PiBackend::steer(RequestId id, const SteerTurn &steer)
{
    if (!m_run || !m_run->accepted || m_run->cancelled || m_run->session != steer.sessionId ||
        m_run->turn != steer.turnId) {
        emit replied(id, Reply{SteerAccepted{false}}); // That reply is no longer running.
        return;
    }
    auto &run = *m_run;
    for (const auto &known : std::as_const(run.steers))
        if (known.client == steer.clientInputId) {
            emit replied(id, Reply{SteerAccepted{known.admitted}});
            return;
        }
    // Only text enters Pi's steer queue here; files are refused, never dropped.
    if (!steer.input.attachments.isEmpty() || steer.input.text.trimmed().isEmpty() ||
        !run.steering.isEmpty()) {
        emit replied(id, Reply{SteerAccepted{false}});
        return;
    }
    run.steers.append({steer.clientInputId, {}, DisplayInput{steer.input.text, {}}, false, false});
    run.steering = steer.clientInputId;
    const auto turn = run.turn, client = steer.clientInputId;
    rpc({{"type", "steer"}, {"message", steer.input.text}},
        [this, id, turn, client](const QJsonObject &reply) {
            const bool queued = reply.value("success").toBool() &&
                                reply.value("data").toObject().value("disposition").toString() ==
                                    QStringLiteral("queued");
            if (!m_run || m_run->turn != turn) {
                // The reply ended first: what Pi queued would open the next turn.
                if (queued)
                    rpc({{"type", "clear_queue"}}, [](const QJsonObject &) {});
                emit replied(id, Reply{SteerAccepted{false}});
                return;
            }
            auto &run = *m_run;
            run.steering.clear();
            const auto it = std::find_if(run.steers.begin(), run.steers.end(),
                                         [&](const Steer &s) { return s.client == client; });
            if (queued) {
                it->admitted = true;
                emit replied(id, Reply{SteerAccepted{true}});
            } else if (reply.value("success").toBool()) {
                run.steers.erase(it); // Handled by Pi itself: not part of this reply.
                emit replied(id, Reply{SteerAccepted{false}});
            } else {
                run.steers.erase(it);
                emit replied(id, failure(QStringLiteral("steering_rejected"),
                                         errorOf(reply, QStringLiteral("Pi did not take it."))));
            }
        });
}
Result PiBackend::execute(const Command &command)
{
    if (std::holds_alternative<Initialize>(command)) {
        if (m_pi.state() == QProcess::NotRunning) {
            if (m_bridgePath.isEmpty() && m_bridgeDir.isValid()) {
                const auto path = m_bridgeDir.filePath(QStringLiteral("openghost-bridge.js"));
                if (QFile::copy(QStringLiteral(":/pi/openghost-bridge.js"), path))
                    m_bridgePath = path;
            }
            QStringList args{QStringLiteral("--mode"), QStringLiteral("rpc"),
                             QStringLiteral("--no-session")};
            if (!m_bridgePath.isEmpty())
                args << QStringLiteral("-e") << m_bridgePath;
            m_pi.start(QStringLiteral("pi"), args);
            if (!m_pi.waitForStarted(5000))
                return failure(QStringLiteral("backend_unavailable"),
                               QStringLiteral("Cannot start pi: ") + m_pi.errorString());
            fprintf(stderr, "[pi] started pid %lld\n", static_cast<long long>(m_pi.processId()));
            // Bridge commands only once Pi lists ours: a missing extension would
            // otherwise turn `/openghost …` into a prompt for the model.
            m_bridge = Bridge::Unknown;
            rpc({{"type", "get_commands"}}, [this](const QJsonObject &reply) {
                const auto commands = reply.value("data").toObject().value("commands").toArray();
                const bool ready =
                    !m_bridgePath.isEmpty() &&
                    std::any_of(commands.begin(), commands.end(), [this](const auto &c) {
                        const auto command = c.toObject();
                        return command.value("name").toString() == QStringLiteral("openghost") &&
                               command.value("sourceInfo").toObject().value("path").toString() ==
                                   m_bridgePath;
                    });
                m_bridge = ready ? Bridge::Ready : Bridge::Missing;
                fprintf(stderr, "[pi] OpenGhost bridge %s\n", ready ? "ready" : "missing");
                const auto queued = std::exchange(m_bridgeQueue, {});
                for (const auto &request : queued)
                    bridge(request.first, request.second);
            });
        }
        Initialized result;
        result.backend = BackendInfo{QStringLiteral("Pi (RPC proof of concept)"), {}, {}};
        result.capabilities.authProviders = true;
        // Required by ChatService: turn journals answer session.get while Pi runs.
        result.capabilities.sessionRecovery = true;
        return Reply{result};
    }
    if (const auto *get = std::get_if<GetSession>(&command)) {
        const auto session = m_sessions.constFind(get->sessionId);
        if (session == m_sessions.cend())
            return Reply{SessionRecovery{MissingSession{}}};
        // The named turn only if Pi accepted it; without one, the running turn.
        QString journal;
        if (get->clientTurnId)
            journal = journalKey(get->sessionId, *get->clientTurnId);
        else if (m_run && m_run->accepted && m_run->session == get->sessionId)
            journal = journalKey(m_run->session, m_run->client);
        std::optional<RecoveredTurn> turn;
        if (const auto it = m_journal.constFind(journal); it != m_journal.cend())
            turn = RecoveredTurn{it->client, it->turn, it->input, it->events};
        return Reply{SessionRecovery{ExistingSession{session->version, session->seq, turn}}};
    }
    if (std::holds_alternative<Shutdown>(command) ||
        std::holds_alternative<DeleteSession>(command))
        return Reply{Null{}};
    return failure(QStringLiteral("unsupported"),
                   QStringLiteral("Not implemented by the Pi proof of concept."));
}
bool PiBackend::send(const QJsonObject &command)
{
    if (m_pi.state() != QProcess::Running)
        return false;
    const auto bytes = jsonLine(command);
    return m_pi.write(bytes) == bytes.size();
}
void PiBackend::rpc(QJsonObject command, Done done, int deadline)
{
    const auto id = QStringLiteral("og-%1").arg(++m_rpc);
    const auto type = command.value("type").toString();
    command.insert("id", id);
    if (!send(command)) {
        done({{"success", false}, {"ok", false}, {"error", "Pi is not running."}});
        return;
    }
    m_waiting.insert(id, std::move(done));
    if (deadline > 0)
        QTimer::singleShot(deadline, this, [this, id, type, deadline] {
            if (const auto done = m_waiting.take(id))
                done({{"success", false},
                      {"ok", false},
                      {"timeout", true},
                      {"error", QStringLiteral("Pi did not answer %1 within %2 seconds.")
                                    .arg(type)
                                    .arg(deadline / 1000)}});
        });
}
// One `/openghost` command; its result is the bridge's `openghost:<token>` status.
void PiBackend::bridge(QJsonObject request, Done done)
{
    if (m_bridge == Bridge::Unknown && m_pi.state() == QProcess::Running) {
        m_bridgeQueue.append({request, std::move(done)});
        return;
    }
    if (m_bridge != Bridge::Ready || m_pi.state() != QProcess::Running) {
        done({{"ok", false},
              {"error", "OpenGhost's Pi bridge is not loaded, so Pi's providers, sign-in "
                        "and Retry are unavailable."}});
        return;
    }
    const auto token = QStringLiteral("og-bridge-%1").arg(++m_rpc);
    request.insert("token", token);
    m_waiting.insert(QStringLiteral("bridge:") + token, std::move(done));
    if (request.contains("provider"))
        m_logins.insert(token, request.value("provider").toString());
    if (!send({{"id", token},
               {"type", "prompt"},
               {"message", QStringLiteral("/openghost ") +
                               QString::fromUtf8(QJsonDocument(request).toJson(
                                   QJsonDocument::Compact))}})) {
        m_logins.remove(token);
        m_waiting.take(QStringLiteral("bridge:") + token)(
            {{"ok", false}, {"error", "Pi is not running."}});
    }
}
// Sign-in, API key, cancel and logout run Pi's own login/logout; the reply comes
// when Pi finishes. Status is then reread from Pi, never assumed.
void PiBackend::auth(RequestId id, const QString &provider, QJsonObject request)
{
    const auto op = request.value("op").toString();
    if (op == QStringLiteral("login") || op == QStringLiteral("setKey"))
        m_cancelled.remove(provider);
    request.insert("provider", provider);
    bridge(request, [this, id, provider, op](const QJsonObject &reply) {
        const bool cancel = op == QStringLiteral("cancel");
        if (!cancel)
            m_steps.remove(provider);
        if (reply.value("ok").toBool()) {
            emit replied(id, Reply{Null{}});
        } else if (!cancel && m_cancelled.remove(provider)) {
            emit replied(id, Reply{Null{}}); // Aborted because it was cancelled.
        } else {
            emit replied(id, failure(QStringLiteral("auth_failed"),
                                     errorOf(reply, QStringLiteral("Pi could not sign in."))));
        }
        if (!cancel)
            emit globalEvent(ModelsChanged{provider});
    });
}
void PiBackend::setModel(const QString &provider, const QString &model,
                         std::function<void(std::optional<Error>)> done)
{
    rpc({{"type", "set_model"}, {"provider", provider}, {"modelId", model}},
        [this, provider, model, done = std::move(done)](const QJsonObject &reply) {
            if (!reply.value("success").toBool()) {
                done(failure(QStringLiteral("model_unavailable"),
                             errorOf(reply, QStringLiteral("Pi could not switch models."))));
                return;
            }
            m_modelProvider = provider;
            m_model = model;
            fprintf(stderr, "[pi] model %s/%s\n", qPrintable(provider), qPrintable(model));
            done(std::nullopt);
        });
}
// Pi's auth event or prompt, as the sign-in step Settings shows for that provider.
// The sign-in page and device code stay offered until the sign-in ends.
void PiBackend::step(const QString &token, const QString &kind, const QJsonObject &value)
{
    const auto provider = m_logins.value(token);
    if (provider.isEmpty())
        return;
    const auto prior = m_steps.value(provider);
    LoginStep step;
    step.provider = provider;
    step.type = QStringLiteral("waiting");
    step.url = prior.url;
    step.userCode = prior.userCode;
    const auto type = value.value("type").toString();
    step.message = value.value("message").toString();
    if (kind == QStringLiteral("prompt")) {
        step.promptId = value.value("promptId").toString();
        step.type = type == QStringLiteral("select") ? QStringLiteral("select")
                                                     : QStringLiteral("prompt");
        step.secret = type == QStringLiteral("secret");
        if (value.value("placeholder").isString())
            step.placeholder = value.value("placeholder").toString();
        for (const auto &option : value.value("options").toArray())
            step.options.append({option.toObject().value("id").toString(),
                                 option.toObject().value("label").toString()});
    } else if (type == QStringLiteral("auth_url")) {
        step.url = value.value("url").toString();
        step.message = value.value("instructions")
                           .toString(QStringLiteral("Finish signing in in the browser…"));
    } else if (type == QStringLiteral("device_code")) {
        step.type = QStringLiteral("device_code");
        step.url = value.value("verificationUri").toString();
        step.userCode = value.value("userCode").toString();
        step.message = QStringLiteral("Enter the code %1 on the verification page.")
                           .arg(*step.userCode);
    } else if (type == QStringLiteral("info")) {
        for (const auto &link : value.value("links").toArray())
            step.links.append({link.toObject().value("url").toString(),
                               link.toObject().value("label").toString()});
    }
    if (step.userCode && step.type == QStringLiteral("waiting"))
        step.type = QStringLiteral("device_code");
    m_steps.insert(provider, step);
    emit globalEvent(step);
}
void PiBackend::readStdout()
{
    m_buffer += m_pi.readAllStandardOutput();
    qsizetype at;
    while ((at = m_buffer.indexOf('\n')) >= 0) { // LF only
        const QByteArray bytes = m_buffer.left(at);
        m_buffer.remove(0, at + 1);
        line(bytes);
    }
    if (m_buffer.size() > MaxLine) { // No record is this large: the stream is broken.
        fprintf(stderr, "[pi] stdout record over %lld bytes; stopping Pi\n",
                static_cast<long long>(MaxLine));
        m_buffer.clear();
        m_pi.kill();
    }
}
void PiBackend::line(const QByteArray &bytes)
{
    QJsonParseError error;
    const auto object = QJsonDocument::fromJson(bytes, &error).object();
    if (error.error != QJsonParseError::NoError) {
        fprintf(stderr, "[pi] unparsed stdout: %s\n", bytes.left(200).constData());
        return;
    }
    const auto type = object.value("type").toString();
    if (type == QStringLiteral("response")) {
        const auto id = object.value("id").toString();
        const bool ok = object.value("success").toBool(true);
        if (!ok)
            fprintf(stderr, "[pi] command failed: %s\n", bytes.left(500).constData());
        if (const auto done = m_waiting.take(id))
            return done(object);
        // A bridge command Pi did not run; its result would never arrive.
        if (!ok && m_waiting.contains(QStringLiteral("bridge:") + id)) {
            m_logins.remove(id);
            return m_waiting.take(QStringLiteral("bridge:") + id)(
                {{"ok", false}, {"error", errorOf(object, QStringLiteral("Pi refused it."))}});
        }
        return;
    }
    if (type == QStringLiteral("extension_ui_request") &&
        object.value("method").toString() == QStringLiteral("setStatus") &&
        object.value("statusKey").toString().startsWith(QStringLiteral("openghost:"))) {
        const auto key = object.value("statusKey").toString().mid(10).split(':');
        const auto value = QJsonDocument::fromJson(object.value("statusText").toString().toUtf8())
                               .object();
        if (key.size() == 2) {
            step(key[0], key[1], value);
        } else if (const auto done = m_waiting.take(QStringLiteral("bridge:") + key[0])) {
            m_logins.remove(key[0]);
            done(value);
        }
        return;
    }
    if (m_run && m_run->accepted)
        runEvent(type, object);
}
// Pi's events for the accepted turn. Messages are Pi's own: deltas stream, and
// message_end replaces them with the authoritative text and stop reason.
void PiBackend::runEvent(const QString &type, const QJsonObject &object)
{
    auto &run = *m_run;
    if (type == QStringLiteral("agent_start")) {
        run.active = true;
    } else if (type == QStringLiteral("queue_update")) {
        QStringList steering;
        for (const auto &text : object.value("steering").toArray())
            steering.append(text.toString());
        // The text Pi queued for the steer awaiting its reply (after Pi's own
        // input handling), which is the text its user message will carry.
        if (!run.steering.isEmpty() && steering.size() > run.queue.size())
            for (auto &steer : run.steers)
                if (steer.client == run.steering && steer.queued.isEmpty())
                    steer.queued = steering.last();
        run.queue = steering;
    } else if (type == QStringLiteral("message_start") || type == QStringLiteral("message_end")) {
        const auto message = object.value("message").toObject();
        const auto role = message.value("role").toString();
        if (role == QStringLiteral("user") && type == QStringLiteral("message_start")) {
            if (!run.retry && !run.prompted) {
                run.prompted = true; // The prompt itself.
                return;
            }
            // Pi takes steering in queue order; the first matching queued input is it.
            const auto text = textOf(message.value("content"));
            for (auto &steer : run.steers)
                if (!steer.applied && !steer.queued.isEmpty() && steer.queued == text) {
                    steer.applied = true;
                    publish(InputAccepted{steer.client, steer.input});
                    break;
                }
            return;
        }
        if (role != QStringLiteral("assistant"))
            return;
        if (type == QStringLiteral("message_start")) {
            run.message = uuid();
            MessageStarted started;
            if (!message.value("model").toString().isEmpty())
                started.model = message.value("model").toString();
            publish(started, true);
            return;
        }
        if (run.message.isEmpty()) {
            run.message = uuid();
            publish(MessageStarted{}, true);
        }
        run.stopReason = message.value("stopReason").toString();
        run.errorMessage = message.value("errorMessage").toString();
        publish(MessageCompleted{textOf(message.value("content")), finishOf(run.stopReason)}, true);
        // Its final usage, once: Pi's per-update usage is cumulative. Cached input is
        // part of OpenGhost's input count; Pi's reasoning is already in its output.
        const auto spent = message.value("usage").toObject();
        Usage usage;
        usage.provider = message.value("provider").toString();
        usage.model = message.value("model").toString();
        usage.cached = spent.value("cacheRead").toDouble();
        usage.written = spent.value("cacheWrite").toDouble();
        usage.input = spent.value("input").toDouble() + usage.cached + usage.written;
        usage.output = spent.value("output").toDouble();
        usage.requests = 1;
        if (!usage.provider.isEmpty() && !usage.model.isEmpty() &&
            (usage.input > 0 || usage.output > 0))
            publish(usage, true);
        run.message.clear();
    } else if (type == QStringLiteral("message_update")) {
        const auto event = object.value("assistantMessageEvent").toObject();
        if (event.value("type").toString() != QStringLiteral("text_delta"))
            return;
        if (run.message.isEmpty()) {
            run.message = uuid();
            publish(MessageStarted{}, true);
        }
        publish(MessageDelta{event.value("delta").toString()}, true);
    } else if (type == QStringLiteral("extension_error")) {
        // The bridge's retry trigger failed before Pi started anything.
        if (run.retry && !run.active &&
            object.value("event").toString() == QStringLiteral("send_message"))
            finish({TurnStatus::Error,
                    {},
                    Error{QStringLiteral("retry_failed"),
                          object.value("error").toString(QStringLiteral("Pi could not retry.")),
                          {}, {}, {}, {}}});
    } else if (type == QStringLiteral("agent_settled")) {
        // Pi's last assistant message decides the outcome; Pi's own automatic
        // retries and recovery have already run by now.
        TurnCompleted done;
        if (run.cancelled || run.stopReason == QStringLiteral("aborted")) {
            done.status = TurnStatus::Cancelled;
        } else if (run.stopReason == QStringLiteral("error")) {
            done.status = TurnStatus::Error;
            done.error = Error{QStringLiteral("model_error"),
                               run.errorMessage.isEmpty() ? QStringLiteral("Pi reported an error.")
                                                          : run.errorMessage,
                               {}, {}, {}, {}};
        } else if (run.stopReason == QStringLiteral("length")) {
            done.finishReason = run.stopReason;
        }
        finish(done);
    }
}
void PiBackend::publish(EventPayload payload, bool message)
{
    auto &run = *m_run;
    auto &session = m_sessions[run.session];
    SessionEvent event{{run.session, ++session.seq, run.turn,
                        message ? std::optional<QString>(run.message) : std::nullopt, run.client},
                       std::move(payload)};
    if (auto journal = m_journal.find(journalKey(run.session, run.client)); journal != m_journal.end()) {
        // Recovery needs a message's final text, not every delta that led to it.
        if (const auto *done = std::get_if<MessageCompleted>(&event.payload); done && done->text)
            journal->events.removeIf([&](const SessionEvent &e) {
                return e.identity.messageId == event.identity.messageId &&
                       std::holds_alternative<MessageDelta>(e.payload);
            });
        journal->events.append(event);
    }
    emit sessionEvent(event);
}
} // namespace openghost
