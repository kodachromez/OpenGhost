#pragma once
#include <QHash>
#include <QJsonObject>
#include <QProcess>
#include <QVector>
#include <functional>

namespace openghost
{
// One `pi --mode rpc` child: JSONL framing, RPC replies correlated by ID with
// deadlines, and `/openghost` bridge commands answered by the bridge's
// `openghost:<token>` setStatus records (src/backend/pi/openghost-bridge.js).
// Bridge commands wait until Pi's get_commands lists this bridge: a missing
// extension would otherwise turn `/openghost …` into a prompt for the model.
class PiProcess final : public QObject
{
  public:
    using Done = std::function<void(const QJsonObject &)>;
    PiProcess(QString bridgePath, QObject *parent = nullptr);
    ~PiProcess() override;
    bool start(const QStringList &args, const QString &cwd, QString *error);
    bool running() const { return m_pi.state() == QProcess::Running; }
    qint64 pid() const { return m_pi.processId(); }
    bool send(const QJsonObject &command);
    void rpc(QJsonObject command, Done done, int deadline = 30000);
    // Returns the bridge token, which names its sign-in steps. A request may name
    // its own token. Without a deadline it waits as long as Pi takes (sign-in).
    QString bridge(QJsonObject request, Done done, int deadline = 0);
    // Nothing is waiting on Pi: no RPC reply and no bridge result.
    bool idle() const { return m_waiting.isEmpty() && m_bridgeQueue.isEmpty(); }
    // Ends the child: stdin closes, so Pi finishes what it read and exits; a child
    // still running after the grace period is killed. `done` runs once it is gone.
    void close(std::function<void()> done = {});
    qint64 used = 0; // when it last had work, for reaping idle children

    std::function<void(const QString &type, const QJsonObject &record)> onRecord;
    std::function<void(const QString &token, const QString &kind, const QJsonObject &value)> onStep;
    std::function<void()> onExit; // after every waiting reply has failed
    // An extension's blocking dialog (select, confirm, input, editor). True: the
    // handler answers it later (send an extension_ui_response). False, or no
    // handler: it is cancelled at once, so Pi never waits on it.
    std::function<bool(const QJsonObject &request)> onDialog;
    // extension_error records and extension notify requests ("notify").
    std::function<void(const QString &type, const QJsonObject &record)> onExtension;

  private:
    void readStdout();
    void line(const QByteArray &bytes);
    QProcess m_pi;
    QByteArray m_buffer;
    QString m_bridgePath;
    int m_next = 0;
    QHash<QString, Done> m_waiting; // RPC id, or "bridge:" + bridge token
    enum class Bridge { Unknown, Ready, Missing } m_bridge = Bridge::Unknown;
    QVector<QPair<QJsonObject, Done>> m_bridgeQueue; // until get_commands answers
    QVector<std::function<void()>> m_closed;
};
} // namespace openghost
