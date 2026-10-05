#include "browser-tools/fixtures.h"
#include "browser/qt_browser_automation.h"
#include "frontend/browser.h"
#include "frontend/browser_tools.h"
#include <QGuiApplication>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
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
    void exactPreparedSchemasAndUnimplementedInput()
    {
        const auto actual = browser->tools();
        QCOMPARE(actual.size(), 7);
        QVERIFY(browser->snapshot().available);
        QCOMPARE(browser->snapshot().status, BrowserState::Status::Empty);
        for (const auto &schema : actual) {
            auto expected = browser_tools_test::schemas();
            auto found = std::find_if(expected.begin(), expected.end(),
                                      [&](const auto &s) { return s.name == schema.name; });
            QVERIFY(found != expected.end());
            QCOMPARE(schema.description, found->description);
            QCOMPARE(schema.parameters, found->parameters);
        }
        for (const auto *name :
             {"browser_click", "browser_press", "browser_type", "browser_select"})
            QVERIFY(run(name).isError);
        QCOMPARE(browser->tabList().size(), 0);
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
        const auto snap = run("browser_snapshot");
        auto scrolled =
            run("browser_scroll", {{"pageId", snap.data->value("pageId")}, {"amount", 0.8}});
        QCOMPARE(code(scrolled), "unavailable");
        QCOMPARE(js("wheels.length").toInt(), 0);
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
    void nativeWheelDeliveryPrototype()
    {
        open("<body "
             "style='margin:0;height:3000px'><script>window.wheels=[];addEventListener('wheel',e=>"
             "wheels.push([e.isTrusted,e.deltaY]),{passive:true})</script>");
        auto *view = qobject_cast<QQuickItem *>(window->property("activeGuest").value<QObject *>());
        QVERIFY(view);
        auto *receiver = view;
        QPointF point(view->width() / 2, view->height() / 2);
        while (auto *child = receiver->childAt(point.x(), point.y())) {
            point = child->mapFromItem(receiver, point);
            receiver = child;
        }
        QWheelEvent direct(point, receiver->mapToGlobal(point), QPoint(0, -480), {}, Qt::NoButton,
                           Qt::NoModifier, Qt::ScrollUpdate, false);
        QCoreApplication::sendEvent(receiver, &direct);
        QTest::qWait(150);
        QCOMPARE(js("wheels.length").toInt(), 0); // not a usable targeted input primitive
        const auto scene = view->mapToScene({view->width() / 2, view->height() / 2});
        window->setProperty("shield", true);
        QWheelEvent covered(scene, window->mapToGlobal(scene.toPoint()), QPoint(0, -480),
                            QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                            false);
        QCoreApplication::sendEvent(window, &covered);
        QTest::qWait(150);
        QCOMPARE(js("wheels.length").toInt(), 0); // normal window route hits app chrome
        window->setProperty("shield", false);
        QWheelEvent normal(scene, window->mapToGlobal(scene.toPoint()), QPoint(0, -480),
                           QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window, &normal);
        QTRY_VERIFY(js("wheels.length").toInt() > 0);
        QCOMPARE(js("wheels[0][0]").toBool(), true);
        // This mouse-wheel route is trusted, but uses angle units: the supplied
        // 480-pixel delta is not the reference's 480 CSS-pixel wheel event.
        QCOMPARE(js("wheels[0][1]").toInt(), 60);
        QTRY_VERIFY(js("scrollY").toInt() > 0);
    }
    void trustedInputPrototypeNotToolImplementation()
    {
        open("<button style='position:absolute;left:0;top:0;width:100px;height:60px' "
             "id=b>Click</button>"
             "<input id=i style='position:absolute;left:0;top:80px;width:180px;height:40px'>"
             "<script>window.events=[];for(const n of "
             "['click','keydown','input'])addEventListener(n,e=>events.push([n,e.isTrusted]));</"
             "script>");
        // Test the normal public Qt event route. This does NOT implement the
        // tool's target/occlusion/final-navigation or Unicode insertion sequence.
        QTest::mouseClick(window, Qt::LeftButton, {}, QPoint(40, 30));
        QTRY_VERIFY(js("events.some(e=>e[0]==='click'&&e[1])").toBool());
        QTest::mouseClick(window, Qt::LeftButton, {}, QPoint(40, 100));
        QTest::keyClick(window, Qt::Key_A);
        QTRY_COMPARE(js("document.getElementById('i').value").toString(), "a");
        QVERIFY(js("events.some(e=>e[0]==='keydown'&&e[1])&&events.some(e=>e[0]==='input'&&e[1])")
                    .toBool());
        js("events=[];document.getElementById('b').click()");
        QCOMPARE(js("events[0][1]").toBool(), false); // why JS .click() is not a replacement
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
