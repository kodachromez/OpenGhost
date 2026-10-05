#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <optional>

namespace openghost::transport
{
// Future transport seam only. No implementation is selected by the native app.
// A wire adapter (not UI or Backend consumers) must own JSONL framing, 64-MiB
// bounds, exact string/integer RPC IDs, validation, deadlines and reverse calls.
struct Status {
    enum class State { None, Stopped, Running, Error, Exited } state = State::None;
    std::optional<qint64> pid;
    std::optional<int> exitCode;
    QString signal, error;
};
class ByteTransport : public QObject
{
    Q_OBJECT
  public:
    using QObject::QObject;
    // false = not queued; true is not backend acceptance. Bounded ordered writes.
    virtual bool write(const QByteArray &bytes) = 0;
    virtual void closeInput() = 0;
  signals:
    void received(const QByteArray &bytes);
    void statusChanged(const openghost::transport::Status &status);
    void writeFailed(const QString &message);
};
} // namespace openghost::transport
