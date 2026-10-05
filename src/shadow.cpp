#include "shadow.h"

#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGRendererInterface>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
constexpr int Rings = 25;      // From 3σ inside the edge to 3σ outside it.
constexpr int CornerSteps = 8; // Segments per quarter circle.
constexpr int SideSteps = 4;   // Points inside each straight side.
constexpr int RingPoints = 4 * (CornerSteps + 1 + SideSteps);
constexpr int BandSlices = 8;  // Rows through each rounded band.

// The share of a unit Gaussian below z.
qreal below(qreal z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

// The opacity at (x, y) of a w × h rounded rectangle (corner radius r,
// top at y0) blurred by a Gaussian of σ: exact across each row, and summed
// over rows by their Gaussian share, the straight middle band in one piece.
qreal blurred(qreal x, qreal y, qreal w, qreal h, qreal y0, qreal r, qreal sigma)
{
    const auto across = [&](qreal inset) {
        return below((w - inset - x) / sigma) - below((inset - x) / sigma);
    };
    const auto share = [&](qreal from, qreal to) { return below((to - y) / sigma) - below((from - y) / sigma); };
    qreal sum = across(0) * share(y0 + r, y0 + h - r);
    for (int k = 0; k < BandSlices; ++k) {
        // A row's distance from the band's straight edge, at the slice's middle.
        const qreal d = r * (1 - (k + 0.5) / BandSlices);
        const qreal inset = r - std::sqrt(std::max(0.0, r * r - d * d));
        const qreal a = y0 + r * k / BandSlices, b = y0 + r * (k + 1) / BandSlices;
        sum += across(inset) * (share(a, b) + share(2 * y0 + h - b, 2 * y0 + h - a));
    }
    return std::clamp(sum, 0.0, 1.0);
}
} // namespace

BoxShadow::BoxShadow(QQuickItem *parent) : QQuickItem(parent) { setFlag(ItemHasContents); }

void BoxShadow::setRadius(qreal radius)
{
    if (qFuzzyCompare(radius, m_radius))
        return;
    m_radius = radius;
    emit changed();
    update();
}

void BoxShadow::setBlur(qreal blur)
{
    if (qFuzzyCompare(blur, m_blur))
        return;
    m_blur = blur;
    emit changed();
    update();
}

void BoxShadow::setOffsetY(qreal offset)
{
    if (qFuzzyCompare(offset, m_offsetY))
        return;
    m_offsetY = offset;
    emit changed();
    update();
}

void BoxShadow::setColor(const QColor &color)
{
    if (color == m_color)
        return;
    m_color = color;
    emit changed();
    update();
}

void BoxShadow::setKnockout(bool knockout)
{
    if (knockout == m_knockout)
        return;
    m_knockout = knockout;
    emit changed();
    update();
}

void BoxShadow::geometryChange(const QRectF &next, const QRectF &previous)
{
    QQuickItem::geometryChange(next, previous);
    if (next.size() != previous.size())
        update();
}

QSGNode *BoxShadow::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGGeometryNode *>(old);
    QQuickWindow *shown = window();
    if (!shown || width() <= 0 || height() <= 0 || m_blur <= 0 || m_color.alpha() == 0 ||
        shown->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new QSGGeometryNode;
        // Rings of RingPoints, then the centre; strips between rings and a
        // fan from the centre to the innermost ring.
        auto *geometry = new QSGGeometry(
            QSGGeometry::defaultAttributes_ColoredPoint2D(), Rings * RingPoints + 1,
            (Rings - 1) * RingPoints * 6 + RingPoints * 3, QSGGeometry::UnsignedShortType);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        quint16 *index = geometry->indexDataAsUShort();
        for (int r = 0; r + 1 < Rings; ++r) {
            for (int j = 0; j < RingPoints; ++j) {
                const int k = (j + 1) % RingPoints;
                const auto a = quint16(r * RingPoints + j), b = quint16(r * RingPoints + k);
                const auto c = quint16(a + RingPoints), d = quint16(b + RingPoints);
                *index++ = a, *index++ = c, *index++ = b;
                *index++ = b, *index++ = c, *index++ = d;
            }
        }
        const auto centre = quint16(Rings * RingPoints);
        for (int j = 0; j < RingPoints; ++j)
            *index++ = centre, *index++ = quint16(j), *index++ = quint16((j + 1) % RingPoints);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    const qreal sigma = m_blur / 2, w = width(), h = height();
    const qreal radius = std::clamp(m_radius, 0.0, std::min(w, h) / 2);
    // Ring 0 is the innermost; none shrinks past a point.
    const qreal inner = std::max(-3 * sigma, -std::min(w, h) / 2 + 0.5);
    auto *v = node->geometry()->vertexDataAsColoredPoint2D();
    const auto colour = [this](qreal opacity) {
        const qreal a = m_color.alphaF() * opacity;
        return std::array<uchar, 4>{uchar(std::lround(m_color.redF() * a * 255)),
                                    uchar(std::lround(m_color.greenF() * a * 255)),
                                    uchar(std::lround(m_color.blueF() * a * 255)),
                                    uchar(std::lround(a * 255))};
    };
    // How far (x, y) lies outside the item's rounded box, as a pixel's
    // coverage: 0 inside it, 1 a pixel or more out.
    const auto outside = [&](qreal x, qreal y) {
        const qreal qx = std::abs(x - w / 2) - (w / 2 - radius);
        const qreal qy = std::abs(y - h / 2) - (h / 2 - radius);
        const qreal d = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) +
                        std::min(std::max(qx, qy), 0.0) - radius;
        return std::clamp(d + 0.5, 0.0, 1.0);
    };
    const auto at = [&](qreal x, qreal y) {
        const qreal cut = m_knockout ? outside(x, y) : 1;
        const auto c = colour(cut * blurred(x, y - m_offsetY, w, h, 0, radius, sigma));
        v->set(float(x), float(y), c[0], c[1], c[2], c[3]);
        ++v;
    };
    for (int r = 0; r < Rings; ++r) {
        const qreal e = inner + (3 * sigma - inner) * r / (Rings - 1);
        const qreal rr = std::max(0.0, radius + e);
        const qreal left = -e + rr, right = w + e - rr;
        const qreal top = m_offsetY - e + rr, bottom = m_offsetY + h + e - rr;
        const QPointF centres[4] = {{right, top}, {right, bottom}, {left, bottom}, {left, top}};
        for (int q = 0; q < 4; ++q) {
            for (int s = 0; s <= CornerSteps; ++s) {
                const qreal angle = M_PI / 2 * (q - 1 + qreal(s) / CornerSteps);
                at(centres[q].x() + rr * std::cos(angle), centres[q].y() + rr * std::sin(angle));
            }
            // The straight side to the next corner.
            const QPointF from = centres[q] + rr * QPointF(std::cos(M_PI / 2 * q), std::sin(M_PI / 2 * q));
            const QPointF to = centres[(q + 1) % 4] + rr * QPointF(std::cos(M_PI / 2 * q), std::sin(M_PI / 2 * q));
            for (int s = 1; s <= SideSteps; ++s)
                at(from.x() + (to.x() - from.x()) * s / (SideSteps + 1), from.y() + (to.y() - from.y()) * s / (SideSteps + 1));
        }
    }
    at(w / 2, m_offsetY + h / 2);
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}
