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
        const QJsonObject exited{{"success", false}, {"ok", false}, {"error", "Pi exited."}};
        const auto waiting = std::exchange(m_waiting, {});
        for (const auto &done : waiting)
            done(exited);
        const auto queued = std::exchange(m_bridgeQueue, {});
        for (const auto &request : queued)
            request.second(exited);
        emit closed(failure(QStringLiteral("backend_unavailable"), QStringLiteral("Pi exited.")));
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
        // The chat's chosen model, made Pi's before the prompt (Pi runs RPC lines
        // concurrently, so the prompt waits for set_model's reply).
        const auto &chosen = start->params.selection;
        if (m_turn.isEmpty() && !chosen.model.isEmpty() &&
            (chosen.provider != m_modelProvider || chosen.model != m_model)) {
            setModel(chosen.provider, chosen.model,
                     [this, id, command](std::optional<Error> error) {
                         if (error)
                             emit replied(id, *error);
                         else
                             startTurn(id, command);
                     });
            return;
        }
        startTurn(id, command);
    } else {
        emit replied(id, execute(command));
    }
}
void PiBackend::startTurn(RequestId id, const Command &command)
{
    emit replied(id, execute(command));
    if (!m_turn.isEmpty())
        publish(TurnStarted{});
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
        result.capabilities.sessionRecovery = true; // required by ChatService
        return Reply{result};
    }
    if (const auto *start = std::get_if<StartTurn>(&command)) {
        if (!m_turn.isEmpty())
            return failure(QStringLiteral("busy"), QStringLiteral("A turn is already running."));
        if (m_pi.state() != QProcess::Running)
            return failure(QStringLiteral("backend_unavailable"),
                           QStringLiteral("Pi is not running."));
        if (m_session != start->sessionId) {
            m_session = start->sessionId;
            m_version = uuid();
            m_seq = 0;
        }
        m_turn = uuid();
        m_client = start->clientTurnId;
        m_message.clear();
        const QJsonObject prompt{
            {"id", QStringLiteral("prompt-%1").arg(++m_prompts)},
            {"type", "prompt"},
            {"message", start->input.text}};
        m_pi.write(QJsonDocument(prompt).toJson(QJsonDocument::Compact) + '\n');
        fprintf(stderr, "[pi] prompt sent (%lld chars)\n",
                static_cast<long long>(start->input.text.size()));
        return Reply{StartAccepted{m_turn, m_version}};
    }
    if (const auto *get = std::get_if<GetSession>(&command)) {
        if (get->sessionId != m_session)
            return Reply{SessionRecovery{MissingSession{}}};
        return Reply{SessionRecovery{ExistingSession{m_version, m_seq, {}}}};
    }
    if (std::holds_alternative<Shutdown>(command) ||
        std::holds_alternative<DeleteSession>(command) ||
        std::holds_alternative<CancelTurn>(command))
        return Reply{Null{}};
    return failure(QStringLiteral("unsupported"),
                   QStringLiteral("Not implemented by the Pi proof of concept."));
}
void PiBackend::rpc(QJsonObject command, Done done)
{
    if (m_pi.state() != QProcess::Running) {
        done({{"success", false}, {"ok", false}, {"error", "Pi is not running."}});
        return;
    }
    const auto id = QStringLiteral("og-%1").arg(++m_rpc);
    command.insert("id", id);
    m_waiting.insert(id, std::move(done));
    m_pi.write(jsonLine(command));
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
              {"error", "OpenGhost's Pi bridge is not loaded, so Pi's providers and sign-in "
                        "are unavailable."}});
        return;
    }
    const auto token = QStringLiteral("og-bridge-%1").arg(++m_rpc);
    request.insert("token", token);
    m_waiting.insert(QStringLiteral("bridge:") + token, std::move(done));
    if (request.contains("provider"))
        m_logins.insert(token, request.value("provider").toString());
    m_pi.write(jsonLine({{"id", token},
                         {"type", "prompt"},
                         {"message", QStringLiteral("/openghost ") +
                                         QString::fromUtf8(QJsonDocument(request).toJson(
                                             QJsonDocument::Compact))}}));
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
    if (m_turn.isEmpty())
        return;
    if (type == QStringLiteral("message_update")) {
        const auto event = object.value("assistantMessageEvent").toObject();
        if (event.value("type").toString() != QStringLiteral("text_delta"))
            return;
        if (m_message.isEmpty()) {
            m_message = uuid();
            publish(MessageStarted{}, true);
        }
        const auto delta = event.value("delta").toString();
        fprintf(stderr, "[pi] delta: %s\n", qPrintable(delta));
        publish(MessageDelta{delta}, true);
    } else if (type == QStringLiteral("agent_settled")) {
        fprintf(stderr, "[pi] agent_settled\n");
        if (!m_message.isEmpty())
            publish(MessageCompleted{}, true);
        publish(TurnCompleted{});
        m_turn.clear();
        m_message.clear();
    }
}
void PiBackend::publish(EventPayload payload, bool message)
{
    emit sessionEvent({{m_session, ++m_seq, m_turn,
                        message ? std::optional<QString>(m_message) : std::nullopt, m_client},
                       std::move(payload)});
}
} // namespace openghost
