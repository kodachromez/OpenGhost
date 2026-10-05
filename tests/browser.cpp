#include "frontend/browser.h"
#include "backend/fake_backend.h"
#include "frontend/chat_service.h"
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace openghost;

// The browser panel's host state (browser-panel.js), without an engine: the
// tests play the guests' webview events. Real guests run in the UI smoke.
class BrowserTest final : public QObject
{
    Q_OBJECT
    static const Browser::Tab &active(const Browser &browser)
    {
        return *browser.tab(browser.activeHandle());
    }

  private slots:
    void normalizesLikeTheReference()
    {
        QCOMPARE(Browser::normalize("  "), QString());
        QCOMPARE(Browser::normalize("https://example.com/a"), QString("https://example.com/a"));
        QCOMPARE(Browser::normalize("DATA:text/html,x"), QString("DATA:text/html,x"));
        QCOMPARE(Browser::normalize("/tmp/page.html"), QString("file:///tmp/page.html"));
        QCOMPARE(Browser::normalize("C:\\pages\\a.html"), QString("file:///C:/pages/a.html"));
        QCOMPARE(Browser::normalize("localhost:8080/x"), QString("http://localhost:8080/x"));
        QCOMPARE(Browser::normalize("10.0.0.1"), QString("http://10.0.0.1"));
        QCOMPARE(Browser::normalize("example.com"), QString("https://example.com"));
        QCOMPARE(Browser::normalize("example.com?q=1"), QString("https://example.com?q=1"));
        QCOMPARE(Browser::normalize("what is qt (6)?"),
                 QString("https://www.google.com/search?q=what%20is%20qt%20(6)%3F"));
        QCOMPARE(Browser::normalize("javascript:alert(1)"),
                 QString("https://www.google.com/search?q=javascript%3Aalert(1)"));
        QCOMPARE(Browser::hostOf("https://www.example.com:8443/x"), QString("example.com"));
        QCOMPARE(Browser::hostOf("not a url"), QString());

        Browser browser;
        QCOMPARE(browser.urlParts("https://www.example.com/"),
                 QStringList({"", "example.com", ""}));
        QCOMPARE(browser.urlParts("http://localhost:8080/a%20b?x=1#top"),
                 QStringList({"http://", "localhost:8080", "/a b?x=1#top"}));
        QCOMPARE(browser.urlParts("about:blank"), QStringList());
        QCOMPARE(browser.urlParts("data:text/html,%3Cb%3E x"),
                 QStringList({"data://", "", "text/html,<b> x"}));
    }

    void pageMenuMatchesTheReference()
    {
        const auto rows = [](const QVariantMap &context) {
            QStringList out;
            for (const auto &value : Browser::menu(context)) {
                const auto row = value.toMap();
                out << (row.value("separator").toBool()
                            ? QStringLiteral("-")
                            : row.value("action").toString() +
                                  (row.value("enabled").toBool() ? "" : "(off)"));
            }
            return out;
        };
        // desktop/browser.js context-menu: link, image, editing, history, Inspect.
        QCOMPARE(rows({{"link", "https://example.com/"},
                       {"image", "https://example.com/a.png"},
                       {"editable", true},
                       {"canCopy", true},
                       {"back", true}}),
                 QStringList({"openLink", "copyLink", "-", "openImage", "copyImage", "-",
                              "cut(off)", "copy", "paste(off)", "selectAll", "-", "back",
                              "forward(off)", "reload", "-", "inspect"}));
        QCOMPARE(rows({{"selection", "words"}, {"forward", true}}),
                 QStringList({"copy", "-", "back(off)", "forward", "reload", "-", "inspect"}));
        QCOMPARE(rows({}), QStringList({"back(off)", "forward(off)", "reload", "-", "inspect"}));
        const auto first = Browser::menu({{"link", "x"}}).first().toMap();
        QCOMPARE(first.value("label").toString(), QString("Open link in new tab"));
        QCOMPARE(Browser::menu({}).last().toMap().value("label").toString(), QString("Inspect"));
    }

    void guestOpenedTabsFollowTheirOpener()
    {
        Browser browser;
        browser.setOpen(true);
        const auto first = browser.newTab("https://one.example/");
        const auto last = browser.newTab("https://three.example/");
        browser.select(first);
        const int inc = browser.tab(first)->incarnation;
        browser.openFrom(first, inc, "https://two.example/", false);
        QCOMPARE(browser.tabList().size(), 3);
        QCOMPARE(browser.tabList().at(1).url, QString("https://two.example/"));
        QCOMPARE(browser.activeHandle(), browser.tabList().at(1).handle);
        // Background: created next to its opener, guest made, selection kept.
        const auto opener = browser.activeHandle();
        browser.openFrom(opener, browser.tab(opener)->incarnation, "https://bg.example/", true);
        QCOMPARE(browser.tabList().at(2).url, QString("https://bg.example/"));
        QVERIFY(browser.tabList().at(2).view);
        QCOMPARE(browser.activeHandle(), opener);
        // A replaced guest's request is not placed by a stale opener.
        browser.openFrom(first, inc + 7, "https://stale.example/", true);
        QCOMPARE(browser.tabList().last().url, QString("https://stale.example/"));
        QCOMPARE(browser.tabList().at(browser.tabList().size() - 2).handle, last);
        browser.openFrom(first, inc, "", false);
        QCOMPARE(browser.tabList().size(), 5);
    }

    void signInHintsAreHostOnlyBoundedAndSaved()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("browser.json");
        {
            Browser browser(path);
            browser.setOpen(true);
            const auto tab = browser.newTab("https://www.example.com/login");
            const int inc = browser.tab(tab)->incarnation;
            QSignalSpy changed(&browser, &HostServices::browserChanged);
            browser.signedIn(tab, inc, "www.Example.com");
            QCOMPARE(changed.size(), 1);
            QCOMPARE(browser.snapshot().signedIn.size(), 1);
            QCOMPARE(browser.snapshot().signedIn.first().host, QString("example.com"));
            QVERIFY(browser.snapshot().signedIn.first().at > 0);
            QVERIFY(!BrowserState::signedInVerified);
            // Not a host name, an old guest or an unknown tab: nothing recorded.
            browser.signedIn(tab, inc, "evil.example/path");
            browser.signedIn(tab, inc, "");
            browser.signedIn(tab, inc + 1, "other.example");
            browser.signedIn("missing", 1, "other.example");
            QCOMPARE(browser.accounts().size(), 1);
            for (int k = 0; k < 35; ++k)
                browser.signedIn(tab, inc, QString("site%1.example").arg(k));
            QCOMPARE(browser.accounts().size(), Browser::AccountsMax);
            QCOMPARE(browser.accounts().first().host, QString("site34.example"));
            browser.signedIn(tab, inc, "site20.example"); // most recent first, once
            QCOMPARE(browser.accounts().first().host, QString("site20.example"));
            QCOMPARE(std::count_if(browser.accounts().begin(), browser.accounts().end(),
                                   [](const auto &a) { return a.host == "site20.example"; }),
                     1);
        }
        Browser restored(path);
        QCOMPARE(restored.accounts().size(), Browser::AccountsMax);
        QCOMPARE(restored.snapshot().signedIn.first().host, QString("site20.example"));
    }

    void panelOpenCloseAndWidth()
    {
        Browser browser;
        QSignalSpy reported(&browser, &HostServices::browserChanged);
        QSignalSpy address(&browser, &Browser::focusAddress);
        QVERIFY(!browser.isOpen());
        QVERIFY(browser.blank());
        // Explicit even when empty and closed; never an executable browser yet.
        auto state = *browser.browser();
        QVERIFY(!state.available);
        QCOMPARE(state.status, BrowserState::Status::Unavailable);
        QVERIFY(!state.open);
        QVERIFY(state.tabs.isEmpty());

        browser.toggle();
        QVERIFY(browser.isOpen());
        QCOMPARE(address.count(), 1); // nothing to show: the address takes the keyboard
        QCOMPARE(reported.count(), 1);
        QVERIFY(reported.last().first().value<BrowserState>().open);
        browser.setOpen(true); // no change, no report
        QCOMPARE(reported.count(), 1);
        browser.toggle();
        QVERIFY(!browser.isOpen());
        QCOMPARE(reported.count(), 2);

        // fit(): 44 % of the room, at least 360, leaving the chat 400.
        QCOMPARE(browser.widthFor(1000), 440);
        QCOMPARE(browser.widthFor(700), 360);
        browser.resize(1000, 900);
        QCOMPARE(browser.savedWidth(), 600);
        QCOMPARE(browser.widthFor(1000), 600);
        QCOMPARE(browser.widthFor(800), 400);
        browser.resize(1000, 10);
        QCOMPARE(browser.widthFor(1000), 360);
    }

    void tabsNavigationAndLoading()
    {
        Browser browser;
        browser.setOpen(true);
        QSignalSpy acts(&browser, &Browser::act);
        const QString first = browser.newTab();
        QCOMPARE(browser.activeHandle(), first);
        QCOMPARE(active(browser).state, QString("lazy"));
        QVERIFY(!active(browser).view); // a blank tab has no guest yet

        browser.go("example.com");
        auto *tab = &active(browser);
        QVERIFY(tab->view);
        QCOMPARE(tab->state, QString("loading"));
        QCOMPARE(tab->source, QString("https://example.com"));
        QCOMPARE(browser.url(), QString("https://example.com"));
        QVERIFY(!browser.ready());
        QCOMPARE(acts.count(), 0); // created on its page, not told to load it
        const int guest = tab->incarnation;

        const auto createdRevision = active(browser).revision;
        const auto createdPage = active(browser).pageId;
        browser.loadStarted(first, guest);
        QVERIFY(browser.loading());
        QCOMPARE(active(browser).revision, createdRevision + 1);
        QVERIFY(active(browser).pageId != createdPage);
        browser.domReady(first, guest);
        QCOMPARE(active(browser).state, QString("ready"));
        QVERIFY(browser.ready());
        browser.titleChanged(first, guest, "Example");
        browser.loadStopped(first, guest, "https://example.com/", "Example Domain");
        QVERIFY(!browser.loading());
        QCOMPARE(active(browser).title, QString("Example Domain"));
        QCOMPARE(browser.url(), QString("https://example.com/"));
        browser.history(first, guest, true, false);
        QVERIFY(browser.canGoBack());
        QVERIFY(!browser.canGoForward());

        // Reload while idle, stop while loading, then navigate the live guest.
        browser.reloadOrStop();
        QCOMPARE(acts.last().at(1).toString(), QString("reload"));
        browser.loadStarted(first, guest);
        browser.reloadOrStop();
        QCOMPARE(acts.last().at(1).toString(), QString("stop"));
        browser.loadStopped(first, guest, "https://example.com/", {});
        browser.go("https://example.org/page");
        QCOMPARE(acts.last().at(1).toString(), QString("load"));
        QCOMPARE(acts.last().at(2).toString(), QString("https://example.org/page"));
        browser.navigated(first, guest, "https://example.org/page", false);
        const auto revision = active(browser).revision;
        browser.navigated(first, guest, "https://example.org/page#part", true);
        QCOMPARE(active(browser).revision, revision + 1); // in-page navigation is a new page state

        // A second tab; the snapshot lists stable IDs in strip order.
        const QString second = browser.newTab("https://second.test/");
        QCOMPARE(browser.activeHandle(), second);
        auto state = browser.snapshot();
        QCOMPARE(state.tabs.size(), 2);
        QCOMPARE(state.tabs.at(0).tabId, first);
        QVERIFY(!state.tabs.at(0).active);
        QVERIFY(state.tabs.at(1).active);
        QCOMPARE(state.tabs.at(1).n, 2);
        browser.select(first);
        QCOMPARE(browser.activeHandle(), first);
        browser.close(first);
        QCOMPARE(browser.activeHandle(), second); // the neighbour takes its place
        browser.close(second);
        QVERIFY(browser.activeHandle().isEmpty());
        QVERIFY(browser.blank());
    }

    void errorsCrashesAndRetry()
    {
        Browser browser;
        browser.setOpen(true);
        browser.go("http://127.0.0.1:9/");
        const QString handle = browser.activeHandle();
        const int guest = active(browser).incarnation;
        browser.loadStarted(handle, guest);
        browser.loadFailed(handle, guest, -3, "net::ERR_ABORTED"); // replaced, not failed
        QVERIFY(browser.error().isEmpty());
        browser.loadFailed(handle, guest, -102, "net::ERR_CONNECTION_REFUSED");
        QCOMPARE(browser.error(), QString("127.0.0.1 · ERR_CONNECTION_REFUSED"));
        QCOMPARE(active(browser).state, QString("failed"));
        QVERIFY(!browser.loading());
        // A late dom-ready of the failed attempt does not revive it.
        browser.domReady(handle, guest);
        QCOMPARE(active(browser).state, QString("failed"));

        QSignalSpy acts(&browser, &Browser::act);
        browser.retry();
        QVERIFY(browser.error().isEmpty());
        QCOMPARE(acts.last().at(1).toString(), QString("reload"));
        browser.loadFailed(handle, guest, -105, QString());
        QCOMPARE(browser.error(), QString("127.0.0.1 · Error -105"));

        browser.crashed(handle, guest);
        auto tab = active(browser);
        QCOMPARE(tab.state, QString("gone"));
        QVERIFY(!tab.view);
        QCOMPARE(tab.error, QString("The browser page crashed. Open the page again."));
        QVERIFY(browser.blank());
        // The crashed guest's late events describe no page.
        const auto revision = tab.revision;
        browser.loadStarted(handle, guest);
        QCOMPARE(active(browser).revision, revision);

        browser.retry(); // recreates the guest on its page
        tab = active(browser);
        QVERIFY(tab.view);
        QCOMPARE(tab.incarnation, guest + 1);
        QCOMPARE(tab.state, QString("loading"));
        QCOMPARE(tab.source, QString("http://127.0.0.1:9/"));
        browser.domReady(handle, tab.incarnation);
        QCOMPARE(active(browser).state, QString("ready"));
    }

    void readinessDeadlineAndGuard()
    {
        Browser browser;
        browser.setReadyMs(30);
        browser.setOpen(true);
        browser.go("https://slow.test/");
        QCOMPARE(active(browser).state, QString("loading"));
        QTRY_COMPARE(active(browser).state, QString("failed"));
        // A guest that became ready in time keeps its state.
        browser.newTab("https://fast.test/");
        browser.domReady(browser.activeHandle(), active(browser).incarnation);
        QTest::qWait(60);
        QCOMPARE(active(browser).state, QString("ready"));
    }

    void tabLimitAndLazyRestore()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("browser.json");
        {
            Browser browser(path);
            browser.setOpen(true);
            browser.resize(1200, 500);
            QString kept;
            for (int i = 0; i < Browser::TabsMax; ++i)
                kept = browser.newTab(QStringLiteral("https://t%1.test/").arg(i));
            browser.select(browser.tabList().first().handle);
            const QString active = browser.activeHandle();
            const QString added = browser.newTab("https://t12.test/");
            // A manual tab beyond twelve closes the oldest other inactive one.
            QCOMPARE(browser.tabList().size(), Browser::TabsMax);
            QVERIFY(browser.tab(added));
            QVERIFY(browser.tab(active));
            QCOMPARE(browser.tabList().at(1).url, QString("https://t2.test/"));
            browser.newTab(); // evicts t0 too; blank tabs are not saved
            browser.select(browser.tabList().at(3).handle);
            browser.titleChanged(browser.activeHandle(), browser.tabList().at(3).incarnation,
                                 "Third");
            browser.save();
        }
        Browser restored(path);
        QVERIFY(restored.isOpen());
        QCOMPARE(restored.savedWidth(), 500);
        QCOMPARE(restored.tabList().size(), Browser::TabsMax - 1);
        QCOMPARE(active(restored).url, QString("https://t5.test/"));
        QCOMPARE(active(restored).title, QString("Third"));
        // Fresh handles; only the active, shown tab gets a guest.
        int views = 0;
        for (const auto &tab : restored.tabList()) {
            views += tab.view;
            if (!tab.view)
                QCOMPARE(tab.state, QString("lazy"));
        }
        QCOMPARE(views, 1);
        QVERIFY(active(restored).view);

        QFile broken(path);
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{not json");
        broken.close();
        Browser fresh(path);
        QVERIFY(!fresh.isOpen());
        QVERIFY(fresh.tabList().isEmpty());
    }

    void takeControlAndHandBack()
    {
        Browser browser;
        browser.setOpen(true);
        browser.go("https://site.test/");
        const QString handle = browser.activeHandle();
        browser.domReady(handle, active(browser).incarnation);
        QSignalSpy acts(&browser, &Browser::act);
        QSignalSpy yielded(&browser, &Browser::yieldFocus);

        browser.drive("chat-a", true);
        QVERIFY(browser.driving());
        QCOMPARE(yielded.count(), 1); // the agent's turn takes the keyboard off the page
        browser.drive("chat-a", true);
        QCOMPARE(yielded.count(), 1);
        QCOMPARE(browser.snapshot().control, BrowserState::Control::Agent);

        browser.take();
        QVERIFY(browser.user());
        QVERIFY(!browser.driving());
        QVERIFY(browser.userHas());
        QCOMPARE(browser.snapshot().control, BrowserState::Control::User);
        QCOMPARE(acts.last().at(0).toString(), handle);
        QCOMPARE(acts.last().at(1).toString(), QString("focus"));
        int woke = 0;
        browser.waitForAgent([&] { ++woke; });
        QCOMPARE(woke, 0);

        browser.handBack();
        QCOMPARE(woke, 1);
        QVERIFY(browser.driving());
        QCOMPARE(browser.lent(), handle);
        QCOMPARE(acts.last().at(1).toString(), QString("blur"));
        browser.drive("chat-a", false);
        QVERIFY(!browser.agent());
    }

    void turnEndReleasesOnlyItsChat()
    {
        Browser browser;
        browser.drive("chat-a", true);
        browser.drive("chat-b", true);
        browser.take();
        int woke = 0;
        browser.waitForAgent([&] { ++woke; });
        browser.turnEnded("chat-b");
        QVERIFY(browser.user()); // chat A still waits for the hand-back
        QCOMPARE(woke, 0);
        browser.turnEnded("chat-a");
        QCOMPARE(woke, 1);
        QVERIFY(!browser.agent());
        QCOMPARE(browser.snapshot().control, BrowserState::Control::Agent);
        // The next chat's turn starts with the agent in control again.
        browser.drive("chat-c", true);
        QVERIFY(browser.driving());
        QVERIFY(!browser.user());
        browser.turnEnded("chat-c");
        QVERIFY(!browser.agent());
    }

    void chatTurnsReportAndReleaseTheBrowser()
    {
        FakeBackend fake(nullptr, 0);
        PreferencesStore prefs({});
        Browser browser;
        ChatService chat(&fake, &prefs, nullptr, &browser);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        chat.choose({QStringLiteral("fake"), QStringLiteral("echo"), {}}, false);
        // Every start carries the panel's snapshot; the fake takes it as context.
        QVERIFY(chat.send(QStringLiteral("first")) > 0);
        QTRY_VERIFY(chat.busy() && !chat.pending());
        const QString first = chat.current().id;
        // The panel was open in another chat's turn: that chat keeps it.
        browser.drive(QStringLiteral("elsewhere"), true);
        browser.drive(first, true);
        for (int i = 0; i < 200 && chat.busy(); ++i)
            fake.advance();
        QTRY_VERIFY(!chat.busy());
        QVERIFY(browser.agent());
        browser.turnEnded(QStringLiteral("elsewhere"));
        QVERIFY(!browser.agent());

        // A stopped turn releases its chat's hold too.
        chat.newChat();
        QVERIFY(chat.send(QStringLiteral("second")) > 0);
        QTRY_VERIFY(chat.busy() && !chat.pending());
        browser.drive(chat.current().id, true);
        browser.take();
        int woke = 0;
        browser.waitForAgent([&] { ++woke; });
        chat.stop();
        QTRY_VERIFY(!chat.busy());
        QTRY_COMPARE(woke, 1);
        QVERIFY(!browser.agent());
        QVERIFY(!browser.user());
    }

    void unpublishedRunsSettleOnce()
    {
        Browser browser;
        QVERIFY(browser.tools().isEmpty());
        QSignalSpy finished(&browser, &HostServices::finished);
        browser.run(7, {});
        browser.run(8, {});
        browser.cancel(8);
        QCOMPARE(finished.count(), 0); // never inline
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().first().value<RequestId>(), RequestId(7));
        QTest::qWait(10);
        QCOMPARE(finished.count(), 1);
    }
};

QTEST_GUILESS_MAIN(BrowserTest)
#include "browser.moc"
