#pragma once
#include "frontend/frontend_plugin.h"

namespace openghost
{
// Thinking: draws each assistant message's thinking as Ghosty's thinking row
// (a "Thinking" disclosure above the reply; while it streams, "Thinking…" and
// its newest item). Off, chats look as they did without thinking rows. The
// thinking's data is the host's either way (ChatService), so turning it on
// shows earlier thinking too.
//
// One hook: the "thinking" row renderer. Its option "startCollapsed" says
// whether rows arrive shut (the default, as Ghosty's do) or open; changing it
// registers the renderer again, so rows already shown follow it at once.
// Settings offers both under Appearance → Chat Settings.
class ThinkingPlugin final : public FrontendPlugin
{
  public:
    static inline const QString Id = QStringLiteral("openghost.thinking");
    static inline const QString StartCollapsed = QStringLiteral("startCollapsed");
    static inline const QString Delegate = QStringLiteral("qrc:/OpenGhost/Ui/ThinkingEntry.qml");
    FrontendPluginInfo info() const override;
    void enable(FrontendPluginContext &context) override;
    void disable() override;
    void optionChanged(const QString &key) override;
    bool active() const { return m_context != nullptr; }

  private:
    void draw();
    FrontendPluginContext *m_context = nullptr;
};
} // namespace openghost
