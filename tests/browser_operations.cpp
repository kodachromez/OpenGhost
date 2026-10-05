#include "browser-tools/fixtures.h"
#include "frontend/browser.h"
#include "frontend/browser_tools.h"
#include <QtTest>
using namespace openghost;

// Deliberately hostile completion timing. Real-page behavior is covered by the
// separate engine test; this seam proves the owner ignores cancelled work even
// if an implementation returns its already-issued result late.
class DelayedAutomation final : public BrowserAutomation
{
  public:
    struct Call {
        quint64 id;
        Target target;
        Query kind;
        Done done;
    };
    QVector<Call> calls;
    QStringList navigation;
    QVector<quint64> cancelled;
    void attach(const QString &, int, QObject *) override {}
    void query(quint64 id, const Target &target, Query kind, const QJsonObject &, qint64,
               Done done) override
    {
        calls.append({id, target, kind, std::move(done)});
    }
    void navigate(const Target &, const QString &verb, const QString &) override
    {
        navigation << verb;
    }
    void capture(quint64, const Target &, const QJsonObject &, bool, Done) override
    {
        QFAIL("unexpected capture");
    }
    void wheel(quint64, const Target &, double, Done) override { QFAIL("unexpected wheel"); }
    void cancel(quint64 call) override
    {
        cancelled << call;
    } // intentionally do not drop test callbacks
    static QJsonObject snapshot()
    {
        return {{"lines", QJsonArray{"text \"Fixture\""}},
                {"refs", QJsonObject{}},
                {"truncated", false},
                {"scroll",
                 QJsonObject{{"height", 600}, {"vh", 600}, {"vw", 800}, {"top", 0}, {"below", 0}}}};
    }
};
class BrowserOperationsTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<Browser> browser;
    DelayedAutomation *engine = nullptr;
    QString tab;
    RequestId serial = 0;
    QHash<RequestId, HostToolResult> results;
    RequestId start(const QString &name = "browser_snapshot", const QJsonObject &args = {},
                    const QString &session = "one")
    {
        const auto id = ++serial;
        browser->run(id, {session, "turn", QString::number(id), name, args});
        return id;
    }
    QString code(RequestId id) { return results[id].data->value("code").toString(); }
  private slots:
    void init()
    {
        results.clear();
        browser = std::make_unique<Browser>();
        auto adapter = std::make_unique<DelayedAutomation>();
        engine = adapter.get();
        browser->setAutomation(std::move(adapter));
        browser->setOpen(true);
        tab = browser->newTab("about:blank");
        browser->go("about:blank");
        const auto inc = browser->tab(tab)->incarnation;
        browser->loadStarted(tab, inc);
        browser->domReady(tab, inc);
        browser->loadStopped(tab, inc, "about:blank", "Fixture");
        connect(browser.get(), &Browser::finished, this,
                [this](RequestId id, const HostToolResult &r) {
                    QVERIFY(!results.contains(id));
                    results[id] = r;
                });
    }
    void cleanup() { browser.reset(); }
    void formatterUsesPreparedResults()
    {
        for (const auto &value : browser_tools_test::fixture("results.json")) {
            const auto row = value.toObject();
            if (row["answer"].toObject().contains("html"))
                continue; // real parser + UTF-16 engine/owner tests
            browser_tools_test::compare(BrowserTools::format(row["answer"]),
                                        browser_tools_test::body(row["expected"].toObject()));
        }
    }
    void queuedCancellationKeepsPredecessorAndLateCompletionIsIgnored()
    {
        const auto first = start();
        const auto removed = start("browser_navigate", {{"url", "data:text/html,NO"}});
        const auto third = start("browser_snapshot", {}, "two");
        QTRY_COMPARE(engine->calls.size(), 1);
        browser->handBack(); // redundant UI hand-back must not requeue a live step
        QTest::qWait(20);
        QCOMPARE(engine->calls.size(), 1);
        browser->cancel(removed);
        QTest::qWait(30);
        QCOMPARE(engine->calls.size(), 1);
        QVERIFY(engine->navigation.isEmpty());
        auto callback = engine->calls.first().done;
        callback(DelayedAutomation::snapshot());
        QTRY_COMPARE(engine->calls.size(), 2);
        QVERIFY(results.contains(first));
        browser->cancel(third);
        engine->calls.last().done(DelayedAutomation::snapshot());
        callback(DelayedAutomation::snapshot());
        QVERIFY(!results.contains(removed));
        QVERIFY(!results.contains(third));
        QCOMPARE(results.size(), 1);
    }
    void callAndQueueDeadlinesBlockLateDispatch()
    {
        browser->toolOwner()->setLimits({1000, 1000, 30, 1000});
        const auto first = start("browser_wait", {{"text", "eventually"}});
        QTRY_COMPARE(engine->calls.size(), 1);
        QTRY_VERIFY(results.contains(first));
        QCOMPARE(code(first), "timeout");
        const auto count = engine->calls.size();
        engine->calls.first().done({{"found", true}});
        QTest::qWait(160);
        QCOMPARE(engine->calls.size(), count); // no settle/next JS after timeout
        browser->toolOwner()->setLimits({1000, 50, 1000, 1000});
        const auto running = start();
        const auto queued = start("browser_navigate", {{"url", "data:text/html,NO"}});
        QTRY_VERIFY(results.contains(running) && results.contains(queued));
        QCOMPARE(code(running), "timeout");
        QCOMPARE(code(queued), "timeout");
        QVERIFY(engine->navigation.isEmpty());
    }
    void cancellationStopsNavigationAndTurnEndSuppressesCallbacks()
    {
        const auto navigating = start("browser_navigate", {{"url", "data:text/html,Issued"}});
        QTRY_VERIFY(engine->navigation.contains("load"));
        browser->cancel(navigating);
        QVERIFY(engine->navigation.contains("stop"));
        QVERIFY(!results.contains(navigating));
        const auto pending = start();
        QTRY_COMPARE(engine->calls.size(), 1);
        const auto queued = start("browser_tabs", {{"action", "new"}});
        browser->turnEnded("one");
        engine->calls.first().done(DelayedAutomation::snapshot());
        QTest::qWait(30);
        QVERIFY(!results.contains(pending));
        QVERIFY(!results.contains(queued));
        QCOMPARE(browser->tabList().size(), 1);
        const auto rejected = start("browser_click");
        browser->cancel(rejected);
        QTest::qWait(20);
        QVERIFY(!results.contains(rejected));
    }
    void staleQueuedPageClosedGuestAndGuestReplacement()
    {
        const auto first = start();
        const auto second = start();
        QTRY_COMPARE(engine->calls.size(), 1);
        browser->navigated(tab, browser->tab(tab)->incarnation, "about:blank#new", true);
        engine->calls.first().done(DelayedAutomation::snapshot());
        QTRY_VERIFY(results.contains(first) && results.contains(second));
        QCOMPARE(code(first), "stale_page");
        QCOMPARE(code(second), "stale_page");
        QCOMPARE(engine->calls.size(), 1);
        const auto closed = start();
        QTRY_COMPARE(engine->calls.size(), 2);
        browser->close(tab);
        QTRY_VERIFY(results.contains(closed));
        QCOMPARE(code(closed), "tab_gone");
        engine->calls.last().done(DelayedAutomation::snapshot());
        QCOMPARE(code(closed), "tab_gone");
    }
    void takeControlHoldsWithoutDeadlineAndMessageCancelsOnlyOwner()
    {
        browser->toolOwner()->setLimits({50, 50, 1000, 1000});
        const auto issued = start();
        QTRY_COMPARE(engine->calls.size(), 1);
        browser->take();
        const auto other = start("browser_tabs", {{"action", "close"}, {"tabId", tab}}, "two");
        engine->calls.first().done(DelayedAutomation::snapshot());
        QTest::qWait(100);
        QVERIFY(results.isEmpty());
        browser->inputQueued("one");
        QVERIFY(results.contains(issued));
        QCOMPARE(results[issued].status, HostToolResult::Status::Cancelled);
        QCOMPARE(results[issued].reason.value(), "message");
        QVERIFY(results[issued].content.isEmpty());
        QVERIFY(!results.contains(other));
        browser->toolOwner()->setLimits({1000, 1000, 1000, 1000});
        browser->handBack();
        QTRY_COMPARE(engine->calls.size(), 2);
        QCOMPARE(engine->calls.last().kind, BrowserAutomation::Query::Snapshot);
        engine->calls.last().done(DelayedAutomation::snapshot());
        QTRY_VERIFY(results.contains(other));
        QCOMPARE(results[other].status, HostToolResult::Status::HandedBack);
        QCOMPARE(browser->tabList().size(), 1); // old close never replayed
    }
    void frozenUtf16SurrogatesAndInvalidationAfterPartialReveal()
    {
        const auto first = start("browser_read");
        QTRY_COMPARE(engine->calls.size(), 1);
        engine->calls.first().done(
            {{"text", "A😀B"}, {"url", "about:blank"}, {"sourceTruncated", true}});
        QTRY_VERIFY(results.contains(first));
        const auto readId = results[first].data->value("readId");
        const auto next = start("browser_read", {{"readId", readId}, {"start", 2}});
        QTRY_VERIFY(results.contains(next));
        const auto content = std::get<HostToolResult::Text>(results[next].content.first()).text;
        QVERIFY(content.at(QString("about:blank\n\n").size()).isLowSurrogate());
        QCOMPARE(results[next].data->value("total").toInt(), 4);
        QCOMPARE(results[next].data->value("start").toInt(), 2);
        const auto oldPage = browser->tab(tab)->pageId;
        const auto navigationRevision = browser->tab(tab)->revision;
        const auto reveal = start("browser_scroll", {{"pageId", oldPage}, {"ref", 1}});
        QTRY_COMPARE(engine->calls.size(), 2);
        browser->cancel(reveal);
        QVERIFY(browser->tab(tab)->pageId != oldPage);
        QCOMPARE(browser->tab(tab)->revision, navigationRevision);
        engine->calls.last().done({{"revealed", true}});
        QTest::qWait(400);
        QCOMPARE(engine->calls.size(), 2); // no late settle or snapshot
        const auto expired = start("browser_read", {{"readId", readId}, {"start", 1}});
        QTRY_VERIFY(results.contains(expired));
        QCOMPARE(code(expired), "stale_read");
    }
};
QTEST_GUILESS_MAIN(BrowserOperationsTest)
#include "browser_operations.moc"
