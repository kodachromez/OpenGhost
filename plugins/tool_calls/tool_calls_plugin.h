#pragma once
#include "frontend/frontend_plugin.h"

namespace openghost
{
// Tool Calls: draws each tool call of a chat as Ghosty's tool card (its
// arguments, live output, result and how it ended), between the reply's
// parts. Off, chats look as they did without tool rows: a reply is one
// message and its calls are not shown. The calls' data is the host's either
// way (ChatService), so turning it on shows earlier calls too.
//
// One hook: the "tool" row renderer (its card component, its selectable texts
// and the output text the host trims while it streams).
class ToolCallsPlugin final : public FrontendPlugin
{
  public:
    static inline const QString Id = QStringLiteral("openghost.tool-calls");
    static inline const QString Delegate = QStringLiteral("qrc:/OpenGhost/Ui/ToolEntry.qml");
    FrontendPluginInfo info() const override;
    void enable(FrontendPluginContext &context) override;
    void disable() override;
    bool active() const { return m_context != nullptr; }

  private:
    FrontendPluginContext *m_context = nullptr;
};
} // namespace openghost
