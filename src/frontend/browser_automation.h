#pragma once
#include <QJsonObject>
#include <QObject>
#include <functional>

namespace openghost
{
// Browser-owned engine seam. No transport, backend, DevTools or page objects in
// tool contracts. Implementations never reply inline. cancel drops completion,
// not already-issued effects. The host rechecks its lease at every continuation.
class BrowserAutomation : public QObject
{
    Q_OBJECT
  public:
    using QObject::QObject;
    struct Target {
        QString tab, page;
        int incarnation = 0;
        QString document; // isolated-world document incarnation, not a DOM revision
    };
    enum class Query { Snapshot, Has, Read, Metrics, Quiet, Reveal };
    using Done = std::function<void(QJsonObject)>;
    virtual void attach(const QString &tab, int incarnation, QObject *view) = 0;
    virtual void query(quint64 call, const Target &, Query, const QJsonObject &args, qint64 expires,
                       Done) = 0;
    virtual void navigate(const Target &, const QString &verb, const QString &url) = 0;
    virtual void capture(quint64 call, const Target &, const QJsonObject &metrics, bool full,
                         Done) = 0;
    virtual void wheel(quint64 call, const Target &, double amount, Done) = 0;
    virtual void cancel(quint64 call) = 0;
};
} // namespace openghost
