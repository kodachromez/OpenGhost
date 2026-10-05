#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

// The splash's software-only radial light. Unlike Qt 6.11's software Shape
// node, QQuickPaintedItem snapshots the item during sync (GUI blocked); the
// render phase only uses the node's pixels, even if the Loader has retired us.
class SplashAura : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
  public:
    explicit SplashAura(QQuickItem *parent = nullptr);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    void paint(QPainter *painter) override;

  signals:
    void colorChanged();

  private:
    QColor m_color = Qt::transparent;
};
