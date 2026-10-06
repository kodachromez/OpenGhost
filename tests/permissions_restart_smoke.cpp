#include "backend/fake_backend.h"
#include "frontend/permissions_plugin.h"
#include "frontend/preferences.h"
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
    if (!item)
        return nullptr;
    if (item->objectName() == name)
        return item;
    for (auto *child : item->childItems())
        if (auto *found = find(child, name))
            return found;
    return nullptr;
}

// One run of OpenGhost over the fake backend, as main builds it: the window
// controller, then the Permissions plugin registered, then the production
// Settings sheet. Destroying it is quitting; a new one on the same profile is
// the restart.
struct Instance {
    openghost::FakeBackend backend;
    std::unique_ptr<WindowController> controller;
    std::unique_ptr<QObject> dialog;
    QQuickItem *root = nullptr;
    Instance(QQmlApplicationEngine &engine, QQuickWindow *window, const QString &preferences)
        : controller(std::make_unique<WindowController>(&backend, preferences))
    {
        controller->frontendPlugins()->add(std::make_unique<openghost::PermissionsPlugin>());
        QQmlComponent component(&engine,
                                QUrl(QStringLiteral("qrc:/OpenGhost/Ui/SettingsDialog.qml")));
        dialog.reset(component.createWithInitialProperties(
            {{"frontend", QVariant::fromValue(controller.get())},
             {"parent", QVariant::fromValue(window->contentItem())}}));
        if (!dialog)
            qCritical() << component.errors();
    }
    ~Instance()
    {
        if (dialog)
            QMetaObject::invokeMethod(dialog.get(), "close");
        QTest::qWait(400);
        dialog.reset();
        controller.reset();
    }
    // Settings open on Plugins.
    bool openPlugins(QQuickWindow *window)
    {
        if (!dialog)
            return false;
        QMetaObject::invokeMethod(dialog.get(), "open");
        if (!QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 1000))
            return false;
        root = dialog->property("contentItem").value<QQuickItem *>();
        if (!click(window, QStringLiteral("settingsTab-plugins")))
            return false;
        QTest::qWait(500); // Existing page entrance.
        return dialog->property("page") == QStringLiteral("plugins");
    }
    QQuickItem *item(const QString &name) const
    {
        if (auto *found = find(root, name))
            return found;
        // The confirmation is a popup in the window's overlay.
        auto *confirm = dialog ? dialog->findChild<QObject *>(QStringLiteral("restartConfirm"))
                               : nullptr;
        return confirm ? find(confirm->property("contentItem").value<QQuickItem *>(), name)
                       : nullptr;
    }
    bool click(QQuickWindow *window, const QString &name) const
    {
        auto *target = item(name);
        if (!target || !target->isVisible())
            return false;
        QTest::mouseClick(
            window, Qt::LeftButton, Qt::NoModifier,
            target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint());
        return true;
    }
    QString toggle() const
    {
        const auto *button =
            item(QStringLiteral("frontendPluginToggle-") + openghost::PermissionsPlugin::Id);
        return button ? button->property("text").toString() : QString();
    }
    bool confirming() const
    {
        auto *confirm = dialog ? dialog->findChild<QObject *>(QStringLiteral("restartConfirm"))
                               : nullptr;
        return confirm && confirm->property("visible").toBool();
    }
    bool permissionUi() const
    {
        return controller->permissions() &&
               controller->modes().value(QStringLiteral("known")).toBool();
    }
};
} // namespace

// Turning Permissions on or off restarts OpenGhost: Settings warns before the
// toggle is touched, asks first, and only Restart OpenGhost changes anything.
// The restart stops the active run and closes its waiting approval, saves the
// choice without switching Pi half way, and after the restart the toggle, the
// permission UI and the backend's plugin agree.
int permissionsRestartSmoke(QQmlApplicationEngine &engine, QQuickWindow *window)
{
    int failures = 0;
    const auto check = [&](bool ok, const char *what) {
        if (!ok) {
            qCritical() << "FAIL:" << what;
            ++failures;
        }
    };
    const auto &id = openghost::PermissionsPlugin::Id;
    QTemporaryDir dir;
    const auto preferences = dir.filePath(QStringLiteral("preferences.json"));
    const auto saved = [&]() -> QVariant {
        openghost::PreferencesStore store(preferences);
        const auto &choices = store.value().frontendPlugins;
        return choices.contains(id) ? QVariant(choices.value(id)) : QVariant();
    };
    const QString warning = QStringLiteral("frontendPluginRestart-") + id;
    const QString toggle = QStringLiteral("frontendPluginToggle-") + id;

    {
        Instance app(engine, window, preferences);
        check(QTest::qWaitFor([&] { return app.controller->ready(); }, 1000), "initialized");
        check(app.openPlugins(window), "Settings opens on Plugins");
        QSignalSpy restarts(app.controller.get(), &WindowController::restartRequested);

        // The warning is there before the toggle is touched.
        auto *note = app.item(warning);
        check(note && note->isVisible() && app.toggle() == QStringLiteral("On"),
              "Requires restart shows beside the On toggle");
        check(note && note->property("tip").toString() ==
                          QStringLiteral("Enabling or disabling Permissions restarts OpenGhost."),
              "the warning says why");
        bool says = false;
        for (auto *text : note ? note->findChildren<QQuickItem *>() : QList<QQuickItem *>{})
            says |= text->property("text").toString() == QStringLiteral("Requires restart");
        check(says, "it reads Requires restart");

        // Cancel: nothing changes.
        check(app.click(window, toggle) &&
                  QTest::qWaitFor([&] { return app.confirming(); }, 1000),
              "the toggle asks first");
        const auto *text = app.item(QStringLiteral("restartConfirmText"));
        check(text && text->property("text").toString() ==
                          QStringLiteral("Changing Permissions requires OpenGhost to restart."),
              "the confirmation says a restart is needed");
        check(app.toggle() == QStringLiteral("On") && app.controller->frontendPlugins()->enabled(id),
              "the toggle does not change while asking");
        check(app.click(window, QStringLiteral("restartConfirmCancel")) &&
                  QTest::qWaitFor([&] { return !app.confirming(); }, 1000),
              "Cancel closes the confirmation");
        check(app.toggle() == QStringLiteral("On") && app.permissionUi() &&
                  app.backend.permissionsEnabled() && !saved().isValid() && restarts.isEmpty() &&
                  !app.controller->restarting(),
              "Cancel leaves Permissions, its saved choice and OpenGhost as they were");

        // Disable + restart, with a run waiting on an approval.
        check(app.controller->send(QStringLiteral("/fake decisions")) != 0, "a run starts");
        check(QTest::qWaitFor(
                  [&] { return !app.controller->approvals().isEmpty() && app.controller->busy(); },
                  2000),
              "its approval waits");
        check(app.click(window, toggle) &&
                  QTest::qWaitFor([&] { return app.confirming(); }, 1000),
              "Off asks first");
        check(app.click(window, QStringLiteral("restartConfirmAccept")),
              "Restart OpenGhost is chosen");
        check(QTest::qWaitFor([&] { return restarts.size() == 1; }, 3000),
              "OpenGhost asks to restart");
        check(app.controller->approvals().isEmpty(), "the waiting approval is closed first");
        check(!app.controller->busy() && !app.controller->canCancel(),
              "the active run is stopped first");
        check(saved() == QVariant(false), "the Off choice is saved");
        check(app.backend.permissionsEnabled() && app.toggle() == QStringLiteral("On") &&
                  app.permissionUi(),
              "nothing switches half way before the restart");
        check(app.controller->restarting() && app.controller->send(QStringLiteral("more")) == 0,
              "nothing new starts while restarting");
        check(!app.controller->restartWithPlugin(id, true), "a second restart is not asked");
        QTest::qWait(100);
        check(restarts.size() == 1, "the restart is asked once");
    }
    {
        // After the restart: off, and everything agrees.
        Instance app(engine, window, preferences);
        check(QTest::qWaitFor([&] { return app.controller->ready(); }, 1000), "restarted");
        check(app.openPlugins(window), "Settings opens on Plugins after the restart");
        check(!app.controller->frontendPlugins()->enabled(id) &&
                  app.toggle() == QStringLiteral("Off") && !app.backend.permissionsEnabled(),
              "after Off, the toggle and the backend's plugin are both off");
        check(!app.permissionUi(), "after Off, the permission UI stays absent");
        check(app.item(warning) && app.item(warning)->isVisible(), "the warning stays");

        // Enable + restart (no run: at once).
        QSignalSpy restarts(app.controller.get(), &WindowController::restartRequested);
        check(app.click(window, toggle) &&
                  QTest::qWaitFor([&] { return app.confirming(); }, 1000) &&
                  app.toggle() == QStringLiteral("Off"),
              "On asks first, the toggle unchanged");
        check(app.click(window, QStringLiteral("restartConfirmAccept")) &&
                  QTest::qWaitFor([&] { return restarts.size() == 1; }, 3000),
              "On restarts");
        check(saved() == QVariant(true) && !app.backend.permissionsEnabled(),
              "the On choice is saved, applied only by the restart");
    }
    {
        Instance app(engine, window, preferences);
        check(QTest::qWaitFor([&] { return app.controller->ready(); }, 1000), "restarted again");
        check(app.openPlugins(window), "Settings opens on Plugins after the second restart");
        check(app.controller->frontendPlugins()->enabled(id) &&
                  app.toggle() == QStringLiteral("On") && app.backend.permissionsEnabled(),
              "after On, the toggle and the backend's plugin are both on");
        check(app.permissionUi(), "after On, Ask / Auto / Full and the permission UI return");
        check(app.controller->send(QStringLiteral("/fake decisions")) != 0 &&
                  QTest::qWaitFor(
                      [&] {
                          return !app.controller->approvals().isEmpty() &&
                                 !app.controller->approvals()
                                      .first()
                                      .toMap()
                                      .value(QStringLiteral("actions"))
                                      .toList()
                                      .isEmpty();
                      },
                      2000),
              "after On, a request carries the plugin's decisions again");
    }
    return failures;
}
