#include "frontend/browser.h"
#include "window.h"
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
#include <csignal>

namespace
{
QQuickItem *find(QQuickItem *item, const QString &name)
{
    if (item->objectName() == name)
        return item;
    for (auto *child : item->childItems())
        if (auto *found = find(child, name))
            return found;
    return nullptr;
}
QPointF centre(QQuickItem *item)
{
    return item->mapToScene({item->width() / 2, item->height() / 2});
}
bool near(QColor a, QColor b)
{
    return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) +
               std::abs(a.blue() - b.blue()) <
           24;
}
} // namespace

// The production browser panel in the real window, on real Qt WebEngine
// guests showing local pages only (file: pages in a private temporary
// directory and a refused loopback port).
// Without the browser build: no panel, no toggle, no browser context.
int browserSmoke(QQmlApplicationEngine &engine, WindowController &controller)
{
    int failures = 0;
    const auto check = [&](bool ok, const char *what) {
        if (!ok) {
            qCritical() << "FAIL: Browser:" << what;
            ++failures;
        }
    };
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    auto *root = window->contentItem();
    auto *toggle = find(root, QStringLiteral("browserToggle"));
#ifndef OPENGHOST_BROWSER
    check(!controller.browser(), "no browser host without the browser build");
    check(!toggle || !toggle->isVisible(), "no browser toggle without the browser build");
    check(!find(root, QStringLiteral("browserPanel")),
          "no browser panel without the browser build");
    return failures;
#else
    auto *browser = qobject_cast<openghost::Browser *>(controller.browser());
    check(browser, "the desktop build composes the browser host");
    if (!browser || !toggle)
        return failures + 1;
    auto *panel = find(root, QStringLiteral("browserPanel"));
    check(panel && toggle->isVisible(),
          "the panel and its globe toggle exist with or without a backend");
    if (!panel)
        return failures + 1;
    check(!browser->isOpen() && !panel->isEnabled(),
          "closed at first: the panel is inert under the chat");
    check(window->property("chatRight").toReal() == 8, "closed, the chat keeps the full width");
    const auto wait = [](auto condition, int ms = 8000) { return QTest::qWaitFor(condition, ms); };

    // Settings may still be closing (a modal popup) from the steps before.
    check(wait(
              [&] {
                  for (auto *popup : window->findChildren<QObject *>())
                      if (popup->inherits("QQuickPopup") && popup->property("visible").toBool())
                          return false;
                  return true;
              },
              3000),
          "no popup covers the window");

    // Open with the toggle: the chat makes room and the address takes the keyboard.
    QTest::mouseClick(window, Qt::LeftButton, {}, centre(toggle).toPoint());
    check(browser->isOpen() && panel->isEnabled(), "the toggle opens the panel");
    check(wait(
              [&] {
                  return window->property("chatRight").toReal() ==
                         16 + window->property("browserWidth").toReal();
              },
              2000),
          "the chat card narrows beside the panel");
    auto *field = find(panel, QStringLiteral("browserUrl"));
    check(field && wait([&] { return field->hasActiveFocus(); }, 1000),
          "an empty panel focuses its address");
    check(find(panel, QStringLiteral("browserEmpty"))->isVisible(), "the empty state shows");

    // Local pages with plain file: URLs, so the guest's location.href is
    // exactly the view's URL and document readiness is observed (an encoded
    // data: URL never matches it).
    QTemporaryDir pages;
    const auto writePage = [&](const char *name, const char *html) {
        QFile file(pages.filePath(QString::fromLatin1(name)));
        const bool written = file.open(QIODevice::WriteOnly) && file.write(html) > 0;
        file.close();
        return written ? QUrl::fromLocalFile(file.fileName()).toString() : QString();
    };
    const QString green =
        writePage("green.html", "<!doctype html><title>Green</title>"
                                "<body style='margin:0;background:rgb(10,200,30)'></body>");
    const QString blue =
        writePage("blue.html", "<!doctype html><title>Blue</title>"
                               "<body style='margin:0;background:rgb(20,40,220)'></body>");
    check(pages.isValid() && !green.isEmpty() && !blue.isEmpty(), "the local pages are written");

    // Navigate from the address bar to a local page and present its frame.
    for (const QChar c : green)
        QTest::keyClick(window, c.toLatin1());
    QTest::keyClick(window, Qt::Key_Return);
    check(wait([&] { return browser->ready() && !browser->loading(); }),
          "the page loads and is ready");
    check(!field->hasActiveFocus(), "Enter hands the address bar's keyboard back");
    const QString first = browser->activeHandle();
    check(browser->tab(first)->title == QStringLiteral("Green"), "the tab takes the page's title");
    check(!find(panel, QStringLiteral("browserEmpty"))->isVisible(),
          "the empty state gives way to the page");
    auto *stage = find(panel, QStringLiteral("browserStage"));
    const QPoint inStage = centre(stage).toPoint();
    check(wait([&] { return near(window->grabWindow().pixelColor(inStage), QColor(10, 200, 30)); }),
          "the stage presents the page's frame");

    // A second page; Back returns to the first through the guest's history.
    browser->go(blue);
    check(wait([&] {
              return browser->canGoBack() && !browser->loading() &&
                     near(window->grabWindow().pixelColor(inStage), QColor(20, 40, 220));
          }),
          "navigating draws the new page and enables Back");
    QTest::mouseClick(window, Qt::LeftButton, {},
                      centre(find(panel, QStringLiteral("browserBack"))).toPoint());
    check(wait([&] { return near(window->grabWindow().pixelColor(inStage), QColor(10, 200, 30)); }),
          "Back shows the previous page");

    // A refused connection: the failure state names the host and the error.
    QTcpServer closed;
    closed.listen(QHostAddress::LocalHost);
    const quint16 port = closed.serverPort();
    closed.close();
    browser->go(QStringLiteral("http://127.0.0.1:%1/").arg(port));
    auto *error = find(panel, QStringLiteral("browserError"));
    check(wait([&] { return error->isVisible(); }), "a failed load shows the failure state");
    check(browser->error() == QStringLiteral("127.0.0.1 · ERR_CONNECTION_REFUSED"),
          "the failure names the host and the engine's error");
    check(browser->snapshot().tabs.first().state == QStringLiteral("ready"),
          "a later failed navigation does not unready a ready guest");
    QTest::mouseClick(window, Qt::LeftButton, {},
                      centre(find(panel, QStringLiteral("browserRetry"))).toPoint());
    check(wait([&] { return error->isVisible() && !browser->loading(); }),
          "Try again retries the page");
    browser->go(green);
    check(wait([&] { return !error->isVisible() && !browser->loading(); }),
          "a new page clears the failure");

    // Focus: a click on the page gives it the keyboard; a click on the chat takes it back.
    QTest::mouseClick(window, Qt::LeftButton, {}, inStage);
    const auto guestFocused = [&] {
        auto *focus = window->activeFocusItem();
        for (; focus; focus = focus->parentItem())
            if (focus->objectName() == QStringLiteral("browserGuest"))
                return true;
        return false;
    };
    check(wait([&] { return guestFocused(); }, 2000), "clicking the page focuses it");
    QTest::mouseClick(window, Qt::LeftButton, {},
                      QPoint(int(window->property("sidebarWidth").toReal()) + 200, 300));
    check(!guestFocused(), "a press on the chat takes the keyboard back from the page");

    // The agent drives (as a host step would); the user takes control and hands it back.
    QTest::mouseClick(window, Qt::LeftButton, {}, inStage);
    wait([&] { return guestFocused(); }, 2000);
    browser->drive(QStringLiteral("smoke"), true);
    auto *agent = find(panel, QStringLiteral("browserAgent"));
    check(!guestFocused(), "driving takes the keyboard off the page");
    check(wait([&] { return agent->opacity() == 1; }, 2000) &&
              find(root, QStringLiteral("browserLive"))->isVisible(),
          "the driving overlay and the toggle's live dot show");
    QTest::mouseMove(window, inStage);
    auto *take = find(panel, QStringLiteral("browserTake"));
    check(wait([&] { return take->opacity() == 1; }, 2000),
          "Take control appears under the pointer");
    QTest::mouseClick(window, Qt::LeftButton, {}, centre(take).toPoint());
    check(browser->user() && wait([&] { return guestFocused(); }, 2000),
          "Take control gives the user the page and its keyboard");
    auto *user = find(panel, QStringLiteral("browserUser"));
    check(wait([&] { return user->opacity() == 1; }, 2000), "the user's banner shows");
    auto *handBack = find(panel, QStringLiteral("browserHandBack"));
    QTest::mouseClick(window, Qt::LeftButton, {}, centre(handBack).toPoint());
    check(browser->driving() && !guestFocused() && browser->lent() == first,
          "Hand back returns the browser and keeps the keyboard off the page");
    browser->turnEnded(QStringLiteral("smoke"));
    check(wait([&] { return agent->opacity() == 0 && user->opacity() == 0; }, 2000) &&
              !find(root, QStringLiteral("browserLive"))->isVisible(),
          "the turn's end clears every overlay");

    // A crashed page is dropped and comes back on Try again.
    QQuickItem *page = nullptr;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (item->objectName() == QStringLiteral("browserGuest") && item->isVisible())
            page = item;
        for (auto *child : item->childItems())
            visit(child);
    };
    visit(stage);
    const qint64 pid = page ? page->property("renderProcessPid").toLongLong() : 0;
    check(pid > 0 && ::kill(pid_t(pid), SIGKILL) == 0, "the page's renderer is killed");
    check(wait([&] { return browser->tab(first)->state == QStringLiteral("gone"); }),
          "a crashed guest is gone");
    check(wait([&] { return error->isVisible(); }), "the crash is shown as a failure");
    QTest::mouseClick(window, Qt::LeftButton, {},
                      centre(find(panel, QStringLiteral("browserRetry"))).toPoint());
    check(wait([&] {
              return browser->tab(first)->state == QStringLiteral("ready") && !error->isVisible();
          }),
          "Try again recreates the guest");

    // Tabs: New tab, then a middle click closes it.
    QTest::mouseClick(window, Qt::LeftButton, {},
                      centre(find(panel, QStringLiteral("browserNewTab"))).toPoint());
    check(browser->tabList().size() == 2 && browser->blank() &&
              wait([&] { return field->hasActiveFocus(); }, 1000),
          "New tab opens a blank tab at the address bar");
    QQuickItem *second = nullptr;
    for (auto *item : find(panel, QStringLiteral("browserTabs"))->parentItem()->childItems())
        if (item->objectName() == QStringLiteral("browserTab") && item->property("active").toBool())
            second = item;
    check(second != nullptr, "the new tab is in the strip");
    if (second)
        QTest::mouseClick(window, Qt::MiddleButton, {}, centre(second).toPoint());
    check(browser->tabList().size() == 1 && browser->activeHandle() == first,
          "a middle click closes the tab and selects its neighbour");

    // Close: the chat takes the room back; the page stops showing.
    QTest::mouseClick(window, Qt::LeftButton, {}, centre(toggle).toPoint());
    check(!browser->isOpen() && !panel->isEnabled(), "the toggle closes the panel");
    check(wait([&] { return window->property("chatRight").toReal() == 8; }, 2000),
          "the chat card widens again");
    check(!browser->snapshot().open && browser->snapshot().tabs.size() == 1,
          "the closed panel still reports its tabs");
    return failures;
#endif
}
