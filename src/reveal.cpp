#include "reveal.h"

#include "motion.h"
#include "rich.h"

#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGTextureMaterial>

#include <cmath>
#include <memory>

namespace
{
constexpr int RevealMs = 760, FadeMs = 420;
constexpr qreal Feather = 0.16; // REVEAL.feather: the soft share of the circle.
constexpr int Bands = 16;       // Steps of the soft edge.
constexpr int Segments = 160;   // Per ring.

// The picture's texture, kept with the nodes that draw it.
class PictureNode final : public QSGNode
{
  public:
    std::unique_ptr<QSGTexture> texture;
};

QSGGeometryNode *ring(QSGTexture *texture)
{
    auto *node = new QSGGeometryNode;
    auto *geometry =
        new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 2 * (Segments + 1));
    geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    auto *material = new QSGTextureMaterial;
    material->setTexture(texture);
    material->setFiltering(QSGTexture::Linear);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

// The picture between radii `inner` and `outer` about `centre` (a whole
// rectangle when both are negative), in a size × size item.
void shape(QSGGeometryNode *node, QPointF centre, qreal inner, qreal outer, QSizeF size)
{
    QSGGeometry *geometry = node->geometry();
    auto *v = geometry->vertexDataAsTexturedPoint2D();
    const auto set = [&](int i, qreal x, qreal y) {
        v[i].set(float(x), float(y), float(x / size.width()), float(y / size.height()));
    };
    if (inner < 0) {
        // A rectangle as a degenerate strip: its four corners, then the last repeated.
        const qreal w = size.width(), h = size.height();
        const QPointF corners[4] = {{0, 0}, {0, h}, {w, 0}, {w, h}};
        for (int i = 0; i < 2 * (Segments + 1); ++i)
            set(i, corners[std::min(i, 3)].x(), corners[std::min(i, 3)].y());
    } else {
        for (int k = 0; k <= Segments; ++k) {
            const qreal a = 2 * M_PI * k / Segments, c = std::cos(a), s = std::sin(a);
            set(2 * k, centre.x() + inner * c, centre.y() + inner * s);
            set(2 * k + 1, centre.x() + outer * c, centre.y() + outer * s);
        }
    }
    node->markDirty(QSGNode::DirtyGeometry);
}
} // namespace

ThemeReveal::ThemeReveal(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    m_clock.setStartValue(0.0);
    m_clock.setEndValue(1.0);
    connect(&m_clock, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        const qreal t = value.toReal();
        m_t = m_fade ? cubicBezier(0.25, 0.1, 0.25, 1, t) : cubicBezier(0.32, 0.72, 0, 1, t);
        update();
    });
    connect(&m_clock, &QVariantAnimation::finished, this, &ThemeReveal::end);
    connect(ThemeSignal::instance(), &ThemeSignal::changing, this, &ThemeReveal::begin);
}

void ThemeReveal::begin(QPointF origin)
{
    QQuickWindow *shown = window();
    if (m_reducedMotion || !shown || !isVisible() || width() <= 0 || height() <= 0 ||
        shown->rendererInterface()->graphicsApi() == QSGRendererInterface::Software)
        return;
    // A second change cuts the first short: the picture is of what shows now.
    QImage picture = shown->grabWindow();
    if (picture.isNull())
        return;
    const bool was = running();
    m_picture = std::move(picture);
    m_fresh = true;
    m_fade = origin.isNull();
    m_origin = mapFromScene(origin);
    m_t = 0;
    m_clock.stop();
    m_clock.setDuration(m_fade ? FadeMs : RevealMs);
    m_clock.start();
    update();
    if (!was)
        emit runningChanged();
}

void ThemeReveal::end()
{
    m_picture = QImage();
    m_fresh = false;
    update();
    emit runningChanged();
}

QSGNode *ThemeReveal::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *root = static_cast<PictureNode *>(old);
    if (m_picture.isNull() || !window()) {
        delete root;
        return nullptr;
    }
    if (!root || m_fresh) {
        delete root;
        root = new PictureNode;
        root->texture.reset(window()->createTextureFromImage(m_picture));
        // The rectangle for a fade, or the rings: every band of the soft edge,
        // then all that lies beyond the circle.
        for (int k = 0; k <= Bands; ++k) {
            auto *opacity = new QSGOpacityNode;
            opacity->appendChildNode(ring(root->texture.get()));
            root->appendChildNode(opacity);
        }
        m_fresh = false;
    }
    const QSizeF size(width(), height());
    const qreal x = m_origin.x(), y = m_origin.y();
    // theme.js: the mask grows to twice the reach, where its solid part
    // covers the farthest corner.
    const qreal reach =
        std::hypot(std::max(x, size.width() - x), std::max(y, size.height() - y)) / (1 - Feather);
    const qreal radius = reach * m_t, far = std::hypot(size.width(), size.height()) * 2;
    int k = 0;
    for (QSGNode *child = root->firstChild(); child; child = child->nextSibling(), ++k) {
        auto *opacity = static_cast<QSGOpacityNode *>(child);
        auto *node = static_cast<QSGGeometryNode *>(opacity->firstChild());
        if (m_fade) {
            // Only the first node draws: the whole picture, fading out.
            shape(node, {}, -1, -1, size);
            opacity->setOpacity(k == 0 ? 1 - m_t : 0);
        } else if (k < Bands) {
            const qreal from = radius * (1 - Feather + Feather * k / Bands),
                        to = radius * (1 - Feather + Feather * (k + 1) / Bands);
            shape(node, m_origin, from, to, size);
            opacity->setOpacity((k + 0.5) / Bands);
        } else {
            shape(node, m_origin, radius, std::max(far, radius + 1), size);
            opacity->setOpacity(1);
        }
    }
    return root;
}
