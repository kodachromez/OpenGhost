#include "plugin_fixture.h"
#include "window.h"
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
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

// Drive the actual Settings QML over a scripted Backend, including asynchronous
// failures and external notifications. No test-only plugin page or real RPC.
int pluginSmoke(QQmlApplicationEngine &engine, QQuickWindow *window)
{
    int failures = 0;
    const auto check = [&](bool ok, const char *what) {
        if (!ok) {
            qCritical() << "FAIL: Plugins:" << what;
            ++failures;
        }
    };
    PluginBackend backend;
    WindowController controller(&backend, {});
    check(QTest::qWaitFor([&] { return controller.ready(); }, 1000), "initialized");
    check(backend.count<openghost::PluginsList>() == 1, "initial plugin.list requested");
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/OpenGhost/Ui/SettingsDialog.qml")));
    std::unique_ptr<QObject> dialog(component.createWithInitialProperties(
        {{"frontend", QVariant::fromValue(&controller)},
         {"parent", QVariant::fromValue(window->contentItem())}}));
    check(dialog != nullptr, "production Settings instantiated");
    if (!dialog) {
        qCritical() << component.errors();
        return failures;
    }
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
    const QString toggle = QStringLiteral("pluginToggle-vendor.future");
    const auto text = [&] {
        const auto *item = find(root, toggle);
        return item ? item->property("text").toString() : QString();
    };
    // Toggleable: an available row's button stays enabled (focusable) and
    // only `actionable` says whether a press can send anything.
    const auto enabled = [&] {
        const auto *item = find(root, toggle);
        return item && item->isEnabled() && item->property("actionable").toBool();
    };
    click("settingsTab-plugins");
    QTest::qWait(500); // Existing body/title entrance, not a custom plugin animation.
    check(dialog->property("page") == "plugins", "capability exposes Plugins tab");
    check(controller.pluginsLoading(), "loading is visible before list completion");
    auto future = plugin();
    future.name = "Future <b>plugin</b>"; // backend display strings are plain text
    backend.finish(backend.next<openghost::PluginsList>(),
                   listed({future, plugin("not.configured", "enabled", false)}));
    QTest::qWait(1);
    auto *row = find(root, "plugin-vendor.future");
    check(row && row->property("name").toString() == future.name,
          "unlisted third-party name and ID render without registration");
    check(text() == "Off" && enabled(), "list populates authoritative Off control");
    auto *unavailable = find(root, "pluginToggle-not.configured");
    check(unavailable && !unavailable->isEnabled(), "unavailable control disabled");
    click("pluginToggle-not.configured");
    check(backend.count<openghost::DisablePlugin>() == 0, "unavailable cannot dispatch");
    click(toggle);
    check(backend.count<openghost::EnablePlugin>() == 1 &&
              backend.command<openghost::EnablePlugin>().pluginId == future.id,
          "On sends EnablePlugin with the exact pluginId");
    check(text() == "Off" && !enabled(), "pending does not fake enabled state");
    click(toggle);
    check(backend.count<openghost::EnablePlugin>() == 1, "pending cannot toggle twice");
    backend.finish(
        backend.next<openghost::EnablePlugin>(),
        openghost::Error{"plugin_activation_failed", "Activation refused", {}, {}, {}, {}});
    check(backend.next<openghost::PluginsList>() != 0, "failed operation reads actual state");
    QTest::qWait(1);
    auto *note = find(root, "pluginNote-vendor.future");
    check(note && note->isVisible() && note->property("text") == "Activation refused",
          "non-destructive row error shown");
    check(text() == "Off" && !enabled(), "unconfirmed row cannot toggle before the read");
    backend.finish(backend.next<openghost::PluginsList>(), listed({future}));
    QTest::qWait(1);
    note = find(root, "pluginNote-vendor.future");
    check(note && !note->isVisible(), "reconciled row drops the stale error");
    check(text() == "Off" && enabled() && controller.ready(),
          "failure retains authoritative Off and connection");
    click(toggle);
    backend.finish(backend.next<openghost::EnablePlugin>(), updated(plugin(future.id, "enabled")));
    QTest::qWait(1);
    check(text() == "On" && enabled(), "successful reply sets On");
    click(toggle);
    check(backend.count<openghost::DisablePlugin>() == 1 &&
              backend.command<openghost::DisablePlugin>().pluginId == future.id,
          "Off sends DisablePlugin with the exact pluginId");
    auto disabling = plugin(future.id, "disabling");
    disabling.activeCalls = 1;
    backend.publish(disabling);
    backend.finish(backend.next<openghost::DisablePlugin>(), updated(disabling));
    QTest::qWait(1);
    row = find(root, "plugin-vendor.future");
    check(enabled() && text() == "Off" && row &&
              row->property("status").toString().contains("waiting for 1 running call"),
          "disabling shows its running call and can be turned back on");
    // Keyboard: the pending flip, the event and the answer update the row in
    // place, so the focused button (and the tab strip) are never recreated.
    auto *button = find(root, toggle);
    auto *tab = find(root, "settingsTab-plugins");
    if (button)
        button->forceActiveFocus(Qt::TabFocusReason);
    check(button && button->hasActiveFocus(), "toggle takes keyboard focus");
    QTest::keyClick(window, Qt::Key_Space);
    check(backend.count<openghost::EnablePlugin>() == 3 &&
              backend.command<openghost::EnablePlugin>().pluginId == future.id,
          "Space re-enables a disabling plugin");
    check(find(root, toggle) == button && button && button->hasActiveFocus() && !enabled(),
          "pending keeps the same focused, non-actionable button");
    QTest::keyClick(window, Qt::Key_Space);
    check(backend.count<openghost::EnablePlugin>() == 3, "pending cannot be pressed again");
    backend.publish(plugin(future.id, "enabled"));
    backend.finish(backend.next<openghost::EnablePlugin>(),
                   updated(plugin(future.id, "enabled"), {}, QStringLiteral("memory")));
    QTest::qWait(1);
    check(find(root, toggle) == button && button && button->hasActiveFocus() && text() == "On" &&
              enabled(),
          "answer updates the focused row in place");
    check(find(root, "settingsTab-plugins") == tab, "plugin updates do not rebuild the tabs");
    note = find(root, "pluginNote-vendor.future");
    check(note && note->isVisible() &&
              note->property("text").toString().contains("until the backend restarts"),
          "a change kept only in memory says it will not survive restart");
    backend.publish(plugin());
    QTest::qWait(1);
    check(text() == "Off" && enabled() && find(root, toggle) == button,
          "external notification updates the same row");
    backend.publish(plugin(future.id, "enabled"));
    QTest::qWait(1);
    check(text() == "On" && enabled(), "external state change updates existing UI");
    {
        QSignalSpy rows(controller.plugins(), &QAbstractItemModel::dataChanged);
        QSignalSpy page(&controller, &WindowController::pluginsChanged);
        backend.publish(plugin(future.id, "enabled")); // identical to the shown row
        backend.publishFrom(QStringLiteral("replaced-connection"), plugin("old.connection"));
        backend.publishFrom(QStringLiteral("replaced-connection"), plugin(future.id));
        QTest::qWait(1);
        check(rows.isEmpty() && page.isEmpty() && !find(root, "plugin-old.connection") &&
                  text() == "On",
              "identical and other-connection events change nothing shown");
    }
    backend.publish(plugin("new.plugin.at.runtime"));
    QTest::qWait(1);
    check(find(root, "plugin-new.plugin.at.runtime") != nullptr, "new external ID appears live");
    backend.disconnectBackend();
    check(QTest::qWaitFor([&] { return dialog->property("page") == "general"; }, 1000),
          "disconnect safely leaves removed page");
    tab = find(root, "settingsTab-plugins");
    check(!tab || !tab->isVisible(), "no capability means no Plugins tab");
    check(!controller.runtimePlugins() && controller.pluginCount() == 0,
          "stale plugin controls discarded");
    QMetaObject::invokeMethod(dialog.get(), "close");
    QTest::qWait(400);
    return failures;
}
