#pragma once
#include "backend.h"
#include <QProcess>

namespace openghost
{
// Proof-of-concept: one `pi --mode rpc --no-session` child, prompt text in,
// text_delta out. No tools, sessions, settings, models, retry or recovery.
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
    Result execute(const Command &command);
    void readStdout();
    void line(const QByteArray &bytes);
    void publish(EventPayload payload, bool message = false);
    QProcess m_pi;
    QByteArray m_buffer;
    QString m_session, m_version, m_turn, m_client, m_message;
    Sequence m_seq = 0;
    int m_prompts = 0;
};
} // namespace openghost
