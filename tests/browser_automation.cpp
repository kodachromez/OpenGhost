#include "browser-tools/fixtures.h"
#include "browser/qt_browser_automation.h"
#include "frontend/browser.h"
#include "frontend/browser_tools.h"
#include <QGuiApplication>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QtTest>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <csignal>

using namespace openghost;
class BrowserAutomationTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<Browser> browser;
    std::unique_ptr<QQmlEngine> engine;
    QQuickWindow *window = nullptr;
    QTemporaryDir files;
    RequestId serial = 0;
    QHash<RequestId, HostToolResult> results;
    QString text(const HostToolResult &r)
    {
        return r.content.isEmpty() ? QString()
                                   : std::get<HostToolResult::Text>(r.content.first()).text;
    }
    QString code(const HostToolResult &r)
    {
        return r.data ? r.data->value("code").toString() : QString();
    }
    RequestId start(const QString &name, QJsonObject args = {}, const QString &session = "test")
    {
        const auto id = ++serial;
        browser->run(id, {session, "turn", QString::number(id), name, args});
        return id;
    }
    HostToolResult run(const QString &name, QJsonObject args = {})
    {
        const auto id = start(name, args);
        if (!QTest::qWaitFor([&] { return results.contains(id); }, 10000)) {
            QTest::qFail(qPrintable("No result: " + name), __FILE__, __LINE__);
            return {};
        }
        return results.take(id);
    }
    QVariant js(const QString &source, int world = 0)
    {
        QSignalSpy spy(window, SIGNAL(evaluated(QVariant)));
        QMetaObject::invokeMethod(window, "evaluate", Q_ARG(QVariant, source),
                                  Q_ARG(QVariant, world));
        if (spy.isEmpty() && !spy.wait(5000)) {
            QTest::qFail("No JS result", __FILE__, __LINE__);
            return {};
        }
        return spy.first().first();
    }
    static QString json(const QString &value)
    {
        return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact))
            .mid(1)
            .chopped(1);
    }
    QString page(const QString &html, const QString &name = "page.html")
    {
        QFile file(files.filePath(name));
        if (!file.open(QIODevice::WriteOnly))
            qFatal("fixture write failed");
        file.write(html.toUtf8());
        file.close();
        return QUrl::fromLocalFile(file.fileName()).toString();
    }
    // Loopback HTTP: path -> {headers, body}. Unknown paths are 404.
    void serve(QTcpServer &server, QHash<QByteArray, QPair<QByteArray, QByteArray>> routes)
    {
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, &server, [&server, routes] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, &server, [socket, routes] {
                    const auto line = socket->readAll().split('\n').first().split(' ');
                    const auto path = line.value(1).split('?').first();
                    const auto route = routes.value(path, {"Content-Type: text/plain", ""});
                    socket->write((routes.contains(path) ? "HTTP/1.1 200 OK\r\n"
                                                         : "HTTP/1.1 404 Not Found\r\n") +
                                  route.first +
                                  "\r\nContent-Length: " + QByteArray::number(route.second.size()) +
                                  "\r\nConnection: close\r\n\r\n" + route.second);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    void open(const QString &html)
    {
        const auto result = run("browser_navigate", {{"url", page(html)}});
        QVERIFY2(!result.isError, qPrintable(text(result)));
    }
    // Helpers for the input tools: refs come only from a real snapshot.
    int refOf(const HostToolResult &snap, const QString &needle)
    {
        const auto refs = snap.data->value("refs").toObject();
        for (auto it = refs.begin(); it != refs.end(); ++it)
            if (it.value().toString().contains(needle))
                return it.key().toInt();
        return 0;
    }
    QJsonValue pageOf(const HostToolResult &r) { return r.data->value("pageId"); }
    QJsonArray events()
    {
        return QJsonDocument::fromJson(js("JSON.stringify(events)").toString().toUtf8()).array();
    }
    QQuickItem *guest()
    {
        return qobject_cast<QQuickItem *>(window->property("activeGuest").value<QObject *>());
    }
    bool inGuest(QQuickItem *item)
    {
        for (; item; item = item->parentItem())
            if (item == guest())
                return true;
        return false;
    }
    static constexpr const char *recorder =
        "<script>window.events=[];for(const n of ['pointerdown','mousedown','mouseup','click',"
        "'dblclick','keydown','keypress','keyup','beforeinput','input','change','wheel'])"
        "addEventListener(n,e=>events.push({type:n,trusted:e.isTrusted,target:e.target.id||"
        "e.target.nodeName,detail:e.detail,x:e.clientX,y:e.clientY,key:e.key,code:e.code,"
        "keyCode:e.keyCode,ctrl:e.ctrlKey,shift:e.shiftKey,alt:e.altKey,meta:e.metaKey,"
        "inputType:e.inputType,data:e.data,deltaY:e.deltaY,deltaX:e.deltaX,deltaMode:e.deltaMode,"
        "bubbles:e.bubbles}),{capture:true,passive:true});</script>";
  private slots:
    void init()
    {
        results.clear();
        browser =
            std::make_unique<Browser>(QString(), QString(), nullptr, files.filePath("downloads"));
        browser->setAutomation(std::make_unique<QtBrowserAutomation>());
        browser->setOpen(true);
        connect(browser.get(), &Browser::finished, this,
                [this](RequestId id, const HostToolResult &result) { results[id] = result; });
        engine = std::make_unique<QQmlEngine>();
        QQmlComponent component(engine.get(),
                                QUrl::fromLocalFile(QFINDTESTDATA("browser-tools/engine.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(component.createWithInitialProperties(
            {{"browser", QVariant::fromValue(browser.get())}}));
        QVERIFY(window);
        QVERIFY(QTest::qWaitFor([&] { return window->isExposed(); }));
    }
    void cleanup()
    {
        browser->turnEnded("test");
        browser->turnEnded("other");
        delete window;
        window = nullptr;
        engine.reset();
        browser.reset();
    }
    void exactPreparedSchemasAndInputPageRequirement()
    {
        const auto actual = browser->tools();
        const auto expected = browser_tools_test::schemas();
        QCOMPARE(actual.size(), 11);
        QCOMPARE(actual.size(), expected.size());
        QVERIFY(browser->snapshot().available);
        QCOMPARE(browser->snapshot().status, BrowserState::Status::Empty);
        for (int i = 0; i < actual.size(); ++i) {
            QCOMPARE(actual[i].name, expected[i].name); // reference order
            QCOMPARE(actual[i].description, expected[i].description);
            QCOMPARE(actual[i].parameters, expected[i].parameters);
        }
        open("<h1>Page</h1>");
        for (const auto *name : {"browser_click", "browser_press", "browser_type", "browser_select",
                                 "browser_scroll"}) {
            const auto refused = run(name, {{"key", "a"}, {"text", "a"}, {"ref", 1}});
            QCOMPARE(code(refused), "stale_page");
            QCOMPARE(text(refused), "Error: Pass pageId from a fresh snapshot with browser input.");
        }
    }
    void snapshotIsolationRefsFramesAndBounds()
    {
        open("<title>Isolated</title><h1>Hello</h1><label>Password<input type=password "
             "value=secret></label>"
             "<button>First</button><div id=host></div><iframe srcdoc='<button>Frame "
             "button</button>'></iframe>"
             "<iframe "
             "src='data:text/html,Foreign'></"
             "iframe><script>window.__og={snapshot:()=>({lines:['FORGED']})};"
             "document.getElementById('host').attachShadow({mode:'open'}).innerHTML='<button>"
             "Shadow button</button>';</script>"
             "<div style='height:1000px'></div><button>Below</button>");
        auto snap = run("browser_snapshot");
        QVERIFY2(!snap.isError, qPrintable(text(snap)));
        QVERIFY(text(snap).contains("value=••••"));
        QVERIFY(!text(snap).contains("secret"));
        QVERIFY(!text(snap).contains("FORGED"));
        QVERIFY(text(snap).contains("Shadow button"));
        QVERIFY(text(snap).contains("Frame button"));
        QVERIFY(text(snap).contains("content not readable"));
        QVERIFY(!text(snap).contains("Below"));
        auto full = run("browser_snapshot", {{"full", true}});
        QVERIFY(text(full).contains("Below"));
        QCOMPARE(full.data->value("pageId"), snap.data->value("pageId"));
        QCOMPARE(js("window.__og.snapshot().lines[0]").toString(), "FORGED");
        auto again = run("browser_snapshot");
        QCOMPARE(again.data->value("refs"), snap.data->value("refs"));
        QVERIFY(js("typeof window.__og.pageId").toString() == "undefined");
        js("document.body.innerHTML=Array.from({length:4000},(_,i)=>'<button>Button "
           "'+i+'</button>').join('')");
        auto bounded = run("browser_snapshot", {{"full", true}});
        QVERIFY(bounded.data->value("truncated").toBool());
        QVERIFY(text(bounded).size() < 42000);
    }
    void domReadinessBeforeSlowResourcesAndIsolatedDocumentVersion()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, &server, [socket] {
                    const auto request = socket->readAll();
                    if (!request.startsWith("GET / "))
                        return; // /slow deliberately never answers
                    const QByteArray body =
                        "<title>DOM ready</title><h1>Ready before image</h1><img src='/slow'>";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                                  QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        browser->newTab(QString("http://127.0.0.1:%1/").arg(server.serverPort()));
        QVERIFY(QTest::qWaitFor([&] { return browser->ready() && browser->loading(); }));
        auto snapshot = run("browser_snapshot");
        QVERIFY2(!snapshot.isError, qPrintable(text(snapshot)));
        QVERIFY(text(snapshot).contains("The page is still loading."));
        QVERIFY(text(snapshot).contains("Ready before image"));
        QCOMPARE(js("typeof window.__ogDocument").toString(), "undefined");
        const auto previous = snapshot.data->value("pageId");
        js("history.pushState({},'', '#changed')");
        QTRY_VERIFY(browser->tab(browser->activeHandle())->pageId != previous.toString());
        QCOMPARE(code(run("browser_snapshot", {{"pageId", previous}})), "stale_page");
        browser->close(browser->activeHandle());
    }
    void tabsNavigationHistoryAndFailures()
    {
        auto empty = run("browser_tabs", {{"action", "list"}});
        QCOMPARE(text(empty), "Tabs:\nNo tabs are open.");
        QCOMPARE(empty.data->keys(), QStringList{"tabs"});
        auto blank = run("browser_tabs", {{"action", "new"}});
        QVERIFY(!blank.data->contains("pageId"));
        const auto first = blank.data->value("tabId").toString();
        open("<title>First</title><h1>One</h1>");
        const auto snap = run("browser_snapshot");
        const auto old = snap.data->value("pageId");
        auto next = run("browser_navigate",
                        {{"url", page("<title>Second</title><h1>Two</h1>", "second.html")}});
        QVERIFY2(!next.isError, qPrintable(text(next)));
        QVERIFY(next.data->value("pageId") != old);
        QCOMPARE(code(run("browser_snapshot", {{"pageId", old}})), "stale_page");
        auto back = run("browser_navigate", {{"url", "back"}});
        QVERIFY(text(back).contains("First"));
        auto forward = run("browser_navigate", {{"url", "forward"}});
        QVERIFY(text(forward).contains("Second"));
        auto reload = run("browser_navigate", {{"url", "reload"}});
        QVERIFY2(!reload.isError, qPrintable(text(reload)));
        QVERIFY(reload.data->value("pageId") != forward.data->value("pageId"));
        QCOMPARE(code(run("browser_tabs", {{"action", "switch"}, {"tab", 1}})), "invalid_request");
        QCOMPARE(code(run("browser_tabs", {{"action", "list"}, {"tabId", "missing"}})), "tab_gone");
        auto fresh = run(
            "browser_tabs",
            {{"action", "new"}, {"url", page("<h1>New</h1>", "new.html")}, {"pageId", "ignored"}});
        QVERIFY2(!fresh.isError, qPrintable(text(fresh)));
        QVERIFY(fresh.data->contains("tabs"));
        auto switched =
            run("browser_tabs", {{"action", "switch"}, {"tabId", first}, {"pageId", "ignored"}});
        QVERIFY2(!switched.isError, qPrintable(text(switched)));
        QVERIFY(!switched.data->contains("tabs"));
        for (int i = 2; i < 12; ++i)
            QVERIFY(!run("browser_tabs", {{"action", "new"}}).isError);
        QCOMPARE(code(run("browser_tabs", {{"action", "new"}})), "tab_limit");
        QCOMPARE(browser->tabList().size(), 12);
        QVERIFY(browser->tab(first));
        auto closed =
            run("browser_tabs", {{"action", "close"}, {"tabId", browser->activeHandle()}});
        QCOMPARE(closed.data->keys(), QStringList{"tabs"});
        QTcpServer port;
        QVERIFY(port.listen(QHostAddress::LocalHost));
        const auto url = QString("http://127.0.0.1:%1/").arg(port.serverPort());
        port.close();
        auto failed = run("browser_navigate", {{"url", url}});
        QCOMPARE(code(failed), "navigation_failed");
    }
    void waitsCancellationQueueTargetsAndTurnEnd()
    {
        open("<h1>Wait</h1><div "
             "id=host></"
             "div><script>document.getElementById('host').attachShadow({mode:'open'}).innerHTML='<"
             "p>Inside shadow</p>'</script>");
        QVERIFY(!run("browser_wait", {{"text", "inside SHADOW"}, {"seconds", 0.5}}).isError);
        const auto missing = run("browser_wait", {{"text", "missing %2"}, {"seconds", 0.5}});
        QCOMPARE(code(missing), "wait_timeout");
        QVERIFY(text(missing).contains("\"missing %2\" did not appear"));
        QVERIFY(!run("browser_wait", {{"seconds", 0.5}}).isError);
        const auto first = browser->activeHandle();
        const auto blocked = start("browser_wait", {{"seconds", 10}});
        const auto cancelled = start("browser_navigate", {{"url", "data:text/html,WRONG"}});
        const auto other = start("browser_snapshot", {}, "other");
        QTest::qWait(80);
        browser->cancel(cancelled);
        QVERIFY(!results.contains(other));
        browser->cancel(blocked);
        QVERIFY(QTest::qWaitFor([&] { return results.contains(other); }));
        QVERIFY(!text(results[other]).contains("WRONG"));
        QVERIFY(!results.contains(cancelled));
        QVERIFY(!results.contains(blocked));
        const auto switching = start("browser_wait", {{"seconds", 10}});
        const auto queued = start("browser_snapshot");
        QTest::qWait(80);
        browser->newTab({}, false, false);
        browser->select(first); // away and back still invalidates the active lease
        QVERIFY(QTest::qWaitFor(
            [&] { return results.contains(switching) && results.contains(queued); }));
        QCOMPARE(code(results[switching]), "stale_tab");
        QCOMPARE(code(results[queued]), "stale_tab");
        const auto ending = start("browser_wait", {{"seconds", 10}});
        const auto behind = start("browser_navigate", {{"url", "data:text/html,TOO-LATE"}});
        QTest::qWait(50);
        browser->turnEnded("test");
        QTest::qWait(550);
        QVERIFY(!results.contains(ending));
        QVERIFY(!results.contains(behind));
        QVERIFY(!browser->url().contains("TOO-LATE"));
    }
    void frozenReadAndRealParser()
    {
        open("<title>Read title</title><nav>Excluded navigation</nav><h1>Heading</h1><p>Body</p>"
             "<ul><li>Item</li></ul><pre><span class=line>a()</span><span "
             "class=line>b()</span></pre>"
             "<table><tr><td>A</td><td>B</td></tr></table><a "
             "href='https://example.invalid/x'>Link</a>"
             "<span style='display:none'>Hidden text</span><input value='DO NOT "
             "READ'><textarea>SECRET</textarea>"
             "<div id=host></div><script>host.attachShadow({mode:'open'}).innerHTML='<p>Shadow "
             "excluded</p>';</script>"
             "<p>" +
             QString(41000, 'z') + "😀tail</p>");
        auto read = run("browser_read");
        QVERIFY2(!read.isError, qPrintable(text(read)));
        const auto body = text(read);
        QVERIFY(body.contains("# Read title"));
        QVERIFY(body.contains("# Heading"));
        QVERIFY(body.contains("- Item"));
        QVERIFY(body.contains("```\na()\nb()\n```"));
        QVERIFY(body.contains("A | B"));
        QVERIFY(body.contains("[Link](https://example.invalid/x)"));
        QVERIFY(body.contains("Hidden text"));
        QVERIFY(!body.contains("DO NOT READ"));
        QVERIFY(!body.contains("SECRET"));
        QVERIFY(!body.contains("Shadow excluded"));
        QVERIFY(!body.contains("Excluded navigation"));
        QCOMPARE(read.data->value("end").toInt(), 40000);
        QVERIFY(read.data->value("hasMore").toBool());
        const auto id = read.data->value("readId");
        js("document.body.textContent='Changed DOM'");
        auto continued = run("browser_read", {{"readId", id}, {"start", 40000}});
        QVERIFY(text(continued).contains("😀tail"));
        QVERIFY(!text(continued).contains("Changed DOM"));
        QCOMPARE(continued.data->value("readId"), id);
        auto repeat = run("browser_read", {{"readId", id}, {"start", 0}});
        QCOMPARE(text(repeat), body);
        auto beyond = run("browser_read", {{"readId", id}, {"start", 999999}});
        QCOMPARE(beyond.data->value("end").toInt(), 999999);
        auto refreshed = run("browser_read");
        QVERIFY(text(refreshed).contains("Changed DOM"));
        QCOMPARE(code(run("browser_read", {{"readId", id}, {"start", 0}})), "stale_read");
        auto reload = run("browser_navigate", {{"url", "reload"}});
        QVERIFY(!reload.isError);
        QCOMPARE(
            code(run("browser_read", {{"readId", refreshed.data->value("readId")}, {"start", 1}})),
            "stale_read");
    }
    void boundedReadSourceAndSmallFullScreenshot()
    {
        open("<title>Small %3</title><body "
             "style='margin:0;background:rgb(10,200,30)'><h1>Fits</h1>");
        auto shot = run("browser_screenshot", {{"full_page", true}});
        QVERIFY2(!shot.isError, qPrintable(text(shot)));
        QVERIFY(text(shot).contains("the page from the top: Small %3"));
        QVERIFY(!shot.data->value("truncated").toBool());
        js("document.body.innerHTML='<p>'+ 'x'.repeat(4*1024*1024+200)+'</p>'");
        auto read = run("browser_read");
        QVERIFY2(!read.isError, qPrintable(text(read)));
        QVERIFY(read.data->value("sourceTruncated").toBool());
        QVERIFY(read.data->value("total").toInt() <= 4 * 1024 * 1024);
        QCOMPARE(read.data->value("end").toInt(), 40000);
        QVERIFY(text(read).contains("Source HTML truncated at 4 Mi UTF-16 code units."));
    }
    void viewportPixelsAndRefScroll()
    {
        open("<title>Pixels</title><body style='margin:0;background:rgb(10,200,30)'><div "
             "style='height:2400px'></div><button>Bottom</button>"
             "<script>window.wheels=[];addEventListener('wheel',e=>wheels.push({trusted:e."
             "isTrusted,delta:e.deltaY}),{passive:true});</script>");
        QTest::qWait(100);
        auto shot = run("browser_screenshot");
        QVERIFY2(!shot.isError, qPrintable(text(shot)));
        QCOMPARE(shot.content.size(), 2);
        const auto image = std::get<HostToolResult::Image>(shot.content[1]);
        auto bytes = QByteArray::fromBase64(image.dataUrl.section(',', 1).toLatin1());
        auto pixels = QImage::fromData(bytes, "JPEG");
        QVERIFY(!pixels.isNull());
        QCOMPARE(pixels.width(), 1280);
        QCOMPARE(pixels.width(), shot.data->value("width").toInt());
        QCOMPARE(pixels.height(), shot.data->value("height").toInt());
        const auto color = pixels.pixelColor(100, 100);
        QVERIFY2(std::abs(color.green() - 200) < 6, qPrintable(color.name()));
        QVERIFY(!shot.data->value("truncated").toBool());
        QCOMPARE(js("scrollY").toInt(), 0);
        const auto full = run("browser_snapshot", {{"full", true}});
        const auto refs = full.data->value("refs").toObject();
        QVERIFY(!refs.isEmpty());
        const auto ref = refs.keys().first().toInt();
        auto revealed =
            run("browser_scroll", {{"pageId", full.data->value("pageId")}, {"ref", ref}});
        QVERIFY2(!revealed.isError, qPrintable(text(revealed)));
        QVERIFY(text(revealed).contains("Bottom"));
        QVERIFY(revealed.data->value("scroll").toObject()["top"].toInt() > 0);
        QVERIFY(revealed.data->value("pageId") != full.data->value("pageId"));
        QCOMPARE(
            code(run("browser_scroll", {{"pageId", full.data->value("pageId")}, {"ref", ref}})),
            "stale_page");
        QCOMPARE(code(run("browser_scroll",
                          {{"pageId", revealed.data->value("pageId")}, {"ref", 999999}})),
                 "stale_ref");
    }
    // Three 1000 px bands; the test window's viewport is 1400×600.
    QString bands()
    {
        return "<title>Tall</title><body style='margin:0'>"
               "<div style='height:1000px;background:rgb(10,200,30)'></div>"
               "<div style='height:1000px;background:rgb(20,40,220)'></div>"
               "<div style='height:1000px;background:rgb(220,30,30)'></div>"
               "<script>window.events=[];addEventListener('resize',()=>events.push('resize'));"
               "</script>";
    }
    QQuickItem *guest() const
    {
        return qobject_cast<QQuickItem *>(window->property("activeGuest").value<QObject *>());
    }
    QObject *cover() const
    {
        return guest() ? guest()->property("captureCover").value<QObject *>() : nullptr;
    }
    static bool near(QColor color, int r, int g, int b)
    {
        return std::abs(color.red() - r) < 24 && std::abs(color.green() - g) < 24 &&
               std::abs(color.blue() - b) < 24;
    }
    void fullPageFromTheTopWithoutDisturbingThePanel()
    {
        open(bands());
        js("scrollTo(0,1500)");
        QTRY_COMPARE(js("scrollY").toInt(), 1500);
        QTest::qWait(100);
        // Sample what the window shows while the guest is stretched.
        QImage during;
        QTimer watch;
        connect(&watch, &QTimer::timeout, this, [&] {
            if (during.isNull() && guest() && guest()->height() > window->height() + 1) {
                QVERIFY(cover());
                during = window->grabWindow();
            }
        });
        watch.start(5);
        auto shot = run("browser_screenshot", {{"full_page", true}});
        watch.stop();
        QVERIFY2(!shot.isError, qPrintable(text(shot)));
        QVERIFY(text(shot).contains("Screenshot of the page from the top: Tall, 1280×2194;"));
        QCOMPARE(shot.data->value("pageWidth").toInt(), 1400);
        QCOMPARE(shot.data->value("pageHeight").toInt(), 2400); // four viewports
        QCOMPARE(shot.data->value("height").toInt(), 2194);
        QVERIFY(shot.data->value("truncated").toBool()); // 3000 px of content
        QVERIFY(std::abs(shot.data->value("scale").toDouble() - 1280.0 / 1400) < 1e-9);
        const auto image = std::get<HostToolResult::Image>(shot.content[1]);
        const auto pixels = QImage::fromData(
            QByteArray::fromBase64(image.dataUrl.section(',', 1).toLatin1()), "JPEG");
        QCOMPARE(pixels.size(), QSize(1280, 2194));
        const double scale = 1280.0 / 1400;
        QVERIFY2(near(pixels.pixelColor(100, int(300 * scale)), 10, 200, 30),
                 qPrintable(pixels.pixelColor(100, int(300 * scale)).name()));
        QVERIFY(near(pixels.pixelColor(100, int(1500 * scale)), 20, 40, 220));
        QVERIFY(near(pixels.pixelColor(100, int(2300 * scale)), 220, 30, 30));
        // The visible panel kept the scrolled page (blue) while the guest drew
        // from the top (green) at the capture size.
        QVERIFY2(!during.isNull(), "the guest was never stretched");
        const auto shown = during.pixelColor(during.width() / 2, during.height() / 2);
        QVERIFY2(near(shown, 20, 40, 220), qPrintable(shown.name()));
        // Every temporary page and view change is undone before the result.
        QCOMPARE(guest()->height(), window->height());
        QCOMPARE(js("innerHeight").toInt(), 600);
        QCOMPARE(js("scrollY").toInt(), 1500);
        QTRY_VERIFY(!cover());
        // Viewport capture still starts at the scroll position.
        auto viewport = run("browser_screenshot");
        QVERIFY(!viewport.data->value("truncated").toBool());
        QCOMPARE(viewport.data->value("pageHeight").toInt(), 600);
        QCOMPARE(js("scrollY").toInt(), 1500);
    }
    void coveredGuestCapturesOnlyItsOwnPixels()
    {
        open(bands());
        window->setProperty("covered", true);
        for (const bool full : {false, true}) {
            auto shot = run("browser_screenshot", {{"full_page", full}});
            QVERIFY2(!shot.isError, qPrintable(text(shot)));
            const auto image = std::get<HostToolResult::Image>(shot.content[1]);
            const auto pixels = QImage::fromData(
                QByteArray::fromBase64(image.dataUrl.section(',', 1).toLatin1()), "JPEG");
            QVERIFY(near(pixels.pixelColor(100, 100), 10, 200, 30));
            if (full)
                QVERIFY(near(pixels.pixelColor(100, pixels.height() - 20), 220, 30, 30));
        }
        window->setProperty("covered", false);
        QTRY_VERIFY(!cover());
    }
    void fullPageCancellationPageChangeAndRendererLossRestore()
    {
        const auto stretched = [&] {
            return QTest::qWaitFor(
                [&] { return guest() && guest()->height() > window->height() + 1; }, 5000);
        };
        open(bands());
        js("scrollTo(0,700)");
        QTRY_COMPARE(js("scrollY").toInt(), 700);
        const auto cancelled = start("browser_screenshot", {{"full_page", true}});
        QVERIFY(stretched());
        QVERIFY(cover());
        browser->cancel(cancelled);
        QTRY_COMPARE(guest()->height(), window->height());
        QTRY_VERIFY(!cover());
        QTRY_COMPARE(js("scrollY").toInt(), 700);
        QTest::qWait(200);
        QVERIFY(!results.contains(cancelled)); // no late image after cancellation
        // Cancelled as soon as the panel is covered, before the guest changes.
        const auto early = start("browser_screenshot", {{"full_page", true}});
        QVERIFY(QTest::qWaitFor([&] { return cover(); }, 5000));
        browser->cancel(early);
        QTRY_VERIFY(!cover());
        QTRY_COMPARE(js("scrollY").toInt(), 700);
        QCOMPARE(guest()->height(), window->height());
        QTest::qWait(200);
        QVERIFY(!results.contains(early));
        // A page replaced mid-capture fails; its own scroll position is kept.
        const auto replaced = start("browser_screenshot", {{"full_page", true}});
        QVERIFY(stretched());
        js("location.href=" + json(page(bands(), "other.html")));
        QVERIFY(QTest::qWaitFor([&] { return results.contains(replaced); }, 10000));
        QCOMPARE(code(results[replaced]), "stale_page");
        QTRY_COMPARE(guest()->height(), window->height());
        QTRY_VERIFY(!cover());
        QTRY_COMPARE(js("location.pathname.endsWith('other.html')").toBool(), true);
        QCOMPARE(js("scrollY").toInt(), 0);
        // Renderer loss mid-capture settles once and leaves no cover behind.
        js("scrollTo(0,900)");
        const auto lost = start("browser_screenshot", {{"full_page", true}});
        QVERIFY2(stretched(), qPrintable(results.contains(lost) ? text(results[lost]) : ""));
        QVERIFY(cover());
        QCOMPARE(::kill(pid_t(guest()->property("renderProcessPid").toLongLong()), SIGKILL), 0);
        QVERIFY(QTest::qWaitFor([&] { return results.contains(lost); }, 10000));
        QVERIFY2(QStringList({"guest_crashed", "tab_gone"}).contains(code(results[lost])),
                 qPrintable(code(results[lost])));
        QTRY_VERIFY(!cover());
        auto recovered = run("browser_screenshot", {{"full_page", true}});
        QVERIFY2(!recovered.isError, qPrintable(text(recovered)));
        QTRY_VERIFY(!cover());
        QCOMPARE(guest()->height(), window->height());
    }
    void downloadsSavedUniquelyAndAttributedToTheirStep()
    {
        QTcpServer server;
        serve(server, {{"/",
                        {"Content-Type: text/html",
                         "<title>Files</title><a id=d href='/notes.txt' download>notes</a>"}},
                       {"/notes.txt",
                        {"Content-Type: text/plain\r\nContent-Disposition: attachment; "
                         "filename=notes.txt",
                         "downloaded bytes"}}});
        const auto root = QString("http://127.0.0.1:%1/").arg(server.serverPort());
        QVERIFY(!run("browser_navigate", {{"url", root}}).isError);
        QSignalSpy toasts(browser.get(), &Browser::downloaded);
        const QDir saved(files.filePath("downloads"));
        // Started while browser_wait runs on this guest: that step's download.
        const auto waiting = start("browser_wait", {{"seconds", 2.5}});
        QTest::qWait(300);
        js("document.getElementById('d').click()");
        QVERIFY(QTest::qWaitFor([&] { return results.contains(waiting); }, 10000));
        const auto waited = results.take(waiting);
        QVERIFY2(!waited.isError, qPrintable(text(waited)));
        const auto listed = waited.data->value("downloads").toArray();
        QCOMPARE(listed.size(), 1);
        const auto file = saved.absoluteFilePath("notes.txt");
        QCOMPARE(listed[0].toObject()["file"].toString(), file);
        QVERIFY(!listed[0].toObject()["operationId"].toString().isEmpty());
        QVERIFY(text(waited).contains("\nDownloaded: " + file + "\n"));
        QFile bytes(file);
        QVERIFY(bytes.open(QIODevice::ReadOnly));
        QCOMPARE(bytes.readAll(), QByteArray("downloaded bytes"));
        QTRY_COMPARE(toasts.size(), 1);
        QCOMPARE(toasts.first().first().toString(), QString("notes.txt"));
        // Outside a step: saved beside the first without replacing it, and
        // never claimed by a later step.
        js("document.getElementById('d').click()");
        QTRY_COMPARE(toasts.size(), 2);
        QCOMPARE(toasts.last().first().toString(), QString("notes (1).txt"));
        QVERIFY(QFileInfo::exists(saved.absoluteFilePath("notes (1).txt")));
        auto later = run("browser_snapshot");
        QCOMPARE(later.data->value("downloads").toArray().size(), 0);
        QVERIFY(!text(later).contains("Downloaded:"));
    }
    void pageMenuPopupsAndGuestOpenedTabs()
    {
        const auto target = page("<title>Target</title><h1>Target</h1>", "target.html");
        open("<title>Opener</title><a id=l href='" + target +
             "' style='position:absolute;left:0;top:0;width:200px;height:40px'>Target link</a>");
        const auto opener = browser->activeHandle();
        // A right click on the link opens the reference's page menu.
        QTest::mouseClick(window, Qt::RightButton, {}, QPoint(30, 20));
        auto *menu = guest()->findChild<QObject *>("browserPageMenu");
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QStringList labels;
        for (int k = 0; k < menu->property("count").toInt(); ++k) {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, item),
                                      Q_ARG(int, k));
            labels << (item ? item->property("text").toString() : QString());
        }
        QCOMPARE(labels, QStringList({"Open link in new tab", "Copy link address", "", "Back",
                                      "Forward", "Reload", "", "Inspect"}));
        QMetaObject::invokeMethod(menu, "close");
        QMetaObject::invokeMethod(guest(), "pageAction", Q_ARG(QVariant, "openLink"));
        QCOMPARE(browser->tabList().size(), 2);
        QCOMPARE(browser->tabList().at(1).url, target);
        QCOMPARE(browser->activeHandle(), browser->tabList().at(1).handle);
        browser->select(opener);
        QTRY_COMPARE(window->property("activeGuest").value<QObject *>(),
                     static_cast<QObject *>(guest()));
        // window.open without features: a panel tab; a script URL: refused.
        js("window.open(" + json(target + "?tab") + ",'_blank')");
        QTRY_COMPARE(browser->tabList().size(), 3);
        QCOMPARE(browser->tabList().at(1).url, target + "?tab");
        browser->select(opener);
        js("window.open('javascript:1','_blank')");
        QTest::qWait(300);
        QCOMPARE(browser->tabList().size(), 3);
        // A popup with window features: its own 520×700 window, not a tab.
        const auto before = QGuiApplication::topLevelWindows().size();
        js("window.open(" + json(target + "?popup") + ",'p','width=300,height=200')");
        QQuickWindow *popup = nullptr;
        QVERIFY(QTest::qWaitFor([&] {
            for (auto *top : QGuiApplication::topLevelWindows())
                if (top->objectName() == "browserPopup")
                    popup = qobject_cast<QQuickWindow *>(top);
            return popup;
        }));
        QCOMPARE(QGuiApplication::topLevelWindows().size(), before + 1);
        QCOMPARE(popup->size(), QSize(520, 700));
        auto *popupView = popup->property("view").value<QObject *>();
        QTRY_COMPARE(popupView->property("url").toUrl().toString(), target + "?popup");
        QTRY_COMPARE(popup->title(), QString("Target"));
        QCOMPARE(browser->tabList().size(), 3);
        // The page closing its popup closes the window.
        QPointer<QQuickWindow> closing = popup;
        QMetaObject::invokeMethod(popupView, "runJavaScript", Q_ARG(QString, "window.close()"));
        QTRY_VERIFY(!closing);
    }
    void signInHintsComeOnlyFromTheIsolatedObserver()
    {
        QTcpServer server;
        serve(server, {{"/",
                        {"Content-Type: text/html",
                         "<title>Login</title><form action='/' method=get><input "
                         "type=password name=p><button id=b>Sign in</button></form>"}}});
        const auto root = QString("http://127.0.0.1:%1/").arg(server.serverPort());
        QVERIFY(!run("browser_navigate", {{"url", root}}).isError);
        // The page's own world has neither the channel nor its library.
        QCOMPARE(js("typeof qt + typeof QWebChannel").toString(), QString("undefinedundefined"));
        QCOMPARE(js("typeof qt.webChannelTransport", 1).toString(), QString("object"));
        // An empty password is not a sign-in.
        js("document.getElementById('b').addEventListener('click',e=>e.preventDefault());"
           "document.getElementById('b').click()");
        QTest::qWait(300);
        QVERIFY(browser->accounts().isEmpty());
        QSignalSpy changed(browser.get(), &HostServices::browserChanged);
        js("document.querySelector('input').value='secret';document.getElementById('b').click()");
        QTRY_COMPARE(browser->accounts().size(), 1);
        QCOMPARE(browser->accounts().first().host, QString("127.0.0.1"));
        QVERIFY(!changed.isEmpty());
        const auto state = browser->snapshot();
        QCOMPARE(state.signedIn.size(), 1);
        QVERIFY(!state.signedInVerified);
        // Once per document: another submission adds nothing new.
        js("document.getElementById('b').click()");
        QTest::qWait(300);
        QCOMPARE(browser->accounts().size(), 1);
    }
    void handBackNeverReplaysAndCrashRecovery()
    {
        open("<h1>Original</h1>");
        const auto wait = start("browser_wait", {{"seconds", 10}});
        const auto navigation = start("browser_navigate", {{"url", "data:text/html,WRONG"}});
        QTest::qWait(80);
        browser->take();
        browser->go(page("<title>User page</title><h1>User page</h1>", "user.html"));
        QVERIFY(QTest::qWaitFor([&] {
            return !browser->loading() &&
                   browser->tab(browser->activeHandle())->title == "User page";
        }));
        const auto document = js("window.__ogDocument.key", 1).toString();
        QTRY_COMPARE(browser->tab(browser->activeHandle())->document, document);
        browser->handBack();
        QVERIFY(QTest::qWaitFor(
            [&] { return results.contains(wait) && results.contains(navigation); }));
        for (const auto id : {wait, navigation}) {
            QCOMPARE(results[id].status, HostToolResult::Status::HandedBack);
            QVERIFY2(text(results[id]).contains("User page"), qPrintable(text(results[id])));
            QVERIFY(!text(results[id]).contains("WRONG"));
        }
        auto *guest = window->property("activeGuest").value<QObject *>();
        QVERIFY(guest);
        const auto waiting = start("browser_wait", {{"seconds", 10}});
        QTest::qWait(80);
        const auto pid = guest->property("renderProcessPid").toLongLong();
        QVERIFY(pid > 0);
        QCOMPARE(::kill(pid_t(pid), SIGKILL), 0);
        QVERIFY(QTest::qWaitFor([&] { return results.contains(waiting); }));
        QCOMPARE(code(results[waiting]), "guest_crashed");
        auto recovered = run("browser_snapshot");
        QVERIFY2(!recovered.isError, qPrintable(text(recovered)));
        QVERIFY(text(recovered).contains("User page"));
    }
    void clickTrustedRefCoordinatesDoubleAndFocusReturn()
    {
        open(QString("<title>Click</title><body style='margin:0'>"
                     "<button id=go style='position:absolute;left:20px;top:10px;width:200px;"
                     "height:40px'>Go</button>"
                     "<div id=area style='position:absolute;left:300px;top:200px;width:100px;"
                     "height:100px;background:#ccc'></div>") +
             recorder);
        window->setProperty("shield", true); // the production driving shield
        window->contentItem()->forceActiveFocus();
        const auto snap = run("browser_snapshot");
        const int go = refOf(snap, "button \"Go\"");
        QVERIFY(go);
        auto clicked = run("browser_click", {{"pageId", pageOf(snap)}, {"ref", go}});
        QVERIFY2(!clicked.isError, qPrintable(text(clicked)));
        QCOMPARE(clicked.status, HostToolResult::Status::Ok);
        QVERIFY(text(clicked).startsWith("Page: Click\nURL: "));
        QCOMPARE(clicked.data->value("coverage").toString(), "viewport-dom-heuristic");
        QVERIFY(pageOf(clicked) != pageOf(snap)); // input revises the observation
        auto seen = events();
        QStringList kinds;
        for (const auto &e : seen)
            kinds << e.toObject()["type"].toString();
        QCOMPARE(kinds, QStringList({"pointerdown", "mousedown", "mouseup", "click"}));
        for (const auto &e : seen) {
            QVERIFY(e.toObject()["trusted"].toBool());
            QCOMPARE(e.toObject()["target"].toString(), "go");
        }
        // Left-centre: x = left + min(width/2, 40), y = vertical centre.
        QCOMPARE(seen[3].toObject()["x"].toInt(), 60);
        QCOMPARE(seen[3].toObject()["y"].toInt(), 30);
        QCOMPARE(seen[3].toObject()["detail"].toInt(), 1);
        // giveBack(): the keyboard returns to where it was; the page is lent.
        QVERIFY(!inGuest(window->activeFocusItem()));
        QCOMPARE(browser->lent(), browser->activeHandle());
        QCOMPARE(code(run("browser_click", {{"pageId", pageOf(snap)}, {"ref", go}})), "stale_page");
        js("events=[]");
        auto twice = run("browser_click",
                         {{"pageId", pageOf(clicked)}, {"x", 350}, {"y", "250"}, {"double", true}});
        QVERIFY2(!twice.isError, qPrintable(text(twice)));
        kinds.clear();
        QList<int> details;
        for (const auto &e : events()) {
            kinds << e.toObject()["type"].toString();
            details << e.toObject()["detail"].toInt();
            QVERIFY(e.toObject()["trusted"].toBool());
            QCOMPARE(e.toObject()["target"].toString(), "area");
        }
        QCOMPARE(kinds, QStringList({"pointerdown", "mousedown", "mouseup", "click", "pointerdown",
                                     "mousedown", "mouseup", "click", "dblclick"}));
        QCOMPARE(details, QList<int>({0, 1, 1, 1, 0, 2, 2, 2, 2}));
        // Separate steps never join into a double click.
        js("events=[]");
        auto one = run("browser_click", {{"pageId", pageOf(twice)}, {"x", 350}, {"y", 250}});
        auto two = run("browser_click", {{"pageId", pageOf(one)}, {"x", 350}, {"y", 250}});
        QVERIFY(!two.isError);
        for (const auto &e : events())
            QVERIFY(e.toObject()["type"].toString() != "dblclick");
        const auto bad = run("browser_click", {{"pageId", pageOf(two)}, {"x", "left"}, {"y", 1}});
        QCOMPARE(code(bad), "browser_error");
        QCOMPARE(text(bad), "Error: Pass ref from the snapshot, or x and y in page pixels");
        QCOMPARE(code(run("browser_click", {{"pageId", pageOf(two)}, {"ref", 999}})), "stale_ref");
        js("events=[]");
        QVERIFY(events().isEmpty());
    }
    void clickCoveredMovedAndFinalNavigation()
    {
        const auto next =
            page("<title>Next</title><h1>Arrived</h1>" + QString(recorder), "next.html");
        open(QString("<title>Guarded</title><body style='margin:0'>"
                     "<button id=under style='position:absolute;left:0;top:0;width:200px;"
                     "height:50px'>Under</button><div id=cover style='position:absolute;left:0;"
                     "top:0;width:300px;height:60px;background:red'></div>"
                     "<button id=mover style='position:absolute;left:0;top:100px;width:200px;"
                     "height:40px'>Mover</button>"
                     "<a id=link href='") +
             next +
             "' style='position:absolute;left:0;top:200px'>Next page</a>"
             "<button id=nav ondblclick='' onclick=\"location.href='" +
             next +
             "'\" "
             "style='position:absolute;left:0;top:300px;width:200px;height:40px'>Leave</button>" +
             recorder);
        auto snap = run("browser_snapshot");
        // Covered: describe the cover, send nothing.
        js("document.getElementById('cover').setAttribute('role','button');"
           "document.getElementById('cover').textContent='Banner'");
        snap = run("browser_snapshot");
        const int under = refOf(snap, "Under");
        QVERIFY(under);
        js("events=[]");
        auto covered = run("browser_click", {{"pageId", pageOf(snap)}, {"ref", under}});
        QCOMPARE(code(covered), "element_covered");
        QCOMPARE(text(covered),
                 QString("Error: Element [%1] is covered by button \"Banner\". No click was sent.")
                     .arg(under));
        QVERIFY(events().isEmpty());
        // Moved during the open panel's 420 ms pointer delay: refused, no click.
        snap = run("browser_snapshot");
        const int mover = refOf(snap, "Mover");
        QVERIFY(mover);
        js("events=[]");
        const auto moving = start("browser_click", {{"pageId", pageOf(snap)}, {"ref", mover}});
        QTest::qWait(200);
        js("document.getElementById('mover').style.top='400px'");
        QVERIFY(QTest::qWaitFor([&] { return results.contains(moving); }));
        QCOMPARE(code(results[moving]), "stale_target");
        QCOMPARE(text(results[moving]),
                 "Error: The click target moved or is covered. Take a new snapshot.");
        QVERIFY(events().isEmpty());
        // A final click may navigate; its observation follows the new page.
        snap = run("browser_snapshot");
        const int link = refOf(snap, "Next page");
        auto followed = run("browser_click", {{"pageId", pageOf(snap)}, {"ref", link}});
        QVERIFY2(!followed.isError, qPrintable(text(followed)));
        QVERIFY(text(followed).startsWith("Page: Next\n"));
        QVERIFY(text(followed).contains("Arrived"));
        // Navigation on the first click of a double click stops the second.
        open(QString("<title>Leave</title><body style='margin:0'><button id=nav "
                     "onclick=\"location.href='") +
             next +
             "'\" style='position:absolute;left:0;top:0;width:200px;height:40px'>Leave</button>" +
             recorder);
        snap = run("browser_snapshot");
        const int nav = refOf(snap, "Leave");
        js("events=[]");
        auto doubled =
            run("browser_click", {{"pageId", pageOf(snap)}, {"ref", nav}, {"double", true}});
        QCOMPARE(code(doubled), "stale_page");
        QTRY_VERIFY(browser->url().endsWith("next.html"));
        QTRY_VERIFY(js("typeof events").toString() == "object");
        for (const auto &e : events())
            QVERIFY2(e.toObject()["type"].toString() != "mousedown",
                     "second press reached the new page");
    }
    void typeInsertsTextClearsAppendsAndSubmits()
    {
        const auto done = page("<title>Sent</title><h1>Submitted</h1>", "sent.html");
        open(QString("<title>Form</title><body style='margin:0'><form action='") + done +
             "'><input id=field name=q value=old style='position:absolute;left:0;top:0;"
             "width:300px;height:30px'><input id=secret type=password style='position:absolute;"
             "left:0;top:60px;width:300px;height:30px'><button style='position:absolute;"
             "left:0;top:120px'>Send</button></form>" +
             recorder);
        window->contentItem()->forceActiveFocus();
        auto snap = run("browser_snapshot");
        const int field = refOf(snap, "textbox");
        QVERIFY(field);
        js("events=[]");
        const QString unicode = QString::fromUtf8("héllo 😀 world");
        auto typed =
            run("browser_type", {{"pageId", pageOf(snap)}, {"ref", field}, {"text", unicode}});
        QVERIFY2(!typed.isError, qPrintable(text(typed)));
        QCOMPARE(js("document.getElementById('field').value").toString(), unicode);
        QVERIFY(text(typed).contains("value=\"" + unicode + "\""));
        // Focus click, Control+A (no text), then one IME-style insertion: no
        // per-character keys, every event trusted.
        QStringList kinds;
        for (const auto &v : events()) {
            const auto e = v.toObject();
            QVERIFY2(e["trusted"].toBool(), qPrintable(e["type"].toString()));
            kinds << e["type"].toString();
            if (e["type"] == "keydown") {
                QCOMPARE(e["code"].toString(), "KeyA");
                QCOMPARE(e["keyCode"].toInt(), 65);
                QVERIFY(e["ctrl"].toBool());
            }
            if (e["type"] == "beforeinput") {
                QCOMPARE(e["inputType"].toString(), "insertText");
                QCOMPARE(e["data"].toString(), unicode);
            }
        }
        QVERIFY2(kinds == QStringList({"pointerdown", "mousedown", "mouseup", "click", "keydown",
                                       "keyup", "beforeinput", "input"}),
                 qPrintable(kinds.join(',')));
        QVERIFY(!inGuest(window->activeFocusItem()));
        // Focus emulation: the keyboard left the guest, but the page saw no
        // blur/change and still acts focused, as with the reference debugger.
        QVERIFY(js("document.hasFocus()").toBool());
        QCOMPARE(js("document.activeElement.id").toString(), "field");
        // Append without clearing, at the page's focus: the lent keyboard comes back first.
        snap = run("browser_snapshot");
        auto appended =
            run("browser_type", {{"pageId", pageOf(snap)}, {"text", "!"}, {"clear", false}});
        QVERIFY2(!appended.isError, qPrintable(text(appended)));
        QCOMPARE(js("document.getElementById('field').value").toString(), unicode + "!");
        // Empty text clears with Delete.
        js("events=[]");
        auto emptied = run("browser_type", {{"pageId", pageOf(appended)}, {"text", ""}});
        QVERIFY(!emptied.isError);
        QCOMPARE(js("document.getElementById('field').value").toString(), QString());
        bool deleted = false;
        for (const auto &v : events())
            deleted |= v.toObject()["inputType"] == "deleteContentForward" &&
                       v.toObject()["trusted"].toBool();
        QVERIFY(deleted);
        // A password is typed but never shown back.
        snap = run("browser_snapshot");
        const int secret = refOf(snap, "textbox \"secret\"") ? refOf(snap, "textbox \"secret\"")
                                                             : refOf(snap, "•");
        Q_UNUSED(secret)
        const auto refs = snap.data->value("refs").toObject();
        int password = 0;
        for (auto it = refs.begin(); it != refs.end(); ++it)
            if (it.key().toInt() != field && it.value().toString().startsWith("textbox"))
                password = it.key().toInt();
        QVERIFY(password);
        auto hidden =
            run("browser_type", {{"pageId", pageOf(snap)}, {"ref", password}, {"text", "hunter2"}});
        QVERIFY(!hidden.isError);
        QVERIFY(text(hidden).contains("value=••••"));
        QVERIFY(!text(hidden).contains("hunter2"));
        // Submit presses Enter after the text; the navigation is observed.
        snap = run("browser_snapshot");
        auto sent =
            run("browser_type",
                {{"pageId", pageOf(snap)}, {"ref", field}, {"text", "query"}, {"submit", true}});
        QVERIFY2(!sent.isError, qPrintable(text(sent)));
        QVERIFY2(text(sent).startsWith("Page: Sent\n"), qPrintable(text(sent)));
        QVERIFY(text(sent).contains("sent.html?q=query"));
    }
    void typeAndPressAtPageFocusWithoutAppKeyboard()
    {
        open(QString("<body style='margin:0'><input id=field>") + recorder);
        window->contentItem()->forceActiveFocus();
        js("document.getElementById('field').focus()"); // the page's own focus
        auto snap = run("browser_snapshot");
        QVERIFY(browser->lent().isEmpty()); // nothing lent: no app focus change
        js("events=[]");
        auto typed =
            run("browser_type", {{"pageId", pageOf(snap)}, {"text", "abc"}, {"clear", false}});
        QVERIFY2(!typed.isError, qPrintable(text(typed)));
        QCOMPARE(js("document.getElementById('field').value").toString(), "abc");
        auto pressed = run("browser_press", {{"pageId", pageOf(typed)}, {"key", "Backspace"}});
        QVERIFY(!pressed.isError);
        QCOMPARE(js("document.getElementById('field').value").toString(), "ab");
        for (const auto &v : events())
            QVERIFY(v.toObject()["trusted"].toBool());
        QVERIFY(!inGuest(window->activeFocusItem()));
        QVERIFY(js("document.hasFocus()").toBool());
    }
    void typeFieldMovedRefusesAndCancelSendsNothing()
    {
        open(QString("<body style='margin:0'><input id=field style='position:absolute;left:0;"
                     "top:0;width:300px;height:30px'>") +
             recorder);
        auto snap = run("browser_snapshot");
        const int field = refOf(snap, "textbox");
        js("events=[]");
        const auto moving =
            start("browser_type", {{"pageId", pageOf(snap)}, {"ref", field}, {"text", "x"}});
        QTest::qWait(200);
        js("document.getElementById('field').style.top='300px'");
        QVERIFY(QTest::qWaitFor([&] { return results.contains(moving); }));
        QCOMPARE(code(results[moving]), "stale_target");
        QCOMPARE(text(results[moving]),
                 "Error: The field moved or is covered. Take a new snapshot.");
        QVERIFY(events().isEmpty());
        snap = run("browser_snapshot");
        const auto cancelled =
            start("browser_type", {{"pageId", pageOf(snap)}, {"ref", field}, {"text", "late"}});
        QTest::qWait(200);          // inside the pointer delay
        browser->cancel(cancelled); // partial input invalidates the observation
        snap = run("browser_snapshot");
        js("events=[]");
        const auto ended =
            start("browser_type", {{"pageId", pageOf(snap)}, {"ref", field}, {"text", "ended"}});
        QTest::qWait(200);
        browser->turnEnded("test");
        QTest::qWait(700);
        QVERIFY(!results.contains(cancelled));
        QVERIFY(!results.contains(ended));
        QCOMPARE(js("document.getElementById('field').value").toString(), QString());
        QVERIFY(events().isEmpty());
    }
    void selectMatchesTextValueAndDispatchesInputChange()
    {
        open(
            QString("<body style='margin:0'><select id=size><option value=s>Small</option>"
                    "<option value=m>  Medium   size </option><option value=xl>Extra large</option>"
                    "</select><button id=plain>Plain</button>") +
            recorder);
        auto snap = run("browser_snapshot");
        const int size = refOf(snap, "combobox");
        QVERIFY(size);
        js("events=[]");
        auto chosen = run("browser_select",
                          {{"pageId", pageOf(snap)}, {"ref", size}, {"option", "medium SIZE"}});
        QVERIFY2(!chosen.isError, qPrintable(text(chosen)));
        QVERIFY(text(chosen).startsWith("Chose \"Medium size\".\nPage: "));
        QCOMPARE(js("document.getElementById('size').value").toString(), "m");
        QStringList kinds;
        for (const auto &v : events()) {
            kinds << v.toObject()["type"].toString();
            QVERIFY(v.toObject()["bubbles"].toBool());
            QVERIFY(!v.toObject()["trusted"].toBool()); // as the reference's dispatchEvent
        }
        QCOMPARE(kinds, QStringList({"input", "change"}));
        QCOMPARE(js("document.activeElement.id").toString(), "size");
        auto byValue =
            run("browser_select", {{"pageId", pageOf(chosen)}, {"ref", size}, {"option", "XL"}});
        QVERIFY(text(byValue).startsWith("Chose \"Extra large\"."));
        auto partial =
            run("browser_select", {{"pageId", pageOf(byValue)}, {"ref", size}, {"option", "mal"}});
        QVERIFY(text(partial).startsWith("Chose \"Small\"."));
        auto missing =
            run("browser_select", {{"pageId", pageOf(partial)}, {"ref", size}, {"option", "Huge"}});
        QCOMPARE(code(missing), "browser_error");
        QCOMPARE(text(missing), QString("Error: No option like \"Huge\" in [%1]. Options: Small | "
                                        "Medium size | Extra large")
                                    .arg(size));
        snap = run("browser_snapshot");
        const int plain = refOf(snap, "Plain");
        auto wrong =
            run("browser_select", {{"pageId", pageOf(snap)}, {"ref", plain}, {"option", "x"}});
        QCOMPARE(text(wrong), QString("Error: [%1] is not a dropdown list. Click it and then click "
                                      "the option you need.")
                                  .arg(plain));
        // The failed choice already ran in the page: its observation is spent.
        QCOMPARE(
            code(run("browser_select", {{"pageId", pageOf(snap)}, {"ref", size}, {"option", "x"}})),
            "stale_page");
        snap = run("browser_snapshot");
        QCOMPARE(
            code(run("browser_select", {{"pageId", pageOf(snap)}, {"ref", 999}, {"option", "x"}})),
            "stale_ref");
    }
    void pressKeysModifiersRepeatAndNavigationBarrier()
    {
        const auto next =
            page("<title>Pressed</title><h1>After enter</h1>" + QString(recorder), "pressed.html");
        open(QString("<body style='margin:0'><input id=field style='position:absolute;left:0;"
                     "top:0;width:300px;height:30px'>") +
             recorder);
        auto snap = run("browser_snapshot");
        auto focused =
            run("browser_click", {{"pageId", pageOf(snap)}, {"ref", refOf(snap, "textbox")}});
        QVERIFY(!focused.isError);
        js("events=[]");
        auto pressed = run("browser_press", {{"pageId", pageOf(focused)}, {"key", "Shift+a"}});
        QVERIFY2(!pressed.isError, qPrintable(text(pressed)));
        QCOMPARE(js("document.getElementById('field').value").toString(), "a");
        auto down = events().first().toObject();
        QCOMPARE(down["type"].toString(), "keydown");
        QVERIFY(down["trusted"].toBool());
        QCOMPARE(down["key"].toString(), "a");
        QCOMPARE(down["code"].toString(), "KeyA");
        QCOMPARE(down["keyCode"].toInt(), 65);
        QVERIFY(down["shift"].toBool());
        // Control chords: the reference's literal key, no character.
        const auto chords = [&](const QString &combo) {
            js("events=[]");
            auto r = run("browser_press", {{"pageId", pageOf(pressed)}, {"key", combo}});
            pressed = r;
            QStringList out;
            for (const auto &v : events()) {
                const auto e = v.toObject();
                if (!e["trusted"].toBool())
                    out << "UNTRUSTED";
                out << e["type"].toString() + ":" + e["key"].toString() + ":" +
                           e["code"].toString() + ":" + QString::number(e["keyCode"].toInt());
            }
            return out.join(' ');
        };
        // Exact matches with keyOf()'s fields:
        QCOMPARE(chords("ctrl+q"), "keydown:q:KeyQ:81 keyup:q:KeyQ:81");
        QCOMPARE(chords("Meta+b"), "keydown:b:KeyB:66 keyup:b:KeyB:66");
        QCOMPARE(chords("Control+Space"), "keydown: :Space:32 keyup: :Space:32");
        // Recorded divergences (browser-tools-port.md): through QKeyEvent,
        // WebEngine derives a chord's letter case and punctuation code/keyCode
        // itself, and modified Enter also gets its keypress. Reference values:
        // "A", "X", "x"; "/" with code "" and keyCode 47; no keypress/change.
        QCOMPARE(chords("Control+A"), "keydown:a:KeyA:65 keyup:a:KeyA:65");
        QCOMPARE(chords("Alt+X"), "keydown:x:KeyX:88 keyup:x:KeyX:88");
        QCOMPARE(chords("Control+Shift+x"), "keydown:X:KeyX:88 keyup:X:KeyX:88");
        QCOMPARE(chords("ctrl+/"), "keydown:/:Slash:191 keyup:/:Slash:191");
        QCOMPARE(chords(QString::fromUtf8("Control+é")),
                 QString::fromUtf8("keydown:é::0 keyup:é::0"));
        QCOMPARE(chords("Control+Enter"),
                 "keydown:Enter:Enter:13 keypress:Enter:Enter:13 change:::0 keyup:Enter:Enter:13");
        QCOMPARE(chords("Tab"), "keydown:Tab:Tab:9 keyup:Tab:Tab:9"); // focus moves on
        QVERIFY(js("document.activeElement.id").toString() != "field");
        QCOMPARE(js("document.getElementById('field').value").toString(), "a");
        auto focusedAgain =
            run("browser_click", {{"pageId", pageOf(pressed)}, {"x", 250}, {"y", 15}});
        pressed = focusedAgain;
        // Unmodified characters: exact key/code/text; keyCode 0 for
        // non-alphanumerics (the reference's is charCodeAt, here 47).
        QCOMPARE(chords("/"), "keydown:/::0 keypress:/::47 beforeinput:::0 input:::0 keyup:/::0");
        QCOMPARE(chords("7"), "keydown:7:Digit7:55 keypress:7:Digit7:55 beforeinput:::0 "
                              "input:::0 keyup:7:Digit7:55");
        QCOMPARE(chords("Q"), "keydown:Q:KeyQ:81 keypress:Q:KeyQ:81 beforeinput:::0 input:::0 "
                              "keyup:Q:KeyQ:81");
        QCOMPARE(chords("space"), "keydown: :Space:32 keypress: :Space:32 beforeinput:::0 "
                                  "input:::0 keyup: :Space:32");
        QCOMPARE(js("document.getElementById('field').value").toString(), "a/7Q ");
        QCOMPARE(chords("Escape"), "keydown:Escape:Escape:27 keyup:Escape:Escape:27");
        js("document.getElementById('field').value='a'");
        js("events=[]");
        auto repeated = run("browser_press",
                            {{"pageId", pageOf(pressed)}, {"key", "ArrowLeft"}, {"times", 2.6}});
        QVERIFY(!repeated.isError);
        int downs = 0, presses = 0;
        for (const auto &v : events()) {
            const auto e = v.toObject();
            downs += e["type"] == "keydown";
            presses += e["type"] == "keypress";
            if (e["type"] == "keydown") {
                QCOMPARE(e["code"].toString(), "ArrowLeft");
                QCOMPARE(e["keyCode"].toInt(), 37);
            }
        }
        QCOMPARE(downs, 3);   // round(2.6)
        QCOMPARE(presses, 0); // rawKeyDown: no text
        js("events=[]");
        auto many =
            run("browser_press",
                {{"pageId", pageOf(repeated)}, {"key", "Control+Shift+Meta+Alt+X"}, {"times", 50}});
        QVERIFY(!many.isError);
        downs = 0;
        for (const auto &v : events()) {
            const auto e = v.toObject();
            if (e["type"] != "keydown")
                continue;
            ++downs;
            QVERIFY(e["ctrl"].toBool() && e["shift"].toBool() && e["meta"].toBool() &&
                    e["alt"].toBool());
            QCOMPARE(e["code"].toString(), "KeyX");
        }
        QCOMPARE(downs, 20);
        QCOMPARE(js("document.getElementById('field').value").toString(), "a");
        QCOMPARE(text(run("browser_press", {{"pageId", pageOf(many)}, {"key", " + "}})),
                 "Error: key is empty");
        QCOMPARE(text(run("browser_press", {{"pageId", pageOf(many)}, {"key", "Hyper+a"}})),
                 "Error: Unknown modifier Hyper. Use Control, Alt, Shift or Meta.");
        QCOMPARE(text(run("browser_press", {{"pageId", pageOf(many)}, {"key", "F5"}})),
                 "Error: Unknown key F5. Use Enter, Tab, Escape, Backspace, Delete, Space, arrows, "
                 "PageUp, PageDown, Home, End or a single character.");
        // A navigating key stops the remaining repetitions.
        js(QString("addEventListener('keydown',e=>{if(e.key==='Enter')location.href='%1'})")
               .arg(next));
        auto leaving =
            run("browser_press", {{"pageId", pageOf(many)}, {"key", "Enter"}, {"times", 3}});
        QCOMPARE(code(leaving), "stale_page");
        QTRY_VERIFY(browser->url().endsWith("pressed.html"));
        QTRY_COMPARE(js("typeof events").toString(), "object");
        QVERIFY(events().isEmpty());
        // The final repetition may navigate.
        snap = run("browser_snapshot");
        QVERIFY(!snap.isError);
    }
    void scrollWheelExactDirectionAndDelta()
    {
        open(QString("<title>Tall</title><body style='margin:0;height:20000px'>") + recorder);
        window->setProperty("shield", true);
        auto snap = run("browser_snapshot");
        const double vh = js("visualViewport.height").toDouble();
        const double vw = js("visualViewport.width").toDouble();
        auto down = run("browser_scroll",
                        {{"pageId", pageOf(snap)}, {"direction", "down"}, {"amount", 0.5}});
        QVERIFY2(!down.isError, qPrintable(text(down)));
        auto wheel = events().last().toObject();
        QCOMPARE(wheel["type"].toString(), "wheel");
        QVERIFY(wheel["trusted"].toBool());
        QCOMPARE(wheel["deltaY"].toDouble(), vh * 0.5);
        QCOMPARE(wheel["deltaX"].toDouble(), 0.0);
        QCOMPARE(wheel["deltaMode"].toInt(), 0);
        QCOMPARE(wheel["x"].toDouble(), std::floor(vw / 2));
        QCOMPARE(wheel["y"].toDouble(), std::floor(vh / 2));
        QCOMPARE(js("scrollY").toDouble(), vh * 0.5);
        QCOMPARE(down.data->value("scroll").toObject()["top"].toDouble(), vh * 0.5);
        QVERIFY(pageOf(down) != pageOf(snap));
        QCOMPARE(code(run("browser_scroll", {{"pageId", pageOf(snap)}})), "stale_page");
        js("events=[]");
        auto up = run("browser_scroll",
                      {{"pageId", pageOf(down)}, {"direction", "UP"}, {"amount", 0.25}});
        QVERIFY(!up.isError);
        QCOMPARE(events().last().toObject()["deltaY"].toDouble(), -vh * 0.25);
        QCOMPARE(js("scrollY").toDouble(), vh * 0.25);
        js("events=[]");
        auto standard = run("browser_scroll", {{"pageId", pageOf(up)}});
        QCOMPARE(events().last().toObject()["deltaY"].toDouble(), vh * 0.8);
        js("events=[]");
        auto capped = run("browser_scroll", {{"pageId", pageOf(standard)}, {"amount", 50}});
        QCOMPARE(events().last().toObject()["deltaY"].toDouble(), vh * 10);
        js("events=[]");
        auto floor = run("browser_scroll",
                         {{"pageId", pageOf(capped)}, {"amount", -3}, {"direction", "up"}});
        QVERIFY(!floor.isError);
        QCOMPARE(events().last().toObject()["deltaY"].toDouble(), -vh * 0.1);
        QCOMPARE(js("scrollY").toDouble(), vh * (0.25 + 0.8 + 10 - 0.1));
    }
    void inputRendererLossAndTakeControl()
    {
        open(QString("<body style='margin:0'><input id=field style='position:absolute;left:0;"
                     "top:0;width:300px;height:30px'>") +
             recorder);
        auto snap = run("browser_snapshot");
        const int field = refOf(snap, "textbox");
        const auto taken =
            start("browser_type",
                  {{"pageId", pageOf(snap)}, {"ref", field}, {"text", "secret"}, {"submit", true}});
        QTest::qWait(200);
        browser->take();
        QTest::qWait(600);
        QVERIFY(!results.contains(taken));
        QCOMPARE(js("document.getElementById('field').value").toString(), QString());
        browser->handBack();
        QVERIFY(QTest::qWaitFor([&] { return results.contains(taken); }));
        QCOMPARE(results[taken].status, HostToolResult::Status::HandedBack);
        QCOMPARE(js("document.getElementById('field').value").toString(), QString());
        QVERIFY(!text(results[taken]).contains("secret"));
        snap = run("browser_snapshot");
        js("events=[]");
        const auto crashing = start("browser_click", {{"pageId", pageOf(snap)}, {"ref", field}});
        QTest::qWait(150);
        const auto pid = guest()->property("renderProcessPid").toLongLong();
        QVERIFY(pid > 0);
        QCOMPARE(::kill(pid_t(pid), SIGKILL), 0);
        QVERIFY(QTest::qWaitFor([&] { return results.contains(crashing); }));
        QCOMPARE(code(results[crashing]), "guest_crashed");
        auto recovered = run("browser_snapshot");
        QVERIFY2(!recovered.isError, qPrintable(text(recovered)));
        QCOMPARE(code(run("browser_click", {{"pageId", pageOf(snap)}, {"ref", field}})),
                 "stale_page");
    }
};
int main(int argc, char **argv)
{
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    BrowserAutomationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "browser_automation.moc"
