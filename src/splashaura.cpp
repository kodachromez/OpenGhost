#include "splashaura.h"

#include <QPainter>
#include <QRadialGradient>

SplashAura::SplashAura(QQuickItem *parent) : QQuickPaintedItem(parent) {}

void SplashAura::setColor(const QColor &color)
{
    if (color == m_color)
        return;
    m_color = color;
    update();
    emit colorChanged();
}

void SplashAura::paint(QPainter *painter)
{
    // Same 1200 x 1200 radial fill as the original Shape. Opacity and scale
    // animate in QML; they reuse these pixels rather than repainting per frame.
    QRadialGradient gradient(QPointF(width() / 2, height() / 2), width() / 2);
    gradient.setColorAt(0, m_color);
    gradient.setColorAt(1, Qt::transparent);
    painter->fillRect(boundingRect(), gradient);
}
