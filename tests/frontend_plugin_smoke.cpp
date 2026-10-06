#include "backend/fake_backend.h"
#include "frontend/example_plugin.h"
#include "window.h"
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

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
} // namespace

// The example frontend plugin through the real transcript (over the fake
// backend) and the production Settings sheet: registration shows its toggle,
// On/Off applies at once with every hook removed by Off, and the choice
// survives a restart. Without a frontend plugin nothing differs.
int frontendPluginSmoke(QQmlApplicationEngine &engine, QQuickWindow *window)
{
    int failures = 0;
    const auto check = [&](bool ok, const char *what) {
        if (!ok) {
            qCritical() << "FAIL: Frontend plugins:" << what;
            ++failures;
        }
    };
    QTemporaryDir dir;
    const auto preferences = dir.filePath(QStringLiteral("preferences.json"));
    const QString id = openghost::ExamplePlugin::Id;
    {
        // Chat rendering and events over a live (fake) conversation.
        openghost::FakeBackend backend;
        WindowController controller(&backend, {});
        auto *plugins = controller.frontendPlugins();
        check(QTest::qWaitFor([&] { return controller.ready(); }, 1000), "initialized");
        auto *transcript = controller.transcript();
        const auto decorated = [&](bool on) {
            bool all = transcript->rowCount() > 0;
            for (int i = 0; i < transcript->rowCount(); ++i) {
                const auto map =
                    transcript->data(transcript->index(i), TranscriptModel::DecorationsRole)
                        .toMap();
                all &= on ? map.value(id).toMap().value("seen").toBool() : map.isEmpty();
            }
            return all;
        };
        const auto exchange = [&](const QString &text) {
            const int rows = transcript->rowCount();
            check(controller.send(text) != 0, "fake backend accepts a message");
            check(QTest::qWaitFor(
                      [&] { return transcript->rowCount() >= rows + 2 && !controller.busy(); },
                      10000),
                  "fake reply finished");
        };
        exchange(QStringLiteral("Before any plugin"));
        check(decorated(false), "no plugin: rows carry no decorations");
        check(plugins->count() == 0 && plugins->hooks() == 0 && plugins->visibility().isEmpty(),
              "no plugin: no hooks");
        check(plugins->add(std::make_unique<openghost::ExamplePlugin>()) &&
                  !plugins->enabled(id) && decorated(false),
              "registered off: rows untouched");
        check(plugins->setEnabled(id, true) && plugins->hooks() == 3,
              "On registers the plugin's hooks");
        check(decorated(true), "On re-renders the open chat with decorations at once");
        exchange(QStringLiteral("With the plugin on"));
        check(decorated(true), "new rows are decorated");
        const auto status = plugins->entries().value(0).toMap().value("status").toString();
        check(status.startsWith(QStringLiteral("Active")) && !status.contains(" 0 chat"),
              "chat events reach the plugin");
        check(plugins->setEnabled(id, false) && plugins->hooks() == 0 &&
                  plugins->visibility().isEmpty(),
              "Off leaves no hooks");
        check(decorated(false), "Off re-renders rows without decorations at once");
        exchange(QStringLiteral("With the plugin off"));
        check(decorated(false), "no decorations after Off");
    }

    // Settings, as the disconnected window shows it.
    auto controller = std::make_unique<WindowController>(nullptr, preferences);
    auto *plugins = controller->frontendPlugins();
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/OpenGhost/Ui/SettingsDialog.qml")));
    std::unique_ptr<QObject> dialog(component.createWithInitialProperties(
        {{"frontend", QVariant::fromValue(controller.get())},
         {"parent", QVariant::fromValue(window->contentItem())}}));
    check(dialog != nullptr, "production Settings instantiated");
    if (!dialog) {
        qCritical() << component.errors();
        return failures;
    }
    const auto tabs = [&] {
        QStringList ids;
        for (const auto &page : dialog->property("pages").toList())
            ids.append(page.toMap().value("id").toString());
        return ids;
    };
    check(!tabs().contains("plugins") && tabs().size() == 4, "no plugin: Settings tabs unchanged");
    // Registered before Settings opens, as built-in plugins are at startup.
    check(plugins->add(std::make_unique<openghost::ExamplePlugin>()), "example registers");
    QTest::qWait(1);
    check(tabs().contains("plugins"), "registration adds the Plugins tab");
    QMetaObject::invokeMethod(dialog.get(), "open");
    check(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 1000), "opened");
    auto *root = dialog->property("contentItem").value<QQuickItem *>();
    if (!root) {
        check(false, "Settings content");
        return failures;
    }
    const auto click = [&](const QString &name) {
        auto *item = find(root, name);
        check(item != nullptr, "requested control exists");
        if (item)
            QTest::mouseClick(
                window, Qt::LeftButton, Qt::NoModifier,
                item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    };
    const QString toggle = QStringLiteral("frontendPluginToggle-") + id;
    const QString row = QStringLiteral("frontendPlugin-") + id;
    const auto text = [&] {
        const auto *item = find(root, toggle);
        return item ? item->property("text").toString() : QString();
    };
    const auto hint = [&] {
        const auto *item = find(root, row);
        return item ? item->property("hint").toString() : QString();
    };
    click("settingsTab-plugins");
    QTest::qWait(500); // Existing page entrance.
    check(dialog->property("page") == "plugins", "Plugins page opens");
    check(find(root, row) && text() == "Off" && plugins->hooks() == 0,
          "its toggle appears, off by default");
    click(toggle);
    QTest::qWait(1);
    check(text() == "On" && plugins->enabled(id) && plugins->hooks() == 3,
          "On applies at once");
    check(plugins->visible(QStringLiteral("example"), false), "visibility hook applies");
    auto *button = find(root, toggle);
    plugins->publish({openghost::events::ChatChanged, {}, {}});
    QTest::qWait(1);
    check(hint().contains(QStringLiteral("1 chat updates seen")),
          "the Settings entry follows the plugin's status");
    check(find(root, toggle) == button, "status updates keep the same toggle");
    click(toggle);
    QTest::qWait(1);
    check(text() == "Off" && !plugins->enabled(id) && plugins->hooks() == 0 &&
              plugins->visibility().isEmpty() && !hint().contains(QStringLiteral("seen")),
          "Off applies at once and leaves no hooks");
    click(toggle);
    QTest::qWait(1);
    check(plugins->hooks() == 3, "turned on again");
    check(plugins->remove(id) && plugins->hooks() == 0, "unregistering cleans up");
    check(QTest::qWaitFor([&] { return dialog->property("page") == "general"; }, 1000) &&
              !find(root, "settingsTab-plugins"),
          "unregistering the last plugin removes the Plugins tab");
    QMetaObject::invokeMethod(dialog.get(), "close");
    QTest::qWait(400);
    dialog.reset();
    controller.reset();
    WindowController restarted(nullptr, preferences);
    restarted.frontendPlugins()->add(std::make_unique<openghost::ExamplePlugin>());
    check(restarted.frontendPlugins()->enabled(id) && restarted.frontendPlugins()->hooks() == 3,
          "the On choice survives a restart");
    return failures;
}
