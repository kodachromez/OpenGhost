#include "permissions_plugin.h"

namespace openghost
{
FrontendPluginInfo PermissionsPlugin::info() const
{
    return {Id, QStringLiteral("Permissions"),
            QStringLiteral("Pi's permission plugin (plugin-permissions): Ask, Auto and Full, and "
                           "the approval card's decisions. Off, Pi's tools run without asking."),
            true};
}
} // namespace openghost
