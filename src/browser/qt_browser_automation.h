#pragma once
#include "frontend/browser_automation.h"
#include <QHash>
#include <QPointer>
#include <QQuickItem>
#include <memory>

namespace openghost
{
// Public Qt Quick/WebEngine adapter. JS goes only to ApplicationWorld (1).
// The QML bridge has no page web-channel and never accepts page-origin messages.
class QtBrowserAutomation final : public BrowserAutomation
{
    Q_OBJECT
  public:
    using BrowserAutomation::BrowserAutomation;
    ~QtBrowserAutomation() override;
    void attach(const QString &, int, QObject *) override;
    void query(quint64, const Target &, Query, const QJsonObject &, qint64, Done) override;
    void navigate(const Target &, const QString &, const QString &) override;
    void capture(quint64, const Target &, const QJsonObject &, bool, Done) override;
    void wheel(quint64, const Target &, double, Done) override;
    void cancel(quint64 call) override;
    Q_INVOKABLE void scriptResult(const QString &tab, int incarnation, const QString &call,
                                  const QVariant &result);
    // The site profile's downloadRequested (desktop/browser.js will-download).
    Q_INVOKABLE void download(QObject *request);
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
    struct Capture;
    using Shot = std::shared_ptr<Capture>;
    QHash<QString, Guest> m_guests;
    QHash<quint64, Pending> m_pending;
    QHash<quint64, Shot> m_captures;
    QHash<QString, std::function<void(const QJsonObject &)>> m_steps;
    quint64 m_downloads = 0;
    QQuickItem *item(const Target &) const;
    void complete(quint64, QJsonObject);
    void stretched(const Shot &);
    void grab(const Shot &);
    void restoreCapture(const Shot &);
    void restored(const Shot &);
    void thaw(const Shot &);
    void finishCapture(quint64 call);
    void frames(const Shot &, int count, int ms, std::function<void()> next,
                bool restoring = false);
    void captureScript(const Shot &, const QString &body,
                       std::function<void(const QJsonObject &)> next,
                       std::function<void()> stale = {});
};
} // namespace openghost
