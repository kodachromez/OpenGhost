#pragma once
#include "frontend/browser_automation.h"
#include <QHash>
#include <QPointer>
#include <QQuickItem>

namespace openghost
{
// Public Qt Quick/WebEngine adapter. JS goes only to ApplicationWorld (1).
// The QML bridge has no page web-channel and never accepts page-origin messages.
class QtBrowserAutomation final : public BrowserAutomation
{
    Q_OBJECT
  public:
    using BrowserAutomation::BrowserAutomation;
    void attach(const QString &, int, QObject *) override;
    void query(quint64, const Target &, Query, const QJsonObject &, qint64, Done) override;
    void navigate(const Target &, const QString &, const QString &) override;
    void capture(quint64, const Target &, const QJsonObject &, bool, Done) override;
    void wheel(quint64, const Target &, double, Done) override;
    void cancel(quint64 call) override { m_pending.remove(call); }
    Q_INVOKABLE void scriptResult(const QString &tab, int incarnation, const QString &call,
                                  const QVariant &result);
  signals:
    void script(const QString &tab, int incarnation, const QString &call, const QString &source);
    void navigation(const QString &tab, int incarnation, const QString &verb, const QString &url);

  private:
    struct Guest {
        int incarnation = 0;
        QPointer<QQuickItem> item;
    };
    struct Pending {
        Target target;
        Done done;
    };
    QHash<QString, Guest> m_guests;
    QHash<quint64, Pending> m_pending;
    QQuickItem *item(const Target &) const;
    void complete(quint64, QJsonObject);
};
} // namespace openghost
