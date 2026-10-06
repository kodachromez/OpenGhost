#include "pi_process.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>

namespace openghost
{
namespace
{
constexpr qsizetype MaxLine = 64 * 1024 * 1024;
constexpr int CloseGrace = 3000;
QString errorOf(const QJsonObject &reply, const QString &fallback)
{
    return reply.value("error").toString(fallback);
}
} // namespace

PiProcess::PiProcess(QString bridgePath, QObject *parent)
    : QObject(parent), m_bridgePath(std::move(bridgePath))
{
    used = QDateTime::currentMSecsSinceEpoch();
    // Pi's stderr goes to ours: visible when launched from a terminal, no window.
    m_pi.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(&m_pi, &QProcess::readyReadStandardOutput, this, &PiProcess::readStdout);
    connect(&m_pi, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        fprintf(stderr, "[pi] process error: %s\n", qPrintable(m_pi.errorString()));
    });
    connect(&m_pi, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        fprintf(stderr, "[pi] %lld exited with code %d\n", static_cast<long long>(pid()), code);
        const QJsonObject exited{{"success", false}, {"ok", false}, {"transport", true},
                                 {"error", "Pi exited."}};
        const auto waiting = std::exchange(m_waiting, {});
        for (const auto &done : waiting)
            done(exited);
        const auto queued = std::exchange(m_bridgeQueue, {});
        for (const auto &request : queued)
            request.second(exited);
        if (onExit)
            onExit();
        const auto closed = std::exchange(m_closed, {});
        for (const auto &done : closed)
            if (done)
                done();
    });
}
PiProcess::~PiProcess()
{
    // Destruction abandons every pending callback (the Backend contract).
    disconnect(&m_pi, nullptr, this, nullptr);
    if (m_pi.state() != QProcess::NotRunning) {
        m_pi.closeWriteChannel();
        if (!m_pi.waitForFinished(2000)) {
            m_pi.kill();
            m_pi.waitForFinished(1000);
        }
    }
}
bool PiProcess::start(const QStringList &args, const QString &cwd, QString *error)
{
    if (!cwd.isEmpty())
        m_pi.setWorkingDirectory(cwd);
    m_pi.start(QStringLiteral("pi"), args);
    if (!m_pi.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("Cannot start pi: ") + m_pi.errorString();
        return false;
    }
    fprintf(stderr, "[pi] started pid %lld\n", static_cast<long long>(pid()));
    m_bridge = Bridge::Unknown;
    rpc({{"type", "get_commands"}}, [this](const QJsonObject &reply) {
        const auto commands = reply.value("data").toObject().value("commands").toArray();
        const bool ready = !m_bridgePath.isEmpty() &&
                           std::any_of(commands.begin(), commands.end(), [this](const auto &c) {
                               const auto command = c.toObject();
                               return command.value("name").toString() ==
                                          QStringLiteral("openghost") &&
                                      command.value("sourceInfo").toObject().value("path").toString() ==
                                          m_bridgePath;
                           });
        m_bridge = ready ? Bridge::Ready : Bridge::Missing;
        if (!ready)
            fprintf(stderr, "[pi] OpenGhost bridge missing\n");
        const auto queued = std::exchange(m_bridgeQueue, {});
        for (const auto &request : queued)
            bridge(request.first, request.second); // Its deadline is already armed.
    });
    return true;
}
bool PiProcess::send(const QJsonObject &command)
{
    if (m_pi.state() != QProcess::Running)
        return false;
    used = QDateTime::currentMSecsSinceEpoch();
    const auto bytes = QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
    return m_pi.write(bytes) == bytes.size();
}
void PiProcess::rpc(QJsonObject command, Done done, int deadline)
{
    const auto id = QStringLiteral("og-%1").arg(++m_next);
    const auto type = command.value("type").toString();
    command.insert("id", id);
    if (!send(command)) {
        done({{"success", false}, {"ok", false}, {"transport", true},
              {"error", "Pi is not running."}});
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
QString PiProcess::bridge(QJsonObject request, Done done, int deadline)
{
    const auto token = request.value("token").toString().isEmpty()
                           ? QStringLiteral("og-bridge-%1").arg(++m_next)
                           : request.value("token").toString();
    request.insert("token", token);
    if (deadline > 0) {
        // One answer: the bridge's result or the deadline, whichever comes first.
        auto once = std::make_shared<Done>(std::move(done));
        done = [once](const QJsonObject &reply) {
            if (const auto call = std::exchange(*once, {}))
                call(reply);
        };
        QTimer::singleShot(deadline, this, [this, once, token, deadline] {
            m_waiting.remove(QStringLiteral("bridge:") + token);
            m_bridgeQueue.removeIf([&](const auto &pending) {
                return pending.first.value("token").toString() == token;
            });
            if (const auto call = std::exchange(*once, {}))
                call({{"ok", false},
                      {"timeout", true},
                      {"error", QStringLiteral("Pi's bridge did not answer within %1 seconds.")
                                    .arg(deadline / 1000)}});
        });
    }
    if (m_bridge == Bridge::Unknown && running()) {
        m_bridgeQueue.append({request, std::move(done)});
        return token;
    }
    if (m_bridge != Bridge::Ready || !running()) {
        done({{"ok", false},
              {"error", "OpenGhost's Pi bridge is not loaded, so Pi's providers, sign-in, "
                        "instructions and Retry are unavailable."}});
        return token;
    }
    m_waiting.insert(QStringLiteral("bridge:") + token, std::move(done));
    if (!send({{"id", token},
               {"type", "prompt"},
               {"message", QStringLiteral("/openghost ") +
                               QString::fromUtf8(QJsonDocument(request).toJson(
                                   QJsonDocument::Compact))}}))
        m_waiting.take(QStringLiteral("bridge:") + token)(
            {{"ok", false}, {"transport", true}, {"error", "Pi is not running."}});
    return token;
}
void PiProcess::close(std::function<void()> done)
{
    if (m_pi.state() == QProcess::NotRunning) {
        if (done)
            done();
        return;
    }
    m_closed.append(std::move(done));
    if (m_closed.size() > 1)
        return;
    m_pi.closeWriteChannel();
    QTimer::singleShot(CloseGrace, this, [this] {
        if (m_pi.state() != QProcess::NotRunning)
            m_pi.kill();
    });
}
void PiProcess::readStdout()
{
    m_buffer += m_pi.readAllStandardOutput();
    qsizetype at;
    while ((at = m_buffer.indexOf('\n')) >= 0) { // LF only
        if (at > MaxLine) { // Check complete records too, before parsing/copying.
            m_buffer.clear();
            m_pi.kill();
            return;
        }
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
void PiProcess::line(const QByteArray &bytes)
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
        if (!ok && m_waiting.contains(QStringLiteral("bridge:") + id))
            return m_waiting.take(QStringLiteral("bridge:") + id)(
                {{"ok", false}, {"error", errorOf(object, QStringLiteral("Pi refused it."))}});
        return;
    }
    if (type == QStringLiteral("extension_ui_request") &&
        object.value("method").toString() == QStringLiteral("setStatus") &&
        object.value("statusKey").toString().startsWith(QStringLiteral("openghost:"))) {
        const auto key = object.value("statusKey").toString().mid(10).split(':');
        const auto value =
            QJsonDocument::fromJson(object.value("statusText").toString().toUtf8()).object();
        if (key.size() == 2) {
            if (onStep)
                onStep(key[0], key[1], value);
        } else if (const auto done = m_waiting.take(QStringLiteral("bridge:") + key[0])) {
            done(value);
        }
        return;
    }
    if (type == QStringLiteral("extension_ui_request")) {
        const auto method = object.value("method").toString();
        if (method == QStringLiteral("select") || method == QStringLiteral("confirm") ||
            method == QStringLiteral("input") || method == QStringLiteral("editor")) {
            if (!onDialog || !onDialog(object)) {
                fprintf(stderr, "[pi] declined extension %s \"%s\"\n", qPrintable(method),
                        qPrintable(object.value("title").toString()));
                send({{"type", "extension_ui_response"},
                      {"id", object.value("id").toString()},
                      {"cancelled", true}});
            }
            return;
        }
        if (method == QStringLiteral("notify") && onExtension)
            onExtension(QStringLiteral("notify"), object);
    } else if (type == QStringLiteral("extension_error") && onExtension) {
        onExtension(type, object);
    }
    if (onRecord)
        onRecord(type, object);
}
} // namespace openghost
