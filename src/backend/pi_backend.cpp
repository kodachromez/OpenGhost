#include "pi_backend.h"
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>
#include <cstdio>

namespace openghost
{
namespace
{
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
Error failure(const QString &code, const QString &message)
{
    return {code, message, {}, QStringLiteral("none"), false, {}};
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
    QTimer::singleShot(0, this, [this, id, command] {
        emit replied(id, execute(command));
        if (std::holds_alternative<StartTurn>(command) && !m_turn.isEmpty())
            publish(TurnStarted{});
    });
}
Result PiBackend::execute(const Command &command)
{
    if (std::holds_alternative<Initialize>(command)) {
        if (m_pi.state() == QProcess::NotRunning) {
            m_pi.start(QStringLiteral("pi"),
                       {QStringLiteral("--mode"), QStringLiteral("rpc"),
                        QStringLiteral("--no-session")});
            if (!m_pi.waitForStarted(5000))
                return failure(QStringLiteral("backend_unavailable"),
                               QStringLiteral("Cannot start pi: ") + m_pi.errorString());
            fprintf(stderr, "[pi] started pid %lld\n", static_cast<long long>(m_pi.processId()));
        }
        Initialized result;
        result.backend = BackendInfo{QStringLiteral("Pi (RPC proof of concept)"), {}, {}};
        result.capabilities.sessionRecovery = true; // required by ChatService
        return Reply{result};
    }
    if (std::holds_alternative<ModelsList>(command))
        return Reply{QVector<Model>{{QStringLiteral("pi"), QStringLiteral("pi"),
                                     QStringLiteral("Pi"), {}, false, {}, {}}}};
    if (std::holds_alternative<ProvidersList>(command))
        return Reply{QVector<Provider>{}}; // Pi owns its auth/config; no provider UI.
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
    if (type == QStringLiteral("response") && !object.value("success").toBool(true))
        fprintf(stderr, "[pi] command failed: %s\n", bytes.left(500).constData());
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
