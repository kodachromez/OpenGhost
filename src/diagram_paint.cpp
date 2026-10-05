#include "diagram_paint.h"

#include "filekinds.h"
#include "theme.h"

#include <QFontInfo>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPathStroker>

#include <algorithm>
#include <cmath>

// How each item draws: diagram.js's TYPES (create, refresh, apply) with the
// dg-* rules of styles.css applied in the stylesheet's order, so a later
// rule wins over an earlier one of the same weight as the cascade has it.
namespace diagram
{
namespace
{
const theme::Palette &pal() { return theme::current(); }

QColor alpha(QColor c, double a)
{
    c.setAlphaF(float(clamp01(c.alphaF() * a)));
    return c;
}
QColor fgA(double a) { return alpha(pal().fgBase, a); }
// --ink, --ink-2, --ink-3.
QColor ink(int k) { return fgA(pal().dgInk[std::clamp(k, 0, 2)]); }
// color-mix(in srgb, a p, b).
QColor mix(const QColor &a, double p, const QColor &b)
{
    return QColor::fromRgbF(float(a.redF() * p + b.redF() * (1 - p)),
                            float(a.greenF() * p + b.greenF() * (1 - p)),
                            float(a.blueF() * p + b.blueF() * (1 - p)));
}
QColor chatBg() { return pal().chatBg; }
// The colour of a slot (.t-s1 … .t-mute); none takes .dg-svg's --s1.
QColor toneColor(int tone)
{
    if (tone == Mute)
        return pal().series[0];
    return pal().series[tone >= 1 && tone <= 7 ? tone : 1];
}
QColor none() { return QColor(0, 0, 0, 0); }

// color-mix(in oklab, c p, black): OKLab's lightness and chroma × p.
QColor towardBlack(const QColor &c, double p)
{
    const auto linear = [](double v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    const auto encode = [](double v) {
        v = std::clamp(v, 0.0, 1.0);
        return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
    };
    const double r = linear(c.redF()), g = linear(c.greenF()), b = linear(c.blueF());
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b) * p;
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b) * p;
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b) * p;
    const double L = l * l * l, M = m * m * m, S = s * s * s;
    return QColor::fromRgbF(float(encode(4.0767416621 * L - 3.3077115913 * M + 0.2309699292 * S)),
                            float(encode(-1.2684380046 * L + 2.6097574011 * M - 0.3413193965 * S)),
                            float(encode(-0.0041960863 * L - 0.7034186147 * M + 1.7076147010 * S)));
}

// An element's classes.
class Classes
{
  public:
    explicit Classes(const QString &cls) : list(cls.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {}
    bool has(const char *name) const
    {
        for (const QString &c : list) {
            if (c == QLatin1String(name))
                return true;
        }
        return false;
    }
    void add(const QString &name) { list << name; }

  private:
    QStringList list;
};

// --c: the colour of a rise or a fall (.dg-svg .is-up / .is-down).
QColor upDown(const Classes &c)
{
    return c.has("is-down") ? pal().danger : c.has("is-up") ? pal().success : QColor(0, 0, 0);
}

/* Text */

struct TextStyle {
    QColor fill = QColor(0, 0, 0);
    double size = 16; // Inherited from the message.
    int weight = 400;
    double spacing = 0; // em
    bool tabular = false, halo = false, mono = false;
};

// The rules a text (or the group holding it) takes, in stylesheet order.
TextStyle textStyle(const Classes &c, int tone)
{
    TextStyle s;
    const QColor t = toneColor(tone);
    const auto font = [&s](double size, int weight) {
        s.size = size;
        s.weight = weight;
    };
    if (c.has("dg-title-text"))
        s.fill = ink(0), font(13.5, 650);
    if (c.has("dg-eyebrow") || c.has("dg-cluster-title") || c.has("dg-frame-kind") ||
        c.has("dg-quad-label") || c.has("dg-today-tag"))
        s.fill = ink(2), font(10.5, 650), s.spacing = 0.06;
    if (c.has("dg-tick") || c.has("dg-unit") || c.has("dg-axis-title") || c.has("dg-zone-label"))
        s.fill = ink(2), font(11, 450), s.tabular = true;
    if (c.has("dg-value") || c.has("dg-end") || c.has("dg-goal-label"))
        s.fill = ink(1), font(11, 600), s.tabular = true;
    if (c.has("dg-end"))
        s.fill = ink(0);
    if (c.has("dg-value") || c.has("dg-goal-label") || c.has("dg-zone-label") ||
        (c.has("dg-tick") && c.has("is-radar")) || c.has("dg-q-name") || c.has("dg-sankey-name") ||
        c.has("dg-sankey-value") || c.has("dg-cluster-title") || c.has("dg-task-meta") ||
        (c.has("dg-eyebrow") && c.has("is-over")))
        s.halo = true;
    if (c.has("dg-label"))
        s.fill = ink(0), font(12.5, 500);
    if (c.has("dg-card-title"))
        s.fill = ink(0), font(12.5, 650);
    if (c.has("dg-card-sub"))
        s.fill = ink(2), font(10.5, 500);
    if (c.has("dg-card-text"))
        s.fill = ink(0), font(12, 500);
    if (c.has("dg-card-meta") || c.has("dg-card-lead"))
        s.fill = ink(2), font(11, 450), s.mono = true;
    if (c.has("dg-card-badge"))
        s.fill = ink(2), font(9.5, 650), s.spacing = 0.04;
    if (c.has("dg-card-badge") && c.has("is-key"))
        s.fill = t;
    if (c.has("dg-edge-label"))
        s.fill = ink(1), font(11.5, 500);
    if (c.has("dg-mind-leaf"))
        s.fill = ink(1), font(12.5, 450);
    if (c.has("dg-chip"))
        s.fill = ink(1), font(11.5, 500);
    if (c.has("dg-center-value"))
        s.fill = ink(0), font(20, 650);
    if (c.has("dg-center-label"))
        s.fill = ink(2), font(11, 450);
    if (c.has("dg-legend-label"))
        s.fill = ink(0), font(12, 500);
    if (c.has("dg-legend-value"))
        s.fill = ink(0), font(12, 600), s.tabular = true;
    if (c.has("dg-legend-share"))
        s.fill = ink(2), font(11, 450), s.tabular = true;
    if (c.has("dg-row-name"))
        s.fill = ink(0), font(12.5, 500);
    if (c.has("dg-row-note") || c.has("dg-row-share"))
        s.fill = ink(2), font(11, 450), s.tabular = true;
    if (c.has("dg-row-value"))
        s.fill = ink(0), font(12, 600), s.tabular = true;
    if (c.has("dg-row-total"))
        s.fill = ink(0), font(12.5, 650), s.tabular = true;
    if (c.has("dg-metric-name"))
        s.fill = ink(1), font(12, 500);
    if (c.has("dg-figure-unit"))
        s.fill = ink(1), font(12, 450);
    if (c.has("dg-metric-change"))
        s.fill = ink(1), font(11.5, 600), s.tabular = true;
    if (c.has("dg-metric-change") && c.has("is-good"))
        s.fill = pal().success;
    if (c.has("dg-metric-change") && c.has("is-bad"))
        s.fill = pal().danger;
    if (c.has("dg-metric-note"))
        s.fill = ink(2), font(11, 450), s.tabular = true;
    if (c.has("dg-range-way"))
        s.fill = pal().warn, font(12, 650);
    if (c.has("dg-col-name"))
        s.fill = ink(0), font(12.5, 650);
    if (c.has("dg-col-sub"))
        s.fill = ink(2), font(11, 450);
    if (c.has("dg-step-n"))
        s.fill = ink(1), font(11, 650), s.tabular = true;
    if (c.has("dg-step-text"))
        s.fill = ink(0), font(13, 500);
    if (c.has("dg-step-meta"))
        s.fill = ink(2), font(11, 450), s.tabular = true;
    if (c.has("dg-step-note") || c.has("dg-step-warn"))
        s.fill = ink(1), font(12, 450);
    if (c.has("dg-step-warn"))
        s.fill = ink(0);
    if (c.has("dg-step-text") && c.has("is-done"))
        s.fill = ink(1);
    if (c.has("dg-funnel-rate"))
        s.fill = ink(2), font(10.5, 600), s.tabular = true;
    if (c.has("dg-sankey-name"))
        s.fill = ink(0), font(12, 500);
    if (c.has("dg-sankey-value"))
        s.fill = ink(2), font(11, 450), s.tabular = true;
    if (c.has("dg-cell-text"))
        s.fill = ink(0), font(11, 600), s.tabular = true;
    if (c.has("dg-tile-name") || c.has("dg-tile-value"))
        s.fill = ink(0), font(11.5, 500);
    if (c.has("dg-tile-value"))
        s.fill = ink(1), font(11, 450), s.tabular = true;
    if (c.has("dg-row-name") && c.has("is-quiet"))
        s.fill = ink(2);
    if (c.has("dg-tag"))
        s.fill = ink(1), font(11, 600);
    if (c.has("dg-acell-text"))
        s.fill = ink(0), font(12, 400), s.mono = true;
    if (c.has("dg-apoint-name"))
        s.fill = ink(1), font(11, 400), s.mono = true;
    if (c.has("dg-team"))
        s.fill = ink(0), font(12.5, 500);
    if (c.has("dg-score"))
        s.fill = ink(0), font(12, 600), s.tabular = true;
    if ((c.has("dg-team") || c.has("dg-score")) && c.has("is-out"))
        s.fill = ink(2);
    if (c.has("dg-value") && c.has("is-over"))
        s.fill = pal().danger;
    if (c.has("dg-was"))
        s.fill = ink(2), font(12, 600), s.tabular = true;
    if (c.has("dg-outline-mark"))
        s.fill = ink(2), font(12, 600), s.tabular = true;
    if (c.has("dg-outline-mark") && c.has("is-head"))
        s.fill = ink(1);
    if (c.has("dg-match-time"))
        s.fill = ink(1), font(12, 600), s.tabular = true;
    if (c.has("dg-row-total") && c.has("is-quiet"))
        s.fill = ink(2);
    if (c.has("dg-gloss-word"))
        s.fill = ink(0), font(13, 650);
    if (c.has("dg-gloss-means"))
        s.fill = ink(1), font(12.5, 500);
    if (c.has("dg-form"))
        s.fill = ink(0), font(13, 500);
    if (c.has("dg-form-end"))
        s.fill = ink(0), font(13, 650);
    if (c.has("dg-set-value"))
        s.fill = ink(0), font(12, 600), s.tabular = true;
    if (c.has("dg-route-leg"))
        s.fill = ink(2), font(11.5, 500);
    if (c.has("dg-last-tag"))
        s.fill = chatBg(), font(11, 600), s.tabular = true;
    if (c.has("dg-price"))
        s.fill = ink(0), font(24, 650);
    if (c.has("dg-change"))
        s.fill = upDown(c), font(12, 600), s.tabular = true;
    if (c.has("dg-period"))
        s.fill = ink(0), font(13, 650), s.tabular = true;
    if (c.has("dg-event"))
        s.fill = ink(1), font(12.5, 450);
    if (c.has("dg-event") && c.has("is-lead"))
        s.fill = ink(0);
    if (c.has("dg-task-name"))
        s.fill = ink(0), font(12.5, 500);
    if (c.has("dg-task-name") && c.has("is-done"))
        s.fill = ink(2);
    if (c.has("dg-task-meta"))
        s.fill = ink(2), font(11, 450), s.tabular = true;
    if (c.has("dg-today-tag"))
        s.fill = pal().danger;
    if (c.has("dg-quad-label") && c.has("is-lead"))
        s.fill = ink(1);
    if (c.has("dg-q-name"))
        s.fill = ink(0), font(12, 500);
    if (c.has("dg-radar-axis"))
        s.fill = ink(1), font(12, 500);
    if (c.has("dg-message-label"))
        s.fill = ink(0), font(12, 500);
    if (c.has("dg-badge"))
        s.fill = ink(1), font(10, 650), s.tabular = true;
    if (c.has("dg-note"))
        s.fill = ink(1), font(12, 450);
    if (c.has("dg-frame-label"))
        s.fill = ink(1), font(11.5, 500), s.spacing = 0;
    if (c.has("dg-wf-mark"))
        s.fill = fgA(0.82);
    if (c.has("dg-wf-strong"))
        s.fill = fgA(0.92);
    if (c.has("dg-wf-strong") && c.has("is-soft"))
        s.fill = fgA(pal().primary);
    if (c.has("dg-wf-text"))
        s.fill = fgA(pal().secondary);
    if (c.has("dg-wf-muted"))
        s.fill = fgA(pal().tertiary);
    if (c.has("dg-wf-on"))
        s.fill = chatBg();
    if (c.has("dg-wf-logo"))
        s.fill = fgA(0.3);
    if (c.has("dg-wf-quote"))
        s.fill = fgA(0.2);
    if (c.has("dg-wf-pill-text"))
        s.fill = fgA(pal().secondary);
    if (c.has("dg-wf-sectag"))
        s.fill = chatBg(), font(11, 600);
    if (c.has("dg-fv-title"))
        s.fill = fgA(pal().primary), font(14, 650);
    if (c.has("dg-fv-path"))
        s.fill = fgA(0.38), font(12, 500);
    if (c.has("dg-fv-total"))
        s.fill = fgA(pal().primary), font(12.5, 650), s.tabular = true;
    if (c.has("dg-fv-sum"))
        s.fill = fgA(pal().secondary), font(12.5, 500);
    if (c.has("dg-fv-name"))
        s.fill = fgA(pal().primary), font(13.5, 500);
    if (c.has("dg-fv-note") || c.has("dg-fv-date") || c.has("dg-fv-count") || c.has("dg-fv-more"))
        s.fill = fgA(0.4), font(12, 500), s.tabular = true;
    if (c.has("dg-fv-size"))
        s.fill = fgA(pal().secondary), font(12.5, 550), s.tabular = true;
    return s;
}

/* Shapes */

struct Stroke {
    QColor color = none();
    double width = 1;
    QVector<qreal> dash; // In user units.
    Qt::PenCapStyle cap = Qt::FlatCap;
    Qt::PenJoinStyle join = Qt::MiterJoin;
};
struct Shape {
    QColor fill = QColor(0, 0, 0); // SVG's default fill.
    Stroke stroke;
    double opacity = 1;
};

// The rules a rect, line, path or circle takes from its classes.
Shape shapeStyle(const Classes &c, int tone, bool hot, bool probing)
{
    Shape s;
    const QColor t = toneColor(tone);
    const auto stroke = [&s](QColor color, double width = -1) {
        s.stroke.color = color;
        if (width >= 0)
            s.stroke.width = width;
    };
    if (c.has("dg-shape"))
        s.fill = mix(pal().fgBase, 0.04, chatBg()), stroke(fgA(0.12), 1),
        s.stroke.join = Qt::RoundJoin;
    if (c.has("dg-shape-line"))
        s.fill = none(), stroke(fgA(0.12), 1);
    if (c.has("dg-dot"))
        s.fill = ink(1);
    if (c.has("dg-ring"))
        s.fill = none(), stroke(ink(1), 1.25);
    if (c.has("dg-card-head"))
        s.fill = fgA(0.03);
    if (c.has("dg-card-rule"))
        stroke(fgA(c.has("is-soft") ? 0.07 : 0.1));
    if (c.has("dg-mind-dot"))
        s.fill = t;
    if (c.has("dg-grid"))
        stroke(fgA(0.08));
    if ((c.has("dg-grid") && c.has("is-zero")) || c.has("dg-line"))
        stroke(fgA(0.16));
    if (c.has("dg-bar"))
        s.fill = t;
    if (c.has("dg-bar") && probing && !hot)
        s.opacity = 0.45;
    if (c.has("dg-point"))
        s.fill = t, stroke(chatBg(), 2);
    if (c.has("dg-zone"))
        s.fill = alpha(t, 0.08);
    if (c.has("dg-zone") && c.has("is-plain"))
        s.fill = fgA(0.045);
    if (c.has("dg-goal"))
        stroke(ink(1)), s.stroke.dash = {3, 3};
    if (c.has("dg-chip"))
        s.fill = t;
    if (c.has("dg-slice"))
        s.fill = none(), stroke(t);
    if (c.has("dg-swatch"))
        s.fill = t;
    if (c.has("dg-hit"))
        s.fill = none();
    if (c.has("dg-ledger-bar"))
        s.fill = t;
    if (c.has("dg-meter-track"))
        s.fill = alpha(t, 0.2);
    if (c.has("dg-meter-fill"))
        s.fill = t;
    if (c.has("dg-range-track"))
        stroke(fgA(0.12), 2), s.stroke.cap = Qt::RoundCap;
    if (c.has("dg-range-zone"))
        s.fill = fgA(0.24);
    if (c.has("dg-range-mark"))
        s.fill = t, stroke(chatBg(), 2);
    if (c.has("dg-range-mark") && c.has("is-out"))
        s.fill = pal().warn;
    if (c.has("dg-plate"))
        s.fill = fgA(hot ? 0.075 : 0.04);
    if (c.has("dg-step-mark"))
        s.fill = chatBg(), stroke(fgA(0.24), 1);
    if (c.has("dg-step-mark") && c.has("is-done"))
        stroke(alpha(t, 0.7));
    if (c.has("dg-rail"))
        stroke(fgA(0.14));
    if (c.has("dg-bar") && (c.has("is-up") || c.has("is-down")))
        s.fill = alpha(upDown(c), 0.86);
    if (c.has("dg-bridge"))
        stroke(fgA(0.24));
    if (c.has("dg-funnel-bar"))
        s.fill = t;
    if (c.has("dg-ribbon"))
        s.fill = alpha(t, hot ? 0.55 : 0.24);
    if (c.has("dg-sankey-node"))
        s.fill = t;
    if (c.has("dg-cell")) {
        s.fill = fgA(0.06);
        static const double levels[2][4] = {{0.3, 0.52, 0.76, 1}, {0.14, 0.3, 0.46, 0.64}};
        static const char *const names[] = {"is-l1", "is-l2", "is-l3", "is-l4"};
        const int soft = c.has("is-soft") ? 1 : 0;
        for (int k = 0; k < 4; ++k) {
            if (c.has(names[k]))
                s.fill = alpha(t, levels[soft][k]);
        }
        if (hot)
            stroke(ink(0), 1);
    }
    if (c.has("dg-cell-dot"))
        s.fill = t;
    if (c.has("dg-cell-off"))
        stroke(fgA(0.22)), s.stroke.cap = Qt::RoundCap;
    if (c.has("dg-grid") && c.has("is-faint"))
        stroke(fgA(0.045));
    if (c.has("dg-q-point") && c.has("is-bubble"))
        s.fill = alpha(t, 0.32), stroke(t, 1.25);
    if (c.has("dg-tile"))
        s.fill = alpha(t, hot ? 0.5 : 0.3);
    if (c.has("dg-commit"))
        s.fill = t, stroke(chatBg(), 2);
    if (c.has("dg-commit") && c.has("is-merge"))
        s.fill = chatBg(), stroke(t, 1.75);
    if (c.has("dg-acell"))
        s.fill = fgA(0.055);
    if (c.has("dg-acell") && c.has("is-lit"))
        s.fill = alpha(t, 0.2), stroke(alpha(t, 0.75));
    if (c.has("dg-apoint"))
        stroke(t, 1.5), s.stroke.cap = Qt::RoundCap;
    if (c.has("dg-state"))
        s.fill = ink(2);
    if (c.has("dg-state") && c.has("is-good"))
        s.fill = pal().success;
    if (c.has("dg-state") && c.has("is-warn"))
        s.fill = pal().warn;
    if (c.has("dg-state") && c.has("is-bad"))
        s.fill = pal().danger;
    if (c.has("dg-state") && c.has("is-open"))
        s.fill = none(), stroke(ink(2), 1.25);
    if (c.has("dg-slice") && c.has("is-track"))
        stroke(fgA(0.09));
    if (c.has("dg-check-mark")) {
        s.fill = chatBg(), stroke(fgA(0.24), 1.25);
        if (c.has("is-good"))
            s.fill = alpha(pal().success, 0.14), stroke(alpha(pal().success, 0.7));
        if (c.has("is-warn"))
            s.fill = alpha(pal().warn, 0.14), stroke(alpha(pal().warn, 0.75));
        if (c.has("is-bad"))
            s.fill = alpha(pal().danger, 0.14), stroke(alpha(pal().danger, 0.7));
    }
    if (c.has("dg-outline-dot"))
        s.fill = ink(2);
    if (c.has("dg-gloss-rule"))
        stroke(fgA(0.2), 1.5), s.stroke.cap = Qt::RoundCap;
    if (c.has("dg-gloss-rule") && c.has("is-key"))
        stroke(t, 2);
    if (c.has("dg-form-rule"))
        stroke(t, 1.5), s.stroke.cap = Qt::RoundCap;
    if (c.has("dg-leader"))
        stroke(fgA(0.26), 1.25), s.stroke.dash = {0.1, 4}, s.stroke.cap = Qt::RoundCap;
    if (c.has("dg-toggle"))
        s.fill = fgA(0.1), stroke(fgA(0.22), 1);
    if (c.has("dg-toggle") && c.has("is-on"))
        s.fill = t, stroke(none());
    if (c.has("dg-toggle-knob"))
        s.fill = c.has("is-on") ? QColor(255, 255, 255) : fgA(0.5);
    if (c.has("dg-route-rail"))
        stroke(alpha(t, 0.6), 1.5), s.stroke.cap = Qt::RoundCap;
    if (c.has("dg-route-stop"))
        s.fill = t;
    if (c.has("dg-route-stop") && c.has("is-end"))
        s.fill = chatBg(), stroke(t, 2);
    if (c.has("dg-wick"))
        stroke(upDown(c), 1);
    if (c.has("dg-candle-body"))
        s.fill = upDown(c);
    if (c.has("dg-vol"))
        s.fill = alpha(upDown(c), 0.28);
    if ((c.has("dg-candle") || c.has("dg-vol")) && probing && !hot)
        s.opacity = 0.35;
    if (c.has("dg-last"))
        stroke(alpha(upDown(c), 0.7), 1), s.stroke.dash = {2, 3};
    if (c.has("dg-bracket"))
        stroke(fgA(0.12));
    if (c.has("dg-axis-line"))
        stroke(fgA(0.16), 1);
    if (c.has("dg-tl-dot"))
        s.fill = chatBg(), stroke(t, 1.5);
    if (c.has("dg-sep"))
        stroke(fgA(0.08));
    if (c.has("dg-task")) {
        const QColor own = c.has("is-crit") ? pal().danger : t;
        s.fill = alpha(own, c.has("is-crit") ? 0.85 : 0.6);
        if (c.has("is-active") || hot)
            s.fill = own;
        if (c.has("is-done"))
            s.fill = fgA(0.2);
        if (c.has("is-milestone"))
            s.fill = ink(0), stroke(chatBg(), 2);
    }
    if (c.has("dg-today"))
        stroke(pal().danger, 1);
    if (c.has("dg-quad"))
        s.fill = fgA(0.035);
    if (c.has("dg-quad-axis"))
        stroke(fgA(0.16));
    if (c.has("dg-q-point") && !c.has("is-bubble"))
        s.fill = t, stroke(chatBg(), 2);
    if (c.has("dg-radar-grid"))
        s.fill = none(), stroke(fgA(c.has("is-outer") ? 0.16 : 0.08));
    if (c.has("dg-radar-spoke"))
        stroke(fgA(0.08));
    if (c.has("dg-radar-curve"))
        s.fill = alpha(t, hot ? 0.22 : 0.1), stroke(t, 1.5), s.stroke.join = Qt::RoundJoin;
    if (c.has("dg-life"))
        stroke(fgA(0.12), 1);
    if (c.has("dg-divider"))
        stroke(fgA(0.14)), s.stroke.dash = {3, 3};
    if (c.has("dg-wf-window"))
        s.fill = mix(pal().fgBase, 0.03, chatBg()), stroke(fgA(0.1), 1);
    if (c.has("dg-wf-window") && c.has("is-phone"))
        stroke(fgA(0.18), 1.5);
    if (c.has("dg-wf-light"))
        s.fill = fgA(0.13);
    if (c.has("dg-wf-address"))
        s.fill = fgA(0.05);
    if (c.has("dg-wf-island"))
        s.fill = QColor(0, 0, 0), stroke(fgA(0.06));
    if (c.has("dg-wf-battery"))
        s.fill = none(), stroke(fgA(0.45));
    if (c.has("dg-wf-charge") || c.has("dg-wf-home"))
        s.fill = fgA(0.6);
    if (c.has("dg-wf-rule"))
        stroke(fgA(c.has("is-step") ? 0.16 : 0.08), 1);
    if (c.has("dg-wf-btn"))
        s.fill = none(), stroke(fgA(0.22), 1);
    if (c.has("dg-wf-btn") && c.has("is-primary"))
        s.fill = fgA(0.92), stroke(none());
    if (c.has("dg-wf-skel"))
        s.fill = c.has("is-on") ? alpha(chatBg(), 0.3) : fgA(0.08);
    if (c.has("dg-wf-media"))
        s.fill = fgA(0.05);
    if (c.has("dg-wf-card"))
        s.fill = fgA(0.028), stroke(fgA(0.07), 1);
    if (c.has("dg-wf-card") && c.has("is-featured"))
        s.fill = fgA(0.045), stroke(fgA(0.5), 1);
    if (c.has("dg-wf-icon"))
        s.fill = fgA(0.07);
    if (c.has("dg-wf-avatar"))
        s.fill = fgA(0.1);
    if (c.has("dg-wf-step"))
        s.fill = mix(pal().fgBase, 0.06, chatBg()), stroke(fgA(0.16), 1);
    if (c.has("dg-wf-pill"))
        s.fill = fgA(0.08);
    if (c.has("dg-wf-banner"))
        s.fill = fgA(0.045);
    if (c.has("dg-wf-input"))
        s.fill = fgA(0.025), stroke(fgA(0.13), 1);
    if (c.has("dg-wf-band"))
        s.fill = alpha(t, hot ? 0.035 : 0), stroke(alpha(t, hot ? 0.4 : 0), 1);
    if (c.has("dg-fv-card"))
        s.fill = pal().composerBg, stroke(pal().composerBorder, 1);
    if (c.has("dg-fv-rule"))
        stroke(fgA(0.06), 1);
    if (c.has("dg-fv-guide"))
        stroke(fgA(0.09), 1.2), s.stroke.cap = Qt::RoundCap;
    return s;
}

QPen penOf(const Stroke &stroke)
{
    QPen pen(stroke.color, stroke.width, Qt::SolidLine, stroke.cap, stroke.join);
    pen.setMiterLimit(4);
    if (!stroke.dash.isEmpty() && stroke.width > 0) {
        QVector<qreal> units;
        for (const qreal d : stroke.dash)
            units << std::max(1e-3, d / stroke.width);
        pen.setDashPattern(units);
    }
    return pen;
}

/* Geometry */

QPainterPath cubicPath(const Pts &pts)
{
    QPainterPath path;
    if (pts.size() < 2)
        return path;
    path.moveTo(pts[0], pts[1]);
    for (int i = 2; i + 5 < pts.size(); i += 6)
        path.cubicTo(pts[i], pts[i + 1], pts[i + 2], pts[i + 3], pts[i + 4], pts[i + 5]);
    return path;
}

QPainterPath circlePath(QPointF c, double r)
{
    QPainterPath path;
    path.addEllipse(c, r, r);
    return path;
}

// headPath: an arrow's end, by kind.
QPainterPath headPath(const QString &kind, const Pts &pts, bool atStart)
{
    QPainterPath path;
    const int n = int(pts.size());
    if (n < 8)
        return path;
    double px, py, qx, qy;
    if (atStart) {
        px = pts[0], py = pts[1], qx = pts[2], qy = pts[3];
        if (std::hypot(px - qx, py - qy) < 0.01)
            qx = pts[6], qy = pts[7];
    } else {
        px = pts[n - 2], py = pts[n - 1], qx = pts[n - 4], qy = pts[n - 3];
        if (std::hypot(px - qx, py - qy) < 0.01)
            qx = pts[n - 8], qy = pts[n - 7];
    }
    double dx = px - qx, dy = py - qy;
    const double length = std::hypot(dx, dy) > 0 ? std::hypot(dx, dy) : 1;
    dx /= length;
    dy /= length;
    const double nx = -dy, ny = dx, L = ARROW.length, H = ARROW.half;
    const auto at = [&](double back, double side = 0) {
        return QPointF(px - dx * back + nx * side, py - dy * back + ny * side);
    };
    const auto ring = [&](double back) { path.addPath(circlePath(at(back), 4)); };
    const auto bar = [&](double back) {
        path.moveTo(at(back, 6));
        path.lineTo(at(back, -6));
    };
    const auto crow = [&] {
        path.moveTo(at(0, 7));
        path.lineTo(at(12));
        path.moveTo(at(0));
        path.lineTo(at(12));
        path.moveTo(at(0, -7));
        path.lineTo(at(12));
    };
    if (kind == u"arrow") {
        path.moveTo(px + dx * L, py + dy * L);
        path.lineTo(px + nx * H, py + ny * H);
        path.lineTo(px - nx * H, py - ny * H);
        path.closeSubpath();
    } else if (kind == u"open") {
        path.moveTo(px + nx * 5, py + ny * 5);
        path.lineTo(px + dx * L, py + dy * L);
        path.lineTo(px - nx * 5, py - ny * 5);
    } else if (kind == u"circle") {
        path.addPath(circlePath(QPointF(px + dx * 4, py + dy * 4), 4));
    } else if (kind == u"triangle") {
        path.moveTo(at(0));
        path.lineTo(at(12, 7));
        path.lineTo(at(12, -7));
        path.closeSubpath();
    } else if (kind == u"diamond" || kind == u"odiamond") {
        path.moveTo(at(0));
        path.lineTo(at(8, 5.5));
        path.lineTo(at(16));
        path.lineTo(at(8, -5.5));
        path.closeSubpath();
    } else if (kind == u"vee") {
        path.moveTo(at(9, 5));
        path.lineTo(at(0));
        path.lineTo(at(9, -5));
    } else if (kind == u"one") {
        bar(7);
        bar(12);
    } else if (kind == u"zero-one") {
        bar(7);
        ring(16);
    } else if (kind == u"one-many") {
        crow();
        bar(16);
    } else if (kind == u"zero-many") {
        crow();
        ring(18);
    } else {
        const double cx = px + dx * 4, cy = py + dy * 4;
        path.moveTo(cx - 4, cy - 4);
        path.lineTo(cx + 4, cy + 4);
        path.moveTo(cx + 4, cy - 4);
        path.lineTo(cx - 4, cy + 4);
    }
    return path;
}

// barPath: flat where it stands, rounded where the value ends.
QPainterPath barPath(double x, double top, double w, double base, double radius)
{
    QPainterPath path;
    const double h = std::abs(base - top);
    if (h < 0.3) {
        path.moveTo(x, base);
        path.lineTo(x + w, base);
        return path;
    }
    const double r = std::max(0.0, std::min({radius, w / 2, h})), s = top < base ? 1 : -1;
    path.moveTo(x, base);
    path.lineTo(x, top + s * r);
    path.quadTo(x, top, x + r, top);
    path.lineTo(x + w - r, top);
    path.quadTo(x + w, top, x + w, top + s * r);
    path.lineTo(x + w, base);
    path.closeSubpath();
    return path;
}

QPainterPath roundRect(double x, double y, double w, double h, double rx)
{
    QPainterPath path;
    w = std::max(0.0, w);
    h = std::max(0.0, h);
    rx = std::clamp(rx, 0.0, std::min(w, h) / 2);
    path.addRoundedRect(QRectF(x, y, w, h), rx, rx);
    return path;
}

// shapePath: the outlines of a scheme's blocks that are not boxes.
QPainterPath shapePath(const QString &shape, double w, double h)
{
    QPainterPath path;
    const double x = -w / 2, y = -h / 2;
    if (shape == u"diamond") {
        path.moveTo(0, y);
        path.lineTo(-x, 0);
        path.lineTo(0, -y);
        path.lineTo(x, 0);
        path.closeSubpath();
    } else if (shape == u"hexagon") {
        const double e = h * 0.3;
        path.moveTo(x + e, y);
        path.lineTo(-x - e, y);
        path.lineTo(-x, 0);
        path.lineTo(-x - e, -y);
        path.lineTo(x + e, -y);
        path.lineTo(x, 0);
        path.closeSubpath();
    } else if (shape == u"lean") {
        path.moveTo(x + 12, y);
        path.lineTo(-x, y);
        path.lineTo(-x - 12, -y);
        path.lineTo(x, -y);
        path.closeSubpath();
    } else if (shape == u"flag") {
        path.moveTo(x, y);
        path.lineTo(-x, y);
        path.lineTo(-x, -y);
        path.lineTo(x, -y);
        path.lineTo(x + 10, 0);
        path.closeSubpath();
    } else if (shape == u"cylinder") {
        // The top's back edge, down the side, the bottom's front edge, up.
        path.moveTo(x, y + 6);
        path.arcTo(QRectF(x, y, w, 12), 180, -180);
        path.lineTo(-x, -y - 6);
        path.arcTo(QRectF(x, -y - 12, w, 12), 0, -180);
        path.closeSubpath();
    }
    return path;
}

// GLYPHS (diagram.js): wireframe and list marks on a 24 grid.
const QHash<QString, QPainterPath> &glyphs()
{
    static const QHash<QString, QPainterPath> table = [] {
        const std::pair<const char *, const char *> list[] = {
            {"image", "M-9-7h18a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2h-18a2 2 0 0 1-2-2v-10a2 2 0 0 1 2-2z"
                      "M-8 5.5l5.2-5.5 3.6 3.6 2.7-2.6 4.8 4.5M2.8-3.2a1.7 1.7 0 1 0 3.4 0"
                      "a1.7 1.7 0 1 0-3.4 0"},
            {"play", "M0-10a10 10 0 1 1 0 20a10 10 0 1 1 0-20zM-2.6-4.6l7 4.6-7 4.6z"},
            {"pin", "M0 9c-4.5-4.6-7-8-7-11.5a7 7 0 0 1 14 0c0 3.5-2.5 6.9-7 11.5z"
                    "M0-5.2a2.3 2.3 0 1 1 0 4.6a2.3 2.3 0 1 1 0-4.6z"},
            {"check", "M-5 0.5l3.4 3.4 6.6-7"},
            {"plus", "M-5 0h10M0-5v10"},
            {"minus", "M-5 0h10"},
            {"menu", "M-7-5h14M-7 0h14M-7 5h14"},
            {"lock", "M-4.5-1h9v7h-9zM-3-1v-2.5a3 3 0 0 1 6 0v2.5"},
            {"spark", "M0-7l1.8 5.2 5.2 1.8-5.2 1.8-1.8 5.2-1.8-5.2-5.2-1.8 5.2-1.8z"},
            {"bolt", "M1.5-8l-6.5 9h5l-1.5 7 6.5-9h-5z"},
            {"shield", "M0-8l6.5 2.5v4.5c0 4-2.8 6.8-6.5 8-3.7-1.2-6.5-4-6.5-8v-4.5z"},
            {"chart", "M-6 6v-5M-2 6v-9M2 6v-6M6 6v-11"},
            {"heart", "M0 6.5c-4-3-6.8-5.6-6.8-8.6a3.6 3.6 0 0 1 6.8-1.8 3.6 3.6 0 0 1 6.8 1.8"
                      "c0 3-2.8 5.6-6.8 8.6z"},
            {"clock", "M0-7.5a7.5 7.5 0 1 1 0 15a7.5 7.5 0 1 1 0-15zM0-4v4l2.8 2"},
            {"star", "M0-7.5l2.2 4.6 5 .7-3.6 3.5.9 5-4.5-2.4-4.5 2.4.9-5-3.6-3.5 5-.7z"},
            {"warn", "M0-8.5l9 15.5h-18zM0-3v5M0 4.6v.4"},
            {"bang", "M0-6v7M0 5v.4"},
            {"cross", "M-4.5-4.5l9 9M4.5-4.5l-9 9"},
            {"ask", "M-3.2-3.2a3.3 3.3 0 1 1 5 2.8c-1.2.8-1.8 1.4-1.8 2.9M0 6.2v.4"},
            {"sheet", "M-6-9h7.5l4.5 4.5v13.5h-12zM1.5-9v4.5h4.5M-3 1h6M-3 4.5h6"},
        };
        QHash<QString, QPainterPath> out;
        for (const auto &[name, d] : list)
            out.insert(QString::fromLatin1(name), svgpath::parse(d));
        return out;
    }();
    return table;
}

// glyphs.js's folder (viewBox 30 30 60 60), shut as .dg-svg draws it.
const QPainterPath &folderGlyph()
{
    static const QPainterPath path =
        svgpath::parse("M38 70V46a4 4 0 0 1 4-4h10a4 4 0 0 1 3.2 1.6L58 47h20a4 4 0 0 1 4 4v2"
                       "M38 53L82 53L82 72A4 4 0 0 1 78 76L42 76A4 4 0 0 1 38 72Z");
    return path;
}

const QString Start = QStringLiteral("start"), Middle = QStringLiteral("middle"),
              End = QStringLiteral("end"), Central = QStringLiteral("central");

/* The painter */

class Painter
{
  public:
    Painter(QPainter &p, Ctx &fonts, const Look &look, const QString &kind, QVector<QRectF> *texts,
            QVector<Hit> *hits)
        : p(p), fonts(fonts), look(look), texts(texts), hits(hits)
    {
        // Where a whole row answers the pointer, the marks over it let it
        // through (ledger, ranges, git).
        rowsAnswer = kind == u"ledger" || kind == u"ranges" || kind == u"git";
    }

    void paint(const Live &item, int layer)
    {
        current = &item;
        const Spec &spec = item.spec;
        // A cluster's name lies over the arrows that cross it.
        if (spec.type == Type::Cluster && layer == Labels) {
            p.save();
            clusterTitle(item);
            p.restore();
            return;
        }
        if (layerOf(spec) != layer)
            return;
        p.save();
        switch (spec.type) {
        case Type::View:
            break;
        case Type::Node:
            node(item);
            break;
        case Type::Edge:
            edge(item);
            break;
        case Type::Label:
            label(item);
            break;
        case Type::Line:
            line(item);
            break;
        case Type::Bar:
            bar(item);
            break;
        case Type::Area:
            area(item);
            break;
        case Type::Arc:
            arc(item);
            break;
        case Type::Legend:
            legend(item);
            break;
        case Type::Dot:
            dot(item);
            break;
        case Type::Badge:
            badge(item);
            break;
        case Type::Note:
            note(item);
            break;
        case Type::Frame:
            frame(item);
            break;
        case Type::Cluster:
            cluster(item);
            break;
        case Type::Candle:
            candle(item);
            break;
        case Type::Column:
            column(item);
            break;
        case Type::Span:
            span(item);
            break;
        case Type::Rect:
            rect(item);
            break;
        case Type::Poly:
            poly(item);
            break;
        case Type::Chip:
            chip(item);
            break;
        case Type::Hit:
            hit(item);
            break;
        case Type::Figure:
            figure(item);
            break;
        case Type::Ribbon:
            ribbon(item);
            break;
        case Type::WBox:
            wbox(item);
            break;
        case Type::WGlyph:
            wglyph(item);
            break;
        case Type::FCard:
            fcard(item);
            break;
        case Type::FHead:
            fhead(item);
            break;
        case Type::FSeg:
            fseg(item);
            break;
        case Type::FRow:
            frow(item);
            break;
        }
        p.restore();
    }

  private:
    // Things come in the way a print develops: they clear and settle.
    void pop(double a, QPointF centre)
    {
        if (a >= 1)
            return;
        p.setOpacity(p.opacity() * std::min(1.0, std::max(0.0, a) * 1.6));
        const double s = 0.94 + 0.06 * easeOut(clamp01(a));
        p.translate(centre);
        p.scale(s, s);
        p.translate(-centre);
    }
    void fade(double a, double rise = 0)
    {
        if (a >= 1)
            return;
        const double e = easeOut(clamp01(a));
        p.setOpacity(p.opacity() * e);
        if (rise)
            p.translate(0, (1 - e) * rise);
    }
    void dim(double a)
    {
        if (a < 1)
            p.setOpacity(p.opacity() * clamp01(a));
    }

    void fillShape(const QPainterPath &path, const Shape &s)
    {
        p.save();
        if (s.opacity < 1)
            p.setOpacity(p.opacity() * s.opacity);
        if (s.fill.alpha() > 0)
            p.fillPath(path, s.fill);
        if (s.stroke.color.alpha() > 0 && s.stroke.width > 0)
            p.strokePath(path, penOf(s.stroke));
        p.restore();
    }

    void addHit(const QPainterPath &shape, bool node = false)
    {
        if (!hits || !current)
            return;
        if (rowsAnswer && layerOf(current->spec) != Back)
            return;
        hits->append({current->spec.key, p.worldTransform().map(shape), node});
    }
    // Where a filled or stroked shape answers the pointer (a transparent
    // fill counts as painted, as in SVG).
    void hitShape(const QPainterPath &path, const Shape &s, bool filled = true)
    {
        if (!hits)
            return;
        QPainterPath area;
        if (filled && (s.fill.alpha() > 0 || Classes(current->spec.fixed.cls).has("dg-hit")))
            area = path;
        if (s.stroke.color.alpha() > 0 && s.stroke.width > 0) {
            QPainterPathStroker stroker;
            stroker.setWidth(std::max(1.0, s.stroke.width));
            area = area.united(stroker.createStroke(path));
        }
        if (!area.isEmpty())
            addHit(area);
    }

    const Face &faceOf(const TextStyle &style)
    {
        return fonts.face(style.size, style.weight, style.spacing, style.mono, style.tabular);
    }
    // Where a baseline lies from y. Chromium lays SVG text out with the font
    // at its size on the device and whole-pixel ascent and descent there:
    // central halfway between them, hanging at .8 of the ascent, each rounded.
    double baselineShift(const Face &face, const QString &baseline) const
    {
        if (baseline == u"above" || baseline == u"auto")
            return 0; // alphabetic
        const QTransform &t = p.worldTransform();
        const double k = std::max(1e-6, std::sqrt(std::abs(t.determinant())) *
                                            (p.device() ? p.device()->devicePixelRatio() : 1));
        const double ascent = std::round(face.ascent() * k),
                     descent = std::round(face.descent() * k);
        if (baseline == u"central")
            return std::floor((ascent - descent) / 2 + 0.5) / k;
        return std::round(ascent * 0.8) / k; // "below": hanging
    }
    static double lineOffset(const QString &baseline, int i, int n, double lineHeight)
    {
        return baseline == u"central" ? (i - (n - 1) / 2.0) * lineHeight
               : baseline == u"above" ? -(n - 1 - i) * lineHeight
                                      : i * lineHeight;
    }
    // Lines of text around (x, y), as textBlock: anchor start/middle/end and a
    // central, above (alphabetic) or hanging baseline.
    void text(const QStringList &lines, double x, double y, const TextStyle &style,
              const QString &anchor, const QString &baseline, double lineHeight,
              bool answers = true)
    {
        const Face &face = faceOf(style);
        const int n = int(lines.size());
        for (int i = 0; i < n; ++i) {
            const double dy =
                lineOffset(baseline, i, n, lineHeight) + baselineShift(face, baseline);
            const double w = face.advance(lines[i]);
            const double dx = anchor == u"middle" ? -w / 2 : anchor == u"end" ? -w : 0;
            drawLine(lines[i], QPointF(x + dx, y + dy), face, style, w, answers);
        }
    }
    void drawLine(const QString &line, QPointF at, const Face &face, const TextStyle &style,
                  double w, bool answers)
    {
        if (line.isEmpty())
            return;
        // Set at the face's scale and drawn scaled down: fractional sizes.
        // The baseline lands on a whole device pixel, as Chromium's does.
        p.save();
        {
            const double dpr = p.device() ? p.device()->devicePixelRatio() : 1;
            const QPointF device = p.worldTransform().map(at) * dpr;
            const QPointF snapped(device.x(), std::round(device.y()));
            bool invertible = false;
            const QTransform back = p.worldTransform().inverted(&invertible);
            if (invertible && p.worldTransform().isAffine() && !p.worldTransform().isRotating())
                at = back.map(snapped / dpr);
        }
        p.translate(at);
        p.scale(1 / Face::Scale, 1 / Face::Scale);
        p.setFont(face.font);
        if (style.halo) {
            // paint-order: stroke, a 3 px rim of the surface round the glyphs,
            // outlined once per face and text.
            const QString key = face.font.key() + QLatin1Char('\x1f') + line;
            auto found = fonts.halos.constFind(key);
            if (found == fonts.halos.constEnd()) {
                if (fonts.halos.size() > 2000)
                    fonts.halos.clear();
                QPainterPath outline;
                outline.addText(QPointF(), face.font, line);
                QPainterPathStroker stroker;
                stroker.setWidth(3 * Face::Scale);
                stroker.setCapStyle(Qt::RoundCap);
                stroker.setJoinStyle(Qt::RoundJoin);
                found = fonts.halos.insert(key, stroker.createStroke(outline).simplified());
            }
            p.fillPath(*found, chatBg());
        }
        p.setPen(style.fill);
        p.drawText(QPointF(), line);
        p.restore();
        const QRectF box(at.x(), at.y() - face.ascent(), w, face.ascent() + face.descent());
        if (texts)
            *texts << p.worldTransform().mapRect(box);
        if (answers) {
            QPainterPath shape;
            shape.addRect(box);
            addHit(shape);
        }
    }
    // The box text lines take: a transform's origin (the fill-box centre).
    QRectF textBox(const QStringList &lines, const TextStyle &style, const QString &anchor,
                   const QString &baseline, double lineHeight)
    {
        const Face &face = faceOf(style);
        QRectF box;
        const int n = int(lines.size());
        for (int i = 0; i < n; ++i) {
            const double dy =
                lineOffset(baseline, i, n, lineHeight) + baselineShift(face, baseline);
            const double w = face.advance(lines[i]);
            const double dx = anchor == u"middle" ? -w / 2 : anchor == u"end" ? -w : 0;
            box |= QRectF(dx, dy - face.ascent(), w, face.ascent() + face.descent());
        }
        return box;
    }
    void textAs(const QStringList &lines, double x, double y, const char *cls, int tone,
                const QString &anchor, bool answers = true)
    {
        text(lines, x, y, textStyle(Classes(QString::fromLatin1(cls)), tone), anchor, Central, 16,
             answers);
    }

    Shape shape(const QString &cls, int tone) const
    {
        return shapeStyle(Classes(cls), tone, current->hot, look.probing);
    }

    /* Types */

    void node(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const Props &c = item.cur;
        p.translate(c.x, c.y);
        Classes cls(QStringLiteral("dg-node ") + fx.cls);
        if (fx.key)
            cls.add(QStringLiteral("is-key"));
        const bool hovered = !look.hovered.isEmpty() && look.hovered == item.spec.key;
        pop(item.appear, QPointF(0, 0));
        const QColor t = toneColor(fx.tone);
        Shape body;
        body.fill = mix(pal().fgBase, hovered ? 0.07 : 0.04, chatBg());
        body.stroke.color = fgA(hovered ? 0.28 : 0.12);
        body.stroke.join = Qt::RoundJoin;
        if (cls.has("is-key")) {
            body.fill = mix(t, hovered ? 0.14 : 0.09, chatBg());
            body.stroke.color = hovered ? t : alpha(t, 0.62);
        }
        if (cls.has("is-root")) {
            body.fill = mix(pal().fgBase, 0.07, chatBg());
            body.stroke.color = fgA(0.22);
        }
        const double w = c.w, h = c.h;
        if (fx.shape == u"card" && fx.card) {
            card(*fx.card, body, w, h, fx.tone);
            return;
        }
        if (fx.shape == u"start") {
            const QPainterPath dot = circlePath({}, 5);
            p.fillPath(dot, ink(1));
            addHit(dot, true);
            return;
        }
        if (fx.shape == u"end") {
            p.strokePath(circlePath({}, 7), QPen(ink(1), 1.25));
            p.fillPath(circlePath({}, 3.5), ink(1));
            addHit(circlePath({}, 7.6), true);
            return;
        }
        QPainterPath outline;
        if (fx.shape == u"circle")
            outline = circlePath({}, w / 2);
        else if (fx.shape == u"diamond" || fx.shape == u"hexagon" || fx.shape == u"lean" ||
                 fx.shape == u"flag" || fx.shape == u"cylinder")
            outline = shapePath(fx.shape, w, h);
        else {
            const double radius = fx.shape == u"stadium" ? h / 2
                                  : fx.shape == u"round" ? std::min(12.0, h / 2)
                                                         : NODE.radius;
            outline = roundRect(-w / 2, -h / 2, w, h, radius);
        }
        fillShape(outline, body);
        addHit(outline, true);
        if (fx.shape == u"cylinder" || fx.shape == u"subroutine") {
            QPainterPath extra;
            if (fx.shape == u"cylinder") {
                extra.moveTo(-w / 2, -h / 2 + 6);
                extra.arcTo(QRectF(-w / 2, -h / 2, w, 12), 180, 180);
            } else {
                extra.moveTo(-w / 2 + 6, -h / 2);
                extra.lineTo(-w / 2 + 6, h / 2);
                extra.moveTo(w / 2 - 6, -h / 2);
                extra.lineTo(w / 2 - 6, h / 2);
            }
            p.strokePath(extra, QPen(fgA(0.12), 1));
        }
        if (fx.lines.isEmpty())
            return;
        TextStyle label = textStyle(Classes(QStringLiteral("dg-label")), fx.tone);
        if (cls.has("is-root"))
            label.size = 13.5, label.weight = 650;
        if (cls.has("is-branch"))
            label.weight = 600;
        if (!fx.headLines) {
            text(fx.lines, 0, 0, label, Middle, Central, TEXT.line, false);
            return;
        }
        // The name of the block and, under it, what it does.
        const int head = std::min(fx.headLines, int(fx.lines.size()));
        const int rest = int(fx.lines.size()) - head;
        double y = -(head * TEXT.line + NODE.part + rest * NODE.detail) / 2;
        TextStyle strong = label, detail = label;
        strong.weight = 650;
        detail.fill = ink(1);
        detail.size = 11.5;
        detail.weight = 450;
        for (int i = 0; i < fx.lines.size(); ++i) {
            const double lh = i < head ? TEXT.line : NODE.detail;
            if (i == head)
                y += NODE.part;
            text({fx.lines[i]}, 0, y + lh / 2, i < head ? strong : detail, Middle, Central, lh,
                 false);
            y += lh;
        }
    }

    void card(const Card &card, const Shape &body, double w, double h, int tone)
    {
        const double x = -card.w / 2, y = -card.h / 2, r = 10;
        const QPainterPath outline = roundRect(-w / 2, -h / 2, w, h, 10);
        fillShape(outline, body);
        addHit(outline, true);
        if (!card.rows.isEmpty()) {
            QPainterPath band;
            band.moveTo(x, y + card.head);
            band.lineTo(x, y + r);
            band.quadTo(x, y, x + r, y);
            band.lineTo(-x - r, y);
            band.quadTo(-x, y, -x, y + r);
            band.lineTo(-x, y + card.head);
            band.closeSubpath();
            p.fillPath(band, fgA(0.03));
            p.setPen(QPen(fgA(0.1), 1));
            p.drawLine(QPointF(x, y + card.head), QPointF(-x, y + card.head));
        }
        const double titleY = card.rows.isEmpty() ? 0 : y + card.head / 2;
        if (!card.sub.isEmpty())
            textAs({card.sub}, 0, titleY - 8, "dg-card-sub", NoTone, Middle, false);
        textAs({card.title}, 0, card.sub.isEmpty() ? titleY : titleY + 7, "dg-card-title", NoTone,
               Middle, false);
        double rowY = y + card.head + 4.5;
        for (int i = 0; i < card.rows.size(); ++i) {
            const CardRow &row = card.rows[i];
            if (i == card.sep) {
                p.setPen(QPen(fgA(0.07), 1));
                p.drawLine(QPointF(x + 10, rowY + 4.5), QPointF(-x - 10, rowY + 4.5));
                rowY += 9;
            }
            const double cy = rowY + CARD.row / 2, left = x + CARD.padX;
            if (!row.lead.isEmpty()) {
                QString lead =
                    row.badge ? QStringLiteral("dg-card-badge") : QStringLiteral("dg-card-lead");
                if (row.badge && row.lead.contains(QLatin1String("PK")))
                    lead += QStringLiteral(" is-key");
                text({row.lead}, left, cy, textStyle(Classes(lead), tone), Start, Central, 16,
                     false);
            }
            textAs({row.text}, left + card.leadW, cy, "dg-card-text", NoTone, Start, false);
            if (!row.meta.isEmpty())
                textAs({row.meta}, -x - CARD.padX, cy, "dg-card-meta", NoTone, End, false);
            rowY += CARD.row;
        }
    }

    void edge(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const Pts &pts = item.cur.pts;
        if (pts.size() < 8)
            return;
        const Classes cls(QStringLiteral("dg-edge is-") + fx.style + QLatin1Char(' ') + fx.cls);
        const bool lit =
            !look.lit.isEmpty() && !fx.a.isEmpty() && (fx.a == look.lit || fx.b == look.lit);
        const QColor t = toneColor(fx.tone);
        // The line (.dg-edge > path:first-child).
        Stroke line;
        line.color = fgA(0.3);
        line.width = 1.25;
        line.cap = Qt::RoundCap;
        if (cls.has("is-thick"))
            line.color = fgA(0.44), line.width = 2;
        if (cls.has("is-dotted"))
            line.dash = {1.5, 4.5};
        if (cls.has("is-dashed"))
            line.dash = {4, 4};
        if (lit)
            line.color = t;
        if (cls.has("dg-branch")) {
            line.color = alpha(t, 0.8), line.width = 1.5;
            if (cls.has("is-d2"))
                line.width = 1.25;
            if (cls.has("is-d3"))
                line.color = alpha(t, 0.6), line.width = 1;
        }
        if (cls.has("dg-stroke")) {
            line.color = t, line.width = 1.75, line.join = Qt::RoundJoin;
            if (cls.has("is-ma"))
                line.width = 1.25;
        }
        if (cls.has("dg-spark"))
            line.width = 1.5;
        if (cls.has("dg-lane"))
            line.width = 1.75;
        if (cls.has("dg-tie"))
            line.color = fgA(0.2), line.width = 1, line.join = Qt::RoundJoin;
        if (cls.has("dg-turn"))
            line.color = fgA(0.34), line.width = 1.25;
        if (cls.has("dg-message"))
            line.color = fgA(0.46);
        // Among lit arrows, the rest step back.
        if (!look.lit.isEmpty() && !lit)
            p.setOpacity(p.opacity() * 0.35);
        const double a = item.appear;
        const QPainterPath path = cubicPath(pts);
        double headOpacity = 1;
        bool drawn = true;
        if (a < 1) {
            const double e = easeInOut(clamp01(a));
            if (fx.grow) {
                p.translate(pts[0], 0);
                p.scale(std::max(1e-3, e), 1);
                p.translate(-pts[0], 0);
            } else if (fx.style == u"solid" || fx.style == u"thick") {
                const double length = path.length();
                drawn = length * e > 0.01;
                line.dash = {length * e, length + 1};
            } else {
                p.setOpacity(p.opacity() * e);
            }
            headOpacity = clamp01((e - 0.82) / 0.18);
        }
        if (drawn)
            p.strokePath(path, penOf(line));
        if (hits) {
            QPainterPathStroker stroker;
            stroker.setWidth(std::max(6.0, line.width));
            addHit(stroker.createStroke(path));
        }
        // Its ends: the end's head first, the start's after it.
        QString start, end;
        if (fx.hasEnds) {
            start = fx.endStart;
            end = fx.endEnd;
        } else {
            const QString head = fx.head.isEmpty() || fx.head == u"none" ? QString() : fx.head;
            start = fx.both ? head : QString();
            end = head;
        }
        const auto drawHead = [&](const QString &kind, bool atStart, bool last) {
            if (kind.isEmpty())
                return;
            const QPainterPath shape = headPath(kind, pts, atStart);
            const bool solidHead = kind == u"arrow" || kind == u"circle" || kind == u"diamond";
            const bool hollowHead = kind == u"triangle" || kind == u"odiamond";
            const bool hollowMark = kind == u"zero-one" || kind == u"zero-many";
            QColor fill = none(), stroke = none();
            if (solidHead) {
                fill = fgA(0.5);
            } else if (hollowHead) {
                fill = chatBg(), stroke = fgA(0.5);
            } else {
                stroke = fgA(0.5);
                if (hollowMark)
                    fill = chatBg();
            }
            if (lit) {
                if (solidHead)
                    fill = t;
                else
                    stroke = t;
            }
            if (cls.has("dg-turn") && last && solidHead)
                fill = fgA(0.46);
            if (cls.has("dg-message") && solidHead)
                fill = fgA(0.62);
            p.save();
            p.setOpacity(p.opacity() * headOpacity);
            if (fill.alpha() > 0)
                p.fillPath(shape, fill);
            if (stroke.alpha() > 0)
                p.strokePath(shape, QPen(stroke, 1.25, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.restore();
        };
        drawHead(end, false, start.isEmpty());
        drawHead(start, true, true);
    }

    void label(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const Classes cls(fx.cls.isEmpty() ? QStringLiteral("dg-text") : fx.cls);
        TextStyle style = textStyle(cls, fx.tone);
        if (fx.size > 0) {
            style.size = fx.size;
            if (fx.weight > 0)
                style.weight = fx.weight;
        }
        // A section's tag shows only while its band is hot.
        if (cls.has("dg-wf-sectag") && !item.hot)
            return;
        p.translate(item.cur.x, item.cur.y);
        if (fx.rotate)
            p.rotate(fx.rotate);
        const double lh = fx.lineHeight > 0 ? fx.lineHeight : 16;
        const QString anchor = fx.anchor.isEmpty() ? Middle : fx.anchor;
        const QString baseline = fx.baseline.isEmpty() ? Central : fx.baseline;
        if (fx.pop) {
            QRectF box = textBox(fx.lines, style, anchor, baseline, lh);
            if (fx.pill)
                box |= QRectF(-fx.w / 2, -fx.h / 2, fx.w, fx.h);
            pop(item.appear, box.center());
        } else {
            fade(item.appear, 4);
        }
        if (fx.pill) {
            QColor fill = none(), stroke = none();
            if (cls.has("dg-edge-label"))
                fill = chatBg();
            if (cls.has("dg-tag"))
                fill = fgA(0.08);
            if (cls.has("dg-tag") && cls.has("is-branch"))
                fill = none(), stroke = alpha(toneColor(fx.tone), 0.75);
            if (cls.has("dg-set-value"))
                fill = fgA(0.07);
            if (cls.has("dg-last-tag"))
                fill = upDown(cls);
            if (cls.has("dg-wf-sectag"))
                fill = toneColor(fx.tone);
            const QPainterPath pill =
                roundRect(-fx.w / 2, -fx.h / 2, fx.w, fx.h, std::isnan(fx.rx) ? 6 : fx.rx);
            if (fill.alpha() > 0)
                p.fillPath(pill, fill);
            if (stroke.alpha() > 0)
                p.strokePath(pill, QPen(stroke, 1));
            addHit(pill);
        }
        // Some numbers let the pointer through to the mark under them.
        const bool answers =
            !(cls.has("dg-value") || cls.has("dg-end") || cls.has("dg-goal-label") ||
              cls.has("dg-cell-text") || cls.has("dg-tile-name") || cls.has("dg-tile-value") ||
              cls.has("dg-acell-text") || cls.has("dg-wf-sectag"));
        text(fx.lines, 0, 0, style, anchor, baseline, lh, answers);
    }

    void line(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const Props &c = item.cur;
        const bool draw = fx.draw != 0;
        const double e = item.appear >= 1 ? 1 : easeOut(clamp01(item.appear));
        const QPointF a(c.x1, c.y1),
            b(draw ? c.x1 + (c.x2 - c.x1) * e : c.x2, draw ? c.y1 + (c.y2 - c.y1) * e : c.y2);
        if (!draw)
            dim(e);
        const Shape s = shape(fx.cls, fx.tone);
        QPainterPath path;
        path.moveTo(a);
        path.lineTo(b);
        if (s.opacity < 1)
            p.setOpacity(p.opacity() * s.opacity);
        if (s.stroke.color.alpha() > 0 && s.stroke.width > 0 &&
            (a != b || s.stroke.cap != Qt::FlatCap))
            p.strokePath(path, penOf(s.stroke));
        hitShape(path, s, false);
    }

    void bar(const Live &item)
    {
        const Props &c = item.cur;
        const double g = item.appear >= 1 ? 1 : easeOut(clamp01(item.appear));
        const QPainterPath path = barPath(c.x, c.base + (c.top - c.base) * g, c.w, c.base,
                                          std::isnan(item.spec.fixed.r) ? 3 : item.spec.fixed.r);
        const Shape s =
            shape(QStringLiteral("dg-bar ") + item.spec.fixed.cls, item.spec.fixed.tone);
        fillShape(path, s);
        hitShape(path, s);
    }

    void area(const Live &item)
    {
        const Pts &pts = item.cur.pts;
        if (pts.size() < 8)
            return;
        QPainterPath path = cubicPath(pts);
        const int n = int(pts.size());
        path.lineTo(pts[n - 2], item.cur.base);
        path.lineTo(pts[0], item.cur.base);
        path.closeSubpath();
        if (item.appear < 1)
            p.setOpacity(p.opacity() * easeOut(clamp01(item.appear)));
        // The tone at .17 fading to nothing over the area's box.
        const QRectF box = path.boundingRect();
        QLinearGradient gradient(box.topLeft(), box.bottomLeft());
        const QColor t = toneColor(item.spec.fixed.tone);
        gradient.setColorAt(0, alpha(t, 0.17));
        gradient.setColorAt(1, alpha(t, 0));
        p.fillPath(path, gradient);
    }

    void arc(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const double start = item.cur.start, sweep = item.cur.sweep;
        const double length =
            std::max(0.001, sweep * (item.appear >= 1 ? 1 : clamp01(item.appear)) - fx.gap);
        const Shape s = shapeStyle(Classes(QStringLiteral("dg-slice ") + fx.cls), fx.tone, item.hot,
                                   look.probing);
        const double width = fx.active ? 16 : fx.width;
        const double r = std::isnan(fx.r) ? 0 : fx.r;
        if (r <= 0 || width <= 0)
            return;
        // A circle drawn from the top, clockwise, with a path length of 100.
        QPainterPath path;
        const QRectF box(fx.cx - r, fx.cy - r, 2 * r, 2 * r);
        const double from = 90 - start * 3.6, span = -std::min(100.0, length) * 3.6;
        path.arcMoveTo(box, from);
        path.arcTo(box, from, span);
        p.strokePath(path, QPen(s.stroke.color, width, Qt::SolidLine, Qt::FlatCap));
        if (hits) {
            QPainterPathStroker stroker;
            stroker.setWidth(width);
            stroker.setCapStyle(Qt::FlatCap);
            addHit(stroker.createStroke(path));
        }
    }

    void legend(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        p.translate(item.cur.x, item.cur.y);
        if (item.appear < 1) {
            fade(item.appear, 0);
            p.translate((1 - easeOut(clamp01(item.appear))) * -6, 0);
        }
        const QPainterPath bg = roundRect(-8, -PIE.row / 2 + 1, fx.w + 16, PIE.row - 2, 7);
        if (fx.active)
            p.fillPath(bg, fgA(0.05));
        addHit(bg);
        p.fillPath(roundRect(0, -PIE.swatch / 2, PIE.swatch, PIE.swatch, 2.5), toneColor(fx.tone));
        textAs({fx.label}, PIE.swatch + 10, 0, "dg-legend-label", fx.tone, Start);
        textAs({fx.value}, fx.valueX, 0, "dg-legend-value", fx.tone, End);
        if (!fx.share.isEmpty())
            textAs({fx.share}, fx.w, 0, "dg-legend-share", fx.tone, End);
    }

    void dot(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const Classes cls(fx.cls);
        double r = item.cur.r * (item.appear >= 1 ? 1 : easeOut(clamp01(item.appear)));
        // A point grows under the pointer.
        if (item.hot && cls.has("dg-point"))
            r = 5;
        if (item.hot && cls.has("dg-q-point"))
            r = 6;
        if (r <= 0)
            return;
        const QPainterPath path = circlePath(QPointF(item.cur.x, item.cur.y), r);
        const Shape s = shape(fx.cls, fx.tone);
        fillShape(path, s);
        hitShape(path, s);
    }

    void badge(const Live &item)
    {
        p.translate(item.cur.x, item.cur.y);
        pop(item.appear, QPointF(0, 0));
        const QPainterPath ring = circlePath({}, 7.5);
        p.fillPath(ring, chatBg());
        p.strokePath(ring, QPen(fgA(0.3), 1));
        addHit(ring);
        textAs({item.spec.fixed.n}, 0, 0, "dg-badge", item.spec.fixed.tone, Middle);
    }

    void note(const Live &item)
    {
        const Props &c = item.cur;
        fade(item.appear, 0);
        const QPainterPath box = roundRect(c.x, c.y, c.w, c.h, 9);
        p.fillPath(box, fgA(0.04));
        addHit(box);
        textAs(item.spec.fixed.lines, c.x + c.w / 2, c.y + c.h / 2, "dg-note", NoTone, Middle);
    }

    void frame(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const Props &c = item.cur;
        fade(item.appear, 0);
        p.strokePath(roundRect(c.x, c.y, c.w, c.h, 8), QPen(fgA(0.14), 1));
        // Its kind in small capitals, what it is about beside it.
        const TextStyle kindStyle = textStyle(Classes(QStringLiteral("dg-frame-kind")), NoTone);
        text({fx.kind}, c.x + 10, c.y + 13, kindStyle, Start, Central, 16);
        if (!fx.label.isEmpty()) {
            const double w = faceOf(kindStyle).advance(fx.kind);
            textAs({fx.label}, c.x + 10 + w + 7, c.y + 13, "dg-frame-label", NoTone, Start);
        }
    }

    void cluster(const Live &item)
    {
        const Props &c = item.cur;
        fade(item.appear, 6);
        const QPainterPath box = roundRect(c.x, c.y, c.w, c.h, CLUSTER.radius);
        p.fillPath(box, fgA(item.spec.fixed.depth ? 0.022 : 0.018));
        p.strokePath(box, QPen(fgA(0.09), 1));
    }

    void clusterTitle(const Live &item)
    {
        fade(item.appear, 6);
        textAs({item.spec.fixed.title}, item.cur.x + CLUSTER.padX,
               item.cur.y + CLUSTER.head / 2 + 1, "dg-cluster-title", NoTone, Start);
    }

    void candle(const Live &item)
    {
        const Props &c = item.cur;
        const double g = item.appear >= 1 ? 1 : easeOut(clamp01(item.appear)),
                     mid = (c.o + c.c) / 2;
        const double size = std::max(1.0, std::abs(c.c - c.o) * g);
        if (item.appear < 1)
            p.setOpacity(p.opacity() * clamp01(item.appear * 2.5));
        const Classes cls(QStringLiteral("dg-candle ") + item.spec.fixed.cls);
        if (look.probing && !item.hot)
            p.setOpacity(p.opacity() * 0.35);
        const QColor color = upDown(cls);
        p.setPen(QPen(color, 1));
        p.drawLine(QPointF(c.x, mid + (c.hi - mid) * g), QPointF(c.x, mid + (c.lo - mid) * g));
        const QPainterPath body = roundRect(c.x - c.w / 2, mid - size / 2, c.w, size, 1.5);
        p.fillPath(body, color);
        QPainterPath area = body;
        area.addRect(QRectF(c.x - 2, std::min(c.hi, c.lo), 4, std::abs(c.lo - c.hi)));
        addHit(area);
    }

    void column(const Live &item)
    {
        const Props &c = item.cur;
        const double g = item.appear >= 1 ? 1 : easeOut(clamp01(item.appear));
        const QPainterPath path = barPath(c.x, c.base + (c.top - c.base) * g, c.w, c.base,
                                          std::isnan(item.spec.fixed.r) ? 6 : item.spec.fixed.r);
        const Shape s = shape(item.spec.fixed.cls, item.spec.fixed.tone);
        fillShape(path, s);
        hitShape(path, s);
    }

    void span(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const Props &c = item.cur;
        Classes cls(fx.cls);
        if (fx.milestone)
            cls.add(QStringLiteral("is-milestone"));
        const Shape s = shapeStyle(cls, fx.tone, item.hot, look.probing);
        if (fx.milestone) {
            const double r = c.h / 2 * (item.appear >= 1 ? 1 : easeOut(clamp01(item.appear)));
            QPainterPath path;
            path.moveTo(c.x, c.y - r);
            path.lineTo(c.x + r, c.y);
            path.lineTo(c.x, c.y + r);
            path.lineTo(c.x - r, c.y);
            path.closeSubpath();
            fillShape(path, s);
            hitShape(path, s);
            return;
        }
        // A bar grows from where it starts; one that runs back from a zero
        // line from its far end, and one that is centred from its middle.
        const double width = std::max(std::min(c.w, c.h * 0.2),
                                      c.w * (item.appear >= 1 ? 1 : easeOut(clamp01(item.appear))));
        const double x = fx.back ? c.x + c.w - width : fx.mid ? c.x + (c.w - width) / 2 : c.x;
        const double rx = std::min(std::isnan(fx.rx) ? c.h / 2 : fx.rx, width / 2);
        if (item.appear < 1)
            p.setOpacity(p.opacity() * clamp01(item.appear * 3));
        const QPainterPath path = roundRect(x, c.y - c.h / 2, width, c.h, rx);
        fillShape(path, s);
        hitShape(path, s);
    }

    void rect(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const Props &c = item.cur;
        if (item.appear < 1)
            p.setOpacity(p.opacity() * easeOut(clamp01(item.appear)));
        const QPainterPath path = roundRect(c.x, c.y, c.w, c.h, std::isnan(fx.rx) ? 12 : fx.rx);
        const Shape s = shape(fx.cls, fx.tone);
        fillShape(path, s);
        hitShape(path, s);
    }

    void poly(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const Pts &vs = item.cur.vs;
        if (vs.size() < 4)
            return;
        const double g = item.appear >= 1 ? 1 : easeOut(clamp01(item.appear));
        QPainterPath path;
        for (int i = 0; i + 1 < vs.size(); i += 2) {
            const QPointF at(fx.cx + (vs[i] - fx.cx) * g, fx.cy + (vs[i + 1] - fx.cy) * g);
            if (i)
                path.lineTo(at);
            else
                path.moveTo(at);
        }
        path.closeSubpath();
        if (item.appear < 1)
            p.setOpacity(p.opacity() * clamp01(item.appear * 2));
        const Shape s = shape(fx.cls, fx.tone);
        fillShape(path, s);
        hitShape(path, s);
    }

    void chip(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        p.translate(item.cur.x, item.cur.y);
        fade(item.appear, 0);
        const QColor t = toneColor(fx.tone);
        // The mark repeats how the series is drawn.
        if (fx.mark == u"line")
            p.fillPath(roundRect(0, -1, 12, 2, 1), t);
        else if (fx.mark == u"bar")
            p.fillPath(roundRect(2, -4, 8, 8, 2), t);
        else
            p.fillPath(roundRect(2.5, -3.5, 7, 7, 3.5), t);
        textAs({fx.text}, 18, 0, "dg-chip", fx.tone, Start);
    }

    void hit(const Live &item)
    {
        const Props &c = item.cur;
        QPainterPath area;
        area.addRect(QRectF(c.x, c.y, std::max(0.0, c.w), std::max(0.0, c.h)));
        addHit(area);
    }

    void figure(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        p.translate(item.cur.x, item.cur.y);
        fade(item.appear, 4);
        TextStyle num;
        num.fill = ink(0);
        num.weight = 650;
        num.size = fx.size > 0 ? fx.size : 24;
        const Face &face = faceOf(num);
        const double w = face.advance(fx.figure);
        drawLine(fx.figure, QPointF(0, 0), face, num, w, true);
        if (!fx.unit.isEmpty()) {
            const TextStyle unit = textStyle(Classes(QStringLiteral("dg-figure-unit")), fx.tone);
            const Face &uf = faceOf(unit);
            drawLine(fx.unit, QPointF(w + 5, 0), uf, unit, uf.advance(fx.unit), true);
        }
    }

    void ribbon(const Live &item)
    {
        const Pts &band = item.cur.band;
        if (band.size() < 6)
            return;
        const double x0 = band[0], a0 = band[1], a1 = band[2], x1 = band[3], b0 = band[4],
                     b1 = band[5], m = (x0 + x1) / 2;
        QPainterPath path;
        path.moveTo(x0, a0);
        path.cubicTo(m, a0, m, b0, x1, b0);
        path.lineTo(x1, b1);
        path.cubicTo(m, b1, m, a1, x0, a1);
        path.closeSubpath();
        if (item.appear < 1)
            p.setOpacity(p.opacity() * easeOut(clamp01(item.appear)));
        const Shape s = shape(QStringLiteral("dg-ribbon"), item.spec.fixed.tone);
        fillShape(path, s);
        hitShape(path, s);
    }

    void wbox(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const Props &c = item.cur;
        fade(item.appear, 6);
        const QPainterPath path = roundRect(c.x, c.y, std::max(0.0, c.w), std::max(0.0, c.h),
                                            std::isnan(fx.rx) ? 12 : fx.rx);
        const Shape s = shape(fx.cls, fx.tone);
        fillShape(path, s);
        hitShape(path, s);
    }

    void wglyph(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const auto found = glyphs().constFind(fx.icon);
        if (found == glyphs().constEnd())
            return;
        p.translate(item.cur.x, item.cur.y);
        const double scale = item.cur.s / 24;
        p.scale(scale, scale);
        fade(item.appear, 3);
        const Classes cls(QStringLiteral("dg-wf-glyph ") + fx.cls);
        const QColor t = toneColor(fx.tone);
        Stroke stroke;
        stroke.color = fgA(0.55);
        stroke.width = 1.5;
        stroke.cap = Qt::RoundCap;
        stroke.join = Qt::RoundJoin;
        if (cls.has("is-done"))
            stroke.color = t, stroke.width = 2;
        if (cls.has("is-warn"))
            stroke.color = pal().warn, stroke.width = 1.7;
        if (cls.has("dg-check-icon")) {
            stroke.width = 2;
            if (cls.has("is-good"))
                stroke.color = pal().success;
            if (cls.has("is-warn"))
                stroke.color = pal().warn;
            if (cls.has("is-bad"))
                stroke.color = pal().danger;
            if (cls.has("is-ask"))
                stroke.color = ink(1);
        }
        if (cls.has("is-sheet"))
            stroke.color = t, stroke.width = 1.5;
        if (cls.has("is-media"))
            stroke.color = fgA(0.24), stroke.width = 1.4;
        if (cls.has("is-icon"))
            stroke.color = fgA(pal().primary), stroke.width = 1.6;
        if (cls.has("is-check"))
            stroke.color = fgA(pal().secondary), stroke.width = 1.8;
        if (cls.has("is-lock"))
            stroke.color = fgA(0.4), stroke.width = 2;
        p.strokePath(*found, penOf(stroke));
    }

    void fcard(const Live &item)
    {
        const Props &c = item.cur;
        if (item.appear < 1)
            p.setOpacity(p.opacity() * easeOut(clamp01(item.appear)));
        const QPainterPath path = roundRect(c.x, c.y, std::max(0.0, c.w), std::max(0.0, c.h), 16);
        p.fillPath(path, pal().composerBg);
        p.strokePath(path, QPen(pal().composerBorder, 1));
    }

    // fvIcon: a file's kind icon, or the sidebar's outline folder.
    void fileIcon(const QString &name, bool folder, double x, double y, double h)
    {
        if (folder) {
            p.save();
            p.translate(x, y);
            p.scale(h / 60, h / 60);
            p.translate(-30, -30);
            p.strokePath(folderGlyph(), QPen(fgA(pal().secondary), 5.5, Qt::SolidLine, Qt::RoundCap,
                                             Qt::RoundJoin));
            p.restore();
            return;
        }
        // Each kind's icon is drawn once at the device's size and reused.
        const filekinds::Described kind = filekinds::describe(name);
        const QTransform &t = p.worldTransform();
        const double dpr = p.device() ? p.device()->devicePixelRatio() : 1;
        const double k = std::sqrt(std::abs(t.determinant())) * dpr;
        const QSize size(std::max(1, int(std::ceil(h * 0.8 * k))),
                         std::max(1, int(std::ceil(h * k))));
        const QString key = QStringLiteral("icon\x1f%1\x1f%2\x1f%3\x1f%4x%5\x1f%6")
                                .arg(kind.glyph, kind.label, kind.tone.name())
                                .arg(size.width())
                                .arg(size.height())
                                .arg(pal().light);
        auto found = fonts.icons.constFind(key);
        if (found == fonts.icons.constEnd()) {
            if (fonts.icons.size() > 256)
                fonts.icons.clear();
            QImage sprite(size, QImage::Format_ARGB32_Premultiplied);
            sprite.fill(Qt::transparent);
            QPainter painter(&sprite);
            painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
            QFont font = fonts.face(10, 800).font;
            font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
            filekinds::paintIcon(painter, QRectF(0, 0, size.width(), size.height()), kind,
                                 pal().light, font);
            painter.end();
            found = fonts.icons.insert(key, sprite);
        }
        p.drawImage(QRectF(x, y, h * 0.8, h), *found);
    }

    void fhead(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        p.translate(item.cur.x, item.cur.y);
        fade(item.appear, 3);
        const double cy = 42.0 / 2 - 3;
        fileIcon({}, true, fx.left, cy - 9, 18);
        textAs({fx.name}, fx.left + 28, cy, "dg-fv-title", NoTone, Start);
        if (!fx.path.isEmpty())
            textAs({fx.path}, fx.left + 28 + fx.nameW + 10, cy, "dg-fv-path", NoTone, Start);
        if (!fx.counts.isEmpty())
            textAs({fx.counts}, fx.right, cy, "dg-fv-sum", NoTone, End);
        if (!fx.sizeText.isEmpty())
            textAs({fx.sizeText},
                   fx.counts.isEmpty() ? fx.right
                                       : fx.right - fonts.textWidth(fx.counts, 12.5, 500) - 16,
                   cy, "dg-fv-total", NoTone, End);
    }

    void fseg(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        const double w =
            std::max(0.0, item.cur.w * (item.appear >= 1 ? 1 : easeOut(clamp01(item.appear))));
        const QPainterPath path = roundRect(item.cur.x, fx.y, w, fx.h, fx.h / 2);
        const QColor color = pal().light ? towardBlack(fx.color, 0.85) : fx.color;
        // Under the pointer one kind stands out and the rest step back.
        p.setOpacity(p.opacity() * (look.segHot && !item.hot ? 0.3 : 0.85));
        p.fillPath(path, color);
        addHit(path);
    }

    void frow(const Live &item)
    {
        const Fixed &fx = item.spec.fixed;
        p.translate(item.cur.x, item.cur.y);
        fade(item.appear, 4);
        const double cy = fx.h / 2;
        QPainterPath plate;
        plate.addRect(QRectF(fx.plateX, 0, fx.plateW, fx.h));
        addHit(plate);
        if (fx.folder)
            fileIcon(fx.name, true, fx.iconX, cy - 8, 16);
        else
            fileIcon(fx.name, false, fx.iconX + 1, cy - 10, 20);
        textAs({fx.label}, fx.textX, cy, "dg-fv-name", NoTone, Start);
        if (!fx.note.isEmpty())
            textAs({fx.note}, fx.textX + fx.labelW + 12, cy, "dg-fv-note", NoTone, Start);
        if (!fx.when.isEmpty())
            textAs({fx.when}, fx.dateX, cy, "dg-fv-date", NoTone, End);
        if (!fx.amount.isEmpty())
            textAs({fx.amount}, fx.sizeX, cy, fx.folder ? "dg-fv-count" : "dg-fv-size", NoTone,
                   End);
        if (fx.rule) {
            p.setPen(QPen(fgA(0.06), 1));
            p.drawLine(QPointF(fx.rule, fx.h - 0.5), QPointF(fx.right, fx.h - 0.5));
        }
    }

    QPainter &p;
    Ctx &fonts;
    const Look &look;
    QVector<QRectF> *texts;
    QVector<Hit> *hits;
    const Live *current = nullptr;
    bool rowsAnswer = false;
};
} // namespace

void paintItems(QPainter &painter, Ctx &fonts, const QVector<const Live *> &items, const Look &look,
                const QString &kind, QVector<QRectF> *texts, QVector<Hit> *hits)
{
    Painter paint(painter, fonts, look, kind, texts, hits);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    for (int layer = 0; layer < Layers; ++layer) {
        if (fonts.cancelled())
            return;
        // A files card lies under everything of the back layer.
        if (layer == Back) {
            for (const Live *item : items) {
                if (item->spec.type == Type::FCard)
                    paint.paint(*item, layer);
            }
        }
        for (const Live *item : items) {
            if (layer == Back && item->spec.type == Type::FCard)
                continue;
            paint.paint(*item, layer);
        }
        // The rows' shared highlight glides in the back layer, over its rows.
        if (layer == Back && look.glideOpacity > 0.001 && !look.glide.isEmpty()) {
            painter.save();
            painter.setOpacity(clamp01(look.glideOpacity));
            painter.fillPath(roundRect(look.glide.x(), look.glide.y(), look.glide.width(),
                                       look.glide.height(), 9),
                             fgA(0.05));
            painter.restore();
        }
    }
}
} // namespace diagram
