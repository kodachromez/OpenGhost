#include "tool_calls_plugin.h"
#include "window.h"
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

namespace
{
void collect(QQuickItem *item, const QString &name, QVector<QQuickItem *> &found)
{
    if (item->objectName() == name)
        found.append(item);
    for (auto *child : item->childItems())
        collect(child, name, found);
}
QQuickItem *within(QQuickItem *item, const QString &name)
{
    QVector<QQuickItem *> found;
    collect(item, name, found);
    return found.value(0);
}
} // namespace

// The Tool Calls plugin in the real transcript over the fake backend: a call's
// card appears and updates with its run, opens and shuts, waits on an
// approval, and the plugin turns off and on (twice OFF → ON → OFF → ON) at
// once, with every hook gone while off and exactly one card per call while on.
int toolCallsSmoke(QQuickWindow *window, WindowController &controller)
{
    int failures = 0;
    const auto check = [&](bool ok, const char *what) {
        if (!ok) {
            qCritical() << "FAIL: Tool Calls:" << what;
            ++failures;
        }
    };
    auto *plugins = controller.frontendPlugins();
    const QString id = openghost::ToolCallsPlugin::Id;
    auto *list = window->findChild<QQuickItem *>(QStringLiteral("transcript"));
    auto *model = controller.transcript();
    check(list && plugins->enabled(id) && plugins->hooks() >= 1, "registered and on by default");
    if (!list)
        return failures;
    const auto cards = [&] {
        QVector<QQuickItem *> found;
        collect(list, QStringLiteral("toolCard"), found);
        found.removeIf([](QQuickItem *card) { return !card->isVisible(); });
        return found;
    };
    const auto toolRows = [&] {
        QVector<int> rows;
        for (int i = 0; i < model->rowCount(); ++i)
            if (model->data(model->index(i), TranscriptModel::KindRole).toString() == "tool")
                rows.append(i);
        return rows;
    };
    const auto text = [](QQuickItem *card, const char *name) {
        auto *item = card ? within(card, QString::fromLatin1(name)) : nullptr;
        return item ? item->property("text").toString() : QString();
    };
    const int hooks = plugins->hooks();

    controller.newChat();
    QTest::qWait(50);
    check(controller.send(QStringLiteral("/fake tools")) != 0, "tool run accepted");
    check(QTest::qWaitFor([&] { return !controller.busy() && !controller.admitting(); }, 5000),
          "tool run completes");
    check(toolRows().size() == 1, "one tool row for one call");
    check(QTest::qWaitFor([&] { return cards().size() == 1; }, 2000), "its card is drawn once");
    auto *card = cards().value(0);
    check(text(card, "toolName") == QStringLiteral("Demo"), "card head names the tool");
    check(text(card, "toolSummary") == QStringLiteral("{\"effects\":\"none\"}"),
          "collapsed head shows the arguments");
    check(text(card, "toolState") == QStringLiteral("✓ Done"), "result shows Done");
    check(card && !within(card, QStringLiteral("output")), "a shut card lays out no body");
    check(model->rowTexts(toolRows().value(0)).isEmpty(), "a shut card has no selectable text");
    // Open: the result in its well and how the call ended.
    model->toggle(toolRows().value(0));
    QTest::qWait(50);
    card = cards().value(0);
    auto *output = card ? within(card, QStringLiteral("output")) : nullptr;
    check(output && output->property("content").toString() ==
                        QStringLiteral("Simulated result; no host action"),
          "open card shows the output");
    auto *end = card ? within(card, QStringLiteral("toolEnd")) : nullptr;
    check(end && end->isVisible(), "open card shows how it ended");
    check(model->rowTexts(toolRows().value(0)).size() >= 2,
          "open card's texts reach the selection");
    const int withCards = model->rowCount();

    for (int cycle = 0; cycle < 2; ++cycle) {
        check(plugins->setEnabled(id, false), "Off applies");
        check(plugins->hooks() == hooks - 1 && !plugins->renderers().contains("tool"),
              "Off removes its hook");
        check(toolRows().isEmpty() && model->rowCount() == withCards - 1,
              "Off: the transcript as before tool rows");
        check(QTest::qWaitFor([&] { return cards().isEmpty(); }, 2000), "Off: no card is drawn");
        check(plugins->setEnabled(id, true), "On applies");
        check(plugins->hooks() == hooks, "On registers exactly its hook again");
        check(toolRows().size() == 1 && model->rowCount() == withCards,
              "On: the same rows again, nothing duplicated");
        check(QTest::qWaitFor([&] { return cards().size() == 1; }, 2000),
              "On: one card again, at once");
        card = cards().value(0);
        check(card && within(card, QStringLiteral("output")),
              "the reader's open card stays open");
        check(card && card->parentItem() && card->parentItem()->property("arrival").toDouble() == 1,
              "shown again, not arriving again");
    }

    // A call waiting on an approval, while the plugin goes off and on.
    check(controller.send(QStringLiteral("/fake approval")) != 0, "approval run accepted");
    check(QTest::qWaitFor([&] { return !controller.approvals().isEmpty(); }, 3000),
          "approval asked");
    check(QTest::qWaitFor([&] { return cards().size() == 2; }, 2000), "a second card");
    check(text(cards().value(1), "toolState") == QStringLiteral("Waiting for approval"),
          "an unanswered call waits, not runs");
    check(plugins->setEnabled(id, false) && plugins->setEnabled(id, true) &&
              plugins->setEnabled(id, false) && plugins->setEnabled(id, true),
          "Off and On while it waits");
    check(QTest::qWaitFor([&] { return cards().size() == 2; }, 2000) && toolRows().size() == 2,
          "still one card per call");
    controller.approve(controller.approvals().first().toMap().value("requestId").toString(), true);
    check(QTest::qWaitFor([&] { return !controller.busy(); }, 3000), "approved run completes");
    check(QTest::qWaitFor(
              [&] { return text(cards().value(1), "toolState") == QStringLiteral("✓ Done"); }, 2000),
          "its card follows the run to its result");
    check(plugins->hooks() == hooks && cards().size() == 2, "no hook or card left over");
    return failures;
}
