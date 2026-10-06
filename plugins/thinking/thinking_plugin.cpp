#include "thinking_plugin.h"

namespace openghost
{
FrontendPluginInfo ThinkingPlugin::info() const
{
    FrontendPluginInfo info{Id, QStringLiteral("Thinking"),
                            QStringLiteral("Shows the model's thinking above its reply. Off, "
                                           "replies show their text only."),
                            true};
    info.placement = QStringLiteral("chat");
    info.options.insert(StartCollapsed, true);
    return info;
}
void ThinkingPlugin::enable(FrontendPluginContext &context)
{
    m_context = &context;
    draw();
}
void ThinkingPlugin::disable() { m_context = nullptr; }
void ThinkingPlugin::optionChanged(const QString &key)
{
    if (m_context && key == StartCollapsed)
        draw();
}
void ThinkingPlugin::draw()
{
    RowRenderer renderer;
    renderer.delegate = QUrl(Delegate);
    // As ThinkingEntry.qml shows them: the summary, then (open) the text. Live
    // thinking's newest item beside the summary only repeats the text below.
    renderer.selection = [](const QVariantMap &row) {
        const bool live = row.value(QStringLiteral("messageState")).toString() == QLatin1String("live");
        QVector<SelectionText> texts{
            {QStringLiteral("k:s"), live ? QStringLiteral("Thinking…") : QStringLiteral("Thinking")}};
        if (row.value(QStringLiteral("expanded")).toBool())
            texts.append({QStringLiteral("k:t"), row.value(QStringLiteral("body")).toString()});
        return texts;
    };
    renderer.startExpanded = !m_context->option(StartCollapsed);
    m_context->renderRows(QStringLiteral("thinking"), renderer);
}
} // namespace openghost
