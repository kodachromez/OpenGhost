#include "tool_calls_plugin.h"
#include "toolcard.h"

namespace openghost
{
FrontendPluginInfo ToolCallsPlugin::info() const
{
    return {Id, QStringLiteral("Tool Calls"),
            QStringLiteral("Shows each tool call as a card with its arguments, output and "
                           "result. Off, replies show their text only."),
            true};
}
void ToolCallsPlugin::enable(FrontendPluginContext &context)
{
    m_context = &context;
    RowRenderer renderer;
    renderer.delegate = QUrl(Delegate);
    renderer.selection = [](const QVariantMap &row) {
        toolcard::Row tool;
        tool.name = row.value(QStringLiteral("toolName")).toString();
        tool.arguments = row.value(QStringLiteral("arguments")).toString();
        tool.known = row.value(QStringLiteral("argumentsKnown")).toBool();
        tool.state = row.value(QStringLiteral("messageState")).toString();
        tool.body = row.value(QStringLiteral("body")).toString();
        tool.ending = row.value(QStringLiteral("ending")).toString();
        tool.expanded = row.value(QStringLiteral("expanded")).toBool();
        tool.omittedLines = row.value(QStringLiteral("omittedLines")).toDouble();
        tool.omittedCharacters = row.value(QStringLiteral("omittedCharacters")).toDouble();
        return toolcard::units(tool);
    };
    renderer.trimmedText = QStringLiteral("t/b:t"); // The output (ToolCard.qml).
    context.renderRows(QStringLiteral("tool"), renderer);
}
void ToolCallsPlugin::disable() { m_context = nullptr; }
} // namespace openghost
