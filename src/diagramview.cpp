#include "diagramview.h"

#include "diagram_engine.h"
#include "rich.h"
#include "theme.h"

#include <QCache>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QPainter>
#include <QPointer>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QThreadPool>

#include <algorithm>
#include <cmath>

namespace
{
// The chat's type (16 px) to the type the drawings were made for (14).
constexpr double Zoom = 16.0 / 14;

double nowMs() { return FrameClock::now() * 1000; }

// Settled drawings by a digest of everything that drew them, so a block
// made again (a row scrolled back, a reopened conversation) has its drawing
// and its height at once. Bounded by what each entry holds. GUI thread only.
QCache<QByteArray, DiagramImage::Drawing> &drawings()
{
    static QCache<QByteArray, DiagramImage::Drawing> cache(DiagramImage::CacheBytes);
    return cache;
}

qint64 held(const QByteArray &key, const DiagramImage::Drawing &drawing)
{
    return drawing.image.sizeInBytes() + drawing.error.size() * 2 + key.size() +
           drawing.texts.size() * qint64(sizeof(QRectF)) +
           (drawing.result ? drawing.result->items.size() * 512 : 0) +
           qint64(sizeof(DiagramImage::Drawing));
}

qint64 costOf(const QByteArray &key, const DiagramImage::Drawing &drawing)
{
    return std::max(held(key, drawing), DiagramImage::CacheBytes / DiagramImage::CacheEntries);
}

QColor toneColor(int tone)
{
    const auto &p = theme::current();
    return tone == diagram::Mute ? p.series[0] : p.series[tone >= 1 && tone <= 7 ? tone : 1];
}
} // namespace

DiagramImage::Cached DiagramImage::cached()
{
    Cached out;
    auto &cache = drawings();
    out.count = int(cache.count());
    out.cost = cache.totalCost();
    for (const QByteArray &key : cache.keys())
        out.bytes += held(key, *cache.object(key));
    return out;
}

DiagramImage::DiagramImage(QQuickItem *parent)
    : QQuickItem(parent), m_clock([this](qreal) { tick(); })
{
    setFlag(ItemHasContents);
    setAcceptHoverEvents(true);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &DiagramImage::render);
    connect(&m_clock, &QAbstractAnimation::stateChanged, this, &DiagramImage::animatingChanged);
    // Drawn in the palette's ink: a theme change draws again.
    connect(ThemeSignal::instance(), &ThemeSignal::changed, this, [this] {
        m_dirty = true;
        changed(0);
        if (m_useFrame)
            frame();
        update();
    });
    setImplicitSize(0, diagram::STATUS_HEIGHT);
}

DiagramImage::~DiagramImage()
{
    m_clock.stop();
    forgetRichWork(this);
}

void DiagramImage::setSource(const QString &source)
{
    if (source == m_source)
        return;
    m_source = source;
    emit sourceChanged();
    // A streamed source draws at once the first time, then after a pause.
    changed(m_live && (m_result || m_rendering || !m_error.isEmpty()) ? LiveDelayMs : 0);
}

void DiagramImage::setLang(const QString &lang)
{
    if (lang == m_lang)
        return;
    m_lang = lang;
    emit langChanged();
    changed(0);
}

void DiagramImage::setLive(bool live)
{
    if (live == m_live)
        return;
    m_live = live;
    emit liveChanged();
    if (!live)
        changed(0);
    if (live)
        pointer({}, false);
}

void DiagramImage::setAvailableWidth(qreal width)
{
    if (std::abs(width - m_room) < 2)
        return;
    const bool first = m_room <= 0;
    m_room = width;
    emit availableWidthChanged();
    updateStage();
    changed(first ? 0 : ResizeDelayMs);
}

void DiagramImage::setColumn(qreal column)
{
    if (std::abs(column - m_column) < 2)
        return;
    m_column = column;
    emit columnChanged();
    changed(m_result ? ResizeDelayMs : 0);
}

void DiagramImage::setTone(int tone)
{
    if (tone == m_tone)
        return;
    m_tone = tone;
    emit toneChanged();
    changed(0);
}

void DiagramImage::setAnimate(bool animate)
{
    if (animate == m_animate)
        return;
    m_animate = animate;
    emit animateChanged();
}

void DiagramImage::setReducedMotion(bool reduced)
{
    if (reduced == m_reducedMotion)
        return;
    m_reducedMotion = reduced;
    emit reducedMotionChanged();
    // Motion stops where it would have ended.
    if (reduced && animating()) {
        m_scene.finish();
        m_glide.o = m_glideBox ? 1 : 0;
        m_clock.stop();
        updateStage();
        settle();
    }
}

QString DiagramImage::kind() const { return m_result ? m_result->kind : QString(); }

qreal DiagramImage::stageWidth() const
{
    const diagram::Props *v = m_scene.view();
    return v ? v->w : std::max<qreal>(0, m_room);
}

qreal DiagramImage::stageHeight() const
{
    const diagram::Props *v = m_scene.view();
    return v ? v->h : diagram::STATUS_HEIGHT;
}

QPointF DiagramImage::tools() const
{
    const diagram::Props *v = m_scene.view();
    return v ? QPointF(v->tx, v->ty) : QPointF();
}

QRectF DiagramImage::drawing() const
{
    const diagram::Props *v = m_scene.view();
    if (!v || !m_result)
        return {};
    return QRectF(v->ox, v->top, v->cw, m_result->height * v->s);
}

QVariantList DiagramImage::selectionRects() const
{
    QVariantList out;
    for (const QRectF &r : m_settled.texts)
        out << r;
    return out;
}

qreal DiagramImage::ratio() const
{
    return window() ? window()->effectiveDevicePixelRatio() : qGuiApp->devicePixelRatio();
}

bool DiagramImage::shown() const
{
    const QQuickItem *parent = parentItem();
    return parent ? parent->isVisible() : isVisible();
}

diagram::Options DiagramImage::options() const
{
    diagram::Options o;
    o.width = m_room;
    o.column = m_column > 0 ? std::min(m_column, m_room) : m_room;
    o.zoom = Zoom;
    o.dpr = ratio();
    o.section = diagram::sectionOf(m_tone);
    o.family = QGuiApplication::font().family();
    o.mono = Theme::monoFamily();
    o.hints = m_hints;
    o.sideways = m_sideways;
    return o;
}

QString DiagramImage::inputsKey() const
{
    const diagram::Options o = options();
    QString key = m_lang + QLatin1Char('\x1f') + QString::number(qRound(o.width)) +
                  QLatin1Char('\x1f') + QString::number(qRound(o.column)) + QLatin1Char('\x1f') +
                  QString::number(o.dpr, 'f', 3) + QLatin1Char('\x1f') +
                  QLatin1Char(theme::current().light ? 'l' : 'd') + QString::number(o.section) +
                  QLatin1Char('\x1f') + o.family + QLatin1Char('\x1f') + o.sideways +
                  QLatin1Char('\x1f');
    // A scheme's kept places decide its layout too.
    QStringList hints;
    for (auto it = o.hints.constBegin(); it != o.hints.constEnd(); ++it)
        hints << it.key() + QLatin1Char('=') + QString::number(it.value(), 'f', 1);
    hints.sort();
    return key + hints.join(QLatin1Char(','));
}

QByteArray DiagramImage::drawingKey() const
{
    const QString inputs = inputsKey();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(inputs.size()) + ':');
    for (const QString *part : {&inputs, &m_source})
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(part->constData()),
                                    part->size() * qsizetype(sizeof(QChar))));
    return hash.result();
}

bool DiagramImage::fromCache()
{
    if (m_source.trimmed().isEmpty() || m_room <= 0)
        return false;
    const Drawing *found = drawings().object(drawingKey());
    if (!found)
        return false;
    m_timer.stop();
    if (found->result)
        show(*found, m_source);
    else
        fail(found->error);
    return true;
}

void DiagramImage::changed(int delay)
{
    if (m_source.isEmpty() && !m_result)
        return;
    ++m_request;
    // A drawing that cannot be shown even as progress stops early.
    if (m_flying &&
        !(m_live && m_flightInputs == inputsKey() && m_source.startsWith(m_flightSource)))
        cancelRichWork(this);
    if (!m_rendering && fromCache())
        return;
    const bool wasPending = pending();
    m_timer.start(delay);
    if (!wasPending)
        emit pendingChanged();
}

void DiagramImage::itemChange(ItemChange change, const ItemChangeData &value)
{
    if (change == ItemSceneChange && value.window)
        changed(0);
    if (change == ItemParentHasChanged) {
        disconnect(m_parentShown);
        if (value.item)
            m_parentShown = connect(value.item, &QQuickItem::visibleChanged, this, [this] {
                if (m_hidden && shown()) {
                    m_hidden = false;
                    changed(0);
                }
            });
    }
    QQuickItem::itemChange(change, value);
}

void DiagramImage::render()
{
    if (m_room <= 0 || !window()) {
        emit pendingChanged();
        return;
    }
    if (!shown()) {
        m_hidden = true;
        emit pendingChanged();
        return;
    }
    if (m_source.trimmed().isEmpty()) {
        // Nothing to draw yet while streaming; settled, that is the result.
        if (!m_live)
            fail(QStringLiteral("There is nothing to draw."));
        emit pendingChanged();
        return;
    }
    if (m_rendering) {
        m_again = true;
        return;
    }
    if (fromCache())
        return;
    m_rendering = true;
    wantRichWork(this, [this](const RichCancel &cancel) {
        RichJob job;
        m_rendering = false;
        m_again = false;
        if (m_room <= 0 || !window() || !shown() || m_source.trimmed().isEmpty()) {
            render();
            return job;
        }
        if (fromCache())
            return job;
        m_rendering = m_flying = true;
        m_flightInputs = inputsKey();
        m_flightSource = m_source;
        diagram::Options o = options();
        job = [this, cancel, o, source = m_source, lang = m_lang, inputs = m_flightInputs,
               key = drawingKey(), request = m_request,
               before = m_beforeDraw]() mutable -> std::function<void()> {
            if (before)
                before();
            o.cancel = cancel.get();
            const diagram::Rendered drawn = diagram::render(source, lang, o);
            Drawing out;
            out.result = drawn.result;
            out.image = drawn.image;
            out.left = drawn.left;
            out.view = drawn.view;
            out.texts = drawn.texts;
            out.error = drawn.ok ? QString() : drawn.error;
            const bool cancelled = cancel->load();
            return [this, out = std::move(out), source, inputs, key, request, cancelled] {
                m_rendering = m_flying = false;
                if (!cancelled) {
                    ++m_renders;
                    if (out.result || source.size() <= CachedFailure) {
                        auto *kept = new Drawing(out);
                        drawings().insert(key, kept, costOf(key, *kept));
                    }
                }
                // A result asked for before a replacement, settlement or
                // resize is dropped; streaming text that still starts with
                // its source is progress.
                const bool current = request == m_request && !cancelled;
                const bool progress = !cancelled && out.result && m_live && inputs == inputsKey() &&
                                      m_source.startsWith(source);
                if (current || progress) {
                    if (out.result)
                        show(out, source);
                    else
                        fail(out.error);
                }
                if (m_again || !current) {
                    m_again = false;
                    if (!m_timer.isActive())
                        render();
                }
                emit pendingChanged();
            };
        };
        return job;
    });
    emit pendingChanged();
}

void DiagramImage::fail(const QString &error)
{
    // The last good drawing stands; the block says why the text does not draw.
    m_error = error.isEmpty() ? QStringLiteral("This diagram could not be read.") : error;
    emit resultChanged();
}

void DiagramImage::show(const Drawing &drawing, const QString &source)
{
    const bool first = m_first;
    m_settled = drawing;
    m_result = drawing.result;
    m_error.clear();
    m_shownSource = source;
    m_hints = m_result->hints;
    m_sideways = m_result->sideways;
    m_hitsValid = false;
    m_focused = -1;
    m_lit.clear();
    m_hovered.clear();
    // The view goes first: the stage springs from the status's height.
    diagram::Spec view = diagram::item(diagram::Type::View, QStringLiteral("__view"), 0);
    const diagram::View &v = drawing.view;
    view.props.ox = v.ox;
    view.props.s = v.s;
    view.props.cw = v.cw;
    view.props.top = v.top;
    view.props.h = v.h;
    view.props.w = v.w;
    view.props.tx = v.tx;
    view.props.ty = v.ty;
    if (first) {
        diagram::Props initial = view.props;
        initial.h = diagram::STATUS_HEIGHT;
        view.initial = initial;
    }
    QVector<diagram::Spec> specs;
    specs.reserve(m_result->items.size() + 1);
    specs << view;
    specs += m_result->items;
    const bool instant = m_reducedMotion || (first && !m_animate);
    const double now = nowMs();
    m_scene.set(specs, now, first && !m_live ? diagram::STAGGER.reveal : diagram::STAGGER.live,
                instant);
    m_first = false;
    showTip(nullptr, {}, {});
    if (instant)
        m_scene.tick(now);
    if (m_scene.busy()) {
        m_last = now;
        if (m_clock.state() != QAbstractAnimation::Running)
            m_clock.start();
        frame();
    } else {
        settle();
    }
    updateStage();
    emit resultChanged();
}

void DiagramImage::updateStage()
{
    setImplicitSize(stageWidth(), stageHeight());
    emit stageChanged();
}

void DiagramImage::tick()
{
    const double now = nowMs();
    const bool busy = m_scene.tick(now);
    // The rows' shared highlight glides on the sidebar's springs.
    const double dt = diagram::clamp((now - m_last) / 1000, 0, 0.032);
    m_last = now;
    bool gliding = false;
    if (m_glideOn) {
        if (m_reducedMotion) {
            if (m_glideBox) {
                m_glide.x = m_glideBox->x(), m_glide.y = m_glideBox->y();
                m_glide.w = m_glideBox->width(), m_glide.h = m_glideBox->height();
            }
            m_glide.o = m_glideBox ? 1 : 0;
        } else {
            using diagram::springTo;
            if (m_glideBox) {
                const double goal[] = {m_glideBox->x(), m_glideBox->y(), m_glideBox->width(),
                                       m_glideBox->height()};
                double *cur[] = {&m_glide.x, &m_glide.y, &m_glide.w, &m_glide.h};
                double *vel[] = {&m_glideVel.x, &m_glideVel.y, &m_glideVel.w, &m_glideVel.h};
                for (int k = 0; k < 4; ++k)
                    gliding = springTo(*cur[k], *vel[k], goal[k], dt, diagram::GLIDE_MOVE_K,
                                       diagram::GLIDE_MOVE_C) ||
                              gliding;
            }
            gliding = springTo(m_glide.o, m_glideVel.o, m_glideBox ? 1 : 0, dt,
                               diagram::GLIDE_FADE_K, diagram::GLIDE_FADE_C) ||
                      gliding;
        }
    }
    updateStage();
    if (!busy && !gliding) {
        m_clock.stop();
        settle();
    } else {
        frame();
    }
}

bool DiagramImage::plain() const
{
    const bool pieRests =
        m_focused < 0 || (m_result && m_result->pie && m_focused == m_result->pie->top);
    return !m_scene.busy() && m_hotKeys.isEmpty() && m_lit.isEmpty() && m_hovered.isEmpty() &&
           m_glide.o < 0.001 && pieRests && !m_settled.image.isNull();
}

void DiagramImage::settle()
{
    if (plain()) {
        // The worker's image is this very drawing.
        ++m_generation;
        m_useFrame = false;
        m_frameWanted = false;
        m_frame = QImage();
        m_dirty = true;
        update();
        return;
    }
    frame();
}

namespace
{
// Frames of drawings in motion are painted here, one at a time, never on the
// GUI thread: a frame of a large drawing can take several milliseconds.
QThreadPool &painters()
{
    static QThreadPool *pool = [] {
        auto *made = new QThreadPool(QCoreApplication::instance());
        made->setMaxThreadCount(1);
        made->setObjectName(QStringLiteral("diagram frames"));
        return made;
    }();
    return *pool;
}

struct FrameJob {
    QVector<diagram::Live> items;
    diagram::Props view;
    diagram::Look look;
    QString kind, family, mono;
    double margin = 0, dpr = 1;
};

struct Painted {
    QImage image;
    double left = 0;
};

Painted paintFrame(const FrameJob &job)
{
    const diagram::Props &v = job.view;
    const double x0 = std::max(0.0, v.ox - job.margin),
                 x1 = std::min(v.w, v.ox + v.cw + job.margin);
    const double w = std::ceil(std::max(1.0, x1 - x0)), h = std::ceil(std::max(1.0, v.h));
    double dpr = job.dpr;
    if (w * h * dpr * dpr > diagram::MaxPixels)
        dpr = std::sqrt(diagram::MaxPixels / (w * h));
    QImage image(int(std::ceil(w * dpr)), int(std::ceil(h * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    diagram::CompileOptions fonts;
    fonts.family = job.family;
    fonts.mono = job.mono;
    thread_local std::unique_ptr<diagram::Ctx> ctx;
    if (!ctx || ctx->options.family != fonts.family || ctx->options.mono != fonts.mono)
        ctx = std::make_unique<diagram::Ctx>(fonts);
    QVector<const diagram::Live *> items;
    items.reserve(job.items.size());
    for (const auto &item : job.items)
        items << &item;
    QPainter painter(&image);
    painter.translate(-x0, 0);
    painter.translate(v.ox, v.top);
    painter.scale(v.s, v.s);
    diagram::paintItems(painter, *ctx, items, job.look, job.kind);
    painter.end();
    return {image, x0};
}
} // namespace

void DiagramImage::frame()
{
    // The latest state is what the next frame shows; while one is being
    // painted, the next waits for it.
    m_useFrame = true;
    m_frameWanted = true;
    if (!m_framing)
        paintNext();
}

void DiagramImage::paintNext()
{
    const diagram::Props *v = m_scene.view();
    if (!m_frameWanted || !v || !m_result)
        return;
    m_frameWanted = false;
    m_framing = true;
    FrameJob job;
    for (const diagram::Live *item : m_scene.shown())
        job.items << *item;
    job.view = *v;
    job.kind = m_result->kind;
    job.family = QGuiApplication::font().family();
    job.mono = Theme::monoFamily();
    job.margin = diagram::margin(*m_result);
    job.dpr = ratio();
    job.look.probing = !m_hotKeys.isEmpty();
    job.look.lit = m_lit;
    job.look.hovered = m_hovered;
    for (const QString &key : m_hotKeys) {
        const diagram::Live *hot = m_scene.find(key);
        if (hot && hot->spec.type == diagram::Type::FSeg)
            job.look.segHot = true;
    }
    if (m_glide.o > 0.001) {
        job.look.glide =
            QRectF(m_glide.x, m_glide.y, std::max(0.0, m_glide.w), std::max(0.0, m_glide.h));
        job.look.glideOpacity = m_glide.o;
    }
    QPointer<DiagramImage> self(this);
    const quint64 generation = m_generation;
    painters().start([self, job = std::move(job), generation] {
        Painted painted = paintFrame(job);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, painted = std::move(painted), generation] {
                if (!self)
                    return;
                self->m_framing = false;
                // A frame asked for before the drawing settled is not shown.
                if (generation == self->m_generation && self->m_useFrame &&
                    !painted.image.isNull()) {
                    self->m_frame = painted.image;
                    self->m_frameLeft = painted.left;
                    self->m_dirty = true;
                    self->update();
                }
                self->paintNext();
            },
            Qt::QueuedConnection);
    });
}

QSGNode *DiagramImage::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGImageNode *>(old);
    const QImage &image = m_useFrame ? m_frame : m_settled.image;
    const double left = m_useFrame ? m_frameLeft : m_settled.left;
    if (!m_result || image.isNull()) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);
        m_dirty = true;
    }
    if (m_dirty) {
        node->setTexture(window()->createTextureFromImage(image));
        m_dirty = false;
    }
    const qreal dpr = image.devicePixelRatio();
    node->setRect(QRectF(left, 0, image.width() / dpr, image.height() / dpr));
    node->setFiltering(QSGTexture::Linear);
    return node;
}

/* The pointer */

void DiagramImage::hoverMoveEvent(QHoverEvent *event) { pointer(event->position(), true); }

void DiagramImage::hoverLeaveEvent(QHoverEvent *) { pointer({}, false); }

void DiagramImage::pointAt(qreal x, qreal y) { pointer(QPointF(x, y), true); }

void DiagramImage::pointAway() { pointer({}, false); }

const QVector<diagram::Hit> &DiagramImage::hits()
{
    if (m_hitsValid)
        return m_hits;
    m_hits.clear();
    const diagram::Props *v = m_scene.view();
    if (v && m_result) {
        QImage scratch(1, 1, QImage::Format_ARGB32_Premultiplied);
        diagram::CompileOptions fonts;
        fonts.family = QGuiApplication::font().family();
        fonts.mono = Theme::monoFamily();
        diagram::Ctx ctx(fonts);
        QPainter painter(&scratch);
        // In stage coordinates, as the drawing will stand.
        const diagram::View &t = m_settled.view;
        painter.translate(t.ox, t.top);
        painter.scale(t.s, t.s);
        // Items as they settle: hits for where they go, not where they are.
        QVector<diagram::Live> settled;
        for (const diagram::Live *live : m_scene.shown()) {
            diagram::Live item = *live;
            item.cur = item.spec.props;
            item.appear = 1;
            settled << item;
        }
        QVector<const diagram::Live *> items;
        for (const auto &item : settled)
            items << &item;
        diagram::paintItems(painter, ctx, items, {}, m_result->kind, nullptr, &m_hits);
    }
    m_hitsValid = true;
    return m_hits;
}

QString DiagramImage::hitAt(QPointF at, bool *node)
{
    const auto &list = hits();
    // The last painted is on top.
    for (int i = int(list.size()) - 1; i >= 0; --i) {
        if (list[i].shape.contains(at)) {
            if (node)
                *node = list[i].node;
            return list[i].key;
        }
    }
    if (node)
        *node = false;
    return {};
}

void DiagramImage::pointer(QPointF at, bool inside)
{
    const diagram::Props *v = m_scene.view();
    bool over = false;
    double cx = diagram::NaN, cy = diagram::NaN;
    if (inside && v && m_result && !m_live) {
        using diagram::TOOLS;
        const double left = std::min(v->ox, v->tx) - TOOLS.reach,
                     right = std::max(v->ox + v->cw, v->tx + TOOLS.width) + TOOLS.reach;
        over = at.x() >= left && at.x() <= right && at.y() >= 0 && at.y() <= v->h + 4;
        if (over) {
            cx = (at.x() - v->ox) / v->s;
            cy = (at.y() - v->top) / v->s;
        }
    }
    if (over != m_hovering) {
        m_hovering = over;
        emit hoveringChanged();
    }
    bool node = false;
    const QString target = over ? hitAt(at, &node) : QString();
    const diagram::Live *item = target.isEmpty() ? nullptr : m_scene.find(target);
    // A block lights its arrows, and takes the pointer's own hover.
    QString lit, hovered;
    if (node && item) {
        hovered = target;
        if (m_result->edges)
            lit = item->spec.fixed.id;
    }
    bool repaint = false;
    if (lit != m_lit || hovered != m_hovered) {
        m_lit = lit;
        m_hovered = hovered;
        repaint = true;
    }
    probe(cx, cy, target);
    if (m_result && m_result->pie) {
        const int index =
            item && item->spec.fixed.index >= 0 ? item->spec.fixed.index : m_result->pie->top;
        if (index != m_focused) {
            focusPie(index);
            repaint = true;
        }
    }
    if (repaint && m_clock.state() != QAbstractAnimation::Running)
        settle();
}

void DiagramImage::probe(double cx, double cy, const QString &target)
{
    const diagram::Tip *tip = nullptr;
    QStringList keys;
    QVariantMap at;
    const diagram::Props *v = m_scene.view();
    if (!m_result || !v) {
        showTip(nullptr, {}, {});
        return;
    }
    const auto &p = m_result->probe;
    if (!std::isnan(cx) && p && !p->xs.isEmpty() && cx >= p->x0 - 8 && cx <= p->x1 + 8 &&
        cy >= p->y0 - 16 && cy <= p->y1 + 8) {
        int best = 0;
        double near = INFINITY;
        for (int k = 0; k < p->xs.size(); ++k) {
            const double d = std::abs(p->xs[k] - cx);
            if (d < near) {
                near = d;
                best = k;
            }
        }
        if (best < p->tips.size())
            tip = &p->tips[best];
        if (best < p->keys.size())
            keys = p->keys[best];
        at = {{"mode", "probe"},
              {"x", v->ox + p->xs[best] * v->s},
              {"top", v->top + p->y0 * v->s},
              {"bottom", v->top + p->y1 * v->s}};
    } else if (!std::isnan(cx) && !m_result->tips.isEmpty()) {
        QString key = target;
        // Points are small: among scattered ones the nearest within reach.
        if (!m_result->near.isEmpty() && !m_result->tips.contains(key)) {
            double best = diagram::NEAR * diagram::NEAR;
            for (const auto &point : m_result->near) {
                const double d = (point.x - cx) * (point.x - cx) + (point.y - cy) * (point.y - cy);
                if (d < best) {
                    best = d;
                    key = point.key;
                }
            }
        }
        const auto found = m_result->tips.constFind(key);
        if (!key.isEmpty() && found != m_result->tips.constEnd()) {
            tip = &*found;
            keys = found->hot.isEmpty() ? QStringList{key} : found->hot;
            // The mark's box: where its shapes answer the pointer.
            QRectF box;
            for (const auto &hit : hits()) {
                if (hit.key == keys.first())
                    box |= hit.shape.boundingRect();
            }
            at = {{"mode", "mark"},
                  {"box", box},
                  {"px", v->ox + cx * v->s},
                  {"py", v->top + cy * v->s},
                  {"large", (box.width() > 90 || box.height() > 60)}};
        }
    }
    if (tip && tip->silent) {
        tip = nullptr;
        at.clear();
    }
    showTip(tip, at, keys);
}

void DiagramImage::showTip(const diagram::Tip *tip, const QVariantMap &at, const QStringList &keys)
{
    const QString sig = keys.join(QLatin1Char('|'));
    if (sig != m_hotSig) {
        m_hotKeys = keys;
        m_hotSig = sig;
        m_scene.setHot(keys);
        // A row that says where its highlight goes gets the shared one.
        std::optional<QRectF> box;
        const diagram::Live *item = keys.size() == 1 ? m_scene.find(keys.first()) : nullptr;
        if (item && item->spec.type == diagram::Type::Hit) {
            const auto &t = item->spec.props;
            box = QRectF(t.x, t.y + 1, t.w, t.h - 2);
        } else if (item && item->spec.type == diagram::Type::FRow) {
            const auto &t = item->spec.props;
            const auto &fx = item->spec.fixed;
            box = QRectF(t.x + fx.plateX, t.y + 1, fx.plateW, fx.h - 2);
        }
        if (box || m_glideOn) {
            m_glideOn = true;
            if (box && m_glide.o < 0.02) {
                m_glide.x = box->x(), m_glide.y = box->y();
                m_glide.w = box->width(), m_glide.h = box->height();
                m_glideVel = {};
            }
            m_glideBox = box;
            m_last = nowMs();
            if (m_clock.state() != QAbstractAnimation::Running)
                m_clock.start();
        }
        if (m_clock.state() != QAbstractAnimation::Running)
            settle();
    }
    QVariantMap shownTip;
    if (tip && !at.isEmpty()) {
        QVariantList rows;
        for (const auto &row : tip->rows)
            rows << QVariantMap{{"name", row.name},
                                {"value", row.value},
                                {"cls", row.cls},
                                {"hasColor", row.tone != diagram::NoTone},
                                {"color", toneColor(row.tone)},
                                {"mark", row.mark.isEmpty() ? QStringLiteral("dot") : row.mark}};
        shownTip = at;
        shownTip.insert("shown", true);
        shownTip.insert("title", tip->title);
        shownTip.insert("rows", rows);
        shownTip.insert("sig", sig);
    } else {
        // Hidden where it stood, so it fades in place.
        shownTip = m_tip;
        shownTip.insert("shown", false);
    }
    if (shownTip != m_tip) {
        m_tip = shownTip;
        emit tipChanged();
    }
}

void DiagramImage::focusPie(int index)
{
    if (!m_result || !m_result->pie || index == m_focused)
        return;
    const auto &pie = *m_result->pie;
    m_focused = index;
    for (int i = 0; i < pie.count; ++i) {
        m_scene.patch(QStringLiteral("s:%1").arg(i),
                      [&](diagram::Fixed &fx) { fx.active = i == index; });
        m_scene.patch(QStringLiteral("r:%1").arg(i),
                      [&](diagram::Fixed &fx) { fx.active = i == index; });
    }
    const QString value = index >= 0 && index < pie.values.size() ? pie.values[index] : QString();
    const QString label = index >= 0 && index < pie.labels.size() ? pie.labels[index] : QString();
    m_scene.patch(QStringLiteral("c:v"), [&](diagram::Fixed &fx) { fx.lines = {value}; });
    m_scene.patch(QStringLiteral("c:l"), [&](diagram::Fixed &fx) { fx.lines = {label}; });
}
