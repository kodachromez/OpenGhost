#include "thinking_plugin.h"
#ifdef OPENGHOST_TOOL_CALLS
#include "tool_calls_plugin.h"
#endif
#include "window.h"
#include <QJsonDocument>
#include <QJsonObject>
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
void collect(QQuickItem *item, const QString &name, QVector<QQuickItem *> &found)
{
    if (item->objectName() == name)
        found.append(item);
    for (auto *child : item->childItems())
        collect(child, name, found);
}
void registerChatPlugins(openghost::FrontendPlugins &plugins)
{
#ifdef OPENGHOST_TOOL_CALLS
    plugins.add(std::make_unique<openghost::ToolCallsPlugin>());
#endif
    plugins.add(std::make_unique<openghost::ThinkingPlugin>());
}
} // namespace

// Settings → Appearance → Chat Settings in the production Settings sheet: its
// checkboxes are the Tool Calls and Thinking plugins' own saved on/off and the
// Thinking plugin's Start Collapsed option. Each applies at once, Start
// Collapsed is unavailable while thinking is not shown, Settings → Plugins
// does not offer them again, and every choice survives a restart (saved once).
int chatSettingsSmoke(QQmlApplicationEngine &engine, QQuickWindow *window)
{
    int failures = 0;
    const auto check = [&](bool ok, const char *what) {
        if (!ok) {
            qCritical() << "FAIL: Chat Settings:" << what;
            ++failures;
        }
    };
    QTemporaryDir dir;
    const auto preferences = dir.filePath(QStringLiteral("preferences.json"));
    const QString thinkingId = openghost::ThinkingPlugin::Id;
    const QString collapsed = openghost::ThinkingPlugin::StartCollapsed;
    auto controller = std::make_unique<WindowController>(nullptr, preferences);
    auto *plugins = controller->frontendPlugins();
    registerChatPlugins(*plugins);
#ifdef OPENGHOST_TOOL_CALLS
    const QString toolsId = openghost::ToolCallsPlugin::Id;
    check(plugins->enabled(toolsId), "Show Tool Calls is on by default");
#endif
    check(plugins->enabled(thinkingId) && plugins->option(thinkingId, collapsed),
          "Show Thinking and Start Collapsed are on by default");
    check(plugins->renderer(QStringLiteral("thinking")) &&
              !plugins->renderer(QStringLiteral("thinking"))->startExpanded,
          "thinking rows start shut");

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/OpenGhost/Ui/SettingsDialog.qml")));
    std::unique_ptr<QObject> dialog(component.createWithInitialProperties(
        {{"frontend", QVariant::fromValue(controller.get())},
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
    // A pointer click, as a reader's: the page scrolled to show the control first.
    const auto click = [&](const QString &name) {
        auto *item = find(root, name);
        check(item != nullptr, "requested control exists");
        if (!item)
            return;
        if (auto *page = find(root, QStringLiteral("settingsPage"))) {
            const auto at = item->mapToItem(page, QPointF(0, 0));
            if (at.y() < 0 || at.y() + item->height() > page->height())
                page->setProperty("contentY", page->property("contentY").toReal() + at.y() - 20);
            QTest::qWait(50);
        }
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          item->mapToScene(QPointF(14, item->height() / 2)).toPoint());
        QTest::qWait(1);
    };
    const auto checked = [&](const QString &name) {
        const auto *item = find(root, name);
        return item && item->property("checked").toBool();
    };
    const auto available = [&](const QString &name) {
        const auto *item = find(root, name);
        return item && item->isEnabled() && item->opacity() == 1;
    };
    click(QStringLiteral("settingsTab-appearance"));
    QTest::qWait(600); // Existing page entrance.
    check(dialog->property("page") == "appearance", "Appearance page opens");
    auto *section = find(root, QStringLiteral("chatSettings"));
    check(section && section->isVisible(), "Chat Settings section is shown");
    auto *thinking = find(root, QStringLiteral("showThinking"));
    auto *start = find(root, QStringLiteral("startCollapsed"));
    check(thinking && start && start->x() > thinking->x(), "Start Collapsed is indented under it");
    check(checked("showThinking") && checked("startCollapsed") && available("startCollapsed"),
          "the boxes show the saved defaults");
#ifdef OPENGHOST_TOOL_CALLS
    auto *tools = find(root, QStringLiteral("showToolCalls"));
    check(tools && tools->isVisible() && tools->y() < thinking->y() && checked("showToolCalls"),
          "Show Tool Calls comes first, checked");
    click(QStringLiteral("showToolCalls"));
    check(!checked("showToolCalls") && !plugins->enabled(toolsId) &&
              !plugins->renderer(QStringLiteral("tool")),
          "unchecking Show Tool Calls turns the plugin off at once");
    click(QStringLiteral("showToolCalls"));
    check(checked("showToolCalls") && plugins->enabled(toolsId) &&
              plugins->renderer(QStringLiteral("tool")),
          "checking it turns the plugin on again at once");
#else
    check(!find(root, QStringLiteral("showToolCalls")) ||
              !find(root, QStringLiteral("showToolCalls"))->isVisible(),
          "no Show Tool Calls without the plugin");
#endif
    click(QStringLiteral("startCollapsed"));
    check(!checked("startCollapsed") && !plugins->option(thinkingId, collapsed) &&
              plugins->renderer(QStringLiteral("thinking"))->startExpanded,
          "unchecking Start Collapsed starts thinking open at once");
    click(QStringLiteral("showThinking"));
    check(!checked("showThinking") && !plugins->enabled(thinkingId) &&
              !plugins->renderer(QStringLiteral("thinking")),
          "unchecking Show Thinking hides thinking at once");
    QTest::qWait(300);
    check(!available("startCollapsed") && find(root, QStringLiteral("startCollapsed"))->opacity() < 1,
          "Start Collapsed greys out while thinking is hidden");
    click(QStringLiteral("startCollapsed"));
    check(!checked("startCollapsed") && !plugins->option(thinkingId, collapsed),
          "a greyed-out Start Collapsed does not change");
    click(QStringLiteral("showThinking"));
    QTest::qWait(300);
    check(checked("showThinking") && plugins->enabled(thinkingId) && available("startCollapsed") &&
              plugins->renderer(QStringLiteral("thinking"))->startExpanded,
          "Show Thinking again: Start Collapsed available, its choice kept");
    click(QStringLiteral("startCollapsed"));
    check(checked("startCollapsed") && plugins->option(thinkingId, collapsed) &&
              !plugins->renderer(QStringLiteral("thinking"))->startExpanded,
          "checking Start Collapsed starts thinking shut at once");

    // Settings → Plugins stays, without offering these again.
    const auto pluginsTab = find(root, QStringLiteral("settingsTab-plugins"));
    check(pluginsTab && pluginsTab->isVisible(), "the Plugins tab stays");
    click(QStringLiteral("settingsTab-plugins"));
    QTest::qWait(600);
    check(dialog->property("page") == "plugins", "Plugins page opens");
    check(dialog->property("listedPlugins").toList().isEmpty() &&
              !find(root, QStringLiteral("frontendPlugin-") + thinkingId)
#ifdef OPENGHOST_TOOL_CALLS
              && !find(root, QStringLiteral("frontendPlugin-") + toolsId)
#endif
              ,
          "Plugins does not offer the chat settings again");
    auto *elsewhere = find(root, QStringLiteral("pluginsElsewhere"));
    check(elsewhere && elsewhere->isVisible(), "Plugins says where they are");

    // Saved choices, once each, read back by the next start.
    plugins->setEnabled(thinkingId, false);
    plugins->setOption(thinkingId, collapsed, false);
#ifdef OPENGHOST_TOOL_CALLS
    plugins->setEnabled(toolsId, false);
#endif
    QMetaObject::invokeMethod(dialog.get(), "close");
    QTest::qWait(400);
    dialog.reset();
    controller.reset();
    QFile file(preferences);
    check(file.open(QIODevice::ReadOnly), "preferences saved");
    const auto saved = QJsonDocument::fromJson(file.readAll()).object();
    check(saved.value("plugins").toObject().value(thinkingId) == QJsonValue(false) &&
              saved.value("pluginOptions").toObject().value(thinkingId).toObject() ==
                  QJsonObject{{collapsed, false}},
          "each choice is saved once, with the plugins' on/off");
    controller = std::make_unique<WindowController>(nullptr, preferences);
    plugins = controller->frontendPlugins();
    registerChatPlugins(*plugins);
    check(!plugins->enabled(thinkingId) && !plugins->option(thinkingId, collapsed) &&
              !plugins->renderer(QStringLiteral("thinking")),
          "the choices survive a restart");
#ifdef OPENGHOST_TOOL_CALLS
    check(!plugins->enabled(toolsId) && !plugins->renderer(QStringLiteral("tool")),
          "Show Tool Calls off survives a restart");
    check(plugins->setEnabled(toolsId, true), "Show Tool Calls on again");
#endif
    check(plugins->setEnabled(thinkingId, true) &&
              plugins->renderer(QStringLiteral("thinking"))->startExpanded,
          "thinking on again starts open as saved");
    if (!failures)
        qInfo() << "PASS: Chat Settings";
    return failures;
}

// The Thinking plugin in the real transcript over the fake backend: a reply's
// thinking streams above it, shut showing only its newest item, open showing
// all of it; it settles when the reply's text begins; Start Collapsed and Show
// Thinking apply to the rows already shown, at once.
int thinkingSmoke(QQuickWindow *window, WindowController &controller)
{
    int failures = 0;
    const auto check = [&](bool ok, const char *what) {
        if (!ok) {
            qCritical() << "FAIL: Thinking:" << what;
            ++failures;
        }
    };
    auto *plugins = controller.frontendPlugins();
    const QString id = openghost::ThinkingPlugin::Id;
    const QString collapsed = openghost::ThinkingPlugin::StartCollapsed;
    auto *list = window->findChild<QQuickItem *>(QStringLiteral("transcript"));
    auto *model = controller.transcript();
    check(list && plugins->enabled(id) && plugins->option(id, collapsed),
          "registered and on, starting shut, by default");
    if (!list)
        return failures;
    const auto entries = [&] {
        QVector<QQuickItem *> found;
        collect(list, QStringLiteral("thinkingEntry"), found);
        found.removeIf([](QQuickItem *item) { return !item->isVisible(); });
        return found;
    };
    const auto rows = [&] {
        QVector<int> found;
        for (int i = 0; i < model->rowCount(); ++i)
            if (model->data(model->index(i), TranscriptModel::KindRole).toString() == "thinking")
                found.append(i);
        return found;
    };
    const auto text = [](QQuickItem *entry, const char *name) {
        auto *item = entry ? find(entry, QString::fromLatin1(name)) : nullptr;
        return item ? item->property("text").toString() : QString();
    };
    const auto body = [](QQuickItem *entry) {
        auto *item = entry ? find(entry, QStringLiteral("thinkingBody")) : nullptr;
        return item && item->isVisible() ? item->property("content").toString() : QString();
    };

    controller.newChat();
    QTest::qWait(50);
    check(controller.send(QStringLiteral("/fake thinking")) != 0, "thinking run accepted");
    check(QTest::qWaitFor([&] { return entries().size() == 1; }, 3000), "its thinking is drawn");
    check(QTest::qWaitFor(
              [&] { return text(entries().value(0), "thinkingLatest") == "Planning the reply"; },
              3000),
          "shut while live: only the newest thought shows");
    check(text(entries().value(0), "thinkingSummary") == QStringLiteral("Thinking…") &&
              body(entries().value(0)).isEmpty(),
          "shut while live: Thinking… and no history");
    model->toggle(rows().value(0));
    QTest::qWait(50);
    check(body(entries().value(0)).contains(QStringLiteral("Reading the message")),
          "opened while live: the whole history");
    model->toggle(rows().value(0));
    check(QTest::qWaitFor([&] { return !controller.busy() && !controller.admitting(); }, 5000),
          "thinking run completes");
    QTest::qWait(50);
    auto *entry = entries().value(0);
    check(text(entry, "thinkingSummary") == QStringLiteral("Thinking") &&
              text(entry, "thinkingLatest").isEmpty() && body(entry).isEmpty(),
          "done: Thinking, shut");
    const int row = rows().value(0);
    check(row >= 0 && row + 1 < model->rowCount() &&
              model->data(model->index(row + 1), TranscriptModel::KindRole) == "assistant" &&
              model->data(model->index(row + 1), TranscriptModel::JoinRole).toInt() ==
                  Entry::AfterThinking,
          "the reply follows its thinking, 12 px below");
    check(model->rowTexts(row).size() == 1, "shut: only its summary is selectable");
    model->toggle(row);
    QTest::qWait(50);
    check(body(entries().value(0)) ==
              QStringLiteral("**Reading the message**\nA simulated question; nothing is sent.\n\n"
                             "**Planning the reply**\nEcho it back, as the fake backend always does."),
          "opened: the thinking as the backend finally had it");
    check(model->rowTexts(row).size() == 2, "open: its text is selectable too");
    model->toggle(row);

    // Start Collapsed off: rows shown open at once, and a new run arrives open.
    check(plugins->setOption(id, collapsed, false), "Start Collapsed off");
    check(QTest::qWaitFor([&] { return !body(entries().value(0)).isEmpty(); }, 1000),
          "a shown thinking opens at once");
    check(controller.send(QStringLiteral("/fake thinking")) != 0, "second thinking run accepted");
    check(QTest::qWaitFor(
              [&] {
                  const auto all = entries();
                  return all.size() == 2 && text(all.value(1), "thinkingSummary") == "Thinking…" &&
                         !body(all.value(1)).isEmpty();
              },
              3000),
          "a new thinking streams open");
    check(QTest::qWaitFor([&] { return !controller.busy(); }, 5000), "second run completes");
    check(plugins->setOption(id, collapsed, true), "Start Collapsed on");
    check(QTest::qWaitFor(
              [&] { return body(entries().value(0)).isEmpty() && body(entries().value(1)).isEmpty(); },
              1000),
          "shown thinking shuts at once");

    // Show Thinking off and on: the same rows, nothing sent.
    const int count = model->rowCount();
    check(plugins->setEnabled(id, false), "Show Thinking off");
    check(rows().isEmpty() && model->rowCount() == count - 2 &&
              QTest::qWaitFor([&] { return entries().isEmpty(); }, 1000),
          "off: no thinking drawn, replies as before");
    check(plugins->setEnabled(id, true), "Show Thinking on");
    check(rows().size() == 2 && model->rowCount() == count &&
              QTest::qWaitFor([&] { return entries().size() == 2; }, 1000),
          "on: the same thinking again, at once");
    if (!failures)
        qInfo() << "PASS: Thinking";
    return failures;
}
