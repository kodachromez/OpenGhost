#pragma once

#include <QImage>
#include <QPointF>
#include <QQuickItem>
#include <QVariantAnimation>
#include <QtQml/qqmlregistration.h>

// OpenGhost 1.2's theme transition (theme.js), over the whole window: a theme
// picked by hand spreads from the point it was picked like ink, the last 16 %
// of its circle soft (0.76 s, the motion easing); one the system switched
// fades in (0.42 s). Just before the palette changes (ThemeSignal::changing),
// the window's picture of the old theme is taken, then drawn over the new one
// and cut away as the circle grows: the soft edge as bands of rising opacity.
// The picture takes no input, so every click reaches the new theme under it.
// Nothing with reduced motion or the software renderer.
class ThemeReveal : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool reducedMotion MEMBER m_reducedMotion NOTIFY reducedMotionChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

  public:
    explicit ThemeReveal(QQuickItem *parent = nullptr);
    bool running() const { return !m_picture.isNull(); }

  signals:
    void reducedMotionChanged();
    void runningChanged();

  protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;

  private:
    void begin(QPointF origin);
    void end();

    bool m_reducedMotion = false;
    QImage m_picture;
    bool m_fresh = false; // A picture the scene graph has no texture of yet.
    QPointF m_origin;
    bool m_fade = false;
    qreal m_t = 0; // Eased progress.
    QVariantAnimation m_clock;
};
