#pragma once

#include "motion.h"

#include <QColor>
#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

// OpenGhost's Ghost (ghost-thinking.js): the same procedural body, eyes,
// twelve poses, springs, bob, tail sway, blinks and gaze, drawn as one
// vertex-antialiased scene-graph geometry node (one draw call) rebuilt from a
// few numbers per frame. Its clock follows the render loop and runs only
// while `running` and the item is visible, on screen in its view and in a
// shown window; with reduced motion the Ghost is still. Parked out of view, it
// wakes on any move of itself or an item between it and its viewport. A Ghost
// handed over (splash → welcome → working) continues another's motion.
class GhostItem : public QQuickItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Ghost)
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(
        bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY reducedMotionChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(QColor eyeColor READ eyeColor WRITE setEyeColor NOTIFY eyeColorChanged)
    Q_PROPERTY(bool ticking READ ticking NOTIFY tickingChanged)

  public:
    explicit GhostItem(QQuickItem *parent = nullptr);
    ~GhostItem() override;

    bool running() const { return m_running; }
    void setRunning(bool running);
    bool reducedMotion() const { return m_reducedMotion; }
    void setReducedMotion(bool reduced);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    QColor eyeColor() const { return m_eyeColor; }
    void setEyeColor(const QColor &color);
    bool ticking() const { return m_clock.state() == QAbstractAnimation::Running; }

    // Looks at (x, y) in eye units for holdMs, then resumes its poses.
    Q_INVOKABLE void look(qreal x, qreal y, int holdMs = 1800);
    // Blinks now (the splash's Ghost, a moment after it lands).
    Q_INVOKABLE void blink();
    // A fresh Ghost: its loop restarts from the first pose.
    Q_INVOKABLE void restart();
    // Continues another Ghost's motion (a handoff): its pose, eye springs,
    // blink, gaze, bob and sway, and when each next changes.
    Q_INVOKABLE void continueFrom(GhostItem *other);
    // Frames advanced since creation, and restarts (tests).
    Q_INVOKABLE int frames() const { return m_frames; }
    Q_INVOKABLE int restarts() const { return m_restarts; }
    // The current pose, eye springs and bob, for tests.
    Q_INVOKABLE QVariantMap state() const;

    static constexpr qreal ViewWidth = 64, ViewHeight = 70;
    // Eye offset and scale, in the view box's units.
    struct Pose {
        double x, y, sx, sy;
    };

  signals:
    void runningChanged();
    void reducedMotionChanged();
    void colorChanged();
    void eyeColorChanged();
    void tickingChanged();

  protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void componentComplete() override;

  private slots:
    void sync();

  private:
    struct Spring {
        double value, velocity;
    };
    void tick(qreal elapsed);
    void watchPath(bool watch);
    bool onScreen() const;
    const Pose &pickPose();
    void reset(double now);

    FrameClock m_clock;
    bool m_running = true, m_reducedMotion = false;
    QColor m_color{250, 250, 250}, m_eyeColor{25, 25, 25};
    // ghost-thinking.js state, times in seconds.
    double m_start = 0, m_now = 0, m_nextPose = 0, m_nextBlink = 0, m_blinkAt = -1e9;
    double m_gazeUntil = 0;
    bool m_gazing = false;
    Pose m_gaze{0, 0, 1, 1};
    const Pose *m_pose = nullptr;
    Spring m_eye[4] = {{0, 0}, {0, 0}, {1, 0}, {1, 0}}; // x, y, sx, sy
    double m_float = 0, m_sway = 0, m_blink = 1;
    int m_frames = 0, m_restarts = 0;
    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_visibility;
    // While parked: the items from this one up to its viewport, whose moves
    // can bring it back into view.
    QList<QPointer<QQuickItem>> m_path;
    QList<QMetaObject::Connection> m_pathWatch;
};
