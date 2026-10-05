#pragma once
#include <QString>

// OS integration belongs here, not in renderers or backend adapters.
namespace platform
{
void beforeApplication();
void afterApplication();
bool reducedMotion();
bool systemReducedMotion();
bool openLink(const QString &url, bool allowMailto);
} // namespace platform
