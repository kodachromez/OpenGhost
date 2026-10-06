#include "example_plugin.h"

namespace openghost
{
FrontendPluginInfo ExamplePlugin::info() const
{
    return {Id, QStringLiteral("Example plugin"),
            QStringLiteral("Shows that frontend plugins work. Changes nothing in chats."), false};
}
void ExamplePlugin::enable(FrontendPluginContext &context)
{
    m_context = &context;
    m_seen = 0;
    context.subscribe(events::ChatChanged, [this](const FrontendEvent &) {
        ++m_seen;
        m_context->update();
    });
    context.decorateRows([](const ChatRowView &) { return QVariantMap{{"seen", true}}; });
    context.setVisible(QStringLiteral("example"), true);
}
void ExamplePlugin::disable() { m_context = nullptr; }
QString ExamplePlugin::status() const
{
    return m_context ? QStringLiteral("Active · %1 chat updates seen").arg(m_seen) : QString();
}
} // namespace openghost
