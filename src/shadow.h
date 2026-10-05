#pragma once

#include <QColor>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

// A CSS outer `box-shadow: 0 offsetY blur color` for a rounded rectangle the
// item's size: the rectangle blurred by a Gaussian (σ = blur / 2), sampled
// on rings of the rounded outline and blended between them. Each point has
// the blurred shape's own opacity there, so a box thin against its blur casts
// the lighter shadow Chromium draws, not a straight edge's. It paints outside
// the item's bounds, under the element that covers the item. Drawn as one
// vertex-coloured scene-graph node, as the Ghost is; nothing with the
// software renderer. With `knockout` it is cut away inside the item's own
// rounded box, as CSS clips an outer shadow to outside its element: for a
// translucent element, which would show it through.
class BoxShadow : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal radius MEMBER m_radius WRITE setRadius NOTIFY changed)
    Q_PROPERTY(qreal blur MEMBER m_blur WRITE setBlur NOTIFY changed)
    Q_PROPERTY(qreal offsetY MEMBER m_offsetY WRITE setOffsetY NOTIFY changed)
    Q_PROPERTY(QColor color MEMBER m_color WRITE setColor NOTIFY changed)
    Q_PROPERTY(bool knockout MEMBER m_knockout WRITE setKnockout NOTIFY changed)

  public:
    explicit BoxShadow(QQuickItem *parent = nullptr);
    void setRadius(qreal radius);
    void setBlur(qreal blur);
    void setOffsetY(qreal offset);
    void setColor(const QColor &color);
    void setKnockout(bool knockout);

  signals:
    void changed();

  protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &next, const QRectF &previous) override;

  private:
    qreal m_radius = 0;
    qreal m_blur = 0;
    qreal m_offsetY = 0;
    QColor m_color{0, 0, 0, 0};
    bool m_knockout = false;
};
