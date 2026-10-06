#pragma once
#include "frontend_plugin.h"

namespace openghost
{
// Permissions: the switch for Pi's permission plugin (plugin-permissions) and
// OpenGhost's permission UI (docs/plugin-permissions.md). Settings changes it
// only by restarting OpenGhost (FrontendPluginInfo::restart), so Pi and the
// window never run half switched. On, every chat's Pi
// loads plugin-permissions and the window shows the Ask / Auto / Full picker
// and the approval card's decisions and shortcuts. Off, Pi starts chats
// without it (its tools run without asking, and OpenGhost says so), a request
// already waiting is declined, and none of that UI shows.
//
// It registers no hooks: the window reads whether it is on
// (WindowController::permissions) and tells the backend; the plugin itself
// reaches no backend.
class PermissionsPlugin final : public FrontendPlugin
{
  public:
    static inline const QString Id = QStringLiteral("openghost.plugin-permissions");
    FrontendPluginInfo info() const override;
    void enable(FrontendPluginContext &) override {}
};
} // namespace openghost
