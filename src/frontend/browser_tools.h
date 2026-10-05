#pragma once
#include "browser_automation.h"
#include "host.h"
#include <QElapsedTimer>
#include <QQueue>
#include <memory>

namespace openghost
{
class Browser;
// Serial, UI-thread owner for all browser steps, including tabs and hand-back.
class BrowserTools final : public QObject
{
  public:
    BrowserTools(Browser &, BrowserAutomation &);
    ~BrowserTools() override;
    QVector<HostToolSchema> schemas() const;
    void run(RequestId, const HostToolRequest &);
    void cancel(RequestId);
    void turnEnded(const QString &);
    void controlChanged();
    void inputQueued(const QString &session);
    // Explicit injected clock budgets for focused ownership tests.
    struct Limits {
        int operation = 75000, queue = 90000, call = 12000, load = 30000;
    };
    void setLimits(Limits limits) { m_limits = limits; }
    static HostToolResult format(const QJsonValue &answer);
    // The step acting on `tab`'s guest now (desktop/browser.js found.running):
    // its operation identity and session, or empty while none is.
    std::pair<QString, QString> running(const QString &tab) const;

  private:
    struct Job;
    using Work = std::shared_ptr<Job>;
    struct Read {
        QString page, id, text, url;
        bool truncated = false;
    };
    Browser &m_browser;
    BrowserAutomation &m_engine;
    Limits m_limits;
    QHash<RequestId, Work> m_jobs;
    QQueue<Work> m_queue;
    Work m_active;
    QHash<QString, Read> m_reads;
    QHash<QString, QString> m_turns;
    QSet<QString> m_seen;
    quint64 m_serial = 0;
    bool m_pumping = false;
    void pump();
    bool check(const Work &, bool page = true);
    void finish(const Work &, QJsonObject);
    void fail(const Work &, const QString &code, const QString &message);
    void abandon(const Work &);
    void later(const Work &, int ms, std::function<void()>);
    void ensure(const Work &);
    void dispatch(const Work &);
    void tabs(const Work &);
    void query(const Work &, BrowserAutomation::Query, QJsonObject,
               std::function<void(QJsonObject)>);
    void observe(const Work &, const QString &note = {});
    void settle(const Work &, std::function<void()>, bool history = false);
    void quiet(const Work &, qint64 until, std::function<void()>);
    void navigate(const Work &);
    void wait(const Work &, qint64 until, double seconds);
    void read(const Work &);
    void readResult(const Work &, const Read &);
    void screenshot(const Work &);
    void scroll(const Work &);
    QJsonArray tabData() const;
    QString tabText() const;
    BrowserAutomation::Target target(const Work &) const;
};
} // namespace openghost
