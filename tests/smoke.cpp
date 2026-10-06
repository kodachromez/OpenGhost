#include "diagram.h"
#include "markdown.h"
#include "medialoader.h"
#include "rich.h"
#include "tex.h"
#include "theme.h"
#include "video_fixture.h"
#include "window.h"

#include <QBuffer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QLineF>
#include <QMimeData>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <atomic>
#include <memory>

int browserSmoke(QQmlApplicationEngine &engine, WindowController &controller);

namespace
{
QQuickItem *findVisual(QQuickItem *item, const QString &name)
{
    if (item->objectName() == name)
        return item;
    for (auto *child : item->childItems())
        if (auto *found = findVisual(child, name))
            return found;
    return nullptr;
}

void findAll(QQuickItem *item, const QString &name, QList<QQuickItem *> &out)
{
    if (item->objectName() == name && item->isVisible())
        out << item;
    for (auto *child : item->childItems())
        findAll(child, name, out);
}

QList<QQuickItem *> findAll(QQuickItem *item, const QString &name)
{
    QList<QQuickItem *> out;
    findAll(item, name, out);
    return out;
}

QByteArray fixturePng(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(56, 101, 148));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

// Exercise the splash Loader's retirement between sync and render, rather than
// relying on the scheduler to hit the small window at the normal handoff.
// Qt 6.11's software Shape node dereferences its GUI item during render: that
// item may already be detached/deleted. A painted aura must instead draw the
// snapshot made during sync, while the GUI was blocked.
bool retireSplashDuringRender(QQmlApplicationEngine &engine, QQuickWindow *window)
{
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        Loader { source: "qrc:/OpenGhost/Ui/Splash.qml" }
    )",
                      QUrl());
    std::unique_ptr<QQuickItem> loader(qobject_cast<QQuickItem *>(component.create()));
    if (!loader)
        return false;
    loader->setParentItem(window->contentItem());
    loader->setSize(window->size());
    loader->setZ(2000);
    QPointer<QQuickItem> splash = loader->property("item").value<QQuickItem *>();
    // Exposed, with the aura visible; not the initial expose synchronization.
    if (!splash ||
        !QTest::qWaitFor([&] { return splash->property("now").toReal() >= 900; }, 2500) ||
        window->grabWindow().isNull())
        return false;

    struct State {
        std::atomic<bool> armed{true}, pending{false}, done{false};
        std::atomic<bool> threaded{false}, destroyed{false}, timedOut{false};
        QSemaphore retired;
    };
    const auto state = std::make_shared<State>();
    const auto sync = QObject::connect(
        window, &QQuickWindow::afterSynchronizing, window,
        [state, window, item = QPointer<QQuickItem>(loader.get()), splash] {
            if (!state->armed.exchange(false))
                return;
            state->threaded = QThread::currentThread() != window->thread();
            if (!state->threaded) {
                state->done = true; // No concurrent retirement under the basic loop.
                return;
            }
            state->pending = true;
            QMetaObject::invokeMethod(
                window,
                [state, item, splash] {
                    if (item)
                        item->setProperty("active", false);
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    state->destroyed = !splash;
                    qInfo() << "Splash retirement before render: deleted ="
                            << state->destroyed.load();
                    state->retired.release();
                },
                Qt::QueuedConnection);
        },
        Qt::DirectConnection);
    const auto render = QObject::connect(
        window, &QQuickWindow::beforeRendering, window,
        [state] {
            if (state->pending)
                state->timedOut = !state->retired.tryAcquire(1, 2000);
        },
        Qt::DirectConnection);
    const auto rendered = QObject::connect(
        window, &QQuickWindow::afterRendering, window,
        [state] {
            if (state->pending.exchange(false))
                state->done = true;
        },
        Qt::DirectConnection);
    loader->setX(1); // Dirty the scene so the synchronized frame must render.
    window->update();
    const bool done = QTest::qWaitFor([&] { return state->done.load(); }, 3000);
    QObject::disconnect(sync);
    QObject::disconnect(render);
    QObject::disconnect(rendered);
    const bool expectedThread = qgetenv("QSG_RENDER_LOOP") == "threaded";
    const bool ok = done && !state->timedOut && (!expectedThread || state->threaded) &&
                    (!state->threaded || state->destroyed);
    if (ok && state->threaded)
        qInfo() << "PASS: splash deleted between scenegraph sync and render";
    return ok;
}
} // namespace

int pluginSmoke(QQmlApplicationEngine &engine, QQuickWindow *window);
int frontendPluginSmoke(QQmlApplicationEngine &engine, QQuickWindow *window);
#ifdef OPENGHOST_TOOL_CALLS
int toolCallsSmoke(QQuickWindow *window, WindowController &controller);
#endif
int chatSettingsSmoke(QQmlApplicationEngine &engine, QQuickWindow *window);
int thinkingSmoke(QQuickWindow *window, WindowController &controller);

// Bounded offline rendering and optional fake-backend integration check.
int smokeTest(QQmlApplicationEngine &engine, WindowController &controller)
{
    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            qCritical() << "FAIL:" << what;
            ++failures;
        }
    };
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    check(window && window->isVisible(), "native window launches");
    if (!window)
        return 1;
    check(window->title() == QStringLiteral("OpenGhost"), "OpenGhost title");
    check(QFile::exists(QStringLiteral(":/shaders/mist.frag.qsb")), "original 1.3 mist packaged");
    check(!QFile::exists(QStringLiteral(":/splash/mascot-down.png")), "no custom mascot assets");
    check(!window->findChild<QObject *>(QStringLiteral("transcribe")), "no extra voice buttons");
    check(!window->findChild<QObject *>(QStringLiteral("voiceChat")), "no extra voice chat");
    check(!window->findChild<QObject *>(QStringLiteral("appearanceSplash")),
          "no custom splash picker");
    check(!window->findChild<QObject *>(QStringLiteral("generalExecution")),
          "no execution-backend UI");
    check(!window->findChild<QObject *>(QStringLiteral("workspacePicker")),
          "no startup workspace picker");
    check(!window->findChild<QObject *>(QStringLiteral("unfinished")),
          "no inherited recovery banner");
#ifdef OPENGHOST_TOOL_CALLS
    const bool toolCalls = true;
#else
    const bool toolCalls = false;
#endif
    // Tool cards come only with the Tool Calls frontend plugin; no subagent panel.
    check(QFile::exists(QStringLiteral(":/OpenGhost/Ui/ToolCard.qml")) == toolCalls,
          "tool card packaged only with the Tool Calls plugin");
    check(qmlTypeId("OpenGhost.Cpp", 1, 0, "ToolText") == -1,
          "no tool display helper in the host module");
    check(controller.frontendPlugins()->renderers().contains(QStringLiteral("tool")) ==
              (toolCalls && controller.frontendPlugins()->enabled(QStringLiteral("openghost.tool-calls"))),
          "tool rows are drawn only by the enabled Tool Calls plugin");
    const auto roles = controller.transcript()->roleNames().values();
    check(roles.contains("messageState") && roles.contains("toolName") &&
              !roles.contains("activity") && !roles.contains("activityNote"),
          "transcript exposes message and tool call roles, no subagent activity");

    QSignalSpy accepted(&controller, &WindowController::accepted);
    const bool fake = QCoreApplication::arguments().contains(QStringLiteral("--fake-backend"));
    if (!fake) {
        check(!controller.property("ready").toBool(), "backend is unavailable");
        check(controller.send(QStringLiteral("Never send this")) == 0 && accepted.isEmpty(),
              "disconnected send is refused, not fabricated");
    }
    auto *effort = findVisual(window->contentItem(), QStringLiteral("thinkingChoice"));
    check(effort && effort->isVisible() == !controller.settings()->levels().isEmpty(),
          "effort control is absent (not an empty toolbar slot) without advertised levels");
    auto *effortPopup = effort ? effort->property("popup").value<QObject *>() : nullptr;
    auto *modelButton = findVisual(window->contentItem(), QStringLiteral("modelChoice"));
    auto *sendButton = findVisual(window->contentItem(), QStringLiteral("send"));
    check(effort && effortPopup && modelButton && sendButton &&
              effort->inherits("QQuickComboBox") &&
              !window->findChild<QObject *>(QStringLiteral("effortPanel")),
          "effort is a plain dropdown beside the model, not the slider");
    if (!fake && modelButton && sendButton) {
        check(effort && !effort->isVisible() && !effort->isEnabled(),
              "no catalog means no effort control");
        check(qAbs(sendButton->x() - modelButton->x() - modelButton->width() - 4) < 0.01,
              "hidden effort leaves no empty toolbar slot");
    }
    check(controller.transcript()->rowCount() == 0 && controller.sessions()->rowCount() == 0,
          "no fabricated conversation");
    check(!controller.pick({QUrl::fromLocalFile(QStringLiteral("/does-not-exist"))}, 8).isEmpty(),
          "missing attachment refuses without fabricated payload");

    // Observe the handoff, not just the final frame: fading the app must not
    // also fade/scale the splash overlay (which would flash at opening).
    QPointer<QQuickItem> splash = findVisual(window->contentItem(), QStringLiteral("splash"));
    bool sawOpening = false, independentOverlay = true;
    QElapsedTimer splashClock;
    splashClock.start();
    while (splash && splashClock.elapsed() < 5000) {
        if (splash->property("revealing").toBool()) {
            sawOpening = true;
            for (auto *parent = splash->parentItem(); parent; parent = parent->parentItem())
                independentOverlay &= parent->opacity() > 0.99 && qAbs(parent->scale() - 1) < 0.001;
        }
        QTest::qWait(16);
    }
    check(sawOpening && !splash, "splash opens and hands off within its bounded timeline");
    check(independentOverlay, "splash does not inherit the app reveal transform/opacity");
    auto *welcome = findVisual(window->contentItem(), QStringLiteral("welcome"));
    check(welcome && welcome->property("phase").toString() == "shown" &&
              !welcome->property("held").toBool(),
          "welcome ghost takes over after splash");
    check(!window->grabWindow().isNull(), "window paints");
    check(window->contentItem()->opacity() > 0.99, "1.3 splash reveals the app");
    check(retireSplashDuringRender(engine, window), "splash retirement during rendering is safe");

    if (fake) {
        check(controller.ready(), "fake handshake and catalog available");
        check(controller.settings()->choices().size() == 2, "fake models reach existing picker");
        const QStringList levels{"none", "low", "medium", "high", "xhigh", "max", "ultra"};
        check(controller.settings()->model() == "echo" &&
                  controller.settings()->thinking() == "medium" &&
                  controller.settings()->levels() == levels && effort && effort->isVisible() &&
                  effort->isEnabled(),
              "fresh fake launch offers every level the model advertises");
        const auto shown = [&] { return effort->property("displayText").toString(); };
        const auto options = [&] {
            QStringList names;
            for (int i = 0; i < effort->property("count").toInt(); ++i) {
                QVariant name;
                QMetaObject::invokeMethod(effort, "nameOf", Q_RETURN_ARG(QVariant, name),
                                          Q_ARG(QVariant, controller.settings()->levels().value(i)));
                names << name.toString();
            }
            return names;
        };
        if (effort && effortPopup && modelButton && sendButton) {
            QSignalSpy chosen(controller.settings(), &Settings::chosen);
            for (const auto &canonical : {QString(), QStringLiteral("unlisted")}) {
                controller.settings()->use({"fake", "echo", canonical});
                check(effort->property("currentIndex").toInt() == -1 && shown() == "Effort" &&
                          controller.settings()->thinking() == canonical && chosen.isEmpty(),
                      "absent/unlisted canonical effort shows no level and chooses none");
            }
            controller.settings()->use({"fake", "echo", "medium"});
            check(shown() == "Medium" && effort->property("count").toInt() == 7 &&
                      options() == QStringList{"Instant", "Low", "Medium", "High", "Extra high",
                                               "Max", "Ultra"},
                  "one option per advertised level, and only those");
            check(effort->height() == 34 && QTest::qWaitFor([&] {
                      return qAbs(effort->x() - modelButton->x() - modelButton->width() - 4) < 0.01 &&
                             qAbs(sendButton->x() - effort->x() - effort->width() - 4) < 0.01;
                  }, 1000),
                  "dropdown sits between model and send in the composer row");
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              effort->mapToScene(QPointF(effort->width() / 2, 17)).toPoint());
            check(QTest::qWaitFor([&] { return effortPopup->property("opened").toBool(); }, 1000),
                  "a click opens the dropdown");
            check(chosen.isEmpty(), "opening effort is not an explicit selection");
            // The dropdown's list is in the window's overlay, beside the content.
            // The dropdown's list is in the window's overlay, made as it opens.
            QQuickItem *high = nullptr;
            check(QTest::qWaitFor([&] {
                      high = findVisual(window->contentItem(), QStringLiteral("effortOption-high"));
                      return high && high->isVisible();
                  }, 1000),
                  "the dropdown lists the High option");
            if (high)
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  high->mapToScene(QPointF(high->width() / 2, high->height() / 2))
                                      .toPoint());
            check(controller.settings()->thinking() == "high" && shown() == "High" &&
                      chosen.size() == 1,
                  "picking an option is one explicit selection through Settings");
            check(QTest::qWaitFor([&] { return !effortPopup->property("visible").toBool(); }, 1000),
                  "the dropdown closes after a pick");
            effort->forceActiveFocus();
            for (int i = 0; i < 6; ++i)
                QTest::keyClick(window, Qt::Key_Up);
            check(controller.settings()->thinking() == "none" && chosen.size() == 4,
                  "keyboard steps through the levels, one selection per change");
            QTest::keyClick(window, Qt::Key_Up);
            check(chosen.size() == 4, "first level does not wrap or emit a duplicate choice");
            for (int i = 0; i < 6; ++i)
                QTest::keyClick(window, Qt::Key_Down);
            check(controller.settings()->thinking() == "ultra" && shown() == "Ultra",
                  "keyboard reaches the last level");
            modelButton->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_End);
            check(controller.settings()->model() == "brief" &&
                      controller.settings()->thinking() == "low" && effort->isVisible() &&
                      effort->property("count").toInt() == 2 && shown() == "Low",
                  "model switch refreshes the levels and falls back to the advertised default");
            check(options() == QStringList{"Low", "High"},
                  "the other model's options are its own levels only");
            QTest::keyClick(window, Qt::Key_Home);
            check(controller.settings()->model() == "echo" &&
                      controller.settings()->thinking() == "ultra" && effort->isVisible() &&
                      effort->property("count").toInt() == 7,
                  "selecting full-effort fake model restores the user's supported preference");
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              effort->mapToScene(QPointF(effort->width() / 2, 17)).toPoint());
            check(QTest::qWaitFor([&] { return effortPopup->property("opened").toBool(); }, 1000),
                  "dropdown open before sending"); // Sending must lock and close it.
        }
        auto *composer = findVisual(window->contentItem(), QStringLiteral("composer"));
        check(composer, "real composer found");
        if (composer) {
            composer->setProperty("text", QStringLiteral("A native fake conversation"));
            check(QMetaObject::invokeMethod(window, "submit"), "real submit path invoked");
            check(!composer->property("text").toString().isEmpty(),
                  "draft not cleared synchronously");
            check(effort && effort->isVisible() && !effort->isEnabled() &&
                      effort->property("lockHint").toString() ==
                          "You can change effort when OpenGhost finishes" &&
                      effortPopup &&
                      QTest::qWaitFor([&] { return !effortPopup->property("visible").toBool(); },
                                      1000),
                  "busy effort stays visible, disabled and closes its dropdown");
            const auto runningEffort = controller.settings()->thinking();
            effort->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Up);
            check(controller.settings()->thinking() == runningEffort,
                  "disabled effort cannot change the running selection");
            check(QTest::qWaitFor([&] { return accepted.count() == 1; }, 1000),
                  "backend accepts once");
            check(composer->property("text").toString().isEmpty(),
                  "validated acceptance clears draft");
            check(QTest::qWaitFor([&] { return controller.transcript()->rowCount() == 2; }, 1000),
                  "stream starts assistant row");
            const QString first = controller.session();
            check(!first.isEmpty() && controller.sessions()->rowCount() == 1,
                  "first send creates sidebar chat");
            composer->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Escape);
            check(!controller.busy(), "Escape freezes output immediately; no Stop button");
            const auto before = controller.transcript()->data(controller.transcript()->index(1),
                                                              TranscriptModel::BodyRole);
            QTest::qWait(150);
            check(controller.transcript()->data(controller.transcript()->index(1),
                                                TranscriptModel::BodyRole) == before,
                  "stopped output never resumes");
            check(effort && effort->isEnabled(), "effort unlocks after Stop");
            if (effort) {
                bool configuring = false;
                const auto observed =
                    QObject::connect(&controller, &WindowController::changed, &controller,
                                     [&] { configuring |= controller.admitting(); });
                effort->forceActiveFocus();
                for (int i = 0; i < 6; ++i)
                    QTest::keyClick(window, Qt::Key_Up);
                QObject::disconnect(observed);
                check(configuring, "an effort pick in an existing chat dispatches ConfigureSession");
                check(
                    QTest::qWaitFor([&] { return !controller.admitting(); }, 1000) &&
                        controller.settings()->thinking() == "none" &&
                        effort->property("displayText").toString() == "Instant",
                    "canonical fake session effort returns through the existing frontend contract");
            }
            controller.settings()->choose(QStringLiteral("fake"), QStringLiteral("brief"));
            check(QTest::qWaitFor([&] { return !controller.admitting(); }, 1000),
                  "model configure settles");
            check(controller.settings()->model() == QStringLiteral("brief"),
                  "canonical model reaches picker");
            controller.general()->setInstructions(
                QStringLiteral("Remember this local preference."));
            controller.setPermissionMode(QStringLiteral("auto"));
            check(QTest::qWaitFor([&] { return !controller.admitting(); }, 1000), "mode saves");
            controller.newChat();
            check(controller.session().isEmpty() && controller.transcript()->rowCount() == 0,
                  "new chat is local draft");
            composer->setProperty("text", QStringLiteral("Second chat"));
            QMetaObject::invokeMethod(window, "submit");
            check(QTest::qWaitFor([&] { return accepted.count() == 2; }, 1000),
                  "second chat accepted");
            check(QTest::qWaitFor([&] { return !controller.busy(); }, 3000),
                  "fake streams through completion");
            check(controller.sessions()->rowCount() == 2, "two chats in sidebar");
            controller.open(first);
            check(QTest::qWaitFor([&] { return !controller.admitting(); }, 1000),
                  "opening reconciles fake session");
            check(controller.session() == first && controller.transcript()->rowCount() == 3,
                  "open restores prior display");
            controller.sessions()->togglePin(first);
            check(controller.sessions()->pinnedCount() == 1, "sidebar pin reaches chat index");
            controller.sessions()->setQuery(QStringLiteral("Second"));
            controller.sessions()->setQuery(QString()); // two worker answers from the index
            check(controller.sessions()->pinnedCount() == 1, "saved pin survives a resync");
            controller.sessions()->togglePin(first);
            controller.toggleFolder(QStringLiteral("home:"));
            check(controller.collapsedFolders().value(QStringLiteral("home:")).toBool(),
                  "home collapse saved in chat index");
            controller.toggleFolder(QStringLiteral("home:"));
            check(controller.collapsedFolders().isEmpty(), "home expand saved in chat index");
            controller.newChat();

            QTemporaryDir files;
            QFile file(files.filePath(QStringLiteral("fixture.txt")));
            check(file.open(QIODevice::WriteOnly), "temporary attachment opens");
            file.write("A prepared text attachment.\n");
            file.close();
            check(controller.pick({QUrl::fromLocalFile(file.fileName())}, 20).isEmpty(),
                  "native text preparation");
            auto *cards = window->findChild<QObject *>(QStringLiteral("composerFiles"));
            check(QTest::qWaitFor([&] { return cards && cards->property("count").toInt() == 1; },
                                  500),
                  "prepared attachment reaches real composer card");
            composer->setProperty("text", QStringLiteral("With file"));
            QMetaObject::invokeMethod(window, "submit");
            check(QTest::qWaitFor([&] { return accepted.count() == 3; }, 1000),
                  "attachment input acknowledged");
            check(cards && cards->property("count").toInt() == 0,
                  "only acknowledged attachment removed");
            check(!controller.transcript()
                       ->data(controller.transcript()->index(0), TranscriptModel::AttachmentsRole)
                       .toList()
                       .isEmpty(),
                  "attachment transcript card");
            check(controller.canSteer(), "composer can steer a running turn");
            composer->setProperty("text", QStringLiteral("A steering input"));
            QMetaObject::invokeMethod(window, "submit");
            check(QTest::qWaitFor([&] { return accepted.count() == 4; }, 1000),
                  "steering acknowledged through composer");
            check(composer->property("text").toString().isEmpty(),
                  "steering acknowledgement clears its draft");
            check(QTest::qWaitFor([&] { return !controller.busy(); }, 3000),
                  "steered answer completes");
            check(controller.usage()->totals().value("fake").toMap().value("tokens").toDouble() > 0,
                  "fake usage reaches settings ledger");

            composer->setProperty("text", QStringLiteral("/fake approval"));
            QMetaObject::invokeMethod(window, "submit");
            check(QTest::qWaitFor([&] { return !controller.approvals().isEmpty(); }, 1000),
                  "approval contract reaches UI");
            auto *allow = findVisual(window->contentItem(), QStringLiteral("approvalAllow"));
            check(allow && QMetaObject::invokeMethod(allow, "clicked"),
                  "real approval Allow button answers");
            check(QTest::qWaitFor([&] { return !controller.busy(); }, 3000),
                  "approved simulated tool completes");
            check(controller.approvals().isEmpty(), "resolved approval dismissed");

            composer->setProperty("text", QStringLiteral("/fake error"));
            QMetaObject::invokeMethod(window, "submit");
            check(QTest::qWaitFor([&] { return controller.canRetry(); }, 3000),
                  "terminal error enables Retry");
            QQuickItem *retry = nullptr;
            check(QTest::qWaitFor(
                      [&] {
                          retry = findVisual(window->contentItem(), QStringLiteral("retryTurn"));
                          return retry && retry->isVisible();
                      },
                      1000),
                  "Retry delegate becomes visible after model update");
            check(retry && retry->isVisible() && QMetaObject::invokeMethod(retry, "clicked") &&
                      (controller.admitting() || controller.busy()),
                  "real Retry action dispatches");
            check(QTest::qWaitFor([&] { return !controller.admitting() && !controller.busy(); },
                                  3000),
                  "retry completes without new user input");
            const auto deleted = controller.session();
            QSignalSpy removed(&controller, &WindowController::sessionRemoved);
            controller.remove(deleted);
            check(QTest::qWaitFor([&] { return !removed.isEmpty(); }, 1000) &&
                      removed.first()[1].toBool(),
                  "backend deletion acknowledged");
            check(controller.session().isEmpty(), "deleting active chat opens a clean draft");
        }
    }

    const QString message = QStringLiteral(
        "# Native rendering\n\n**Markdown** and $x^2$.\n\n"
        "```cpp\nint answer = 42;\n```\n\n| One | Two |\n| --- | --- |\n| a | b |\n\n"
        "```mermaid\nflowchart LR\nA --> B\n```\n");
    const auto parsed = markdown::parse(message, {});
    check(parsed.blocks.size() >= 5, "copied Markdown parser");
    const auto math = tex::render(QStringLiteral("\\frac{a}{b}+x^2"), true, {}, 1);
    check(math.ok && !math.image.isNull(), "copied TeX painter");
    const auto drawing = diagram::render(QStringLiteral("flowchart LR\nA --> B"), {}, {});
    check(drawing.ok && !drawing.image.isNull(), "copied 1.3 diagram painter");
    Entry user;
    user.kind = Entry::User;
    user.key = QStringLiteral("smoke-user");
    user.text = QStringLiteral("Display-only smoke input");
    user.preview = QStringLiteral("Not sent");
    user.state = QStringLiteral("refused");
    Entry note;
    note.key = QStringLiteral("smoke-note");
    note.text = QStringLiteral("Display-only notice");
    controller.transcript()->apply({user, note});
    QTest::qWait(150);
    check(findVisual(window->contentItem(), QStringLiteral("entry-user")) &&
              findVisual(window->contentItem(), QStringLiteral("entry-note")),
          "retained user and notice delegates load");
    auto *body = findVisual(window->contentItem(), QStringLiteral("body"));
    check(body && qAbs(body->parentItem()->width() -
                       (qCeil(QFontMetricsF(body->property("font").value<QFont>())
                                  .horizontalAdvance(user.text)) +
                        32)) < 0.01,
          "user bubble has only the reference's horizontal padding, no phantom caret space");
    user.state = QStringLiteral("unconfirmed");
    ++user.revision;
    controller.transcript()->apply({user, note});
    QTest::qWait(50);
    const auto *userItem = findVisual(window->contentItem(), QStringLiteral("entry-user"));
    check(userItem && userItem->property("messageState").toString() == user.state,
          "message-state updates reach the delegate");

    Entry reply;
    reply.kind = Entry::Assistant;
    reply.key = QStringLiteral("smoke-only");
    reply.text = message;
    reply.state = QStringLiteral("done");
    controller.transcript()->apply({reply});
    QTest::qWait(500);
    auto *replyItem = findVisual(window->contentItem(), QStringLiteral("entry-assistant"));
    check(replyItem, "retained Markdown delegate loads");
    auto *rich = replyItem
                     ? qobject_cast<RichDocument *>(replyItem->property("rich").value<QObject *>())
                     : nullptr;
    check(rich && rich->seed() == message,
          "restored Markdown palette is seeded from source like StreamView.render");
    reply.state = QStringLiteral("live");
    reply.text = QStringLiteral("## A growing answer");
    controller.transcript()->reset({reply});
    QTest::qWait(150);
    replyItem = findVisual(window->contentItem(), QStringLiteral("entry-assistant"));
    rich = replyItem ? qobject_cast<RichDocument *>(replyItem->property("rich").value<QObject *>())
                     : nullptr;
    check(rich && rich->seed() == reply.key, "live palette has a stable initial seed");
    reply.text += QStringLiteral("\n\nMore text.");
    reply.state = QStringLiteral("done");
    ++reply.revision;
    controller.transcript()->apply({reply});
    QTest::qWait(150);
    check(rich && rich->seed() == reply.key, "live palette does not change on completion");
    reply.text = QStringLiteral("```mermaid\nflowchart LR\nA --> B\n```");
    controller.transcript()->reset({reply});
    QTest::qWait(200);
    replyItem = findVisual(window->contentItem(), QStringLiteral("entry-assistant"));
    rich = replyItem ? qobject_cast<RichDocument *>(replyItem->property("rich").value<QObject *>())
                     : nullptr;
    auto *blocks = rich ? qobject_cast<BlockModel *>(rich->blocks()) : nullptr;
    check(blocks && blocks->data(blocks->index(0), BlockModel::GapRole).toInt() == 6,
          "first wide diagram retains the reference's higher-specificity top margin");
    auto *markdownView = replyItem ? findVisual(replyItem, QStringLiteral("markdown")) : nullptr;
    check(markdownView && markdownView->property("trailingMargin").toInt() == 22,
          "last wide diagram retains its bottom margin for toolbar collapse");

    // A reply's pictures and videos, their bytes from a fixture: nothing
    // here reaches the network, and nothing untrusted is asked for unasked.
    {
        const QString first = "https://upload.wikimedia.org/wikipedia/commons/first.png";
        const QString second = "https://upload.wikimedia.org/wikipedia/commons/second.png";
        const QString elsewhere = "https://example.com/elsewhere.png";
        const QString gone = "https://th.bing.com/th/id/gone";
        const QString talk = "https://www.youtube.com/watch?v=aaaaaaaaaaa";
        const QString old = "https://youtu.be/bbbbbbbbbbb";
        const QString lost = "https://www.youtube.com/watch?v=ccccccccccc";
        QHash<QString, QByteArray> bytes{
            {first, fixturePng(240, 120)},
            {second, fixturePng(120, 160)},
            {elsewhere, fixturePng(200, 150)},
            {"https://i.ytimg.com/vi/aaaaaaaaaaa/hq720.jpg", fixturePng(480, 270)},
            {"https://i.ytimg.com/vi/bbbbbbbbbbb/hq720.jpg", fixturePng(120, 90)},
            {"https://i.ytimg.com/vi/bbbbbbbbbbb/mqdefault.jpg", fixturePng(320, 180)}};
        auto titles = std::make_shared<FixtureVideoInfo>();
        titles->hold = true;
        VideoTitles::instance()->setService(titles);
        QStringList asked;
        MediaLoader::instance()->setFetch(
            [&bytes, &asked](const QString &url, bool, const MediaLoader::Done &done) {
                asked << url;
                done(bytes.value(url));
            });
        reply.state = QStringLiteral("live");
        reply.key = QStringLiteral("smoke-media");
        reply.text = QStringLiteral("![First](%1)\n![Second](").arg(first);
        controller.transcript()->reset({reply});
        QTest::qWait(300);
        replyItem = findVisual(window->contentItem(), QStringLiteral("entry-assistant"));
        check(replyItem && findVisual(replyItem, QStringLiteral("mediaWait")) &&
                  !findVisual(replyItem, QStringLiteral("mediaStack")) && asked.isEmpty(),
              "pictures still being written wait as one plate and load nothing");
        reply.state = QStringLiteral("done");
        reply.text = QStringLiteral("Here they are:\n![First](%1)\n![Second](%2)\n"
                                    "[![Elsewhere](%3)](https://example.com/page)\n![Gone](%4)\n\n"
                                    "[A talk · A channel · 4:40](%5) [%6](%6)\n\n%7")
                         .arg(first, second, elsewhere, gone, talk, old, lost);
        ++reply.revision;
        controller.transcript()->apply({reply});
        replyItem = findVisual(window->contentItem(), QStringLiteral("entry-assistant"));
        check(replyItem &&
                  QTest::qWaitFor(
                      [&] { return findAll(replyItem, QStringLiteral("mediaCard")).size() == 2; },
                      3000),
              "trusted pictures load into a stack of what came");
        check(!asked.contains(elsewhere), "a picture from elsewhere is not fetched unasked");
        auto *stack = replyItem ? findVisual(replyItem, QStringLiteral("mediaStack")) : nullptr;
        auto *caption =
            replyItem ? findVisual(replyItem, QStringLiteral("mediaCaptionText")) : nullptr;
        check(caption && caption->property("text").toString() == QStringLiteral("First"),
              "the caption is the picture on top's");
        check(stack && QMetaObject::invokeMethod(stack, "go", Q_ARG(QVariant, 1)) &&
                  caption->property("text").toString() == QStringLiteral("Second"),
              "turning the stack changes the caption");
        const QString unrelated = "https://upload.wikimedia.org/wikipedia/commons/unrelated.png";
        MediaLoader::instance()->load(unrelated, false);
        check(QTest::qWaitFor([&] { return MediaLoader::instance()->state(unrelated) == "failed"; },
                              1000),
              "an unrelated picture settles");
        QTest::qWait(50);
        check(stack && stack->property("index").toInt() == 1 &&
                  caption->property("text").toString() == QStringLiteral("Second"),
              "a stack keeps its place while other pictures load");
        check(replyItem &&
                  QTest::qWaitFor(
                      [&] { return findAll(replyItem, QStringLiteral("mediaLost")).size() == 1; },
                      1000),
              "a picture that did not come stays a link");
        const auto asks =
            replyItem ? findAll(replyItem, QStringLiteral("mediaAsk")) : QList<QQuickItem *>();
        check(asks.size() == 1, "a picture from elsewhere waits for a click");
        if (asks.size() == 1) {
            QTest::qWait(200); // Let the preceding slide's spring/layout settle before the pointer.
            const QPointF at = asks.first()->mapToScene(
                QPointF(asks.first()->width() / 2, asks.first()->height() / 2));
            QTest::mouseClick(window, Qt::LeftButton, {}, at.toPoint());
            check(QTest::qWaitFor(
                      [&] { return findAll(replyItem, QStringLiteral("mediaCard")).size() == 3; },
                      3000) &&
                      asked.contains(elsewhere) &&
                      findAll(replyItem, QStringLiteral("mediaAsk")).isEmpty(),
                  "a click on the plate asks for the picture and adds it to the stack");
        }
        if (stack) {
            auto *theme = engine.singletonInstance<Theme *>("OpenGhost.Cpp", "Theme");
            const bool wasReduced = theme->reducedMotion();
            theme->setReducedMotion(
                true); // Deterministic hit geometry; spring was exercised above.
            QMetaObject::invokeMethod(stack, "wake");
            auto click = [&](QQuickItem *item) {
                if (!item)
                    return;
                QTest::mouseClick(
                    window, Qt::LeftButton, {},
                    item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
            };
            stack->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_End);
            check(stack->property("index").toInt() == 2 &&
                      caption->property("text").toString() == "Elsewhere",
                  "End reaches the last picture and its caption");
            auto *source = findVisual(replyItem, "mediaSource");
            check(source && source->property("link").toString() == "https://example.com/page",
                  "the active linked picture supplies its source page");
            QTest::keyClick(window, Qt::Key_Home);
            QTest::keyClick(window, Qt::Key_Right);
            check(stack->property("index").toInt() == 1, "Home and arrow keys leaf the stack");
            click(findVisual(stack, "mediaNext"));
            check(stack->property("index").toInt() == 2, "next arrow leafs, not opens the picture");
            auto dots = findAll(stack, "mediaDot");
            if (!dots.isEmpty())
                click(dots.first());
            check(stack->property("index").toInt() == 0, "dots leaf the stack");
            QTest::qWait(500);
            auto wheel = [&](double dx, double dy, int now) {
                QVariant accepted;
                QMetaObject::invokeMethod(stack, "wheelStep", Q_RETURN_ARG(QVariant, accepted),
                                          Q_ARG(QVariant, dx), Q_ARG(QVariant, dy),
                                          Q_ARG(QVariant, now));
                return accepted.toBool();
            };
            check(!wheel(0, 100, 0), "vertical wheel remains transcript scrolling");
            check(wheel(60, 0, 10) && wheel(25, 0, 20) && stack->property("index").toInt() == 1,
                  "a fading trackpad nudge commits a page before its inertia tail");
            wheel(5, 0, 40);
            check(stack->property("index").toInt() == 1, "inertia tail cannot turn another page");
            QTest::qWait(600);
            auto *pointer = findVisual(stack, "mediaPointer");
            if (pointer) {
                const QPoint from =
                    pointer->mapToScene(QPointF(stack->width() * 0.8, stack->height() / 2))
                        .toPoint();
                const QPoint to =
                    pointer->mapToScene(QPointF(stack->width() * 0.2, stack->height() / 2))
                        .toPoint();
                QTest::mousePress(window, Qt::LeftButton, {}, from);
                QTest::mouseMove(window, (from + to) / 2, 30);
                QTest::mouseMove(window, to, 30);
                QTest::mouseRelease(window, Qt::LeftButton, {}, to);
            }
            check(stack->property("index").toInt() == 2, "drag/fling turns at most one picture");
            QTest::mouseMove(window, QPoint(window->width() - 2, 2));
            theme->setReducedMotion(wasReduced);
        }
        // Blocks below the pictures are built over the next frames (Pacer).
        QList<QQuickItem *> videos;
        check(QTest::qWaitFor(
                  [&] {
                      videos = findAll(replyItem, QStringLiteral("mediaVideo"));
                      return videos.size() == 3;
                  },
                  3000),
              "every video link is a card");
        if (videos.size() == 3) {
            check(QTest::qWaitFor(
                      [&] {
                          return videos[0]->property("loaded").toBool() &&
                                 videos[1]->property("loaded").toBool() &&
                                 videos[2]->property("missing").toBool();
                      },
                      3000),
                  "previews load, a missing wide one falls back, and none at all leaves the link");
            check(videos[1]->property("tried").toInt() == 1,
                  "YouTube's narrow placeholder is not a preview");
            auto *title = findVisual(videos[0], QStringLiteral("mediaVideoTitle"));
            auto *by = findVisual(videos[0], QStringLiteral("mediaVideoBy"));
            check(title && title->property("text").toString() == QStringLiteral("A talk") && by &&
                      by->property("text").toString() == QStringLiteral("A channel"),
                  "a video link's words give its name and maker");
            auto *otherBy = findVisual(videos[1], QStringLiteral("mediaVideoBy"));
            auto *otherTitle = findVisual(videos[1], QStringLiteral("mediaVideoTitle"));
            check(otherBy && otherBy->property("text").toString() == QStringLiteral("YouTube") &&
                      otherTitle && !otherTitle->isVisible(),
                  "a bare video link shows no invented name");
            auto *thumb = findVisual(videos[2], QStringLiteral("mediaVideoThumb"));
            check(thumb && !thumb->isVisible(), "a video without a preview loses its plate");
        }
        check(titles->asked.size() == 3, "every valid video asks the frontend metadata host");
        for (const auto &done : titles->pending)
            done({"Real host title <b>not markup</b>", "Real host author"});
        QTest::qWait(50);
        if (videos.size() == 3) {
            check(findVisual(videos[0], "mediaVideoTitle")->property("text").toString() ==
                          "A talk" &&
                      findVisual(videos[0], "mediaVideoBy")->property("text").toString() ==
                          "A channel",
                  "host metadata never overwrites the words of the link");
            check(findVisual(videos[1], "mediaVideoTitle")->property("text").toString() ==
                          "Real host title <b>not markup</b>" &&
                      findVisual(videos[1], "mediaVideoBy")->property("text").toString() ==
                          "Real host author",
                  "late real metadata fills the bare card, as plain text");
            check(findVisual(videos[2], "mediaVideoTitle")->isVisible(),
                  "a missing thumbnail still gets its real title");
            check(findVisual(videos[0], "mediaPlayGlass") &&
                      findVisual(videos[0], "mediaTimeGlass"),
                  "play and duration have native frosted surfaces");
            auto *glass = findVisual(videos[0], "mediaPlayGlass");
            auto *plane = glass ? glass->property("backdrop").value<QQuickItem *>() : nullptr;
            const auto frostAligned = [&] {
                return plane && QLineF(glass->property("origin").toPointF(),
                                       glass->mapToItem(plane, QPointF()))
                                        .length() < 0.01;
            };
            check(frostAligned(), "frost samples the real picture below the control");
            const auto oldWidth = videos[0]->width();
            videos[0]->setWidth(250);
            QTest::qWait(30);
            check(frostAligned(), "frost sampling follows resize, not a stale initial rectangle");
            videos[0]->setWidth(oldWidth);
            QPointer<QQuickItem> original = videos[0];
            auto *block = videos[0]->parentItem();
            while (block && block->objectName() != "mediaBlock")
                block = block->parentItem();
            if (block) {
                QQmlExpression append(qmlContext(block), block, "items = items.concat([items[0]])");
                append.evaluate();
                QTest::qWait(50);
                check(!append.hasError() && original && findAll(block, "mediaVideo").size() == 2,
                      "duplicate video keys collapse and unchanged cards keep their identity");
            }
        }
        check(!window->grabWindow().isNull(), "a reply with pictures and videos paints");
        VideoTitles::instance()->setService({});
        MediaLoader::instance()->setFetch({});
    }
    controller.transcript()->reset({});

    // Sent cards must expose Preview for every format the reader retains, not
    // just PNG/JPEG/WebP. Text cards must never show a preview error.
    Entry sentFiles;
    sentFiles.kind = Entry::User;
    sentFiles.key = QStringLiteral("attachment-format-audit");
    sentFiles.state = QStringLiteral("done");
    for (const auto *format : {"png", "jpeg", "webp", "gif", "bmp"})
        sentFiles.attachments.append({QStringLiteral("photo.") + QLatin1String(format),
                                      QStringLiteral("image/") + QLatin1String(format), 100});
    sentFiles.attachments.append({QStringLiteral("notes.txt"), QStringLiteral("text/plain"), 10});
    controller.transcript()->reset({sentFiles});
    check(QTest::qWaitFor([&] {
              return findAll(window->contentItem(), QStringLiteral("previewAttachment")).size() == 5;
          }, 1000), "every prepared image format has a sent Preview control");
    check(findAll(window->contentItem(), QStringLiteral("previewError")).isEmpty(),
          "sent cards have no fabricated preview errors");
    controller.transcript()->reset({});

    auto *dialog = window->findChild<QObject *>(QStringLiteral("settingsDialog"));
    check(dialog, "copied settings dialog");
    if (dialog) {
        check(QMetaObject::invokeMethod(dialog, "open"), "settings opens");
        for (const char *page : {"general", "providers", "usage", "appearance"}) {
            check(findVisual(window->contentItem(),
                             QStringLiteral("settingsTab-") + QLatin1String(page)),
                  "OpenGhost settings tab exists");
            check(QMetaObject::invokeMethod(dialog, "show",
                                            Q_ARG(QVariant, QString::fromLatin1(page)),
                                            Q_ARG(QVariant, true)),
                  "settings page switches through navigation");
            QTest::qWait(60);
            if (QString::fromLatin1(page) == "general") {
                QTemporaryDir dir;
                const auto note = dir.filePath(QStringLiteral("pinned.txt"));
                QFile file(note);
                check(file.open(QIODevice::WriteOnly) && file.write("Pinned contents") == 15,
                      "General file fixture created");
                file.close();
                auto *general = findVisual(window->contentItem(), QStringLiteral("generalPage"));
                auto *drop = findVisual(window->contentItem(), QStringLiteral("generalDrop"));
                check(general && drop, "General Files chooser and drop target exist");
                if (general && drop) {
                    QMimeData mime;
                    mime.setUrls({QUrl::fromLocalFile(note)});
                    const auto point = drop->mapToScene(QPointF(drop->width() / 2, drop->height() / 2));
                    QDragEnterEvent enter(point.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton,
                                          Qt::NoModifier);
                    QCoreApplication::sendEvent(window, &enter);
                    QDropEvent dropped(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(window, &dropped);
                    check(enter.isAccepted() && dropped.isAccepted() &&
                              controller.general()->files().size() == 1,
                          "General drop reaches the pinned-file store, not composer refusal");
                    // The chooser's accepted handler calls this same production method.
                    check(QMetaObject::invokeMethod(general, "addFiles", Q_ARG(QVariant,
                              QVariant::fromValue(QList<QUrl>{QUrl::fromLocalFile(note)}))) &&
                              controller.general()->files().size() == 1,
                          "General chooser path replaces an existing pinned copy");
                    check(file.open(QIODevice::WriteOnly) && file.write("%PDF-1.7") == 8,
                          "General unsupported-file fixture created");
                    file.close();
                    QMetaObject::invokeMethod(general, "addFiles", Q_ARG(QVariant,
                        QVariant::fromValue(QList<QUrl>{QUrl::fromLocalFile(note)})));
                    check(general->property("statusError").toBool() &&
                              general->property("status").toString().contains("PDF") &&
                              controller.general()->files().size() == 1,
                          "General says why a selection failed and keeps the old pinned copy");
                    const auto files = controller.general()->files();
                    for (const auto &kept : files)
                        controller.general()->remove(kept.toMap().value("id").toString());
                }
            }
            if (fake && QString::fromLatin1(page) == "providers") {
                auto *logout = findVisual(window->contentItem(), QStringLiteral("logout"));
                check(logout && QMetaObject::invokeMethod(logout, "clicked"),
                      "provider Log out button");
                check(QTest::qWaitFor([&] { return controller.settings()->choices().isEmpty(); },
                                      1000),
                      "catalog invalidation removes unavailable models");
                check(effort && !effort->isVisible() && !effort->isEnabled(),
                      "effort hides when the selected model loses its advertised levels");
                auto *key = findVisual(window->contentItem(), QStringLiteral("apiKey"));
                check(key && QMetaObject::invokeMethod(key, "clicked"), "provider API key prompt");
                auto *answer = findVisual(window->contentItem(), QStringLiteral("loginAnswer"));
                check(answer && answer->isVisible(), "existing secret-input presentation wired");
                if (answer) {
                    answer->setProperty("text", QStringLiteral("fixture"));
                    check(QMetaObject::invokeMethod(answer, "send"), "fixture key submits");
                    check(answer->property("text").toString().isEmpty(),
                          "key cleared from text field");
                }
                check(QTest::qWaitFor([&] { return controller.settings()->choices().size() == 2; },
                                      1000),
                      "fake sign-in refreshes catalog");
                check(effort && effort->isVisible() && effort->isEnabled(),
                      "effort returns when the selected model's capabilities return");
            }
        }
        const auto *pluginsTab =
            findVisual(window->contentItem(), QStringLiteral("settingsTab-plugins"));
        // Without the backend's runtime plugins, only registered frontend plugins.
        check((pluginsTab && pluginsTab->isVisible()) ==
                  (controller.frontendPlugins()->count() > 0),
              "Plugins shown only for frontend plugins when backend does not advertise runtime plugins");
        check(!findVisual(window->contentItem(), QStringLiteral("settingsTab-model")),
              "no extra Model tab");
        check(!findVisual(window->contentItem(), QStringLiteral("settingsTab-notifications")),
              "no extra Notifications tab or bell animation");
        check(QMetaObject::invokeMethod(dialog, "close"), "settings closes");
    }
    QVariant off, local;
    check(effort &&
              QMetaObject::invokeMethod(effort, "nameOf", Q_RETURN_ARG(QVariant, off),
                                        Q_ARG(QVariant, QVariant("off"))) &&
              QMetaObject::invokeMethod(effort, "nameOf", Q_RETURN_ARG(QVariant, local),
                                        Q_ARG(QVariant, QVariant("turbo"))) &&
              off.toString() == "Off" && local.toString() == "Turbo",
          "Pi's off and any other advertised level get a readable name");
    if (dialog) {
        Account account;
        account.providersLoaded = true;
        for (const QString id : {"deepseek", "openai"})
            account.providers.append(QVariantMap{{"id", id},
                                                 {"name", id},
                                                 {"hint", ""},
                                                 {"connected", false},
                                                 {"logout", false},
                                                 {"oauth", false},
                                                 {"apiKey", false},
                                                 {"note", ""},
                                                 {"error", false}});
        controller.settings()->apply(account);
        for (auto *object : dialog->findChildren<QObject *>()) {
            if (!QByteArray(object->metaObject()->className()).startsWith("UsagePage_"))
                continue;
            QVariant first, second, name;
            check(QMetaObject::invokeMethod(object, "tone", Q_RETURN_ARG(QVariant, first),
                                            Q_ARG(QVariant, QStringLiteral("deepseek"))) &&
                      QMetaObject::invokeMethod(object, "tone", Q_RETURN_ARG(QVariant, second),
                                                Q_ARG(QVariant, QStringLiteral("openai"))) &&
                      first.value<QColor>() == theme::current().tones[1] &&
                      second.value<QColor>() == theme::current().tones[0],
                  "usage colours cycle in supplied provider order, not legacy branding");
            check(QMetaObject::invokeMethod(object, "nameOf", Q_RETURN_ARG(QVariant, name),
                                            Q_ARG(QVariant, QStringLiteral("chatgpt"))) &&
                      name.toString() == QStringLiteral("chatgpt"),
                  "usage does not invent a display name for an unlisted provider");
        }
    }
    failures += browserSmoke(engine, controller);
    Theme::choose(QStringLiteral("light"));
    QTest::qWait(100);
    check(!window->grabWindow().isNull(), "light theme paints");
    Theme::choose(QStringLiteral("dark"));
#ifdef OPENGHOST_TOOL_CALLS
    if (fake)
        failures += toolCallsSmoke(window, controller);
#endif
    if (fake)
        failures += thinkingSmoke(window, controller);
    failures += chatSettingsSmoke(engine, window);
    failures += frontendPluginSmoke(engine, window);
    failures += pluginSmoke(engine, window);
    check(!engine.property("smokeWarnings").toBool(), "no QML binding/load warnings");
    QSignalSpy closing(&controller, &WindowController::closeRequested);
    window->close();
    check(closing.count() == 1, "native close reaches the application");
    if (!failures)
        qInfo() << "PASS: OpenGhost 1.3 UI smoke" << (fake ? "with fake backend" : "disconnected");
    return failures ? 1 : 0;
}
