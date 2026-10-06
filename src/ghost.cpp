#include "ghost.h"

#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSGGeometryNode>
#include <QSGRendererInterface>
#include <QSGVertexColorMaterial>
#include <QVarLengthArray>

#include <algorithm>
#include <cmath>

namespace
{
// ghost-thinking.js constants; times in seconds.
constexpr double EyeY = 29.5;
constexpr double EyeX[2] = {17, 41};
constexpr double EyeRx = 3.8, EyeRy = 4.1;
const GhostItem::Pose PoseTable[12] = {
    {0, 0, 1, 1},           {-4, 0, 1, 1},         {4, 0, 1, 1},          {-3.2, -2.4, 1, 1},
    {3.2, -2.4, 1, 1},      {-2.6, 1.8, 1, 1},     {2.6, 1.8, 1, 1},      {0, 1.2, 1.14, 0.34},
    {-3.5, 0.8, 1.1, 0.42}, {3.5, 0.8, 1.1, 0.42}, {0, -0.8, 1.24, 1.34}, {0, -1.6, 1.18, 1.26},
};
constexpr double MoveSpring[2] = {260, 24};
constexpr double ShapeSpring[2] = {320, 26};
constexpr double Hold[2] = {0.65, 1.5};
constexpr double BlinkEvery[2] = {2.2, 4.8};
constexpr double BlinkDuration = 0.15;
constexpr double FloatPeriod = 2.6, FloatAmp = 1.6;
constexpr double SwayPeriod = 1.3, SwayAmp = 1.3;
constexpr double Pi = 3.14159265358979323846;

double (*testRandom)(const GhostItem *) = nullptr;

double uniform(const GhostItem *ghost)
{
    return testRandom ? testRandom(ghost) : QRandomGenerator::global()->generateDouble();
}

double random(const GhostItem *ghost, const double (&range)[2])
{
    return range[0] + uniform(ghost) * (range[1] - range[0]);
}

// The body outline (bodyPath(s)): below a half circle, the bottom edge is
// seven half circles, tail bumps down and notches up, so the shape is
// x-monotone: each column runs from top(x) to bottom(x).
constexpr int ArcSteps[7] = {14, 12, 8, 12, 8, 12, 14};
constexpr int Columns = 1 + 14 + 12 + 8 + 12 + 8 + 12 + 14;
constexpr int EyeSteps = 32;
constexpr int LoopPoints = 2 * Columns;
constexpr int VertexCount = 2 * LoopPoints + 2 * (1 + 2 * EyeSteps);
constexpr int IndexCount = (Columns - 1) * 6 + LoopPoints * 6 + 2 * (EyeSteps * 3 + EyeSteps * 6);

void outline(double s, QPointF (&top)[Columns], QPointF (&bottom)[Columns])
{
    const double right = (5 - s) / 2, left = (5 + s) / 2;
    // (start x, radius, direction: +1 a bump below, -1 a notch above)
    const double arcs[7][3] = {{0, left, 1},   {5 + s, 6, -1},  {17 + s, 3, 1},    {23 + s, 6, -1},
                               {35 + s, 3, 1}, {41 + s, 6, -1}, {53 + s, right, 1}};
    int n = 0;
    bottom[n++] = QPointF(0, 57);
    for (int a = 0; a < 7; ++a) {
        const double r = arcs[a][1], c = arcs[a][0] + r, dir = arcs[a][2];
        for (int k = 1; k <= ArcSteps[a]; ++k) {
            const double theta = Pi * (1 - double(k) / ArcSteps[a]);
            bottom[n++] = QPointF(c + r * std::cos(theta), 57 + dir * r * std::sin(theta));
        }
    }
    for (int i = 0; i < Columns; ++i) {
        const double x = std::clamp(bottom[i].x(), 0.0, 58.0), d = x - 29;
        top[i] = QPointF(x, 29 - std::sqrt(std::max(0.0, 29 * 29 - d * d)));
    }
}

QPointF unit(QPointF v)
{
    const double length = std::hypot(v.x(), v.y());
    return length > 1e-9 ? v / length : QPointF(0, -1);
}

void setVertex(QSGGeometry::ColoredPoint2D &v, QPointF p, const QColor &color, double coverage)
{
    const double a = color.alphaF() * coverage;
    v.set(float(p.x()), float(p.y()), uchar(std::lround(color.red() * a)),
          uchar(std::lround(color.green() * a)), uchar(std::lround(color.blue() * a)),
          uchar(std::lround(255 * a)));
}
} // namespace

GhostItem::GhostItem(QQuickItem *parent)
    : QQuickItem(parent), m_clock([this](qreal elapsed) { tick(elapsed); })
{
    setFlag(ItemHasContents, true);
    setFlag(ItemObservesViewport, true);
    setImplicitSize(30, 33);
    reset(FrameClock::now());
}

GhostItem::~GhostItem()
{
    m_clock.stop();
    watchPath(false);
}

void GhostItem::setRunning(bool running)
{
    if (running == m_running)
        return;
    m_running = running;
    emit runningChanged();
    sync();
}

void GhostItem::setReducedMotion(bool reduced)
{
    if (reduced == m_reducedMotion)
        return;
    m_reducedMotion = reduced;
    emit reducedMotionChanged();
    sync();
    update();
}

void GhostItem::setColor(const QColor &color)
{
    if (color == m_color)
        return;
    m_color = color;
    emit colorChanged();
    update();
}

void GhostItem::setEyeColor(const QColor &color)
{
    if (color == m_eyeColor)
        return;
    m_eyeColor = color;
    emit eyeColorChanged();
    update();
}

void GhostItem::reset(double now)
{
    m_start = now;
    m_now = now;
    m_pose = &PoseTable[0];
    m_nextPose = now + random(this, Hold) * 0.6;
    m_nextBlink = now + random(this, BlinkEvery) * 0.5;
    m_blinkAt = -1e9;
    m_eye[0] = m_eye[1] = {0, 0};
    m_eye[2] = m_eye[3] = {1, 0};
    m_float = m_sway = 0;
    m_blink = 1;
}

void GhostItem::restart()
{
    ++m_restarts;
    reset(FrameClock::now());
    m_gazing = false;
    update();
}

void GhostItem::continueFrom(GhostItem *other)
{
    if (!other || other == this)
        return;
    // Times are on the one monotonic clock, so they carry over as they are.
    m_start = other->m_start;
    m_now = other->m_now;
    m_nextPose = other->m_nextPose;
    m_nextBlink = other->m_nextBlink;
    m_blinkAt = other->m_blinkAt;
    m_gazeUntil = other->m_gazeUntil;
    m_gazing = other->m_gazing;
    m_gaze = other->m_gaze;
    m_pose = other->m_pose;
    std::copy(std::begin(other->m_eye), std::end(other->m_eye), std::begin(m_eye));
    m_float = other->m_float;
    m_sway = other->m_sway;
    m_blink = other->m_blink;
    update();
}

void GhostItem::look(qreal x, qreal y, int holdMs)
{
    m_gaze = {x, y, 1, 1};
    m_gazing = true;
    m_gazeUntil = FrameClock::now() + holdMs / 1000.0;
}

void GhostItem::setTestRandom(double (*source)(const GhostItem *)) { testRandom = source; }

void GhostItem::blink() { m_blinkAt = FrameClock::now(); }

const GhostItem::Pose &GhostItem::pickPose()
{
    const Pose *table = PoseTable;
    if (m_pose != &table[0] && uniform(this) < 0.3)
        return table[0];
    const Pose *pose;
    do
        pose = &table[1 + int(uniform(this) * 11)];
    while (pose == m_pose);
    return *pose;
}

QVariantMap GhostItem::state() const
{
    const Pose *table = PoseTable;
    return {{"x", m_eye[0].value},      {"y", m_eye[1].value},      {"sx", m_eye[2].value},
            {"sy", m_eye[3].value},     {"vx", m_eye[0].velocity},  {"vy", m_eye[1].velocity},
            {"vsx", m_eye[2].velocity}, {"vsy", m_eye[3].velocity}, {"float", m_float},
            {"sway", m_sway},           {"blink", m_blink},         {"pose", int(m_pose - table)},
            {"gazing", m_gazing},       {"ticking", ticking()},     {"frames", m_frames}};
}

bool GhostItem::onScreen() const
{
    const QRectF visible = clipRect();
    return visible.width() > 0 && visible.height() > 0;
}

void GhostItem::componentComplete()
{
    QQuickItem::componentComplete();
    sync();
}

void GhostItem::sync()
{
    // Not before its bindings are set: a clock started and stopped in one
    // pass would only churn the animation driver.
    if (!isComponentComplete())
        return;
    QQuickWindow *shown = window();
    const bool windowShown = shown && shown->isVisible() &&
                             shown->visibility() != QWindow::Minimized &&
                             shown->visibility() != QWindow::Hidden;
    const bool alive = m_running && !m_reducedMotion && isVisible() && windowShown;
    const bool should = alive && onScreen();
    // Parked off screen, it wakes when it or anything up to its view moves.
    watchPath(alive && !should);
    if (should == ticking())
        return;
    if (should)
        m_clock.start();
    else
        m_clock.stop();
    emit tickingChanged();
}

void GhostItem::watchPath(bool watch)
{
    QList<QPointer<QQuickItem>> path;
    if (watch) {
        // Its clip rect is its own rect within its viewport's: only the
        // geometry of the items in between, and the viewport's size, move it.
        const QQuickItem *viewport = viewportItem();
        for (QQuickItem *item = this; item; item = item->parentItem()) {
            path << item;
            if (item == viewport)
                break;
        }
    }
    if (path == m_path)
        return;
    for (const auto &connection : std::as_const(m_pathWatch))
        disconnect(connection);
    m_pathWatch.clear();
    m_path = path;
    for (QQuickItem *item : std::as_const(path)) {
        for (auto changed :
             {&QQuickItem::xChanged, &QQuickItem::yChanged, &QQuickItem::widthChanged,
              &QQuickItem::heightChanged, &QQuickItem::scaleChanged, &QQuickItem::rotationChanged})
            m_pathWatch << connect(item, changed, this, &GhostItem::sync);
        m_pathWatch << connect(item, &QQuickItem::parentChanged, this, &GhostItem::sync);
    }
}

void GhostItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    if (change == ItemSceneChange) {
        disconnect(m_visibility);
        m_window = value.window;
        if (m_window)
            m_visibility =
                connect(m_window.data(), &QWindow::visibilityChanged, this, &GhostItem::sync);
    }
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange || change == ItemVisibleHasChanged ||
        change == ItemParentHasChanged)
        sync();
}

void GhostItem::tick(qreal elapsed)
{
    if (!onScreen()) {
        sync();
        return;
    }
    const double now = FrameClock::now();
    m_now = now;
    const double dt = std::clamp(double(elapsed), 0.0, 0.032);
    if (m_gazing && now >= m_gazeUntil) {
        m_gazing = false;
        m_nextPose = now + random(this, Hold) * 0.5;
    }
    if (!m_gazing && now >= m_nextPose) {
        m_pose = &pickPose();
        m_nextPose = now + random(this, Hold);
    }
    const Pose &pose = m_gazing ? m_gaze : *m_pose;
    if (now >= m_nextBlink) {
        m_blinkAt = now;
        m_nextBlink = now + random(this, BlinkEvery);
    }
    const int steps = std::max(1, int(std::ceil(dt / 0.008)));
    const double step = dt / steps;
    const double goals[4] = {pose.x, pose.y, pose.sx, pose.sy};
    for (int k = 0; k < 4; ++k) {
        const double *spring = k < 2 ? MoveSpring : ShapeSpring;
        Spring &s = m_eye[k];
        for (int i = 0; i < steps; ++i) {
            s.velocity += ((goals[k] - s.value) * spring[0] - s.velocity * spring[1]) * step;
            s.value += s.velocity * step;
        }
    }
    const double blinkT = (now - m_blinkAt) / BlinkDuration;
    m_blink = blinkT < 1 ? 1 - 0.92 * std::sin(Pi * blinkT) : 1;
    const double t = now - m_start;
    m_float = std::sin(t * 2 * Pi / FloatPeriod) * FloatAmp;
    m_sway = std::sin(t * 2 * Pi / SwayPeriod) * SwayAmp;
    ++m_frames;
    update();
}

QSGNode *GhostItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGGeometryNode *>(old);
    QQuickWindow *shown = window();
    if (!shown || width() <= 0 || height() <= 0 ||
        shown->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new QSGGeometryNode;
        auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                         VertexCount, IndexCount, QSGGeometry::UnsignedShortType);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        geometry->setVertexDataPattern(QSGGeometry::StreamPattern);
        // Fixed topology: body fill, body edge, then each eye's fill and edge.
        quint16 *index = geometry->indexDataAsUShort();
        auto inner = [](int j) { return quint16(2 * j); };
        auto outer = [](int j) { return quint16(2 * j + 1); };
        auto topAt = [](int i) { return i; };
        auto bottomAt = [](int i) { return LoopPoints - 1 - i; };
        for (int i = 0; i + 1 < Columns; ++i) {
            const quint16 t0 = inner(topAt(i)), t1 = inner(topAt(i + 1));
            const quint16 b0 = inner(bottomAt(i)), b1 = inner(bottomAt(i + 1));
            *index++ = t0, *index++ = b0, *index++ = t1;
            *index++ = t1, *index++ = b0, *index++ = b1;
        }
        for (int j = 0; j < LoopPoints; ++j) {
            const int k = (j + 1) % LoopPoints;
            *index++ = inner(j), *index++ = outer(j), *index++ = inner(k);
            *index++ = inner(k), *index++ = outer(j), *index++ = outer(k);
        }
        for (int e = 0; e < 2; ++e) {
            const int base = 2 * LoopPoints + e * (1 + 2 * EyeSteps);
            for (int k = 0; k < EyeSteps; ++k) {
                const int n = (k + 1) % EyeSteps;
                *index++ = quint16(base), *index++ = quint16(base + 1 + k),
                *index++ = quint16(base + 1 + n);
            }
            for (int k = 0; k < EyeSteps; ++k) {
                const int n = (k + 1) % EyeSteps;
                const quint16 a = quint16(base + 1 + k), b = quint16(base + 1 + n);
                const quint16 c = quint16(base + 1 + EyeSteps + k),
                              d = quint16(base + 1 + EyeSteps + n);
                *index++ = a, *index++ = c, *index++ = b;
                *index++ = b, *index++ = c, *index++ = d;
            }
        }
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }

    // The view box (-3 -5 64 70), fitted and centred in the item.
    const double scale = std::min(width() / ViewWidth, height() / ViewHeight);
    const double ox = (width() - ViewWidth * scale) / 2 + 3 * scale;
    const double oy = (height() - ViewHeight * scale) / 2 + 5 * scale;
    const bool still = m_reducedMotion;
    const double lift = still ? 0 : m_float;
    auto place = [&](QPointF p) {
        return QPointF(ox + p.x() * scale, oy + (p.y() + lift) * scale);
    };
    // One device pixel of edge: half inside the shape, half outside.
    const double feather = 1 / std::max(1.0, shown->effectiveDevicePixelRatio());

    QPointF top[Columns], bottom[Columns];
    outline(still ? 0 : m_sway, top, bottom);
    QPointF loop[LoopPoints];
    for (int i = 0; i < Columns; ++i) {
        loop[i] = place(top[i]);
        loop[LoopPoints - 1 - i] = place(bottom[i]);
    }
    auto *v = node->geometry()->vertexDataAsColoredPoint2D();
    for (int j = 0; j < LoopPoints; ++j) {
        const QPointF p = loop[(j + LoopPoints - 1) % LoopPoints], c = loop[j],
                      q = loop[(j + 1) % LoopPoints];
        const QPointF d1 = unit(c - p), d2 = unit(q - c);
        const QPointF normal = unit(QPointF(d1.y(), -d1.x()) + QPointF(d2.y(), -d2.x()));
        setVertex(v[2 * j], c - normal * (feather / 2), m_color, 1);
        setVertex(v[2 * j + 1], c + normal * (feather / 2), m_color, 0);
    }
    v += 2 * LoopPoints;
    for (int e = 0; e < 2; ++e) {
        const double sx = still ? 1 : m_eye[2].value;
        const double sy = still ? 1 : std::max(0.05, m_eye[3].value * m_blink);
        const QPointF centre = place(
            QPointF(EyeX[e] + (still ? 0 : m_eye[0].value), EyeY + (still ? 0 : m_eye[1].value)));
        const double rx = EyeRx * sx * scale, ry = EyeRy * sy * scale;
        setVertex(*v++, centre, m_eyeColor, 1);
        for (int ring = 0; ring < 2; ++ring) {
            const double grow = ring ? feather / 2 : -feather / 2;
            for (int k = 0; k < EyeSteps; ++k) {
                const double theta = 2 * Pi * k / EyeSteps;
                const QPointF normal = unit(QPointF(std::cos(theta) / rx, std::sin(theta) / ry));
                const QPointF edge = centre + QPointF(rx * std::cos(theta), ry * std::sin(theta));
                QPointF at = edge + normal * grow;
                if (!ring && QPointF::dotProduct(at - centre, edge - centre) <= 0)
                    at = centre; // Thinner than a pixel: the edge carries it.
                setVertex(*v++, at, m_eyeColor, ring ? 0 : 1);
            }
        }
    }
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}
