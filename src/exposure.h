#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtQml/qqmlregistration.h>

// Whether a window can currently show anything: visible, not minimized and
// exposed (a compositor can unexpose a suspended or covered window without
// minimizing it). Follows the window's own expose and visibility events;
// nothing polls.
class WindowExposure : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool exposed READ exposed NOTIFY exposedChanged)

  public:
    using QObject::QObject;
    QQuickWindow *window() const { return m_window; }
    void setWindow(QQuickWindow *window);
    bool exposed() const { return m_exposed; }

  signals:
    void windowChanged();
    void exposedChanged();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void update();

    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_visibility;
    bool m_exposed = false;
};

// The wheel turns that reach an item (a scroll view), as a browser's wheel
// event reports them: `deltaY` in pixels, positive downward, a mouse notch
// counting as Chromium's 53 px on Linux. It only watches: the item still
// handles the event.
class WheelWatch : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY targetChanged)

  public:
    using QObject::QObject;
    QQuickItem *target() const { return m_target; }
    void setTarget(QQuickItem *target);

  signals:
    void targetChanged();
    void wheeled(qreal deltaY);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QPointer<QQuickItem> m_target;
};

// The wheel turns that reach a scroll view, scrolled as OpenGhost 1.2's
// Chromium scrolls them on Wayland: 120 px a notch (angleDelta 120), and a
// pixel delta 12 times over (kWheelDelta / kAxisValueScale). Turns reach
// `target` when nothing under the pointer (a tool well, WellScroll) took
// them, so a scroll begun over a well that can move that way stays in it
// and one begun at its end chains here. A scroll begun here stays here
// (and one begun in a well stays there) wherever the pointer goes. The view
// takes none itself: notches and phaseless deltas come as `notched` (px,
// positive toward the end) for it to glide, a touchpad gesture's updates as
// `tracked` (px, and the event's timestamp in ms) to follow at once, and
// its end as `released`. `wheeled` reports each turn as WheelWatch does.
class WheelScroll : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY targetChanged)

  public:
    using QObject::QObject;
    QQuickItem *target() const { return m_target; }
    void setTarget(QQuickItem *target);

  signals:
    void targetChanged();
    void notched(qreal pixels);
    void tracked(qreal pixels, qreal timestamp);
    void released();
    void wheeled(qreal deltaY);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QPointer<QQuickItem> m_target;
};

// A scroll view within the transcript (a tool well) that owns the scrolls
// begun over it: Chromium latches a scroll (a touchpad gesture, or a run of
// wheel turns; here, turns under 500 ms apart) to the innermost scroller
// under the pointer that can move its first way, and only that scroller
// moves until the scroll ends. So a scroll begun over `target` while it
// can move that way moves it alone, holding at its end; one it cannot take
// passes on to the transcript (WheelScroll), and a scroll another view owns
// goes there. `target` (a Flickable) scrolls each turn it owns as it would
// without this.
class WellScroll : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY targetChanged)

  public:
    using QObject::QObject;
    QQuickItem *target() const { return m_target; }
    void setTarget(QQuickItem *target);

  signals:
    void targetChanged();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QPointer<QQuickItem> m_target;
    bool m_forwarding = false;
};

// Any key, button or wheel turn that reaches a window, as Chromium's tooltip
// controller hears them (each hides its tooltip). It only watches: the
// window delivers the event as before.
class InputWatch : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)

  public:
    using QObject::QObject;
    QQuickWindow *window() const { return m_window; }
    void setWindow(QQuickWindow *window);

  signals:
    void windowChanged();
    void pressed();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QPointer<QQuickWindow> m_window;
};

// Enter and Return press the focused button as Space does (Qt Quick's
// AbstractButton takes only Space): it is down while the key is held and
// acts on release, so a key activation shows the same press as a click.
// OpenGhost's buttons act on Enter's keydown, which clears their press
// before it is drawn. The button and its handlers see Space; any other
// focused item gets Enter as it is.
class ButtonKeys : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)

  public:
    using QObject::QObject;
    QQuickWindow *window() const { return m_window; }
    void setWindow(QQuickWindow *window);

  signals:
    void windowChanged();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QPointer<QQuickWindow> m_window;
    bool m_held = false; // An Enter press went to a button as Space.
};
