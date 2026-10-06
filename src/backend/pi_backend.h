#pragma once
#include "backend.h"
#include <QHash>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <functional>

namespace openghost
{
// Proof-of-concept: one `pi --mode rpc --no-session` child, prompt text in,
// text_delta out. Models come from Pi's get_available_models and selection
// uses set_model. Providers, sign-in and logout go through Pi's own runtime
// via the OpenGhost bridge extension (src/backend/pi/openghost-bridge.js); Pi
// stores every credential. No tools, sessions, settings, retry or recovery.
class PiBackend final : public Backend
{
    Q_OBJECT
  public:
    explicit PiBackend(QObject *parent = nullptr);
    ~PiBackend() override;
    void request(RequestId id, const Command &command) override;
    void cancelRequest(RequestId) override {}
    void answer(RequestId, const ReverseResult &) override {}
    void browserChanged(const BrowserState &) override {}

  private:
    using Done = std::function<void(const QJsonObject &)>;
    void dispatch(RequestId id, const Command &command);
    Result execute(const Command &command);
    void startTurn(RequestId id, const Command &command);
    void rpc(QJsonObject command, Done done);
    void bridge(QJsonObject request, Done done);
    void auth(RequestId id, const QString &provider, QJsonObject request);
    void setModel(const QString &provider, const QString &model,
                  std::function<void(std::optional<Error>)> done);
    void step(const QString &token, const QString &kind, const QJsonObject &value);
    void readStdout();
    void line(const QByteArray &bytes);
    void publish(EventPayload payload, bool message = false);
    QProcess m_pi;
    QByteArray m_buffer;
    QString m_session, m_version, m_turn, m_client, m_message;
    Sequence m_seq = 0;
    int m_prompts = 0, m_rpc = 0;
    QHash<QString, Done> m_waiting; // RPC id, or "bridge:" + bridge token
    enum class Bridge { Unknown, Ready, Missing } m_bridge = Bridge::Unknown;
    QVector<QPair<QJsonObject, Done>> m_bridgeQueue; // until get_commands answers
    QTemporaryDir m_bridgeDir;
    QString m_bridgePath;
    QHash<QString, QString> m_logins; // bridge token -> provider signing in
    QHash<QString, LoginStep> m_steps; // provider -> its current sign-in step
    QSet<QString> m_cancelled;         // providers whose sign-in was cancelled
    QString m_modelProvider, m_model;  // what set_model last made Pi use
};
} // namespace openghost
