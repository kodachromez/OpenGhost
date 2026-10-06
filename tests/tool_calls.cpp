// The Tool Calls frontend plugin without a window: its one hook (the "tool"
// row renderer) through repeated On/Off, its saved choice, and the card texts
// it shares with the selection (Ghosty's toolcard, with Pi's tool schemas).
#include "frontend/frontend_plugins.h"
#include "frontend/preferences.h"
#include "tool_calls_plugin.h"
#include "toolcard.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace openghost;

namespace
{
QVariantMap toolRow(const QString &name, const QString &arguments, const QString &state,
                    const QString &body, bool expanded)
{
    return {{"toolName", name},      {"arguments", arguments}, {"argumentsKnown", true},
            {"messageState", state}, {"body", body},           {"ending", QString()},
            {"expanded", expanded},  {"omittedLines", 0.0},    {"omittedCharacters", 0.0}};
}
class Rival final : public FrontendPlugin
{
  public:
    FrontendPluginInfo info() const override { return {"test.rival", "Rival", {}, true}; }
    void enable(FrontendPluginContext &context) override
    {
        context.renderRows(QStringLiteral("tool"), {QUrl(QStringLiteral("qrc:/rival.qml")), {}, {}, false});
    }
};
QStringList paths(const QVector<SelectionText> &texts)
{
    QStringList out;
    for (const auto &text : texts)
        out.append(text.path);
    return out;
}
} // namespace

class ToolCallsTest final : public QObject
{
    Q_OBJECT
  private slots:
    void onOffCyclesLeaveNoHooks()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("preferences.json"));
        PreferencesStore store(path);
        FrontendPlugins plugins(&store);
        QSignalSpy renderers(&plugins, &FrontendPlugins::renderersChanged);
        const QString id = ToolCallsPlugin::Id;
        QVERIFY(plugins.add(std::make_unique<ToolCallsPlugin>()));
        // On by default: one hook, the tool row renderer.
        QVERIFY(plugins.enabled(id));
        QCOMPARE(plugins.hooks(), 1);
        QVERIFY(plugins.renderer(QStringLiteral("tool")));
        QCOMPARE(plugins.renderers().value(QStringLiteral("tool")).toString(),
                 ToolCallsPlugin::Delegate);
        QVERIFY(!plugins.renderer(QStringLiteral("thinking")));
        auto generation = plugins.renderGeneration();
        // OFF → ON → OFF → ON, twice: each switch adds or removes exactly the
        // one renderer and tells the window once.
        for (int cycle = 0; cycle < 2; ++cycle) {
            for (const bool on : {false, true, false, true}) {
                const int before = int(renderers.count());
                QVERIFY(plugins.setEnabled(id, on));
                QCOMPARE(plugins.enabled(id), on);
                QCOMPARE(plugins.hooks(), on ? 1 : 0);
                QCOMPARE(plugins.renderers().size(), on ? 1 : 0);
                QCOMPARE(bool(plugins.renderer(QStringLiteral("tool"))), on);
                QCOMPARE(int(renderers.count()), before + 1);
                QVERIFY(plugins.renderGeneration() > generation);
                generation = plugins.renderGeneration();
                // Saved before it applies, through the existing preferences.
                QCOMPARE(PreferencesStore(path).value().frontendPlugins.value(id), on);
            }
        }
        // Asking again for the current state changes nothing.
        const int before = int(renderers.count());
        QVERIFY(plugins.setEnabled(id, true));
        QCOMPARE(int(renderers.count()), before);
        QCOMPARE(plugins.hooks(), 1);
        // Off survives a restart; unregistering leaves nothing behind.
        QVERIFY(plugins.setEnabled(id, false));
        {
            PreferencesStore again(path);
            FrontendPlugins restarted(&again);
            restarted.add(std::make_unique<ToolCallsPlugin>());
            QVERIFY(!restarted.enabled(id));
            QCOMPARE(restarted.hooks(), 0);
            QVERIFY(restarted.renderers().isEmpty());
            QVERIFY(restarted.setEnabled(id, true));
            QVERIFY(restarted.remove(id));
            QCOMPARE(restarted.hooks(), 0);
            QVERIFY(restarted.renderers().isEmpty());
        }
        // One plugin per kind: a later claim does not replace the first.
        QVERIFY(plugins.setEnabled(id, true));
        QVERIFY(plugins.add(std::make_unique<Rival>()));
        QCOMPARE(plugins.renderers().value(QStringLiteral("tool")).toString(),
                 ToolCallsPlugin::Delegate);
        QCOMPARE(plugins.hooks(), 2);
        QVERIFY(plugins.setEnabled(id, false));
        QCOMPARE(plugins.renderers().value(QStringLiteral("tool")).toString(),
                 QStringLiteral("qrc:/rival.qml"));
    }

    void selectionCopiesTheCardTexts()
    {
        FrontendPlugins plugins(nullptr);
        plugins.add(std::make_unique<ToolCallsPlugin>());
        const auto *renderer = plugins.renderer(QStringLiteral("tool"));
        QVERIFY(renderer && renderer->selection);
        // A shut card has no selectable text (its head is not text to select).
        QVERIFY(renderer
                    ->selection(toolRow("bash", "{\"command\":\"ls\"}", "running", "out", false))
                    .isEmpty());
        // Open: the command, the output, the live note; no ending while running.
        QCOMPARE(paths(renderer->selection(
                     toolRow("bash", "{\"command\":\"ls\"}", "running", "out", true))),
                 (QStringList{"t/c:t", "t/b:t", "t/v:t"}));
        const auto done = renderer->selection(
            toolRow("bash", "{\"command\":\"ls\"}", "done", "a\r\nb", true));
        QCOMPARE(paths(done), (QStringList{"t/c:t", "t/b:t", "t/e0:t"}));
        QCOMPARE(done[1].text, QStringLiteral("a\nb")); // As its document holds it.
        QCOMPARE(done[2].text, QStringLiteral("Done"));
        const auto failed = renderer->selection(toolRow("x", "{}", "unconfirmed", "", true));
        QCOMPARE(failed.last().text, QStringLiteral("Result unconfirmed"));
    }

    void piToolsAreDescribed()
    {
        // Pi's bash, read and write match Ghosty's schemas.
        auto info = toolcard::describe("bash", "{\"command\":\"ls -la\"}", true);
        QCOMPARE(info.title, QStringLiteral("Run a command"));
        QCOMPARE(info.code, QStringLiteral("ls -la"));
        QCOMPARE(info.summary, QStringLiteral("ls -la"));
        info = toolcard::describe("read", "{\"path\":\"a.md\",\"offset\":3,\"limit\":2}", true);
        QCOMPARE(info.kind, QStringLiteral("file"));
        QCOMPARE(info.path, QStringLiteral("a.md"));
        QCOMPARE(info.text, QStringLiteral("Lines 3–4"));
        // Pi's edit: edits [{oldText, newText}] as removed and added lines.
        info = toolcard::describe(
            "edit", "{\"path\":\"a.cpp\",\"edits\":[{\"oldText\":\"int a;\",\"newText\":\"int b;\\nint c;\"}]}",
            true);
        QCOMPARE(info.title, QStringLiteral("Edit a file"));
        QCOMPARE(info.removedLines.shown, (QStringList{"int a;"}));
        QCOMPARE(info.addedLines.shown, (QStringList{"int b;", "int c;"}));
        // Pi's legacy top-level form too.
        info = toolcard::describe("edit", "{\"path\":\"a\",\"oldText\":\"x\",\"newText\":\"y\"}", true);
        QCOMPARE(info.removed, QStringLiteral("x"));
        QCOMPARE(info.added, QStringLiteral("y"));
        // Unknown and extension tools keep the generic card: their JSON, nothing guessed.
        info = toolcard::describe("grep", "{ \"pattern\": \"a b\" }", true);
        QCOMPARE(info.title, QStringLiteral("grep"));
        QCOMPARE(info.code, QStringLiteral("{\"pattern\":\"a b\"}"));
        // Ghosty's subagent panel is not part of this plugin.
        info = toolcard::describe("subagent", "{\"task\":\"t\"}", true);
        QCOMPARE(info.title, QStringLiteral("subagent"));
        // Clipped (invalid) arguments are shown raw; unknown ones not at all.
        const QString clipped = QStringLiteral("{\"command\":\"ls") + QChar(0x2026);
        QCOMPARE(toolcard::describe("bash", clipped, true).code, clipped);
        QVERIFY(toolcard::describe("bash", {}, false).code.isEmpty());
        // States are said as they are; none is success but done.
        QCOMPARE(toolcard::endLines("missing", {}), (QStringList{"No result was saved"}));
        QCOMPARE(toolcard::endLines("error", {}), (QStringList{"Failed"}));
        QCOMPARE(toolcard::liveNote("unconfirmed", true), QStringLiteral("Partial live output"));
        QCOMPARE(toolcard::omission(2, 5),
                 QStringLiteral("2 earlier lines and 5 characters omitted"));
    }
};

QTEST_GUILESS_MAIN(ToolCallsTest)
#include "tool_calls.moc"
