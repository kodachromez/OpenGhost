#include "diagram.h"
#include "markdown.h"
#include "rich.h"
#include "tex.h"
#include "window.h"

#include <QFile>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest>

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
} // namespace

// One bounded, offline integration check of the copied UI, not a fake agent.
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
    check(!QFile::exists(QStringLiteral(":/OpenGhost/Ui/ToolCard.qml")),
          "no tool-result panel packaged");
    check(qmlTypeId("OpenGhost.Native", 1, 0, "ToolText") == -1,
          "no tool/subagent display helper registered");
    const auto roles = controller.transcript()->roleNames().values();
    check(roles.contains("messageState") && !roles.contains("toolName") &&
              !roles.contains("activity") && !roles.contains("expanded"),
          "transcript exposes only retained message roles");

    QSignalSpy accepted(&controller, &WindowController::accepted);
    check(!controller.property("ready").toBool(), "backend is unavailable");
    check(controller.send(QStringLiteral("Never send this")) == 0 && accepted.isEmpty(),
          "disconnected send is refused, not fabricated");
    check(controller.transcript()->rowCount() == 0 && controller.sessions()->rowCount() == 0,
          "no fabricated conversation");
    check(!controller.pick({QUrl::fromLocalFile(QStringLiteral("/does-not-exist"))}, 8).isEmpty(),
          "attachments refuse without reading files");

    // Exercise the original splash to completion and the real window render.
    QTest::qWait(5000);
    check(!window->grabWindow().isNull(), "window paints");
    check(window->contentItem()->opacity() > 0.99, "1.3 splash reveals the app");

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
    check(findVisual(window->contentItem(), QStringLiteral("entry-assistant")),
          "retained Markdown delegate loads");
    controller.transcript()->reset({});

    auto *dialog = window->findChild<QObject *>(QStringLiteral("settingsDialog"));
    check(dialog, "copied settings dialog");
    if (dialog) {
        check(QMetaObject::invokeMethod(dialog, "open"), "settings opens");
        for (const char *page : {"general", "providers", "usage", "appearance"}) {
            check(findVisual(window->contentItem(),
                             QStringLiteral("settingsTab-") + QLatin1String(page)),
                  "OpenGhost settings tab exists");
            check(dialog->setProperty("page", QString::fromLatin1(page)), "settings page switches");
            QTest::qWait(60);
        }
        check(!findVisual(window->contentItem(), QStringLiteral("settingsTab-model")),
              "no extra Model tab");
        check(!findVisual(window->contentItem(), QStringLiteral("settingsTab-notifications")),
              "no extra Notifications tab or bell animation");
        check(QMetaObject::invokeMethod(dialog, "close"), "settings closes");
    }
    Theme::choose(QStringLiteral("light"));
    QTest::qWait(100);
    check(!window->grabWindow().isNull(), "light theme paints");
    Theme::choose(QStringLiteral("dark"));
    check(!engine.property("smokeWarnings").toBool(), "no QML binding/load warnings");
    QSignalSpy closing(&controller, &WindowController::closeRequested);
    window->close();
    check(closing.count() == 1, "native close reaches the application");
    if (!failures)
        qInfo() << "PASS: standalone OpenGhost 1.3 UI smoke";
    return failures ? 1 : 0;
}
