#include "exposure.h"

#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QWheelEvent>

void WindowExposure::setWindow(QQuickWindow *window)
{
    if (window == m_window)
        return;
    if (m_window)
        m_window->removeEventFilter(this);
    disconnect(m_visibility);
    m_window = window;
    if (window) {
        window->installEventFilter(this);
        m_visibility = connect(window, &QWindow::visibilityChanged, this, &WindowExposure::update);
    }
    emit windowChanged();
    update();
}

bool WindowExposure::eventFilter(QObject *watched, QEvent *event)
{
    // The platform sets the window's exposure before delivering this.
    if (watched == m_window && event->type() == QEvent::Expose)
        update();
    return false;
}

void WindowExposure::update()
{
    const bool exposed = m_window && m_window->isVisible() &&
                         m_window->visibility() != QWindow::Minimized && m_window->isExposed();
    if (exposed == m_exposed)
        return;
    m_exposed = exposed;
    emit exposedChanged();
}

void WheelWatch::setTarget(QQuickItem *target)
{
    if (target == m_target)
        return;
    if (m_target)
        m_target->removeEventFilter(this);
    m_target = target;
    if (target)
        target->installEventFilter(this);
    emit targetChanged();
}

bool WheelWatch::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_target && event->type() == QEvent::Wheel) {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        const qreal dy = !wheel->pixelDelta().isNull() ? wheel->pixelDelta().y()
                                                       : wheel->angleDelta().y() / 120.0 * 53;
        if (dy != 0)
            emit wheeled(-dy);
    }
    return false;
}

namespace {

// The scroll under way (WellScroll): the view that owns it and its last
// turn's timestamp. A turn continues it unless it begins a touchpad gesture
// or comes 500 ms or more after the last; the owner gone, nobody owns it.
struct WheelLatch {
    QPointer<QQuickItem> owner;
    ulong last = 0;

    QQuickItem *owning(const QWheelEvent *wheel) const
    {
        const ulong at = wheel->timestamp();
        if (wheel->phase() == Qt::ScrollBegin || at < last || at - last >= 500)
            return nullptr;
        return owner;
    }
    void take(QQuickItem *view, const QWheelEvent *wheel)
    {
        owner = wheel->phase() == Qt::ScrollEnd ? nullptr : view;
        last = wheel->timestamp();
    }
};
WheelLatch latch;

// The turn's vertical step, positive toward the end.
qreal stepOf(const QWheelEvent *wheel)
{
    return -(wheel->pixelDelta().isNull() ? wheel->angleDelta().y() : wheel->pixelDelta().y());
}

// Whether a Flickable can move `step`'s way.
bool canScroll(const QQuickItem *view, qreal step)
{
    if (step == 0 || !view->property("interactive").toBool())
        return false;
    const qreal at = view->property("contentY").toReal() - view->property("originY").toReal();
    const qreal end = view->property("contentHeight").toReal() - view->height();
    return step > 0 ? at < end - 0.5 : at > 0.5;
}

} // namespace

void WellScroll::setTarget(QQuickItem *target)
{
    if (target == m_target)
        return;
    if (m_target)
        m_target->removeEventFilter(this);
    m_target = target;
    if (target)
        target->installEventFilter(this);
    emit targetChanged();
}

bool WellScroll::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_target || event->type() != QEvent::Wheel || m_forwarding)
        return false;
    auto *wheel = static_cast<QWheelEvent *>(event);
    QQuickItem *owner = latch.owning(wheel);
    if (!owner) {
        // A new scroll is the well's only if it can move its first way; a
        // touchpad gesture's beginning (no step yet) waits for that.
        if (!canScroll(m_target, stepOf(wheel))) {
            wheel->ignore(); // On to the view under the well.
            return true;
        }
        if (wheel->phase() == Qt::ScrollUpdate || wheel->phase() == Qt::ScrollMomentum) {
            // The Flickable follows a gesture from its beginning, sent a
            // millisecond earlier: it measures each update's speed from the
            // turn before and skips one with no time between.
            QWheelEvent begin(wheel->position(), wheel->globalPosition(), {}, {}, wheel->buttons(),
                              wheel->modifiers(), Qt::ScrollBegin, wheel->inverted(),
                              wheel->source(), wheel->pointingDevice());
            begin.setTimestamp(wheel->timestamp() > 0 ? wheel->timestamp() - 1 : 0);
            m_forwarding = true;
            QCoreApplication::sendEvent(m_target, &begin);
            m_forwarding = false;
        }
        owner = m_target;
    }
    if (owner != m_target) {
        QCoreApplication::sendEvent(owner, wheel);
    } else {
        // Flickable passes on a turn that brings it to an end, and one it
        // cannot yet measure; the well keeps them.
        latch.take(m_target, wheel);
        m_forwarding = true;
        QCoreApplication::sendEvent(m_target, wheel);
        m_forwarding = false;
    }
    wheel->accept();
    return true;
}

void WheelScroll::setTarget(QQuickItem *target)
{
    if (target == m_target)
        return;
    if (m_target)
        m_target->removeEventFilter(this);
    m_target = target;
    if (target)
        target->installEventFilter(this);
    emit targetChanged();
}

bool WheelScroll::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_target || event->type() != QEvent::Wheel)
        return false;
    auto *wheel = static_cast<QWheelEvent *>(event);
    // A scroll a well owns stays in it, the pointer here or not.
    if (QQuickItem *owner = latch.owning(wheel); owner && owner != m_target) {
        QCoreApplication::sendEvent(owner, wheel);
        wheel->accept();
        return true;
    }
    const QPoint pixels = wheel->pixelDelta(), angle = wheel->angleDelta();
    const bool precise = !pixels.isNull();
    // The scroll is the transcript's once it moves it.
    if (wheel->phase() == Qt::ScrollEnd || stepOf(wheel) != 0)
        latch.take(m_target, wheel);
    // Chromium's pixels for the turn, positive toward the end.
    const qreal step = -(precise ? pixels.y() * 12.0 : angle.y());
    switch (wheel->phase()) {
    case Qt::ScrollBegin:
        break;
    case Qt::ScrollEnd:
        emit released();
        break;
    case Qt::ScrollUpdate:
    case Qt::ScrollMomentum:
        if (step == 0)
            return false;
        emit wheeled(-(precise ? pixels.y() : angle.y() / 120.0 * 53));
        emit tracked(step, qreal(wheel->timestamp()));
        break;
    case Qt::NoScrollPhase:
        if (step == 0)
            return false;
        emit wheeled(-(precise ? pixels.y() : angle.y() / 120.0 * 53));
        emit notched(step);
        break;
    }
    wheel->accept();
    return true;
}

void InputWatch::setWindow(QQuickWindow *window)
{
    if (window == m_window)
        return;
    if (m_window)
        m_window->removeEventFilter(this);
    m_window = window;
    if (window)
        window->installEventFilter(this);
    emit windowChanged();
}

bool InputWatch::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window &&
        (event->type() == QEvent::KeyPress || event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::Wheel))
        emit pressed();
    return false;
}

void ButtonKeys::setWindow(QQuickWindow *window)
{
    if (window == m_window)
        return;
    if (m_window)
        m_window->removeEventFilter(this);
    m_window = window;
    m_held = false;
    if (window)
        window->installEventFilter(this);
    emit windowChanged();
}

bool ButtonKeys::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_window ||
        (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease))
        return false;
    const auto *key = static_cast<QKeyEvent *>(event);
    if ((key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter) ||
        (key->modifiers() & ~Qt::KeypadModifier))
        return false;
    if (event->type() == QEvent::KeyPress) {
        QQuickItem *focus = m_window->activeFocusItem();
        if (!m_held && (!focus || !focus->inherits("QQuickAbstractButton") || !focus->isEnabled()))
            return false;
        m_held = true;
    } else if (!m_held) {
        return false;
    } else if (!key->isAutoRepeat()) {
        m_held = false;
    }
    QKeyEvent space(event->type(), Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "),
                    key->isAutoRepeat(), key->count());
    QCoreApplication::sendEvent(m_window, &space);
    return true;
}
