#include "diagram_scene.h"

#include <algorithm>
#include <cmath>

namespace diagram
{
bool springTo(double &x, double &v, double goal, double dt, double k, double c)
{
    if (x == goal && !v)
        return false;
    const int steps = std::max(1, int(std::ceil(dt / 0.008)));
    const double h = dt / steps;
    for (int i = 0; i < steps; ++i) {
        v += ((goal - x) * k - v * c) * h;
        x += v * h;
    }
    if (std::abs(goal - x) < 0.02 && std::abs(v) < 0.05) {
        x = goal;
        v = 0;
        return false;
    }
    return true;
}

namespace
{
bool stepList(Pts &cur, Pts &vel, const Pts &to, double dt)
{
    bool moving = false;
    if (cur.size() != to.size() || vel.size() != to.size())
        return false;
    for (int i = 0; i < to.size(); ++i)
        moving = springTo(cur[i], vel[i], to[i], dt) || moving;
    return moving;
}

bool stepProps(Props &cur, Props &vel, const Props &to, double dt)
{
    bool moving = false;
    for (auto member : PropScalars)
        moving = springTo(cur.*member, vel.*member, to.*member, dt) || moving;
    moving = stepList(cur.pts, vel.pts, to.pts, dt) || moving;
    moving = stepList(cur.vs, vel.vs, to.vs, dt) || moving;
    moving = stepList(cur.band, vel.band, to.band, dt) || moving;
    return moving;
}

Props zero(const Props &props)
{
    Props out;
    out.pts = Pts(props.pts.size(), 0);
    out.vs = Pts(props.vs.size(), 0);
    out.band = Pts(props.band.size(), 0);
    return out;
}
} // namespace

double Scene::enter(const Spec &spec, const Props &cur)
{
    const Fixed &fx = spec.fixed;
    switch (spec.type) {
    case Type::View:
    case Type::Hit:
        return 0;
    case Type::Node:
    case Type::Candle:
        return ENTER.node;
    case Type::Edge:
        return fx.draw ? fx.draw
                       : clamp(roughLength(cur.pts) / ENTER.speed, ENTER.drawMin, ENTER.drawMax);
    case Type::Line:
        return fx.draw ? ENTER.line : ENTER.fade;
    case Type::Bar:
    case Type::Column:
    case Type::FSeg:
        return ENTER.grow;
    case Type::Area:
    case Type::Ribbon:
        return ENTER.line;
    case Type::Arc:
        return std::max(120.0, spec.props.sweep / 100 * ENTER.arc);
    case Type::Span:
        return fx.milestone ? ENTER.label : ENTER.grow;
    case Type::Poly:
        return ENTER.arc;
    case Type::Note:
    case Type::Frame:
    case Type::Cluster:
    case Type::Rect:
    case Type::WBox:
    case Type::FCard:
        return ENTER.fade;
    case Type::Label:
    case Type::Legend:
    case Type::Dot:
    case Type::Badge:
    case Type::Chip:
    case Type::Figure:
    case Type::WGlyph:
    case Type::FHead:
    case Type::FRow:
        return ENTER.label;
    }
    return ENTER.fade;
}

void Scene::retarget(Item &item, const Props &props)
{
    Props to = props;
    Props &cur = item.live.cur;
    if (!to.pts.isEmpty() && !cur.pts.isEmpty() && to.pts.size() != cur.pts.size()) {
        // A line of more or fewer segments: both get as many to spring between.
        const int count = std::max(segmentCount(to.pts), segmentCount(cur.pts));
        cur.pts = resample(cur.pts, count);
        item.vel.pts = resample(item.vel.pts, count);
        to.pts = resample(to.pts, count);
    }
    const auto adopt = [](Pts &c, Pts &v, const Pts &t) {
        if (c.size() != t.size() || v.size() != t.size()) {
            c = t;
            v = Pts(t.size(), 0);
        }
    };
    adopt(cur.pts, item.vel.pts, to.pts);
    adopt(cur.vs, item.vel.vs, to.vs);
    adopt(cur.band, item.vel.band, to.band);
    item.to = to;
}

void Scene::set(const QVector<Spec> &specs, double now, double stagger, bool instant)
{
    QSet<QString> seen;
    QVector<int> fresh;
    for (const Spec &spec : specs) {
        seen.insert(spec.key);
        int at = m_index.value(spec.key, -1);
        if (at >= 0 && m_items[at].live.spec.type != spec.type) {
            destroy(at);
            at = -1;
        }
        if (at < 0) {
            Item item;
            const Props &start = spec.initial ? *spec.initial : spec.props;
            item.live.spec = spec;
            item.live.cur = start;
            item.live.appear = 0;
            item.vel = zero(start);
            item.to = spec.props;
            item.begin = now;
            m_items.push_back(item);
            at = int(m_items.size()) - 1;
            m_index.insert(spec.key, at);
            fresh << at;
        } else {
            Item &item = m_items[at];
            const bool hot = item.live.hot;
            item.live.spec = spec;
            item.live.hot = hot;
            retarget(item, spec.props);
            if (item.goal != 1) {
                item.from = item.live.appear;
                item.goal = 1;
                item.begin = now;
                item.dur = EXIT;
            }
        }
        if (instant) {
            Item &item = m_items[at];
            item.live.cur = item.to;
            item.vel = zero(item.to);
        }
    }
    double first = 0;
    for (int k = 0; k < fresh.size(); ++k)
        first = k ? std::min(first, m_items[fresh[k]].live.spec.order)
                  : m_items[fresh[k]].live.spec.order;
    for (const int at : fresh) {
        Item &item = m_items[at];
        item.begin = now + std::min(STAGGER.cap, (item.live.spec.order - first) * stagger);
        item.dur = instant ? 0 : enter(item.live.spec, item.live.cur);
        if (instant || !item.dur)
            item.live.appear = 1;
    }
    for (Item &item : m_items) {
        if (seen.contains(item.live.spec.key) || item.goal == 0)
            continue;
        item.from = item.live.appear;
        item.goal = 0;
        item.begin = now;
        item.dur = instant ? 0 : EXIT;
    }
    if (m_last < 0)
        m_last = now;
    m_busy = true;
}

bool Scene::tick(double now)
{
    const double dt = clamp((now - m_last) / 1000, 0, 0.05);
    m_last = now;
    bool busy = false;
    QVector<int> gone;
    for (int i = 0; i < m_items.size(); ++i) {
        Item &item = m_items[i];
        const bool moving = stepProps(item.live.cur, item.vel, item.to, dt);
        bool pending = false;
        if (item.live.appear != item.goal) {
            const double t = item.dur ? (now - item.begin) / item.dur : 1;
            item.live.appear = t >= 1   ? item.goal
                               : t <= 0 ? item.from
                                        : item.from + (item.goal - item.from) * t;
            pending = item.live.appear != item.goal;
        }
        if (item.goal == 0 && item.live.appear == 0) {
            gone << i;
            continue;
        }
        if (moving || pending)
            busy = true;
    }
    for (int k = int(gone.size()) - 1; k >= 0; --k)
        m_items.remove(gone[k]);
    if (!gone.isEmpty())
        reindex();
    m_busy = busy;
    return busy;
}

void Scene::finish()
{
    for (int i = int(m_items.size()) - 1; i >= 0; --i) {
        Item &item = m_items[i];
        if (item.goal == 0) {
            m_items.remove(i);
            continue;
        }
        item.live.cur = item.to;
        item.vel = zero(item.to);
        item.live.appear = item.goal;
    }
    reindex();
    m_busy = false;
}

void Scene::patch(const QString &key, const std::function<void(Fixed &)> &change)
{
    const int at = m_index.value(key, -1);
    if (at >= 0)
        change(m_items[at].live.spec.fixed);
}

void Scene::setHot(const QStringList &keys)
{
    for (Item &item : m_items)
        item.live.hot = keys.contains(item.live.spec.key);
}

void Scene::clear()
{
    m_items.clear();
    m_index.clear();
    m_last = -1;
    m_busy = false;
}

QVector<const Live *> Scene::shown() const
{
    QVector<const Live *> out;
    out.reserve(m_items.size());
    for (const Item &item : m_items) {
        if (item.live.spec.type != Type::View)
            out << &item.live;
    }
    return out;
}

const Props *Scene::view() const
{
    const int at = m_index.value(QStringLiteral("__view"), -1);
    return at >= 0 ? &m_items[at].live.cur : nullptr;
}

const Live *Scene::find(const QString &key) const
{
    const int at = m_index.value(key, -1);
    return at >= 0 ? &m_items[at].live : nullptr;
}

void Scene::destroy(int index)
{
    m_items.remove(index);
    reindex();
}

void Scene::reindex()
{
    m_index.clear();
    for (int i = 0; i < m_items.size(); ++i)
        m_index.insert(m_items[i].live.spec.key, i);
}
} // namespace diagram
