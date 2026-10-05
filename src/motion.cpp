#include "motion.h"

#include <QElapsedTimer>

#include <cmath>

FrameClock::FrameClock(std::function<void(qreal)> tick, QObject *parent)
    : QAbstractAnimation(parent), m_tick(std::move(tick))
{
}

double FrameClock::now()
{
    static const QElapsedTimer clock = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    return clock.nsecsElapsed() / 1e9;
}

namespace
{
double lastInterval = 0;
}

double FrameClock::interval() { return lastInterval; }

void FrameClock::updateState(State next, State)
{
    if (next == Running)
        m_last = -1;
}

void FrameClock::updateCurrentTime(int)
{
    // The driver's own time is whole milliseconds; wall time keeps motion
    // smooth at 240 Hz, where a frame is about 4.17 ms.
    const double at = now();
    const double elapsed = m_last < 0 ? 0 : at - m_last;
    m_last = at;
    if (elapsed > 0) {
        lastInterval = elapsed;
        m_tick(elapsed);
    }
}

double cubicBezier(double x1, double y1, double x2, double y2, double t)
{
    if (t <= 0)
        return 0;
    if (t >= 1)
        return 1;
    auto curve = [](double a, double b, double u) {
        const double v = 1 - u;
        return 3 * v * v * u * a + 3 * v * u * u * b + u * u * u;
    };
    auto slope = [](double a, double b, double u) {
        const double v = 1 - u;
        return 3 * v * v * a + 6 * v * u * (b - a) + 3 * u * u * (1 - b);
    };
    // Solve x(u) = t: Newton from t, bisection when the slope is flat.
    double u = t;
    for (int i = 0; i < 8; ++i) {
        const double error = curve(x1, x2, u) - t;
        if (std::abs(error) < 1e-7)
            return curve(y1, y2, u);
        const double d = slope(x1, x2, u);
        if (std::abs(d) < 1e-6)
            break;
        u -= error / d;
    }
    double lo = 0, hi = 1;
    u = t;
    for (int i = 0; i < 40; ++i) {
        const double x = curve(x1, x2, u);
        if (std::abs(x - t) < 1e-7)
            break;
        (x < t ? lo : hi) = u;
        u = (lo + hi) / 2;
    }
    return curve(y1, y2, u);
}
