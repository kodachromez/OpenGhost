#pragma once
#include "frontend_plugin.h"

namespace openghost
{
// The SDK's example: counts the open chat's updates (shown in Settings),
// marks each row's decorations with {"seen": true} and shows the UI target
// "example". Nothing in the window reads either, so it changes nothing shown.
// Registered only with OPENGHOST_EXAMPLE_PLUGIN=1 and by the tests.
class ExamplePlugin final : public FrontendPlugin
{
  public:
    static inline const QString Id = QStringLiteral("openghost.example");
    FrontendPluginInfo info() const override;
    void enable(FrontendPluginContext &context) override;
    void disable() override;
    QString status() const override;
    bool active() const { return m_context != nullptr; }

  private:
    FrontendPluginContext *m_context = nullptr;
    int m_seen = 0;
};
} // namespace openghost
