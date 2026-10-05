#include "platform.h"

// Qt supplies native windows, clipboard, file dialogs and URL dispatch.
// No Linux theme, DBus, POSIX signal, or filesystem assumption enters this path.
// Windows/macOS packaging and native notification adapters remain unqualified.
void platform::beforeApplication() {}
void platform::afterApplication() {}
// Native accessibility preference discovery is a follow-up. The explicit
// OPENGHOST_REDUCED_MOTION override works on every platform now.
bool platform::systemReducedMotion() { return false; }
