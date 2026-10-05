#include "browserfocus.h"

#include <QMouseEvent>
#include <QQuickWindow>
#include <QTouchEvent>

BrowserFocus::BrowserFocus(QQuickItem *parent) : QQuickItem(parent) {}

void BrowserFocus::setPanel(QQuickItem *panel)
{
    if (panel == m_panel)
        return;
    m_panel = panel;
    emit panelChanged();
}

void BrowserFocus::itemChange(ItemChange change, const ItemChangeData &value)
{
    if (change == ItemSceneChange) {
        if (m_window)
            m_window->removeEventFilter(this);
        m_window = value.window;
        if (m_window)
            m_window->installEventFilter(this);
    }
    QQuickItem::itemChange(change, value);
}

bool BrowserFocus::inGuest(const QQuickItem *item, const QQuickItem *panel)
{
    if (!panel)
        return false;
    bool guest = false;
    for (; item; item = item->parentItem()) {
        guest |= item->objectName() == QLatin1String("browserGuest");
        if (item == panel)
            return guest;
    }
    return false;
}

bool BrowserFocus::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window) {
        if (event->type() == QEvent::MouseButtonPress)
            press(static_cast<QMouseEvent *>(event)->scenePosition());
        else if (event->type() == QEvent::TouchBegin) {
            const auto *touch = static_cast<QTouchEvent *>(event);
            if (!touch->points().isEmpty())
                press(touch->points().first().scenePosition());
        }
    }
    return QQuickItem::eventFilter(watched, event);
}

void BrowserFocus::press(QPointF scene)
{
    if (!m_window || !m_panel || !inGuest(m_window->activeFocusItem(), m_panel))
        return;
    if (m_panel->isVisible() && m_panel->contains(m_panel->mapFromScene(scene)))
        return;
    // The application window's content, as the page's blur() leaves it: the
    // root item would hand focus straight back to the page it last held.
    auto *content = m_window->property("contentItem").value<QQuickItem *>();
    (content ? content : m_window->contentItem())->forceActiveFocus(Qt::MouseFocusReason);
    emit yielded();
}
