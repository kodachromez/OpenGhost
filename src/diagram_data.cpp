#include "diagram_engine.h"

#include <QDate>
#include <QLocale>

#include <algorithm>
#include <numeric>

// Data kinds: heatmap/calendar, scatter, treemap, gitGraph, array/cells,
// bracket and nutrition (diagram.js parseHeat … nutritionScene).
namespace diagram
{
namespace
{
/* Small JS idioms */

// Number#toString, for keys and texts made of numbers.
QString jsNum(double v)
{
    if (v != v)
        return QStringLiteral("NaN");
    if (v == 0)
        return QStringLiteral("0");
    if (!finite(v))
        return v > 0 ? QStringLiteral("Infinity") : QStringLiteral("-Infinity");
    const double a = std::abs(v);
    if (a >= 1e-7 && a < 1e21)
        return QString::number(v, 'f', QLocale::FloatingPointShortest);
    // 1e+21, 1.5e-7: no zeros before the exponent's digits, its sign always.
    const QString s = QString::number(v, 'e', QLocale::FloatingPointShortest);
    const qsizetype e = s.indexOf(QLatin1Char('e'));
    if (e < 0)
        return s;
    QString exponent = s.mid(e + 1);
    QChar sign = QLatin1Char('+');
    if (exponent.startsWith(QLatin1Char('-')) || exponent.startsWith(QLatin1Char('+'))) {
        sign = exponent.at(0);
        exponent = exponent.mid(1);
    }
    while (exponent.size() > 1 && exponent.startsWith(QLatin1Char('0')))
        exponent = exponent.mid(1);
    return s.left(e) + QLatin1Char('e') + sign + exponent;
}

// Math.round: a half goes up.
double jsRound(double v)
{
    const double f = std::floor(v);
    return v - f >= 0.5 ? f + 1 : f;
}

// +v.toFixed(10).
double toFixed10(double v)
{
    return std::abs(v) < 1e21 ? QString::number(v, 'f', 10).toDouble() : v;
}

// Math.max(floor, ...values), Math.min(...values): NaN wins.
double maxOf(const QVector<double> &values, double floor = -INFINITY)
{
    double m = floor;
    for (const double v : values) {
        if (v != v)
            return NaN;
        m = std::max(m, v);
    }
    return m;
}

double minOf(const QVector<double> &values)
{
    double m = INFINITY;
    for (const double v : values) {
        if (v != v)
            return NaN;
        m = std::min(m, v);
    }
    return m;
}

TipRow tipRow(const QString &name, const QString &value)
{
    TipRow row;
    row.name = name;
    row.value = value;
    return row;
}

Tip silentTip(const QStringList &hot = {})
{
    Tip tip;
    tip.silent = true;
    tip.hot = hot;
    return tip;
}

Spec boxSpec(Type type, const QString &key, double order, int layer, double x, double y, double w,
             double h)
{
    Spec spec = item(type, key, order, layer);
    spec.props.x = x;
    spec.props.y = y;
    spec.props.w = w;
    spec.props.h = h;
    return spec;
}

Spec rectSpec(const QString &key, double order, int layer, double x, double y, double w, double h,
              const QString &cls, int tone, double rx)
{
    Spec spec = boxSpec(Type::Rect, key, order, layer, x, y, w, h);
    spec.fixed.cls = cls;
    spec.fixed.tone = tone;
    spec.fixed.rx = rx;
    return spec;
}

Spec hitSpec(const QString &key, double order, double x, double y, double w, double h)
{
    return boxSpec(Type::Hit, key, order, Back, x, y, w, h);
}

Spec dotSpec(const QString &key, double order, double x, double y, double r, const QString &cls,
             int tone)
{
    Spec spec = item(Type::Dot, key, order, Nodes);
    spec.props.x = x;
    spec.props.y = y;
    spec.props.r = r;
    spec.fixed.cls = cls;
    spec.fixed.tone = tone;
    return spec;
}

void addTitle(Ctx &c, QVector<Spec> &items, std::optional<Corner> &corner, const QString &text,
              double max)
{
    const Title title = titleOf(c, text, max);
    items << title.item;
    corner = Corner{title.width + 16, 28, 0};
}

// The scene as upstream's view holds it: one item per key, the last spec
// given for a key in the first one's place (DiagramView.set); and nothing
// that is not finite, which upstream would draw as a broken shape.
std::optional<Result> finish(Ctx &c, std::optional<Result> scene)
{
    if (!scene || !c.error.isEmpty())
        return std::nullopt;
    Result r = std::move(*scene);
    QHash<QString, int> at;
    QVector<Spec> items;
    items.reserve(r.items.size());
    for (auto &spec : r.items) {
        const auto found = at.constFind(spec.key);
        if (found != at.constEnd()) {
            items[*found] = std::move(spec);
        } else {
            at.insert(spec.key, int(items.size()));
            items << std::move(spec);
        }
    }
    r.items = std::move(items);
    const auto bad = [&] {
        c.fail(QStringLiteral("This diagram could not be laid out."));
        return std::nullopt;
    };
    if (!finite(r.width) || !finite(r.height))
        return bad();
    for (const auto &spec : r.items) {
        if (!finite(spec.order))
            return bad();
        for (auto member : PropScalars) {
            if (!finite(spec.props.*member))
                return bad();
        }
        for (const double v : spec.props.pts) {
            if (!finite(v))
                return bad();
        }
    }
    for (const auto &point : r.near) {
        if (!finite(point.x) || !finite(point.y))
            return bad();
    }
    return r;
}

/* Dates, as Intl.DateTimeFormat writes them in English (UTC) */

QDate dateOf(double t) { return QDate::fromJulianDay(qint64(std::floor(t / DAY)) + 2440588); }
// getUTCDay: 0 for Sunday.
int weekdayOf(double t) { return dateOf(t).dayOfWeek() % 7; }

const char *const MonthNames[] = {"January",   "February", "March",    "April",
                                  "May",       "June",     "July",     "August",
                                  "September", "October",  "November", "December"};
const char *const DayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// { weekday: 'short' }: Mon.
QString shortWeekday(double t) { return QString::fromLatin1(DayNames[weekdayOf(t)]); }
// { month: 'short' }: Sep.
QString shortMonth(double t)
{
    return QString::fromLatin1(MonthNames[dateOf(t).month() - 1]).left(3);
}
// { day: 'numeric', month: 'long', year: 'numeric' }: September 1, 2026.
QString fullDate(double t)
{
    const QDate d = dateOf(t);
    return QStringLiteral("%1 %2, %3")
        .arg(QString::fromLatin1(MonthNames[d.month() - 1]))
        .arg(d.day())
        .arg(d.year());
}

const Re &titleLine()
{
    static const Re re(QStringLiteral("^title(?:\\s*:\\s*|\\s+)(.+)$"), I);
    return re;
}

/* Tables of marks */

// A value for every day, drawn as a calendar, or a table of rows and columns
// whose cells are numbers or marks. Days are lines of `2026-09-01 | 45`; a
// table starts with `cols A, B, C` and goes on with `Row | 1 | 0 | 3`.
const Re &markOn()
{
    static const Re re(QString::fromUtf8("^(?:x|х|✓|✔|✅|☑|v|yes|y|да|true|on|\\+|●|•|★)$"), I);
    return re;
}
const Re &markOff()
{
    static const Re re(QString::fromUtf8("^(?:-|–|—|−|✗|✘|✕|×|❌|☐|○|no|n|нет|false|off|·|)$"), I);
    return re;
}
// An emoji is often followed by a sign that asks for its coloured form; a
// mark is told without it.
constexpr QChar EmojiForm(0xfe0f);

struct HeatDay {
    double t = 0, value = 0;
};
struct HeatRow {
    QString label;
    QStringList cells;
};
struct HeatData {
    QString title, unit;
    std::optional<QStringList> cols;
    QVector<HeatRow> rows;
    QVector<HeatDay> days;
};

std::optional<HeatData> parseHeat(const QStringList &lines)
{
    static const Re unit(QStringLiteral("^unit(?:\\s*:\\s*|\\s+)(.+)$"), I),
        cols(QStringLiteral("^(?:cols|columns)(?:\\s*:\\s*|\\s+)(.+)$"), I);
    HeatData data;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = bare(lines.at(i));
        QRegularExpressionMatch m;
        if (!line.contains(QLatin1Char('|'))) {
            if ((m = titleLine().match(line)).hasMatch()) {
                data.title = unquote(m.captured(1));
            } else if ((m = unit.match(line)).hasMatch()) {
                data.unit = unquote(m.captured(1));
            } else if ((m = cols.match(line)).hasMatch()) {
                QStringList names;
                for (const auto &name : splitList(m.captured(1)))
                    names << unquote(name);
                data.cols = names;
            }
            continue;
        }
        const QStringList cells = cellsOf(line);
        // parseDate's null is NaN here.
        const double day = data.cols ? NaN : parseDate(cells.value(0));
        if (finite(day)) {
            const auto a = amount(cells.value(1));
            data.days << HeatDay{std::floor(day / DAY) * DAY, a ? std::max(0.0, a->value) : 0};
        } else if (!cells.value(0).isEmpty()) {
            HeatRow row;
            row.label = unquote(cells.at(0));
            for (int k = 1; k < cells.size(); ++k)
                row.cells << unquote(cells.at(k)).remove(EmojiForm);
            data.rows << row;
        }
    }
    if (data.days.size() > 1) {
        data.cols.reset();
        return data;
    }
    if (data.rows.isEmpty())
        return std::nullopt;
    int count = 0;
    for (const auto &row : data.rows)
        count = std::max(count, int(row.cells.size()));
    QStringList names;
    for (int i = 0; i < count; ++i)
        names << (data.cols ? data.cols->value(i) : QString());
    data.cols = names;
    return data;
}

std::optional<Result> heatScene(Ctx &c, const HeatData &data)
{
    const double W = clamp(c.width, 280, 720);
    const int accent = first(c.tones);
    Result r;
    auto &items = r.items;
    auto &tips = r.tips;
    double top = 0;
    if (!data.title.isEmpty()) {
        addTitle(c, items, r.corner, data.title, W - 80);
        top = CHART.head;
    }
    // How strong a cell is: nothing, then four steps of one colour up to the
    // largest value.
    const auto levelOf = [](double value, double most) {
        return value > 0 ? int(clamp(std::ceil(value / most * HEAT.levels), 1, HEAT.levels)) : 0;
    };
    const auto cellCls = [](const QString &extra, int level) {
        return QStringLiteral("dg-cell%1 is-l%2").arg(extra).arg(level);
    };
    if (!data.cols) {
        std::map<double, double> days; // Sorted: its keys are upstream's `times`.
        for (const auto &day : data.days)
            days[day.t] = day.value;
        const double firstT = days.begin()->first, end = days.rbegin()->first;
        const auto monday = [](double t) { return t - ((weekdayOf(t) + 6) % 7) * DAY; };
        const double start = monday(firstT);
        const double weeks = jsRound((monday(end) - start) / (7 * DAY)) + 1;
        // A calendar this long has more cells than a drawing may hold.
        if (!finite(weeks) || 7 * (weeks - 2) > MaxItems)
            return c.fail(QStringLiteral("This diagram is too large to draw.")), std::nullopt;
        double most = 0;
        for (const auto &day : days)
            most = std::max(most, day.second);
        if (!most)
            most = 1;
        const double gutter = 30,
                     cell = clamp(std::floor((W - gutter) / weeks) - HEAT.gap, 8, HEAT.cell),
                     pitch = cell + HEAT.gap;
        const double gridTop = top + 20;
        for (const int row : {0, 2, 4})
            items << labelSpec(QStringLiteral("hw:%1").arg(row), 0.1, 0,
                               gridTop + row * pitch + cell / 2, {shortWeekday(start + row * DAY)},
                               QStringLiteral("dg-tick"), QStringLiteral("start"), Back);
        double named = -INFINITY;
        for (int w = 0; w < int(weeks); ++w) {
            if (!c.budget(7))
                return std::nullopt;
            const double firstDay = start + w * 7 * DAY, x = gutter + w * pitch;
            // A month is named over the week it begins in, when there is room
            // since the last name.
            double turn = NaN;
            for (int d = 0; d < 7; ++d) {
                if (dateOf(firstDay + d * DAY).day() == 1) {
                    turn = firstDay + d * DAY;
                    break;
                }
            }
            if (turn != turn && w == 0)
                turn = firstDay;
            if (turn == turn && turn <= end && x - named >= 30) {
                items << labelSpec(QStringLiteral("hm:%1").arg(w), 0.1, x, top + 8,
                                   {shortMonth(turn)}, QStringLiteral("dg-tick"),
                                   QStringLiteral("start"), Back);
                named = x;
            }
            for (int d = 0; d < 7; ++d) {
                const double t = firstDay + d * DAY;
                if (t < firstT || t > end)
                    continue;
                const auto found = days.find(t);
                const double value = found != days.end() ? found->second : 0;
                const QString key = QStringLiteral("hd:") + jsNum(t);
                items << rectSpec(key, 0.3 + w * 0.06 + d * 0.01, Nodes, x, gridTop + d * pitch,
                                  cell, cell, cellCls(QString(), levelOf(value, most)), accent, 3);
                Tip tip;
                tip.title = fullDate(t);
                tip.rows << tipRow(data.unit.isEmpty() ? QStringLiteral("Value") : data.unit,
                                   format(value));
                tips.insert(key, tip);
            }
        }
        const double bottom = gridTop + 7 * pitch - HEAT.gap;
        const QString less = QStringLiteral("Less"), more = QStringLiteral("More");
        const double scaleW = c.widthOf(less, FONT::tick) + 8 + (HEAT.levels + 1) * (10 + 3) + 5 +
                              c.widthOf(more, FONT::tick),
                     gridW = gutter + weeks * pitch - HEAT.gap;
        double lx = std::max(gutter, gridW - scaleW);
        items << labelSpec(QStringLiteral("hs:less"), 1, lx, bottom + 18, {less},
                           QStringLiteral("dg-tick"), QStringLiteral("start"), Back);
        lx += c.widthOf(less, FONT::tick) + 8;
        for (int level = 0; level <= HEAT.levels; ++level) {
            items << rectSpec(QStringLiteral("hs:%1").arg(level), 1 + level * 0.05, Nodes, lx,
                              bottom + 13, 10, 10, cellCls(QString(), level), accent, 2.5);
            lx += 13;
        }
        items << labelSpec(QStringLiteral("hs:more"), 1.3, lx + 5, bottom + 18, {more},
                           QStringLiteral("dg-tick"), QStringLiteral("start"), Back);
        r.kind = QStringLiteral("heatmap");
        r.width = std::max(gridW, gutter + scaleW);
        r.height = bottom + 28;
        r.flush = true;
        return r;
    }
    const QStringList &cols = *data.cols;
    const int n = int(cols.size());
    if (!n)
        return std::nullopt;
    bool marks = true;
    QVector<double> numbers;
    for (const auto &row : data.rows) {
        for (const auto &cell : row.cells) {
            if (!markOn().match(cell).hasMatch() && !markOff().match(cell).hasMatch())
                marks = false;
            if (const auto a = amount(cell))
                numbers << a->value;
        }
    }
    const double most = maxOf(numbers, 1e-9);
    QVector<double> labelWidths;
    for (const auto &row : data.rows)
        labelWidths << c.widthOf(row.label, FONT::row);
    const double labelW = std::min({W * 0.4, 240.0, maxOf(labelWidths)}) + 14;
    const double cellW = clamp((W - labelW) / n, 30, 88), gridW = labelW + cellW * n;
    const bool named =
        std::any_of(cols.begin(), cols.end(), [](const QString &s) { return !s.isEmpty(); });
    if (named) {
        for (int k = 0; k < n; ++k)
            items << labelSpec(QStringLiteral("hc:%1").arg(k), 0.1 + k * 0.03,
                               labelW + cellW * (k + 0.5), top + 8,
                               {truncate(c, cols.at(k), cellW - 6, FONT::tick)},
                               QStringLiteral("dg-tick"), QStringLiteral("middle"), Back);
    }
    double y = top + (named ? 22 : 2);
    for (int ri = 0; ri < data.rows.size(); ++ri) {
        if (!c.budget(n))
            return std::nullopt;
        const HeatRow &row = data.rows.at(ri);
        const double cy = y + HEAT.row / 2, order = 0.3 + ri * 0.15;
        const QString key = QStringLiteral("hr:%1").arg(ri);
        items << hitSpec(key, order, -8, y, gridW + 16, HEAT.row);
        tips.insert(key, silentTip());
        if (ri)
            items << lineSpec(key + QStringLiteral(":rule"), order, 0, y, gridW, y,
                              QStringLiteral("dg-grid"), false, Back);
        items << labelSpec(key + QStringLiteral(":name"), order, 0, cy,
                           {truncate(c, row.label, labelW - 14, FONT::row)},
                           QStringLiteral("dg-row-name"), QStringLiteral("start"));
        tips.insert(key + QStringLiteral(":name"), silentTip({key}));
        for (int k = 0; k < n; ++k) {
            tips.insert(QStringLiteral("%1:%2").arg(key).arg(k), silentTip({key}));
            tips.insert(QStringLiteral("%1:%2:t").arg(key).arg(k), silentTip({key}));
        }
        for (int k = 0; k < n; ++k) {
            const QString cell = row.cells.value(k);
            const double cx = labelW + cellW * (k + 0.5);
            const QString cellKey = QStringLiteral("%1:%2").arg(key).arg(k);
            if (marks) {
                // A yes is a dot, a no a short dash: a schedule reads at a glance.
                if (markOn().match(cell).hasMatch())
                    items << dotSpec(cellKey, order + 0.05 + k * 0.02, cx, cy, 4,
                                     QStringLiteral("dg-cell-dot"), accent);
                else
                    items << lineSpec(cellKey, order + 0.05, cx - 3, cy, cx + 3, cy,
                                      QStringLiteral("dg-cell-off"), false, Back);
                continue;
            }
            if (const auto value = amount(cell))
                items << rectSpec(cellKey, order + 0.05 + k * 0.02, Nodes, cx - cellW / 2 + 1.5,
                                  y + 2.5, cellW - 3, HEAT.row - 5,
                                  cellCls(QStringLiteral(" is-soft"), levelOf(value->value, most)),
                                  accent, 4);
            if (!cell.isEmpty())
                items << labelSpec(cellKey + QStringLiteral(":t"), order + 0.1 + k * 0.02, cx, cy,
                                   {truncate(c, cell, cellW - 8, FONT::value)},
                                   QStringLiteral("dg-cell-text"));
        }
        y += HEAT.row;
    }
    r.kind = QStringLiteral("heatmap");
    r.width = gridW;
    r.height = y;
    r.flush = true;
    return r;
}

/* Scatter */

// Things placed by two numbers: risk against return, price against mileage.
// A row is a name, x, y and, if wanted, a size and a group; `x-axis` and
// `y-axis` name the axes, `trend` draws the line the points lean to.
struct ScatterPoint {
    QString label, group;
    double x = 0, y = 0;
    std::optional<double> size;
};
struct ScatterData {
    QString title, x, y;
    bool trend = false;
    QVector<ScatterPoint> points;
};

std::optional<ScatterData> parseScatter(const QStringList &lines)
{
    static const Re axisLine(QStringLiteral("^([xy])-axis(?:\\s*:\\s*|\\s+)(.+)$"), I),
        trend(QStringLiteral("^trend\\b"), I);
    ScatterData data;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = bare(lines.at(i));
        QRegularExpressionMatch m;
        if (!line.contains(QLatin1Char('|'))) {
            if ((m = titleLine().match(line)).hasMatch())
                data.title = unquote(m.captured(1));
            else if ((m = axisLine.match(line)).hasMatch())
                (m.captured(1).toLower() == QLatin1String("x") ? data.x : data.y) =
                    unquote(m.captured(2));
            else if (trend.match(line).hasMatch())
                data.trend = true;
            continue;
        }
        const QStringList cells = cellsOf(line);
        const auto x = amount(cells.value(1)), y = amount(cells.value(2)),
                   size = amount(cells.value(3));
        if (!cells.value(0).isEmpty() && x && y) {
            ScatterPoint p;
            p.label = unquote(cells.at(0));
            p.x = x->value;
            p.y = y->value;
            if (size)
                p.size = std::max(0.0, size->value);
            p.group = unquote(cells.value(size ? 4 : 3));
            data.points << p;
        }
    }
    if (data.points.isEmpty())
        return std::nullopt;
    return data;
}

struct Axis {
    double lo = 0, hi = 0;
    QVector<double> ticks;
};

std::optional<Axis> axisOf(const QVector<double> &values)
{
    double lo = minOf(values), hi = maxOf(values);
    if (lo >= 0 && lo < hi * 0.5)
        lo = 0;
    if (hi == lo) {
        hi += 1;
        lo -= lo > 0 ? std::min(1.0, lo) : 0;
    }
    const double step = niceStep(hi - lo);
    Axis a;
    a.lo = std::floor(lo / step + 1e-9) * step;
    a.hi = std::ceil(hi / step - 1e-9) * step;
    // A step too small for the numbers' size would never move on, and ticks
    // the same once rounded would be one key twice.
    for (double v = a.lo; v <= a.hi + step * 1e-6 && a.ticks.size() < 64; v += step) {
        const double t = toFixed10(v);
        if (a.ticks.isEmpty() || t != a.ticks.last())
            a.ticks << t;
        if (v + step == v)
            break;
    }
    // Numbers too large to tell apart have no span to draw (upstream: NaN).
    if (!finite(a.lo) || !finite(a.hi) || !(a.hi > a.lo) || a.ticks.isEmpty())
        return std::nullopt;
    return a;
}

std::optional<Result> scatterScene(Ctx &c, const ScatterData &data)
{
    const double W = clamp(c.width, 300, SCATTER.max);
    const Tones &tones = c.tones;
    const auto &points = data.points;
    Result r;
    auto &items = r.items;
    auto &tips = r.tips;
    // Any two points may end up side by side, so only three groups get a
    // colour of their own; the rest go grey.
    QStringList groups;
    for (const auto &p : points) {
        if (!p.group.isEmpty() && !groups.contains(p.group))
            groups << p.group;
    }
    const Tones hues = ring(tones, int(groups.size()));
    const auto toneOf = [&](const ScatterPoint &p) {
        const int k = int(groups.indexOf(p.group));
        return k < 0 ? first(tones) : k < 3 ? slot(hues, k) : int(Mute);
    };
    QVector<double> xs, ys;
    for (const auto &p : points) {
        xs << p.x;
        ys << p.y;
    }
    const auto ax = axisOf(xs), ay = axisOf(ys);
    if (!ax || !ay)
        return c.fail(QStringLiteral("This diagram could not be laid out.")), std::nullopt;
    double head = 0;
    if (!data.title.isEmpty()) {
        addTitle(c, items, r.corner, data.title, W - 80);
        head = CHART.head;
    }
    if (groups.size() > 1) {
        // The grey points are named too: by their group when it is the only
        // one left, as the rest when there are more.
        QVector<ChipEntry> named;
        for (int k = 0; k < std::min(3, int(groups.size())); ++k)
            named << ChipEntry{groups.at(k), slot(hues, k), QStringLiteral("dot")};
        if (groups.size() > 3)
            named << ChipEntry{groups.size() == 4 ? groups.at(3) : QStringLiteral("Other"), Mute,
                               QStringLiteral("dot")};
        const ChipRow row = chips(c, named, 0, head + 9, W);
        items += row.items;
        if (!r.corner)
            r.corner = Corner{row.width + 16, 20, 0};
        head += row.height + 2;
    }
    if (!data.y.isEmpty())
        items << labelSpec(QStringLiteral("yt"), 0, 0, head + 8,
                           {truncate(c, data.y, W * 0.6, FONT::tick)}, QStringLiteral("dg-unit"),
                           QStringLiteral("start"), Back);
    const double top = head + (data.y.isEmpty() ? 0 : 18) + 12,
                 plotH = std::min(SCATTER.plot, W * 0.5);
    QVector<double> yWidths;
    for (const double t : ay->ticks)
        yWidths << c.widthOf(format(t), FONT::tick);
    const double left = std::ceil(maxOf(yWidths)) + 10, plotW = W - left - 12;
    const auto px = [&](double v) { return left + (v - ax->lo) / (ax->hi - ax->lo) * plotW; };
    const auto py = [&](double v) {
        return top + plotH - (v - ay->lo) / (ay->hi - ay->lo) * plotH;
    };
    for (const double t : ay->ticks) {
        items << lineSpec(QStringLiteral("gy:") + jsNum(t), 0, left, py(t), left + plotW, py(t),
                          t == ay->lo ? QStringLiteral("dg-grid is-zero")
                                      : QStringLiteral("dg-grid"),
                          false, Back);
        items << labelSpec(QStringLiteral("ty:") + jsNum(t), 0, left - 8, py(t), {format(t)},
                           QStringLiteral("dg-tick"), QStringLiteral("end"), Back);
    }
    QVector<double> xWidths;
    for (const double t : ax->ticks)
        xWidths << c.widthOf(format(t), FONT::tick);
    const double tickW = maxOf(xWidths);
    const int every = int(
        clamp(std::ceil((tickW + 14) / (plotW / std::max(1, int(ax->ticks.size()) - 1))), 1, 1e6));
    for (int i = 0; i < ax->ticks.size(); ++i) {
        const double t = ax->ticks.at(i);
        items << lineSpec(QStringLiteral("gx:") + jsNum(t), 0, px(t), top, px(t), top + plotH,
                          QStringLiteral("dg-grid is-faint"), false, Back);
        if (i % every == 0) {
            Spec tick =
                labelSpec(QStringLiteral("tx:") + jsNum(t), 0, px(t), top + plotH + 9, {format(t)},
                          QStringLiteral("dg-tick"), QStringLiteral("middle"), Back);
            tick.fixed.baseline = QStringLiteral("below");
            items << tick;
        }
    }
    if (!data.x.isEmpty())
        items << labelSpec(QStringLiteral("xt"), 0, left + plotW, top + plotH + 34,
                           {truncate(c, data.x, plotW, FONT::tick)}, QStringLiteral("dg-unit"),
                           QStringLiteral("end"), Back);
    if (data.trend && points.size() > 2) {
        // The straight line the points lie closest to.
        const double n = points.size();
        double mx = 0, my = 0;
        for (const auto &p : points) {
            mx += p.x;
            my += p.y;
        }
        mx /= n;
        my /= n;
        double sxx = 0, sxy = 0;
        for (const auto &p : points) {
            sxx += (p.x - mx) * (p.x - mx);
            sxy += (p.x - mx) * (p.y - my);
        }
        const double slope = sxx ? sxy / sxx : 0;
        const auto yAt = [&](double v) { return clamp(my + slope * (v - mx), ay->lo, ay->hi); };
        items << lineSpec(QStringLiteral("trend"), 0.6, px(ax->lo), py(yAt(ax->lo)), px(ax->hi),
                          py(yAt(ax->hi)), QStringLiteral("dg-goal"), true, Edges);
    }
    double most = 0;
    for (const auto &p : points)
        most = std::max(most, p.size.value_or(0));
    const auto radius = [&](const ScatterPoint &p) {
        return most && p.size ? 4 + 11 * std::sqrt(*p.size / most) : 4.5;
    };
    // A name keeps clear of every point, by enough that it can't be taken for
    // a neighbour's.
    struct Box {
        double x, y, w, h;
    };
    QVector<Box> boxes;
    for (const auto &p : points) {
        const double rr = radius(p) + 5;
        boxes << Box{px(p.x) - rr, py(p.y) - rr, rr * 2, rr * 2};
    }
    const auto hit = [](const Box &a, const Box &b) {
        return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    };
    struct Option {
        double x, y;
        const char *anchor;
        double left;
    };
    for (int i = 0; i < points.size(); ++i) {
        if (!c.budget(4 * int(boxes.size())))
            return std::nullopt;
        const auto &p = points.at(i);
        const double x = px(p.x), y = py(p.y), rr = radius(p), tw = c.widthOf(p.label, FONT::point);
        const QString key = QStringLiteral("sp:%1").arg(i);
        const double order = 1 + i * 0.12;
        items << dotSpec(key, order, x, y, rr,
                         p.size ? QStringLiteral("dg-q-point is-bubble")
                                : QStringLiteral("dg-q-point"),
                         toneOf(p));
        // A name goes where it covers nothing: beside its point, over it or
        // under it, moved in from the edge of the plot when it would stick
        // out. With no free place around its point it stays in the tip.
        const double mid = clamp(x, left + tw / 2 + 2, left + plotW - tw / 2);
        const Option options[] = {{x + rr + 6, y, "start", x + rr + 6},
                                  {mid, y - rr - 9, "middle", mid - tw / 2},
                                  {mid, y + rr + 10, "middle", mid - tw / 2},
                                  {x - rr - 6, y, "end", x - rr - 6 - tw}};
        const Option *pick = nullptr;
        for (const auto &option : options) {
            const Box box{option.left, option.y - 8, tw, 16};
            if (!(box.x >= left && box.x + box.w <= left + plotW + 10 && box.y >= top - 6 &&
                  box.y + box.h <= top + plotH + 4))
                continue;
            bool clear = true;
            for (int k = 0; k < boxes.size() && clear; ++k)
                clear = k == i || !hit(box, boxes.at(k));
            if (clear) {
                pick = &option;
                break;
            }
        }
        if (pick) {
            boxes << Box{pick->left, pick->y - 8, tw, 16};
            items << labelSpec(key + QStringLiteral(":name"), order + 0.1, pick->x, pick->y,
                               {p.label}, QStringLiteral("dg-q-name"),
                               QString::fromLatin1(pick->anchor));
        }
        Tip tip;
        tip.title = p.label;
        tip.rows << tipRow(data.x.isEmpty() ? QStringLiteral("x") : data.x, format(p.x))
                 << tipRow(data.y.isEmpty() ? QStringLiteral("y") : data.y, format(p.y));
        if (p.size)
            tip.rows << tipRow(QStringLiteral("Size"), format(*p.size));
        tip.hot = pick ? QStringList{key, key + QStringLiteral(":name")} : QStringList{key};
        tips.insert(key, tip);
        r.near << NearPoint{x, y, key};
    }
    r.kind = QStringLiteral("scatter");
    r.width = W;
    r.height = top + plotH + 26 + (data.x.isEmpty() ? 0 : 18);
    r.flush = true;
    return r;
}

/* Treemap */

// What a whole is made of, as areas: a portfolio, a budget, a disk.
// Mermaid's treemap: a name on a line, a value after a colon, parts indented
// under their group.
struct Tile {
    QString name;
    double value = 0;
    bool own = false;
    double x = 0, y = 0, w = 0, h = 0;
};
struct TreeGroup {
    QString name;
    double value = 0;
    QVector<Tile> parts;
    double x = 0, y = 0, w = 0, h = 0;
};
struct TreeData {
    QString title;
    QVector<TreeGroup> groups;
};

Tile tile(const QString &name, double value, bool own = false)
{
    Tile t;
    t.name = name;
    t.value = value;
    t.own = own;
    return t;
}

std::optional<TreeData> parseTreemap(const QStringList &lines)
{
    static const Re cls(QStringLiteral(":::[\\w-]+\\s*$")),
        entry(QStringLiteral("^(?:\"([^\"]*)\"|'([^']*)'|(.+?))\\s*(?:[:|]\\s*(.+))?$"));
    TreeData data;
    int base = -1;
    for (int i = 1; i < lines.size(); ++i) {
        const QString &raw = lines.at(i);
        const int indent = indentOf(raw);
        const QString line = bare(raw).remove(cls);
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (data.groups.isEmpty() && (m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        m = entry.match(line);
        if (!m.hasMatch())
            return std::nullopt; // Upstream throws on the null match: nothing is drawn.
        const int which = m.capturedStart(1) >= 0 ? 1 : m.capturedStart(2) >= 0 ? 2 : 3;
        const QString name = cleanLabel(m.captured(which));
        std::optional<double> value;
        if (!m.captured(4).isEmpty()) {
            if (const auto a = amount(m.captured(4)))
                value = a->value;
        }
        if (base < 0)
            base = indent;
        if (indent <= base) {
            TreeGroup g;
            g.name = name;
            g.value = value && *value > 0 ? *value : 0;
            data.groups << g;
        } else if (!data.groups.isEmpty() && value && *value > 0) {
            data.groups.last().parts << tile(name, *value);
        }
    }
    for (auto &g : data.groups) {
        if (g.parts.isEmpty())
            continue;
        g.value = 0;
        for (const auto &part : g.parts)
            g.value += part.value;
    }
    QVector<TreeGroup> kept;
    for (const auto &g : data.groups) {
        if (g.value > 0)
            kept << g;
    }
    data.groups = kept;
    if (data.groups.isEmpty())
        return std::nullopt;
    return data;
}

// The entries' order by value, largest first (a stable sort, as upstream's).
template <typename T> QVector<int> byValue(const QVector<T> &entries)
{
    QVector<int> order(entries.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return entries.at(b).value < entries.at(a).value; });
    return order;
}

// Tiles as near to squares as their areas allow: each strip is filled while
// it keeps the tiles from getting longer.
template <typename T>
bool squarify(Ctx &c, QVector<T> &entries, double x, double y, double w, double h)
{
    double total = 0;
    for (const auto &entry : entries)
        total += entry.value;
    if (!total || total != total)
        total = 1;
    struct Area {
        int entry;
        double area;
    };
    QVector<Area> rest;
    for (const int k : byValue(entries))
        rest << Area{k, entries.at(k).value / total * w * h};
    int at = 0;
    while (at < rest.size()) {
        if (!c.budget(int(rest.size()) - at))
            return false;
        const double shortSide = std::max(1e-6, std::min(w, h));
        int count = 0;
        double sum = 0, worst = INFINITY, big = -INFINITY, small = INFINITY;
        for (int k = at; k < rest.size(); ++k) {
            const double area = rest.at(k).area, next = sum + area;
            const double b = std::max(area, big), s = std::min(area, small);
            const double ratio = std::max(shortSide * shortSide * b / (next * next),
                                          next * next / (shortSide * shortSide * s));
            if (count && ratio > worst)
                break;
            ++count;
            sum = next;
            worst = ratio;
            big = b;
            small = s;
        }
        const double thick = sum / shortSide;
        double run = 0;
        for (int k = at; k < at + count; ++k) {
            const double length = rest.at(k).area / thick;
            T &entry = entries[rest.at(k).entry];
            if (w >= h) {
                entry.x = x;
                entry.y = y + run;
                entry.w = thick;
                entry.h = length;
            } else {
                entry.x = x + run;
                entry.y = y;
                entry.w = length;
                entry.h = thick;
            }
            run += length;
        }
        at += count;
        if (w >= h) {
            x += thick;
            w -= thick;
        } else {
            y += thick;
            h -= thick;
        }
    }
    return true;
}

std::optional<Result> treemapScene(Ctx &c, TreeData data)
{
    const double W = clamp(c.width, 280, TREE.max),
                 H = clamp(jsRound(W * 0.48), TREE.low, TREE.high);
    Result r;
    auto &items = r.items;
    auto &tips = r.tips;
    double total = 0;
    for (const auto &g : data.groups)
        total += g.value;
    const auto share = [&](double value) {
        return format(jsRound(value / total * 1000) / 10) + QLatin1Char('%');
    };
    double top = 0;
    if (!data.title.isEmpty()) {
        addTitle(c, items, r.corner, data.title, W - 80);
        top = CHART.head;
    }
    const auto nested = std::count_if(data.groups.begin(), data.groups.end(),
                                      [](const TreeGroup &g) { return !g.parts.isEmpty(); });
    if (nested > 1) {
        QVector<ChipEntry> entries;
        for (int i = 0; i < data.groups.size(); ++i)
            entries << ChipEntry{data.groups.at(i).name, slot(series(), i), QStringLiteral("bar")};
        const ChipRow row = chips(c, entries, 0, top + 9, W);
        items += row.items;
        if (!r.corner)
            r.corner = Corner{row.width + 16, 20, 0};
        top += row.height + 6;
    }
    if (!squarify(c, data.groups, 0, top, W, H))
        return std::nullopt;
    const Tones hues = ring(c.tones, int(data.groups.size()));
    for (int i = 0; i < data.groups.size(); ++i) {
        const TreeGroup &g = data.groups.at(i);
        const int tone = slot(hues, i);
        QVector<Tile> parts = g.parts;
        if (parts.isEmpty())
            parts << tile(g.name, g.value, true);
        if (!squarify(c, parts, g.x, g.y, g.w, g.h))
            return std::nullopt;
        const QVector<int> sorted = byValue(parts);
        for (int k = 0; k < sorted.size(); ++k) {
            const Tile &part = parts.at(sorted.at(k));
            // Tiles are parted by two pixels of the surface, and a group is
            // told by its colour alone: the size of a tile is already its area.
            const QString key = QStringLiteral("tm:%1:%2").arg(i).arg(k);
            const double x = part.x + TREE.gap / 2, y = part.y + TREE.gap / 2,
                         w = std::max(1.0, part.w - TREE.gap), h = std::max(1.0, part.h - TREE.gap);
            items << rectSpec(key, 0.3 + i * 0.2 + k * 0.05, Nodes, x, y, w, h,
                              QStringLiteral("dg-tile"), tone, 3);
            const QString value = format(part.value);
            if (h >= 22 && w >= 44) {
                const QString name = truncate(c, part.name, w - 14, FONT::small);
                items << labelSpec(key + QStringLiteral(":name"), 0.5 + i * 0.2 + k * 0.05, x + 7,
                                   y + 12, {name}, QStringLiteral("dg-tile-name"),
                                   QStringLiteral("start"));
                if (h >= 40 && c.widthOf(value, FONT::small) <= w - 14)
                    items << labelSpec(key + QStringLiteral(":value"), 0.55 + i * 0.2 + k * 0.05,
                                       x + 7, y + 27, {value}, QStringLiteral("dg-tile-value"),
                                       QStringLiteral("start"));
            }
            Tip tip;
            tip.title = part.own ? part.name : g.name + QString::fromUtf8(" › ") + part.name;
            tip.rows << tipRow(QStringLiteral("Value"), value)
                     << tipRow(QStringLiteral("Share"), share(part.value));
            tip.hot = QStringList{key};
            for (const QString &id :
                 {key, key + QStringLiteral(":name"), key + QStringLiteral(":value")})
                tips.insert(id, tip);
        }
    }
    r.kind = QStringLiteral("treemap");
    r.width = W;
    r.height = top + H;
    r.flush = true;
    return r;
}

/* History of a repository: Mermaid's gitGraph, drawn as a log reads */

struct GitCommit {
    int n = 0, branch = 0;
    QVector<int> parents;
    QString id, tag, type, merge, pick;
};
struct GitBranch {
    QString name;
    int lane = 0, head = -1, first = -1;
};
struct GitData {
    QVector<GitCommit> commits;
    QVector<GitBranch> branches; // All of them, in the order they were named.
    QVector<int> drawn;          // Those with a commit of their own.
};

std::optional<GitData> parseGit(const QStringList &lines)
{
    static const Re commitLine(QStringLiteral("^commit\\b(.*)$"), I),
        branchLine(QStringLiteral("^branch\\s+(\"[^\"]+\"|\\S+)"), I),
        checkoutLine(QStringLiteral("^(?:checkout|switch)\\s+(\"[^\"]+\"|\\S+)"), I),
        mergeLine(QStringLiteral("^merge\\s+(\"[^\"]+\"|\\S+)(.*)$"), I),
        pickLine(QStringLiteral("^cherry-pick\\b(.*)$"), I);
    static const Re idAttr(QStringLiteral("\\bid:\\s*\"([^\"]*)\"")),
        tagAttr(QStringLiteral("\\btag:\\s*\"([^\"]*)\"")),
        typeAttr(QStringLiteral("\\btype:\\s*(\\w+)"));
    GitData data;
    QHash<QString, int> index;
    QString current = QStringLiteral("main");
    const auto branch = [&](const QString &name) {
        const auto found = index.constFind(name);
        if (found != index.constEnd())
            return *found;
        const int k = int(data.branches.size());
        GitBranch b;
        b.name = name;
        b.lane = k;
        data.branches << b;
        index.insert(name, k);
        return k;
    };
    const auto attrs = [&](const QString &text) {
        GitCommit commit;
        commit.id = idAttr.match(text).captured(1);
        commit.tag = tagAttr.match(text).captured(1);
        const QString type = typeAttr.match(text).captured(1);
        commit.type = (type.isEmpty() ? QStringLiteral("NORMAL") : type).toUpper();
        return commit;
    };
    const auto add = [&](int b, const QVector<int> &parents, GitCommit commit) {
        commit.n = int(data.commits.size());
        commit.branch = b;
        for (const int parent : parents) {
            if (parent >= 0)
                commit.parents << parent;
        }
        data.commits << commit;
        data.branches[b].head = commit.n;
        if (data.branches[b].first < 0)
            data.branches[b].first = commit.n;
    };
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        QRegularExpressionMatch m;
        if ((m = commitLine.match(line)).hasMatch()) {
            const int b = branch(current);
            add(b, {data.branches[b].head}, attrs(m.captured(1)));
        } else if ((m = branchLine.match(line)).hasMatch()) {
            const int from = branch(current), b = branch(unquote(m.captured(1)));
            if (data.branches[b].head < 0)
                data.branches[b].head = data.branches[from].head;
            current = data.branches[b].name;
        } else if ((m = checkoutLine.match(line)).hasMatch()) {
            current = data.branches[branch(unquote(m.captured(1)))].name;
        } else if ((m = mergeLine.match(line)).hasMatch()) {
            const auto other = index.constFind(unquote(m.captured(1)));
            const int b = branch(current);
            if (other != index.constEnd() && data.branches[*other].head >= 0) {
                GitCommit commit = attrs(m.captured(2));
                commit.merge = data.branches[*other].name;
                add(b, {data.branches[b].head, data.branches[*other].head}, commit);
            }
        } else if ((m = pickLine.match(line)).hasMatch()) {
            const int b = branch(current);
            GitCommit commit = attrs(m.captured(1));
            commit.pick = commit.id;
            commit.id.clear();
            add(b, {data.branches[b].head}, commit);
        }
    }
    if (data.commits.isEmpty())
        return std::nullopt;
    for (const auto &b : data.branches) {
        if (b.first >= 0)
            data.drawn << b.lane;
    }
    return data;
}

std::optional<Result> gitScene(Ctx &c, const GitData &data)
{
    const double W = clamp(c.width, 300, GIT.max);
    Result r;
    auto &items = r.items;
    auto &tips = r.tips;
    QHash<int, int> lanes;
    for (int i = 0; i < data.drawn.size(); ++i)
        lanes.insert(data.drawn.at(i), i);
    const Tones hues = ring(c.tones, int(lanes.size()));
    const auto laneX = [&](int b) { return 7 + lanes.value(b) * GIT.lane; };
    const auto toneOf = [&](int b) { return slot(hues, lanes.value(b)); };
    const double textX = 7 + lanes.size() * GIT.lane + 6;
    const auto rowY = [](const GitCommit &commit) { return commit.n * GIT.row + GIT.row / 2; };
    struct Pill {
        QString text, cls;
        int tone;
    };
    for (int i = 0; i < data.commits.size(); ++i) {
        if (!c.budget(4))
            return std::nullopt;
        const GitCommit &commit = data.commits.at(i);
        const int b = commit.branch;
        const double x = laneX(b), y = rowY(commit), order = 0.3 + i * 0.3;
        const int tone = toneOf(b);
        const QString key = QStringLiteral("gc:%1").arg(i);
        for (int k = 0; k < commit.parents.size(); ++k) {
            // The line of a branch runs straight down; a branch leaving or
            // coming back takes a soft bend between two rows.
            const GitCommit &parent = data.commits.at(commit.parents.at(k));
            const double px = laneX(parent.branch), py = rowY(parent);
            const bool own = parent.branch == b;
            Spec edge =
                item(Type::Edge, QStringLiteral("%1:p%2").arg(key).arg(k), order - 0.15, Edges);
            edge.props.pts = own ? Pts{px, py, px, py + (y - py) / 3, x, y - (y - py) / 3, x, y}
                                 : Pts{px, py, px, py + GIT.row * 0.7, x, y - GIT.row * 0.7, x, y};
            edge.fixed.style = QStringLiteral("solid");
            edge.fixed.head = QStringLiteral("none");
            edge.fixed.tone = own || k == 0 ? tone : toneOf(parent.branch);
            edge.fixed.cls = QStringLiteral("dg-stroke dg-lane");
            edge.fixed.draw = 300;
            items << edge;
        }
        items << hitSpec(key + QStringLiteral(":hit"), order, -8, y - GIT.row / 2, W + 16, GIT.row);
        tips.insert(key + QStringLiteral(":hit"), silentTip());
        items << dotSpec(key, order, x, y, commit.type == QLatin1String("HIGHLIGHT") ? 5.5 : 4.5,
                         commit.merge.isEmpty() ? QStringLiteral("dg-commit")
                                                : QStringLiteral("dg-commit is-merge"),
                         tone);
        // After the message come the name of a branch, where it starts, and
        // the tag of a release.
        QVector<Pill> pills;
        if (data.branches.at(b).first == commit.n)
            pills << Pill{data.branches.at(b).name, QStringLiteral("dg-tag is-branch"), tone};
        if (!commit.tag.isEmpty())
            pills << Pill{commit.tag, QStringLiteral("dg-tag"), NoTone};
        double pillW = 0;
        for (const auto &pill : pills)
            pillW += c.widthOf(pill.text, FONT::value) + 14 + 6;
        const QString text = !commit.id.isEmpty()      ? commit.id
                             : !commit.merge.isEmpty() ? QStringLiteral("Merge ") + commit.merge
                             : !commit.pick.isEmpty() ? QStringLiteral("Cherry-pick ") + commit.pick
                                                      : QString();
        const QString shown = truncate(c, text, std::max(40.0, W - textX - pillW), FONT::row);
        if (!shown.isEmpty())
            items << labelSpec(key + QStringLiteral(":text"), order + 0.05, textX, y, {shown},
                               commit.id.isEmpty() ? QStringLiteral("dg-row-name is-quiet")
                                                   : QStringLiteral("dg-row-name"),
                               QStringLiteral("start"));
        double tx = textX + (shown.isEmpty() ? 0 : c.widthOf(shown, FONT::row) + 10);
        for (int k = 0; k < pills.size(); ++k) {
            const auto &pill = pills.at(k);
            const double w = c.widthOf(pill.text, FONT::value) + 14;
            Spec label = labelSpec(QStringLiteral("%1:pill%2").arg(key).arg(k),
                                   order + 0.1 + k * 0.05, tx + w / 2, y, {pill.text}, pill.cls);
            label.fixed.pill = true;
            label.fixed.rx = 5;
            label.fixed.w = w;
            label.fixed.h = 18;
            label.fixed.tone = pill.tone;
            items << label;
            tx += w + 6;
        }
    }
    r.kind = QStringLiteral("git");
    r.width = W;
    r.height = data.commits.size() * GIT.row;
    r.flush = true;
    return r;
}

/* Cells of an algorithm: an array, or a table of them, with pointers under
   the cells and stretches marked */

// A row is a caption, the values, the pointers as name: index, and the cells
// to mark as 2..4 or as 1, 3. The caption may be left out, and so may what
// follows the values.
struct ArrayRow {
    QString caption;
    QStringList values;
    QVector<std::pair<double, QStringList>> marks; // By index, in the order first named.
    QVector<bool> lit;
};
struct ArrayData {
    QString title;
    QVector<ArrayRow> rows;
};

std::optional<ArrayData> parseArray(const QStringList &lines)
{
    static const Re spanPart(QString::fromUtf8("^\\d+(?:\\s*(?:\\.\\.+|-|–)\\s*\\d+)?$")),
        bracketed(QStringLiteral("^\\[.*\\]$")), brackets(QStringLiteral("^\\[|\\]$")),
        pointer(QStringLiteral("^(.+?)\\s*[:=]\\s*(\\d+)$")),
        span(QString::fromUtf8("^(\\d+)(?:\\s*(?:\\.\\.+|-|–)\\s*(\\d+))?$"));
    const auto spans = [](const QString &text) {
        const QStringList parts = splitList(text);
        return std::all_of(parts.begin(), parts.end(),
                           [](const QString &part) { return spanPart.match(part).hasMatch(); });
    };
    ArrayData data;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = bare(lines.at(i));
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (!line.contains(QLatin1Char('|')) && (m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        const QStringList cells = cellsOf(line);
        int at = -1;
        for (int k = 0; k < cells.size(); ++k) {
            if (cells.at(k).contains(QLatin1Char(',')) || bracketed.match(cells.at(k)).hasMatch()) {
                at = k;
                break;
            }
        }
        if (at < 0 || at > 1)
            continue;
        ArrayRow row;
        for (const auto &value : splitList(QString(cells.at(at)).remove(brackets)))
            row.values << unquote(value);
        row.lit.fill(false, row.values.size());
        // With the pointers left out, the cells to mark come right after the
        // values.
        const QString next = cells.value(at + 1), after = cells.value(at + 2);
        const bool spansOnly = !next.isEmpty() && after.isEmpty() && spans(next);
        const QString pointers = spansOnly ? QString() : next, marked = spansOnly ? next : after;
        for (const auto &part : splitList(pointers)) {
            const auto p = pointer.match(part);
            if (!p.hasMatch())
                continue;
            const double k = p.captured(2).toDouble();
            auto found = std::find_if(row.marks.begin(), row.marks.end(),
                                      [&](const auto &mark) { return mark.first == k; });
            if (found == row.marks.end()) {
                row.marks.push_back({k, {}});
                found = row.marks.end() - 1;
            }
            found->second << unquote(p.captured(1));
        }
        for (const auto &part : splitList(marked)) {
            const auto s = span.match(part);
            if (!s.hasMatch())
                continue;
            const double from = s.captured(1).toDouble(),
                         to = (s.capturedStart(2) >= 0 ? s.captured(2) : s.captured(1)).toDouble();
            for (double k = from; k <= to && k < row.values.size(); ++k)
                row.lit[int(k)] = true;
        }
        if (!row.values.isEmpty()) {
            row.caption = at ? unquote(cells.at(0)) : QString();
            data.rows << row;
        }
    }
    if (data.rows.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> arrayScene(Ctx &c, const ArrayData &data)
{
    const double W = clamp(c.width, 280, CELLS.max);
    const int accent = first(c.tones);
    Result r;
    auto &items = r.items;
    auto &tips = r.tips;
    double top = 0;
    if (!data.title.isEmpty()) {
        addTitle(c, items, r.corner, data.title, W - 80);
        top = CHART.head;
    }
    int most = 0, cells = 0;
    for (const auto &row : data.rows) {
        most = std::max(most, int(row.values.size()));
        cells += int(row.values.size());
    }
    // Two items a cell: more than a drawing may hold is refused before the
    // work, as compile would refuse it after.
    if (2.0 * cells + most > MaxItems)
        return c.fail(QStringLiteral("This diagram is too large to draw.")), std::nullopt;
    double captionMost = 0, longest = -INFINITY;
    for (const auto &row : data.rows) {
        if (!row.caption.isEmpty())
            captionMost = std::max(captionMost, c.widthOf(row.caption, FONT::tick));
        for (const auto &value : row.values)
            longest =
                std::max(longest, c.textWidth(value, FONT::cell.size, FONT::cell.weight, true));
    }
    const double captionW = std::min(W * 0.3, captionMost), x0 = captionW ? captionW + 14 : 0;
    // Cells are as wide as the longest value asks, and narrower when that many
    // would not fit, but never too narrow for the value itself: a row too long
    // for the column makes the drawing wider, and the view brings it to size.
    const double pitch = std::max(std::min(clamp(longest + 16, CELLS.min, CELLS.wide) + CELLS.gap,
                                           (W - x0 + CELLS.gap) / most),
                                  std::min(longest, CELLS.wide - 16) + 8 + CELLS.gap);
    const double cw = pitch - CELLS.gap;
    const auto fit = [&](const QString &value) {
        QString text = value;
        while (text.size() > 1 && c.budget() &&
               c.textWidth(text == value ? text : text + QChar(0x2026), FONT::cell.size,
                           FONT::cell.weight, true) > cw - 6)
            text = cutUnits(text, int(text.size()) - 1);
        return text == value ? value : text + QChar(0x2026);
    };
    for (int i = 0; i < most; ++i)
        items << labelSpec(QStringLiteral("ai:%1").arg(i), 0.1 + i * 0.02, x0 + pitch * i + cw / 2,
                           top + 7, {QString::number(i)}, QStringLiteral("dg-tick"),
                           QStringLiteral("middle"), Back);
    double y = top + 18;
    for (int ri = 0; ri < data.rows.size(); ++ri) {
        const ArrayRow &row = data.rows.at(ri);
        const double order = 0.3 + ri * 0.4, cy = y + CELLS.height / 2;
        if (!row.caption.isEmpty())
            items << labelSpec(QStringLiteral("ac:%1").arg(ri), order, 0, cy,
                               {truncate(c, row.caption, captionW, FONT::tick)},
                               QStringLiteral("dg-tick"), QStringLiteral("start"));
        for (int i = 0; i < row.values.size(); ++i) {
            const QString &value = row.values.at(i);
            const double x = x0 + pitch * i;
            const QString key = QStringLiteral("av:%1:%2").arg(ri).arg(i);
            items << rectSpec(key, order + i * 0.03, Nodes, x, y, cw, CELLS.height,
                              row.lit.at(i) ? QStringLiteral("dg-acell is-lit")
                                            : QStringLiteral("dg-acell"),
                              accent, 5);
            const QString shown = fit(value);
            items << labelSpec(key + QStringLiteral(":t"), order + 0.05 + i * 0.03, x + cw / 2, cy,
                               {shown}, QStringLiteral("dg-acell-text"));
            // A value cut short to fit its cell is told whole under the pointer.
            if (shown != value) {
                Tip tip;
                tip.title = value;
                tips.insert(key, tip);
            }
        }
        // A pointer is a short stroke under its cell and its name under the
        // stroke.
        for (const auto &[i, names] : row.marks) {
            if (i >= row.values.size())
                continue;
            const double x = x0 + pitch * i + cw / 2;
            const QString at = QStringLiteral("%1:").arg(ri) + jsNum(i);
            Spec stroke =
                lineSpec(QStringLiteral("am:") + at, order + 0.3, x, y + CELLS.height + 3, x,
                         y + CELLS.height + 9, QStringLiteral("dg-apoint"), true, Labels);
            stroke.fixed.tone = accent;
            items << stroke;
            items << labelSpec(QStringLiteral("an:") + at, order + 0.35, x, y + CELLS.height + 17,
                               {names.join(QStringLiteral(", "))},
                               QStringLiteral("dg-apoint-name"));
        }
        y += CELLS.height + (row.marks.isEmpty() ? 8 : 30);
    }
    r.kind = QStringLiteral("array");
    r.width = x0 + pitch * most - CELLS.gap;
    r.height = y - 8;
    r.flush = true;
    return r;
}

/* A knockout: rounds side by side, each match feeding the next */

// A line at the left edge names a round; the matches of the round are
// indented under it, as Team 2 - 1 Team, or Team - Team while it is not
// played. A note in brackets after the score tells how a draw was settled.
struct Match {
    QString a, b, note;
    std::optional<double> sa, sb;
    int won = -1;
};
struct Round {
    QString name;
    QVector<Match> matches;
    QVector<double> ys;
};
struct BracketData {
    QString title;
    QVector<Round> rounds;
};

std::optional<BracketData> parseBracket(const QStringList &lines)
{
    static const Re colon(QStringLiteral(":$")),
        scored(
            QString::fromUtf8("^(.+?)\\s+(\\d+)\\s*[-:–—]\\s*(\\d+)\\s+(.+?)(?:\\s*\\((.+)\\))?$")),
        extraScore(QString::fromUtf8("(\\d+)\\s*[-:–]\\s*(\\d+)")),
        versus(QString::fromUtf8("^(.+?)\\s+(?:vs\\.?|v|—|–|-|:)\\s+(.+)$"), I);
    BracketData data;
    int base = -1;
    for (int i = 1; i < lines.size(); ++i) {
        const QString &raw = lines.at(i);
        const int indent = indentOf(raw);
        const QString line = bare(raw);
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (data.rounds.isEmpty() && (m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if (base < 0)
            base = indent;
        if (indent <= base) {
            Round round;
            round.name = cleanLabel(QString(line).remove(colon));
            data.rounds << round;
            continue;
        }
        Match match;
        if ((m = scored.match(line)).hasMatch()) {
            // A draw is decided by what the note says, when it holds a score
            // of its own: 4-3 on penalties.
            const auto extra = extraScore.match(m.captured(5));
            const double a = m.captured(2).toDouble(), b = m.captured(3).toDouble();
            match.a = cleanLabel(m.captured(1));
            match.b = cleanLabel(m.captured(4));
            match.sa = a;
            match.sb = b;
            match.note = cleanLabel(m.captured(5));
            if (a != b) {
                match.won = a > b ? 0 : 1;
            } else if (extra.hasMatch()) {
                const double ea = extra.captured(1).toDouble(), eb = extra.captured(2).toDouble();
                match.won = ea != eb ? (ea > eb ? 0 : 1) : -1;
            }
        } else if ((m = versus.match(line)).hasMatch()) {
            match.a = cleanLabel(m.captured(1));
            match.b = cleanLabel(m.captured(2));
        } else {
            match.a = cleanLabel(line);
        }
        data.rounds.last().matches << match;
    }
    QVector<Round> kept;
    for (const auto &round : data.rounds) {
        if (!round.matches.isEmpty())
            kept << round;
    }
    data.rounds = kept;
    if (data.rounds.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> bracketScene(Ctx &c, BracketData data)
{
    auto &rounds = data.rounds;
    const int n = int(rounds.size());
    const double room = std::max(280.0, c.width - PAD * 2);
    Result r;
    auto &items = r.items;
    auto &tips = r.tips;
    double top = 0;
    if (!data.title.isEmpty()) {
        addTitle(c, items, r.corner, data.title, room - 80);
        top = CHART.head + 2;
    }
    double nameW = -INFINITY;
    bool noted = false;
    for (const auto &round : rounds) {
        for (const auto &match : round.matches) {
            nameW = std::max({nameW, c.widthOf(match.a, FONT::row), c.widthOf(match.b, FONT::row)});
            noted = noted || !match.note.isEmpty();
        }
    }
    const double gap = clamp((room - BRACKET.max * n) / std::max(1, n - 1), 22, 44),
                 cardW = clamp(std::min(nameW + 46, (room - gap * (n - 1)) / n), BRACKET.min,
                               BRACKET.max);
    // A note under a match, how a draw was settled, needs a line of room
    // between the plates.
    const double pitch = BRACKET.card + BRACKET.gap + (noted ? 10 : 0);
    // A match stands midway between the two that feed it; a round that is not
    // half of the one before is stacked evenly beside the middle of it.
    const auto halves = [&](int ri) {
        return ri > 0 && rounds.at(ri - 1).matches.size() == rounds.at(ri).matches.size() * 2;
    };
    for (int ri = 0; ri < n; ++ri) {
        Round &round = rounds[ri];
        const int count = int(round.matches.size());
        round.ys.clear();
        if (halves(ri)) {
            const auto &prev = rounds.at(ri - 1).ys;
            for (int j = 0; j < count; ++j)
                round.ys << (prev.at(2 * j) + prev.at(2 * j + 1)) / 2;
        } else {
            const double middle =
                ri > 0 ? (minOf(rounds.at(ri - 1).ys) + maxOf(rounds.at(ri - 1).ys)) / 2
                       : (count - 1) * pitch / 2;
            for (int j = 0; j < count; ++j)
                round.ys << middle + (j - (count - 1) / 2.0) * pitch;
        }
    }
    QVector<double> all;
    for (const auto &round : rounds)
        all += round.ys;
    const double least = minOf(all), head = 22;
    for (int ri = 0; ri < n; ++ri) {
        const Round &round = rounds.at(ri);
        if (!c.budget(int(round.matches.size())))
            return std::nullopt;
        const double x = ri * (cardW + gap), order = 0.2 + ri * 0.9;
        items << labelSpec(QStringLiteral("br:%1").arg(ri), order, x, top + 8,
                           {caps(c, round.name, cardW)}, QStringLiteral("dg-eyebrow"),
                           QStringLiteral("start"), Back);
        for (int j = 0; j < round.matches.size(); ++j) {
            const Match &match = round.matches.at(j);
            const double y = top + head + round.ys.at(j) - least, at = order + 0.1 + j * 0.08;
            const QString key = QStringLiteral("bm:%1:%2").arg(ri).arg(j);
            const int mark = int(items.size());
            bool cut = false;
            Spec plate = boxSpec(Type::WBox, key, at, Edges, x, y, cardW, BRACKET.card);
            plate.fixed.cls = QStringLiteral("dg-plate");
            plate.fixed.rx = 8;
            items << plate;
            items << lineSpec(key + QStringLiteral(":rule"), at + 0.02, x + 10,
                              y + BRACKET.card / 2, x + cardW - 10, y + BRACKET.card / 2,
                              QStringLiteral("dg-grid"), false, Nodes);
            for (int side = 0; side < 2; ++side) {
                const QString &name = side ? match.b : match.a;
                const auto &score = side ? match.sb : match.sa;
                const double cy = y + BRACKET.card / 4 + side * BRACKET.card / 2;
                const QString out =
                    match.won >= 0 && match.won != side ? QStringLiteral(" is-out") : QString();
                const QString text = score ? jsNum(*score) : QString();
                const double span =
                    cardW - 20 - (text.isEmpty() ? 0 : c.widthOf(text, FONT::amount) + 10);
                const QString shown = truncate(c, name, span, FONT::row);
                cut = cut || shown != name;
                if (!name.isEmpty())
                    items << labelSpec(QStringLiteral("%1:n%2").arg(key).arg(side), at + 0.04,
                                       x + 10, cy, {shown}, QStringLiteral("dg-team") + out,
                                       QStringLiteral("start"));
                if (!text.isEmpty())
                    items << labelSpec(QStringLiteral("%1:s%2").arg(key).arg(side), at + 0.06,
                                       x + cardW - 10, cy, {text}, QStringLiteral("dg-score") + out,
                                       QStringLiteral("end"));
            }
            // A match answers the pointer as one thing, and names cut short to
            // fit its plate are told whole.
            Tip tip;
            if (cut) {
                QStringList names;
                for (const QString &name : {match.a, match.b}) {
                    if (!name.isEmpty())
                        names << name;
                }
                tip.title = names.join(QString::fromUtf8(" – "));
            } else {
                tip.silent = true;
            }
            tip.hot = QStringList{key};
            for (int k = mark; k < items.size(); ++k)
                tips.insert(items.at(k).key, tip);
            if (!match.note.isEmpty())
                items << labelSpec(key + QStringLiteral(":note"), at + 0.08, x + cardW - 10,
                                   y + BRACKET.card + 11,
                                   {truncate(c, match.note, cardW - 20, FONT::tick)},
                                   QStringLiteral("dg-tick"), QStringLiteral("end"), Back);
            if (halves(ri)) {
                const auto &prev = rounds.at(ri - 1).ys;
                for (const int k : {2 * j, 2 * j + 1}) {
                    const double sy = top + head + prev.at(k) - least + BRACKET.card / 2,
                                 sx = x - gap, mx = x - gap / 2, ty = y + BRACKET.card / 2;
                    Spec edge = item(Type::Edge, QStringLiteral("%1:in%2").arg(key).arg(k),
                                     at - 0.05, Back);
                    edge.props.pts = polyline({{sx, sy}, {mx, sy}, {mx, ty}, {x, ty}});
                    edge.fixed.style = QStringLiteral("solid");
                    edge.fixed.head = QStringLiteral("none");
                    edge.fixed.cls = QStringLiteral("dg-tie");
                    edge.fixed.draw = 320;
                    items << edge;
                }
            }
        }
    }
    r.kind = QStringLiteral("bracket");
    r.width = n * cardW + (n - 1) * gap;
    r.height = top + head + maxOf(all) - least + BRACKET.card + (noted ? 18 : 0);
    return r;
}

/* Food: what a day or a dish gives */

// A row named for energy is the total, rows named for protein, fat and carbs
// are what it is made of, each with `of N` for the goal; any other row with
// calories is a meal or a dish, which may carry what it gives of the three
// after a bar: 18 / 12 / 58, or P 18 · F 12 · C 58.
const Re &energyName()
{
    static const Re re(QString::fromUtf8("^(?:калори|энерги|ккал|calor|energy|kcal)"), I);
    return re;
}
const Re &kcalUnit()
{
    static const Re re(QString::fromUtf8("ккал|kcal|cal(?![\\p{L}])"), I);
    return re;
}
struct Macro {
    const char *key;
    Re test;
    double per;
    int tone;
};
const QVector<Macro> &macros()
{
    static const QVector<Macro> table{
        {"protein", Re(QString::fromUtf8("^(?:белк|protein|prot(?![\\p{L}])|б(?![\\p{L}]))"), I), 4,
         1},
        {"fat", Re(QString::fromUtf8("^(?:жир|fat|ж(?![\\p{L}]))"), I), 9, 2},
        {"carbs", Re(QString::fromUtf8("^(?:углевод|carb|у(?![\\p{L}]))"), I), 4, 3},
    };
    return table;
}
const Re &goalCell()
{
    static const Re re(QString::fromUtf8("^(?:of|out of|from|из|/|goal|цель|норма)\\s*:?\\s*(.+)$"),
                       I);
    return re;
}

using Split = std::array<double, 3>; // Grams of protein, fat and carbs.

std::optional<Split> macroSplit(const QString &text)
{
    static const Re named(
        QString::fromUtf8("(?:^|[\\s·,;/(])([бжуpfc])[\\p{L}.]*\\s*:?\\s*(\\d+(?:[.,]\\d+)?)"), I),
        plain(QString::fromUtf8("^\\s*(\\d+(?:[.,]\\d+)?)\\s*/\\s*(\\d+(?:[.,]\\d+)?)\\s*/\\s*"
                                "(\\d+(?:[.,]\\d+)?)\\s*(?:г|g)?\\s*$"),
              I);
    std::optional<double> p, f, carbs;
    for (auto it = named.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        const QString letter = m.captured(1).toLower();
        const double value = number(m.captured(2));
        if (letter == QLatin1String("p") || letter == QString::fromUtf8("б"))
            p = value;
        else if (letter == QLatin1String("f") || letter == QString::fromUtf8("ж"))
            f = value;
        else
            carbs = value;
    }
    if (p && f && carbs)
        return Split{*p, *f, *carbs};
    const auto m = plain.match(text);
    if (!m.hasMatch())
        return std::nullopt;
    return Split{number(m.captured(1)), number(m.captured(2)), number(m.captured(3))};
}

struct Energy {
    double value = 0;
    std::optional<double> goal;
    QString unit;
};
struct Nutrient {
    QString key, name, unit;
    double value = 0, per = 0; // per: kcal a gram; 0 for what is not one of the three.
    std::optional<double> goal;
    int tone = NoTone;
};
struct Meal {
    QString name, text, note;
    double kcal = 0;
    std::optional<Split> split;
};
struct FoodData {
    QString title;
    std::optional<Energy> energy;
    QVector<Nutrient> macros, extras;
    QVector<Meal> meals;
};

std::optional<FoodData> parseNutrition(const QStringList &lines)
{
    static const Re headed(QStringLiteral("^([^:]{1,24}):\\s+(.+)$"));
    FoodData data;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = bare(lines.at(i));
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        const bool barred = line.contains(QLatin1Char('|'));
        if (!barred && (m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        const QStringList cells = barred ? cellsOf(line) : rowCells(line);
        const QString name = unquote(cells.value(0));
        const auto value = amount(cells.value(1));
        if (name.isEmpty() || !value)
            continue;
        QStringList rest;
        for (int k = 2; k < cells.size(); ++k) {
            if (!cells.at(k).isEmpty())
                rest << cells.at(k);
        }
        std::optional<double> aim;
        for (const auto &cell : rest) {
            const auto goal = goalCell().match(cell);
            if (!goal.hasMatch())
                continue;
            if (const auto a = amount(goal.captured(1)))
                aim = a->value;
            break;
        }
        const Macro *macro = nullptr;
        for (const auto &row : macros()) {
            if (row.test.match(name).hasMatch()) {
                macro = &row;
                break;
            }
        }
        const bool energy = kcalUnit().match(value->after).hasMatch();
        if (energyName().match(name).hasMatch()) {
            data.energy = Energy{value->value, aim, value->after};
        } else if (macro && !energy) {
            data.macros << Nutrient{QString::fromLatin1(macro->key),
                                    name,
                                    value->after,
                                    value->value,
                                    macro->per,
                                    aim,
                                    macro->tone};
        } else if (!energy && !value->after.isEmpty()) {
            data.extras << Nutrient{QString(), name, value->after, value->value, 0, aim, NoTone};
        } else {
            Meal meal;
            for (const auto &cell : rest) {
                if ((meal.split = macroSplit(cell)))
                    break;
            }
            const auto head = headed.match(name);
            meal.name = head.hasMatch() ? head.captured(1) : name;
            meal.text = head.hasMatch() ? head.captured(2) : QString();
            meal.kcal = value->value;
            QStringList notes;
            for (const auto &cell : rest) {
                if (!macroSplit(cell) && !goalCell().match(cell).hasMatch())
                    notes << unquote(cell);
            }
            meal.note = notes.join(QString::fromUtf8(" · "));
            data.meals << meal;
        }
    }
    const auto rank = [](const QString &key) {
        const auto &table = macros();
        for (int k = 0; k < table.size(); ++k) {
            if (key == QLatin1String(table.at(k).key))
                return k;
        }
        return -1;
    };
    std::stable_sort(
        data.macros.begin(), data.macros.end(),
        [&](const Nutrient &a, const Nutrient &b) { return rank(a.key) < rank(b.key); });
    if (!data.energy) {
        double eaten = 0;
        if (!data.meals.isEmpty()) {
            for (const auto &meal : data.meals)
                eaten += meal.kcal;
        } else {
            for (const auto &macro : data.macros)
                eaten += macro.value * macro.per;
        }
        if (eaten > 0)
            data.energy = Energy{eaten, std::nullopt, QString()};
    }
    if (!data.energy)
        return std::nullopt;
    return data;
}

std::optional<Result> nutritionScene(Ctx &c, const FoodData &data)
{
    const double W = clamp(c.width, 300, FOOD.max);
    const int accent = first(c.tones);
    Result r;
    auto &items = r.items;
    auto &tips = r.tips;
    double top = 0;
    if (!data.title.isEmpty()) {
        addTitle(c, items, r.corner, data.title, W - 80);
        top = CHART.head + 4;
    }
    // The ring is the day's energy: how much of the goal is eaten and, when
    // the three are known, what it is made of.
    const Energy &energy = *data.energy;
    const bool goal = energy.goal && *energy.goal; // `energy.goal` truthy: 0 is none.
    const double R = FOOD.ring, cx = R + FOOD.width / 2, cy = top + R + FOOD.width / 2, D = cx * 2;
    const double filled = goal ? clamp01(energy.value / *energy.goal) : 1;
    double burn = 0;
    for (const auto &macro : data.macros)
        burn += macro.value * macro.per;
    const auto arc = [&](const QString &key, double order, double start, double sweep, int tone,
                         double gap) {
        Spec spec = item(Type::Arc, key, order);
        spec.props.start = start;
        spec.props.sweep = sweep;
        spec.fixed.cx = cx;
        spec.fixed.cy = cy;
        spec.fixed.r = R;
        spec.fixed.width = FOOD.width;
        spec.fixed.tone = tone;
        spec.fixed.gap = gap;
        return spec;
    };
    Spec track = arc(QStringLiteral("nr:track"), 0.1, 0, 100, Mute, 0);
    track.fixed.cls = QStringLiteral("is-track");
    items << track;
    if (burn > 0 && data.macros.size() > 1) {
        double start = 0;
        for (int k = 0; k < data.macros.size(); ++k) {
            const Nutrient &macro = data.macros.at(k);
            const double sweep = macro.value * macro.per / burn * filled * 100;
            items << arc(QStringLiteral("nr:") + macro.key, 0.3 + k * 0.25, start, sweep,
                         macro.tone, sweep > 3 ? 0.9 : 0);
            start += sweep;
        }
    } else {
        items << arc(QStringLiteral("nr:all"), 0.3, 0, filled * 100, accent, 0);
    }
    static const Re cyrillic(QString::fromUtf8("[а-яё]"), I);
    QString names = data.title;
    for (const auto &macro : data.macros)
        names += macro.name;
    const QString unit = !energy.unit.isEmpty()             ? energy.unit
                         : cyrillic.match(names).hasMatch() ? QString::fromUtf8("ккал")
                                                            : QStringLiteral("kcal");
    Spec value = labelSpec(QStringLiteral("nr:value"), 0.6, cx, cy - 5,
                           {format(jsRound(energy.value))}, QStringLiteral("dg-center-value"));
    value.fixed.pop = true;
    items << value;
    items << labelSpec(QStringLiteral("nr:unit"), 0.7, cx, cy + 13, {unit},
                       QStringLiteral("dg-center-label"));
    if (goal) {
        const bool over = energy.value > *energy.goal;
        const QString text = format(jsRound(energy.value / *energy.goal * 100)) +
                             QString::fromUtf8("% · ") + format(*energy.goal);
        items << labelSpec(QStringLiteral("nr:goal"), 0.8, cx, top + D + 12, {text},
                           over ? QStringLiteral("dg-metric-change is-bad")
                                : QStringLiteral("dg-metric-note"));
    }
    // Beside the ring, what the energy is made of: grams, a thin measure
    // against the goal, and how far along it is.
    const QVector<Nutrient> rows = data.macros + data.extras;
    const bool beside = W >= 430;
    const double x0 = beside ? D + 30 : 0, rowH = FOOD.row;
    const auto hasGoal = [](const Nutrient &row) { return row.goal && *row.goal; };
    const auto amountOf = [&](const Nutrient &row) {
        return hasGoal(row) ? format(row.value) + QStringLiteral(" / ") +
                                  withUnit(*row.goal, QString(), row.unit)
                            : withUnit(row.value, QString(), row.unit);
    };
    const auto tailOf = [&](const Nutrient &row) {
        return hasGoal(row) ? format(jsRound(row.value / *row.goal * 100)) + QLatin1Char('%')
               : burn > 0 && row.per
                   ? format(jsRound(row.value * row.per / burn * 100)) + QLatin1Char('%')
                   : QString();
    };
    double nameW = 0, amountW = 0, tailW = 0;
    for (const auto &row : rows) {
        nameW = std::max(nameW, c.widthOf(row.name, FONT::legend));
        amountW = std::max(amountW, c.widthOf(amountOf(row), FONT::amount));
        tailW = std::max(tailW, c.widthOf(tailOf(row), FONT::value));
    }
    double y =
        beside ? top + std::max(0.0, (D - rows.size() * rowH) / 2) : top + D + (goal ? 30 : 14);
    const double barX = x0 + nameW + 14 + amountW + 14,
                 barW = std::max(40.0, W - barX - (tailW ? tailW + 12 : 0));
    for (int i = 0; i < rows.size(); ++i) {
        const Nutrient &row = rows.at(i);
        const double cyRow = y + rowH / 2, order = 0.9 + i * 0.2;
        const QString key = QStringLiteral("nm:%1").arg(i);
        const double part = hasGoal(row)          ? row.value / *row.goal
                            : burn > 0 && row.per ? row.value * row.per / burn
                                                  : 0;
        items << labelSpec(key + QStringLiteral(":name"), order, x0, cyRow, {row.name},
                           QStringLiteral("dg-metric-name"), QStringLiteral("start"));
        items << labelSpec(key + QStringLiteral(":amount"), order + 0.05, x0 + nameW + 14 + amountW,
                           cyRow, {amountOf(row)}, QStringLiteral("dg-row-value"),
                           QStringLiteral("end"));
        // A nutrient with nothing to measure it against is only named with its
        // amount.
        if (hasGoal(row) || row.per) {
            const int tone = row.tone == NoTone ? int(Mute) : row.tone;
            items << rectSpec(key + QStringLiteral(":track"), order + 0.1, Nodes, barX, cyRow - 2.5,
                              barW, 5, QStringLiteral("dg-meter-track"), tone, 2.5);
            Spec fill = boxSpec(Type::Span, key + QStringLiteral(":fill"), order + 0.15,
                                DefaultLayer, barX, cyRow, std::max(3.0, barW * clamp01(part)), 5);
            fill.fixed.cls = QStringLiteral("dg-meter-fill");
            fill.fixed.tone = tone;
            fill.fixed.rx = 2.5;
            items << fill;
        }
        const QString tail = tailOf(row);
        if (!tail.isEmpty())
            items << labelSpec(key + QStringLiteral(":tail"), order + 0.2, W, cyRow, {tail},
                               hasGoal(row) && row.value > *row.goal * 1.05
                                   ? QStringLiteral("dg-value is-over")
                                   : QStringLiteral("dg-value"),
                               QStringLiteral("end"));
        y += rowH;
    }
    y = std::max(y, top + D + (goal ? 26 : 8));
    // Under them, the meals: what each was, its calories, what it gave of the
    // three, and its part of the day.
    if (!data.meals.isEmpty()) {
        double whole = 0, kcalW = -INFINITY, shareW = -INFINITY;
        bool splits = false;
        for (const auto &meal : data.meals) {
            whole += meal.kcal;
            splits = splits || meal.split;
        }
        if (!whole)
            whole = 1;
        const auto shareOf = [&](const Meal &meal) {
            return format(jsRound(meal.kcal / whole * 100)) + QLatin1Char('%');
        };
        for (const auto &meal : data.meals) {
            kcalW = std::max(kcalW, c.widthOf(format(meal.kcal), FONT::amount));
            shareW = std::max(shareW, c.widthOf(shareOf(meal), FONT::tick));
        }
        shareW += 12;
        const double splitW = splits ? FOOD.split + 16 : 0;
        const double room = W - kcalW - shareW - splitW - 14;
        y += 10;
        items << lineSpec(QStringLiteral("nl:rule"), 1.6, 0, y, W, y, QStringLiteral("dg-line"),
                          true, Back);
        y += 8;
        for (int i = 0; i < data.meals.size(); ++i) {
            if (!c.budget(8))
                return std::nullopt;
            const Meal &meal = data.meals.at(i);
            QStringList subParts;
            for (const QString &part : {meal.text, meal.note}) {
                if (!part.isEmpty())
                    subParts << part;
            }
            const QString sub = subParts.join(QString::fromUtf8(" · "));
            const double h = sub.isEmpty() ? FOOD.meal : FOOD.meal + 16,
                         cyRow = y + (sub.isEmpty() ? h / 2 : 13), order = 1.8 + i * 0.15;
            const QString key = QStringLiteral("nl:%1").arg(i);
            items << hitSpec(key, order, -8, y, W + 16, h);
            tips.insert(key, silentTip());
            items << labelSpec(key + QStringLiteral(":name"), order, 0, cyRow,
                               {truncate(c, meal.name, room, FONT::row)},
                               QStringLiteral("dg-row-name"), QStringLiteral("start"));
            if (!sub.isEmpty())
                items << labelSpec(key + QStringLiteral(":sub"), order + 0.03, 0, cyRow + 17,
                                   {truncate(c, sub, room, FONT::tick)},
                                   QStringLiteral("dg-row-note"), QStringLiteral("start"));
            if (meal.split) {
                // What a meal gave of the three, as one thin bar parted in
                // their colours.
                const auto &table = macros();
                const Split &grams = *meal.split;
                double sum = 0;
                for (int k = 0; k < 3; ++k)
                    sum += grams[k] * table.at(k).per;
                if (!sum)
                    sum = 1;
                double sx = W - shareW - kcalW - 14 - FOOD.split;
                for (int k = 0; k < 3; ++k) {
                    const double w =
                        std::max(2.0, (FOOD.split - 4) * grams[k] * table.at(k).per / sum);
                    Spec span = boxSpec(Type::Span, QStringLiteral("%1:s%2").arg(key).arg(k),
                                        order + 0.06 + k * 0.02, DefaultLayer, sx, cyRow, w, 5);
                    span.fixed.cls = QStringLiteral("dg-meter-fill");
                    span.fixed.tone = table.at(k).tone;
                    span.fixed.rx = 2;
                    items << span;
                    sx += w + 2;
                }
            }
            items << labelSpec(key + QStringLiteral(":kcal"), order + 0.08, W - shareW, cyRow,
                               {format(meal.kcal)}, QStringLiteral("dg-row-value"),
                               QStringLiteral("end"));
            items << labelSpec(key + QStringLiteral(":share"), order + 0.1, W, cyRow,
                               {shareOf(meal)}, QStringLiteral("dg-row-share"),
                               QStringLiteral("end"));
            for (const char *part : {":name", ":sub", ":kcal", ":share"})
                tips.insert(key + QLatin1String(part), silentTip({key}));
            y += h;
        }
    }
    r.kind = QStringLiteral("nutrition");
    r.width = W;
    r.height = y + 2;
    r.flush = true;
    return r;
}
} // namespace

std::optional<Result> heatKind(Ctx &c, const Lines &lines)
{
    const auto data = parseHeat(lines.lines);
    return data ? finish(c, heatScene(c, *data)) : std::nullopt;
}

std::optional<Result> scatterKind(Ctx &c, const Lines &lines)
{
    const auto data = parseScatter(lines.lines);
    return data ? finish(c, scatterScene(c, *data)) : std::nullopt;
}

std::optional<Result> treemapKind(Ctx &c, const Lines &lines)
{
    auto data = parseTreemap(lines.lines);
    return data ? finish(c, treemapScene(c, std::move(*data))) : std::nullopt;
}

std::optional<Result> gitKind(Ctx &c, const Lines &lines)
{
    const auto data = parseGit(lines.lines);
    return data ? finish(c, gitScene(c, *data)) : std::nullopt;
}

std::optional<Result> arrayKind(Ctx &c, const Lines &lines)
{
    const auto data = parseArray(lines.lines);
    return data ? finish(c, arrayScene(c, *data)) : std::nullopt;
}

std::optional<Result> bracketKind(Ctx &c, const Lines &lines)
{
    auto data = parseBracket(lines.lines);
    return data ? finish(c, bracketScene(c, std::move(*data))) : std::nullopt;
}

std::optional<Result> nutritionKind(Ctx &c, const Lines &lines)
{
    const auto data = parseNutrition(lines.lines);
    return data ? finish(c, nutritionScene(c, *data)) : std::nullopt;
}

} // namespace diagram
