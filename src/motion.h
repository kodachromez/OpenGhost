#pragma once

#include <QAbstractAnimation>

#include <functional>

// Calls `tick` with the wall time since the previous call, once per frame
// the animation driver advances (the display's rate under Qt Quick's render
// loop), while running. Nothing ticks while stopped.
class FrameClock final : public QAbstractAnimation
{
  public:
    explicit FrameClock(std::function<void(qreal)> tick, QObject *parent = nullptr);
    int duration() const override { return -1; }
    // Monotonic seconds, the clock's time base.
    static double now();
    // Tests only: hold the clock at `seconds` (a stepped, deterministic
    // timeline); a negative value returns it to real time.
    static void setTestTime(double seconds);
    // The latest interval any clock ticked with: a frame's length (0 before any).
    static double interval();

  protected:
    void updateCurrentTime(int) override;
    void updateState(State next, State previous) override;

  private:
    std::function<void(qreal)> m_tick;
    double m_last = -1;
};

// CSS cubic-bezier(x1, y1, x2, y2) at progress t in [0, 1].
double cubicBezier(double x1, double y1, double x2, double y2, double t);
