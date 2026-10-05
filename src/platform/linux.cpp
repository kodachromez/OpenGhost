#include "platform.h"
#include <QDir>
#include <QGuiApplication>
#include <QSettings>

namespace
{
bool selectedPortal = false;
}

void platform::beforeApplication()
{
    // Ghosty's Wayland/NVIDIA frame pacing and native portal file chooser.
    // Explicit user selections continue to win. Never apply this on Windows/macOS.
    if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP"))
        qputenv("QSG_RENDER_LOOP", "threaded");
    selectedPortal = qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME");
    if (selectedPortal)
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    QGuiApplication::setDesktopFileName(QStringLiteral("openghost-native"));
}

bool platform::systemReducedMotion()
{
    const QSettings kde(QDir::homePath() + QStringLiteral("/.config/kdeglobals"),
                        QSettings::IniFormat);
    return kde.value(QStringLiteral("KDE/AnimationDurationFactor"), 1).toDouble() == 0;
}

void platform::afterApplication()
{
    if (selectedPortal)
        qunsetenv("QT_QPA_PLATFORMTHEME");
}
