#include "diagram_engine.h"

#include <algorithm>

// Figures: metrics, bars/ledger, ranges, plan/board/kanban, journey, steps, waterfall, funnel and
// sankey (diagram.js parseBars … sankeyScene).
namespace diagram
{
namespace
{
// JS's \s and \S take in Unicode spaces, PCRE's only ASCII ones; figures are written with
// no-break spaces (11 600 ₽), so js() spells them out in a pattern written as upstream's.
const QString &jsSpaces()
{
    static const QString s = QStringLiteral("\\s\\x{a0}\\x{1680}\\x{2000}-\\x{200a}\\x{2028}\\x{"
                                            "2029}\\x{202f}\\x{205f}\\x{3000}\\x{feff}");
    return s;
}

Re js(const char *pattern, Re::PatternOptions options = Re::NoPatternOption)
{
    const QString source = QString::fromUtf8(pattern);
    QString out;
    bool inClass = false;
    for (int i = 0; i < source.size(); ++i) {
        const QChar ch = source.at(i);
        if (ch == QLatin1Char('\\') && i + 1 < source.size()) {
            const QChar next = source.at(++i);
            if (next == QLatin1Char('s')) {
                out += inClass ? jsSpaces() : QLatin1Char('[') + jsSpaces() + QLatin1Char(']');
            } else if (next == QLatin1Char('S') && !inClass) {
                out += QStringLiteral("[^") + jsSpaces() + QLatin1Char(']');
            } else {
                out += ch;
                out += next;
            }
            continue;
        }
        if (ch == QLatin1Char('[') && !inClass)
            inClass = true;
        else if (ch == QLatin1Char(']') && inClass)
            inClass = false;
        out += ch;
    }
    return Re(out, options);
}

QString u8(const char *text) { return QString::fromUtf8(text); }

// Math.max/min: a NaN in makes NaN.
double jmax(double a, double b) { return std::isnan(a) || std::isnan(b) ? NaN : std::max(a, b); }
double jmin(double a, double b) { return std::isnan(a) || std::isnan(b) ? NaN : std::min(a, b); }
// Math.round: halves go up.
double jsRound(double v)
{
    double r = std::round(v);
    if (v < 0 && r - v == -0.5)
        r += 1;
    return r;
}
// A NaN sorts as 0 (a NaN comparison counts as equal in JS), keeping the order strict.
double sortKey(double v) { return std::isnan(v) ? 0 : v; }
// `|| 1`.
double orOne(double v) { return v == 0 || std::isnan(v) ? 1 : v; }

// A number as JS writes it into a string (`${t}`): the shortest digits that read back the same.
QString jsNum(double v)
{
    if (std::isnan(v))
        return QStringLiteral("NaN");
    if (std::isinf(v))
        return v > 0 ? QStringLiteral("Infinity") : QStringLiteral("-Infinity");
    if (v == 0)
        return QStringLiteral("0");
    QString e;
    for (int p = 1; p <= 17; ++p) {
        e = QString::number(v, 'e', p - 1);
        if (e.toDouble() == v)
            break;
    }
    const bool negative = e.startsWith(QLatin1Char('-'));
    if (negative)
        e.remove(0, 1);
    const int at = int(e.indexOf(QLatin1Char('e')));
    QString digits = e.left(at);
    digits.remove(QLatin1Char('.'));
    while (digits.size() > 1 && digits.endsWith(QLatin1Char('0')))
        digits.chop(1);
    const int k = int(digits.size()), n = e.mid(at + 1).toInt() + 1;
    QString out;
    if (k <= n && n <= 21)
        out = digits + QString(n - k, QLatin1Char('0'));
    else if (0 < n && n <= 21)
        out = digits.left(n) + QLatin1Char('.') + digits.mid(n);
    else if (-6 < n && n <= 0)
        out = QStringLiteral("0.") + QString(-n, QLatin1Char('0')) + digits;
    else
        out = digits.left(1) + (k > 1 ? QLatin1Char('.') + digits.mid(1) : QString()) +
              QLatin1Char('e') + (n - 1 >= 0 ? QLatin1Char('+') : QLatin1Char('-')) +
              QString::number(std::abs(n - 1));
    return negative ? QLatin1Char('-') + out : out;
}

// `+v.toFixed(10)`.
double fixed10(double v)
{
    if (!finite(v) || std::abs(v) >= 1e21)
        return v;
    return QString::number(v, 'f', 10).toDouble();
}

QString at(const QStringList &list, int i)
{
    return i >= 0 && i < list.size() ? list.at(i) : QString();
}

// A {1,n} in a pattern counts UTF-16 units in JS: the group's length is checked here instead.
bool within(const QRegularExpressionMatch &m, int group, int max)
{
    return m.hasMatch() && m.capturedLength(group) >= 1 && m.capturedLength(group) <= max;
}

const Re &titleLine()
{
    static const Re re = js("^title(?:\\s*:\\s*|\\s+)(.+)$", I);
    return re;
}

// TOTAL_WORD: the word ends where its letters do (no \b for Cyrillic).
const Re &totalWord()
{
    static const Re re = js("^(total|sum|subtotal|result|итого|итог|всего)(?![\\p{L}\\d])", I);
    return re;
}

const QString &sep()
{
    static const QString s = u8(" · ");
    return s;
}

Spec box(Type type, const QString &key, double order, double x, double y, double w, double h,
         int layer = DefaultLayer)
{
    Spec spec = item(type, key, order, layer);
    spec.props.x = x;
    spec.props.y = y;
    spec.props.w = w;
    spec.props.h = h;
    return spec;
}

Spec dot(const QString &key, double order, double x, double y, double r, const QString &cls,
         int tone = NoTone, int layer = DefaultLayer)
{
    Spec spec = item(Type::Dot, key, order, layer);
    spec.props.x = x;
    spec.props.y = y;
    spec.props.r = r;
    spec.fixed.cls = cls;
    spec.fixed.tone = tone;
    return spec;
}

// A label of lines set downward from its y (baseline: 'below').
Spec block(const QString &key, double order, double x, double y, const QStringList &lines,
           const QString &cls, double lineHeight, const QString &anchor = QStringLiteral("start"))
{
    Spec spec = labelSpec(key, order, x, y, lines, cls, anchor);
    spec.fixed.baseline = QStringLiteral("below");
    spec.fixed.lineHeight = lineHeight;
    return spec;
}

Tip silent(const QStringList &hot = {})
{
    Tip tip;
    tip.silent = true;
    tip.hot = hot;
    return tip;
}

Tip tipOf(const QString &title, const QVector<TipRow> &rows)
{
    Tip tip;
    tip.title = title;
    tip.rows = rows;
    return tip;
}

TipRow tipRow(const QString &name, const QString &value, const QString &cls = {})
{
    TipRow row;
    row.name = name;
    row.value = value;
    row.cls = cls;
    return row;
}

void titled(Ctx &c, Result &r, const QString &title, double room)
{
    const Title t = titleOf(c, title, room);
    r.items << t.item;
    r.corner = Corner{t.width + 16, 28, 0};
}

// What upstream would draw with a NaN in it is not drawn at all.
std::optional<Result> finish(Ctx &c, Result r)
{
    const auto bad = [](const Props &p) {
        for (auto member : PropScalars) {
            if (!finite(p.*member))
                return true;
        }
        for (const auto *list : {&p.pts, &p.vs, &p.band}) {
            for (const double v : *list) {
                if (!finite(v))
                    return true;
            }
        }
        return false;
    };
    bool ok = finite(r.width) && finite(r.height);
    for (const Spec &spec : r.items)
        ok = ok && !bad(spec.props) && finite(spec.order);
    if (r.probe) {
        ok = ok && finite(r.probe->x0) && finite(r.probe->x1) && finite(r.probe->y0) &&
             finite(r.probe->y1);
        for (const double x : r.probe->xs)
            ok = ok && finite(x);
    }
    if (!ok)
        return c.fail(QStringLiteral("This diagram could not be laid out.")), std::nullopt;
    return r;
}

/* Bars and the ledger */

struct LedgerUnit {
    QString before, after;
};

struct Bars {
    LedgerData data;
    LedgerUnit unit;
};

std::optional<Bars> parseBars(Ctx &c, const QStringList &lines)
{
    static const Re unitLine = js("^unit(?:\\s*:\\s*|\\s+)(.+)$", I),
                    seriesLine = js("^series(?:\\s*:\\s*|\\s+)(.+)$", I), lead = js("^[:\\s]+"),
                    tail = js("[:.\\s]+$");
    struct Row {
        QString label, note;
        QVector<Amount> values;
    };
    LedgerData data;
    QVector<Row> rows;
    QString unit;
    for (int li = 1; li < lines.size(); ++li) {
        if (!c.budget())
            return std::nullopt;
        const QString line = bare(lines.at(li));
        QRegularExpressionMatch m;
        if (!line.contains(QLatin1Char('|'))) {
            if ((m = titleLine().match(line)).hasMatch()) {
                data.title = unquote(m.captured(1));
                continue;
            }
            if ((m = unitLine.match(line)).hasMatch()) {
                unit = unquote(m.captured(1));
                continue;
            }
            if ((m = seriesLine.match(line)).hasMatch()) {
                data.names.clear();
                for (const QString &name : splitList(m.captured(1)))
                    data.names << unquote(name);
                continue;
            }
            // `total` alone asks for the sum. A word after it is what the line is called; a
            // number after it is the sum as the model counted it, and the word stays as written.
            if ((m = totalWord().match(line)).hasMatch() && !amount(at(rowCells(line), 1))) {
                QString rest = line.mid(m.capturedLength(0));
                rest.remove(lead);
                const QString word = sumName(m.captured(1), lines), named = unquote(rest);
                data.total = amount(rest) ? word : named.isEmpty() ? word : named;
                continue;
            }
        }
        const QStringList cells = rowCells(line);
        const int count = std::max(1, int(data.names.size()));
        // A sum the model wrote out as a row of its own is the total line, not one more bar: the
        // sum is counted here.
        if (count == 1 && cells.size() > 1 && sumRow(unquote(cells.at(0)), NaN, 0)) {
            QString name = unquote(cells.at(0));
            name.remove(tail);
            data.total = name;
            continue;
        }
        QVector<Amount> values;
        for (int k = 1; k < cells.size() && k < 1 + count; ++k) {
            const auto a = amount(cells.at(k));
            if (!a)
                break;
            values << *a;
        }
        if (values.isEmpty())
            continue;
        QStringList notes;
        for (int k = 1 + int(values.size()); k < cells.size(); ++k) {
            if (!cells.at(k).isEmpty())
                notes << unquote(cells.at(k));
        }
        rows << Row{unquote(cells.at(0)), notes.join(sep()), values};
    }
    // The same for a row whose name only opens with such a word, `Итого за день`, when its
    // number is the sum of the rest.
    if (!data.total && rows.size() > 2 && std::all_of(rows.begin(), rows.end(), [](const Row &row) {
            return row.values.size() == 1;
        })) {
        double whole = 0;
        for (const Row &row : rows)
            whole += row.values.at(0).value;
        for (int i = 0; i < rows.size(); ++i) {
            const double v = rows.at(i).values.at(0).value;
            if (sumRow(rows.at(i).label, v, whole - v)) {
                data.total = rows.at(i).label;
                rows.removeAt(i);
                break;
            }
        }
    }
    if (rows.isEmpty())
        return std::nullopt;
    // The unit is the one named, or the one every number carries.
    const Amount head = rows.at(0).values.at(0);
    const auto same = [&](QString Amount::*key) {
        for (const Row &row : rows) {
            for (const Amount &v : row.values) {
                if (v.*key != head.*key)
                    return QString();
            }
        }
        return head.*key;
    };
    const QString before = same(&Amount::before),
                  after = unit.isEmpty() ? same(&Amount::after) : unit;
    data.amount = [before, after](double value) { return withUnit(value, before, after); };
    for (const Row &row : rows) {
        LedgerRow out;
        out.label = row.label;
        out.note = row.note;
        for (const Amount &v : row.values) {
            out.values << LedgerValue{v.value, !before.isEmpty() || !after.isEmpty()
                                                   ? withUnit(v.value, before, after)
                                                   : withUnit(v.value, v.before, v.after)};
        }
        data.rows << out;
    }
    return Bars{data, LedgerUnit{before, after}};
}

// A ledger: names down the left, a thin bar for each number, the numbers in a column on the
// right. One number in a row reads as a ranked list; several become thin bars one under another,
// with a legend. `unit` is parseBars' data.unit (a chart turned on its side has none).
std::optional<Result> ledger(Ctx &c, const LedgerData &data, const LedgerUnit *unit)
{
    const double W = clamp(c.width, 280, LEDGER.max);
    Result r;
    r.kind = QStringLiteral("ledger");
    int count = 1; // upstream's `series`
    for (const LedgerRow &row : data.rows)
        count = std::max(count, int(row.values.size()));
    const bool many = count > 1;
    double top = 0;
    if (!data.title.isEmpty()) {
        titled(c, r, data.title, W - 80);
        top = LEDGER.head;
    }
    if (many && std::any_of(data.names.begin(), data.names.end(),
                            [](const QString &name) { return !name.isEmpty(); })) {
        QVector<ChipEntry> entries;
        for (int k = 0; k < count; ++k) {
            const QString name = at(data.names, k);
            entries << ChipEntry{name.isEmpty() ? QString::number(k + 1) : name, slot(series(), k),
                                 QStringLiteral("bar")};
        }
        const ChipRow row = chips(c, entries, 0, top + 9, W);
        r.items += row.items;
        if (!r.corner)
            r.corner = Corner{row.width + 16, 20, 0};
        top += row.height + 4;
    }
    double lo = 0, hi = 0;
    for (const LedgerRow &row : data.rows) {
        for (const LedgerValue &v : row.values) {
            if (finite(v.value)) {
                lo = std::min(lo, v.value);
                hi = std::max(hi, v.value);
            }
        }
    }
    const double span = orOne(hi - lo);
    std::optional<double> sum;
    if (data.total && !many) {
        double s = 0;
        for (const LedgerRow &row : data.rows) {
            const double v = row.values.isEmpty() ? 0 : row.values.at(0).value;
            s += std::isnan(v) ? 0 : v;
        }
        sum = s;
    }
    const bool shares = sum && *sum != 0 && !std::isnan(*sum);
    const auto share = [&](const LedgerRow &row) {
        const double v = row.values.isEmpty() ? NaN : row.values.at(0).value;
        return format(jsRound(v / *sum * 1000) / 10) + QLatin1Char('%');
    };
    // A unit of several words is said once, over the column of numbers, rather than after every
    // one of them.
    const QString once =
        !many && unit && unit->before.isEmpty() && c.widthOf(unit->after, FONT::amount) > 40
            ? unit->after
            : QString();
    const auto said = [&](double value) {
        if (!once.isEmpty() || !data.amount)
            return withUnit(value, QString(), QString());
        return data.amount(value);
    };
    double valueW = sum ? c.widthOf(said(*sum), FONT::strong) : 0;
    for (const LedgerRow &row : data.rows) {
        for (const LedgerValue &v : row.values)
            valueW = jmax(valueW, c.widthOf(!once.isEmpty() ? said(v.value) : v.text,
                                            many ? FONT::value : FONT::amount));
    }
    double shareW = 0;
    if (shares) {
        double widest = -INFINITY;
        for (const LedgerRow &row : data.rows)
            widest = jmax(widest, c.widthOf(share(row), FONT::tick));
        shareW = widest + 14;
    }
    double widest = -INFINITY;
    for (const LedgerRow &row : data.rows)
        widest = jmax(widest, jmax(c.widthOf(row.label, FONT::row),
                                   row.note.isEmpty() ? 0 : c.widthOf(row.note, FONT::tick)));
    const double labelW = jmin(jmin(W * 0.44, 260), widest);
    if (!once.isEmpty()) {
        r.items << labelSpec(QStringLiteral("lu"), 0.25, W - shareW, top + 7,
                             {truncate(c, once, W * 0.5, FONT::tick)},
                             QStringLiteral("dg-row-note"), QStringLiteral("end"), Back);
        top += 18;
    }
    // The numbers of a lone series stand in a column; those of several ride at the end of their
    // bars.
    const double barX = labelW + 16, barW = jmax(40, W - shareW - valueW - (many ? 8 : 14) - barX);
    const auto x = [&](double v) { return barX + (v - lo) / span * barW; };
    const double zero = x(0);
    double y = top + 2;
    const double first = y;
    for (int i = 0; i < data.rows.size(); ++i) {
        const LedgerRow &row = data.rows.at(i);
        if (!c.budget(int(row.values.size()) + 1))
            return std::nullopt;
        // A note too long for its column takes a second line under the first; past that it is cut.
        QStringList notes;
        if (!row.note.isEmpty())
            notes = many ? QStringList{truncate(c, row.note, labelW, FONT::tick)}
                         : wrap(c, row.note, labelW, FONT::tick);
        const double more = notes.size() > 1 ? 14 : 0;
        if (notes.size() > 2)
            notes = QStringList{
                notes.at(0), truncate(c, notes.mid(1).join(QLatin1Char(' ')), labelW, FONT::tick)};
        const double h = many                  ? jmax(LEDGER.row, count * LEDGER.pitch + 10)
                         : !row.note.isEmpty() ? LEDGER.noted + more
                                               : LEDGER.row;
        const double cy = y + h / 2, order = 0.3 + i * 0.12;
        const QString n = QString::number(i);
        r.items << box(Type::Hit, QStringLiteral("lr:") + n, order, -8, y, W + 16, h, Back);
        r.tips.insert(QStringLiteral("lr:") + n, silent());
        r.items << labelSpec(QStringLiteral("ln:") + n, order, 0,
                             !row.note.isEmpty() ? cy - 8 - more / 2 : cy,
                             {truncate(c, row.label, labelW, FONT::row)},
                             QStringLiteral("dg-row-name"), QStringLiteral("start"));
        if (!row.note.isEmpty()) {
            Spec note = labelSpec(QStringLiteral("lo:") + n, order + 0.05, 0, cy + 9, notes,
                                  QStringLiteral("dg-row-note"), QStringLiteral("start"));
            note.fixed.lineHeight = 14;
            r.items << note;
        }
        for (int k = 0; k < row.values.size(); ++k) {
            const LedgerValue &v = row.values.at(k);
            if (!finite(v.value))
                continue;
            const double by = many ? cy + (k - (count - 1) / 2.0) * LEDGER.pitch : cy;
            const bool back = v.value < 0;
            const double w = jmax(2, std::abs(x(v.value) - zero)), x0 = back ? zero - w : zero;
            Spec bar = box(Type::Span, QStringLiteral("lb:%1:%2").arg(i).arg(k),
                           order + 0.1 + k * 0.03, x0, by, w, many ? 5 : LEDGER.bar);
            bar.fixed.cls = QStringLiteral("dg-ledger-bar");
            bar.fixed.tone = slot(ring(c.tones, count), k);
            bar.fixed.rx = 3;
            bar.fixed.back = back;
            r.items << bar;
            if (many)
                r.items << labelSpec(QStringLiteral("lv:%1:%2").arg(i).arg(k), order + 0.3,
                                     jmax(zero, x0 + w) + 6, by, {v.text},
                                     QStringLiteral("dg-value"), QStringLiteral("start"));
            else
                r.items << labelSpec(QStringLiteral("lv:") + n, order + 0.3, W - shareW, cy,
                                     {!once.isEmpty() ? said(v.value) : v.text},
                                     QStringLiteral("dg-row-value"), QStringLiteral("end"));
        }
        if (shares)
            r.items << labelSpec(QStringLiteral("ls:") + n, order + 0.35, W, cy, {share(row)},
                                 QStringLiteral("dg-row-share"), QStringLiteral("end"));
        y += h;
    }
    if (lo < 0)
        r.items << lineSpec(QStringLiteral("lz"), 0.2, zero, first + 4, zero, y - 4,
                            QStringLiteral("dg-line"), true, Back);
    if (sum) {
        const double order = 0.4 + data.rows.size() * 0.12;
        r.items << lineSpec(QStringLiteral("lt:rule"), order, 0, y + 6, W, y + 6,
                            QStringLiteral("dg-line"), true, Back);
        r.items << labelSpec(QStringLiteral("lt:name"), order + 0.1, 0, y + 23,
                             {truncate(c, *data.total, W * 0.6, FONT::strong)},
                             QStringLiteral("dg-row-total"), QStringLiteral("start"));
        r.items << labelSpec(QStringLiteral("lt:value"), order + 0.2, W - shareW, y + 23,
                             {said(*sum)}, QStringLiteral("dg-row-total"), QStringLiteral("end"));
        y += 36;
    }
    r.width = W;
    r.height = y + 2;
    r.flush = true;
    return finish(c, std::move(r));
}

/* Metrics */

struct Tile {
    QString label, value, note, figure, unit, mood;
    bool change = false;
    QString changeText, changeMood;
    bool target = false;
    double targetValue = 0;
    QString targetText;
    QVector<double> trend; // Empty: none (a trend has three numbers at least).
};

struct Metrics {
    QString title;
    QVector<Tile> tiles;
};

// Figures to take in at a glance. A row is a name and its value and then, in any order: a change
// (+4.2%, −0.6 kg, with good or bad after it to say which way is welcome), a target (of 2200), a
// run of numbers for a small line of the trend, or a note.
std::optional<Metrics> parseMetrics(Ctx &c, const QStringList &lines)
{
    static const Re digit(QStringLiteral("\\d")), leadMinus(QStringLiteral("^-(?=\\d)")),
        moodRe = js("^(?:good|bad|warn)$", I),
        targetRe = js("^(?:of|out of|from|из|\\/)\\s*(.+)$", I),
        changeRe = js("^([+\\-−–▲▼↑↓])\\s*(.*?)(?:\\s+(good|bad))?$", I), runRe = js("[\\s,;]+"),
        figureRe = js("^(\\S*\\d\\S*(?:[ \\x{a0}\\x{202f}]\\d\\S*)*)\\s*(.*)$");
    Metrics data;
    for (int li = 1; li < lines.size(); ++li) {
        if (!c.budget())
            return std::nullopt;
        const QString line = bare(lines.at(li));
        QRegularExpressionMatch m;
        if (!line.contains(QLatin1Char('|')) && (m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        const QStringList cells = rowCells(line);
        // Without bars a row is told from a line of prose by its number: `Note: the figures are
        // rounded` is not a figure.
        if (at(cells, 0).isEmpty() || at(cells, 1).isEmpty() ||
            (!line.contains(QLatin1Char('|')) && !digit.match(cells.at(1)).hasMatch()))
            continue;
        Tile tile;
        tile.label = unquote(cells.at(0));
        tile.value = unquote(cells.at(1)).replace(leadMinus, QString(QChar(0x2212)));
        QString mood;
        for (int k = 2; k < cells.size(); ++k) {
            const QString &cell = cells.at(k);
            if (cell.isEmpty())
                continue;
            const QStringList parts = cell.split(runRe, Qt::SkipEmptyParts);
            if (!c.budget(int(parts.size())))
                return std::nullopt;
            QVector<double> run;
            for (const QString &part : parts)
                run << number(part);
            // good or bad in a cell of its own says which way the change is welcome; it is not a
            // note to print.
            if (moodRe.match(cell).hasMatch()) {
                mood = cell.toLower();
            } else if (!tile.target && (m = targetRe.match(cell)).hasMatch() &&
                       amount(m.captured(1))) {
                tile.target = true;
                tile.targetValue = amount(m.captured(1))->value;
                tile.targetText = unquote(cell);
            } else if (tile.trend.isEmpty() && run.size() >= 3 &&
                       std::all_of(run.begin(), run.end(), [](double v) { return finite(v); })) {
                tile.trend = run;
            } else if (!tile.change && (m = changeRe.match(cell)).hasMatch() &&
                       digit.match(m.captured(2)).hasMatch()) {
                tile.change = true;
                tile.changeText = (u8("+▲↑").contains(m.captured(1)) ? QStringLiteral("+")
                                                                     : QString(QChar(0x2212))) +
                                  m.captured(2);
                tile.changeMood = m.captured(3).toLower();
            } else {
                tile.note = tile.note.isEmpty() ? unquote(cell) : tile.note + sep() + unquote(cell);
            }
        }
        // The figure is the leading run with digits in it; what follows it is the unit. A value
        // of several numbers, 7 h 40 min, is one figure: nothing in it is a unit to set small.
        const auto split = figureRe.match(tile.value);
        const bool whole = !split.hasMatch() || digit.match(split.captured(2)).hasMatch();
        tile.figure = whole ? tile.value : split.captured(1);
        tile.unit = whole ? QString() : split.captured(2);
        // Said of a change, it colours the change; said of the figure alone, it puts a mark by
        // its name.
        if (!mood.isEmpty() && mood != QLatin1String("warn") && tile.change &&
            tile.changeMood.isEmpty())
            tile.changeMood = mood;
        else if (!mood.isEmpty() && !(tile.change && !tile.changeMood.isEmpty()))
            tile.mood = mood;
        data.tiles << tile;
    }
    if (data.tiles.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> metricsScene(Ctx &c, const Metrics &data)
{
    const double W = clamp(c.width, 260, METRICS.max);
    const int n = int(data.tiles.size()), tone = first(c.tones);
    Result r;
    r.kind = QStringLiteral("metrics");
    double top = 0;
    if (!data.title.isEmpty()) {
        titled(c, r, data.title, W - 80);
        top = METRICS.head;
    }
    // Tiles stand in rows of equal columns, parted by hairlines; the last row may be shorter,
    // never a lone tile wider.
    const int fit = std::max(
        1, std::min({n, 4, int(std::floor((W + METRICS.gap) / (METRICS.min + METRICS.gap)))}));
    const int rows = (n + fit - 1) / fit, cols = (n + rows - 1) / rows;
    const double colW = (W - METRICS.gap * (cols - 1)) / cols;
    double y = top + 2;
    for (int row = 0; row < rows; ++row) {
        const int from = row * cols, to = std::min(n, from + cols);
        if (from >= to || !c.budget(to - from))
            break;
        double tall = -INFINITY;
        for (int i = from; i < to; ++i) {
            const Tile &tile = data.tiles.at(i);
            tall = jmax(tall, 57 + (tile.change || !tile.note.isEmpty() ? 20 : 0) +
                                  (tile.target ? 24 : 0) + (!tile.trend.isEmpty() ? 36 : 0));
        }
        if (row)
            r.items << lineSpec(QStringLiteral("mr:%1").arg(row), row, 0, y - 12, W, y - 12,
                                QStringLiteral("dg-grid"), true, Back);
        for (int i = from; i < to; ++i) {
            const Tile &tile = data.tiles.at(i);
            const int k = i - from;
            const double x0 = k * (colW + METRICS.gap), order = 0.2 + i * 0.35;
            const QString key = QStringLiteral("mt:%1").arg(i);
            if (k)
                r.items << lineSpec(key + QStringLiteral(":rule"), order, x0 - METRICS.gap / 2,
                                    y + 2, x0 - METRICS.gap / 2, y + tall - 6,
                                    QStringLiteral("dg-grid"), true, Back);
            const QString name =
                truncate(c, tile.label, colW - (!tile.mood.isEmpty() ? 12 : 0), FONT::legend);
            r.items << labelSpec(key + QStringLiteral(":name"), order, x0, y + 8, {name},
                                 QStringLiteral("dg-metric-name"), QStringLiteral("start"));
            if (!tile.mood.isEmpty())
                r.items << dot(key + QStringLiteral(":mood"), order + 0.03,
                               x0 + c.widthOf(name, FONT::legend) + 9, y + 8.5, 3.5,
                               QStringLiteral("dg-state is-") + tile.mood);
            // The figure keeps its size while it fits the tile, and steps down when it does not.
            double size = METRICS.sizes.back();
            for (const double s : METRICS.sizes) {
                if (c.textWidth(tile.figure, s, FONT::figure.weight) +
                        (!tile.unit.isEmpty() ? c.textWidth(tile.unit, 12, 450) + 5 : 0) <=
                    colW) {
                    size = s;
                    break;
                }
            }
            Spec figure = item(Type::Figure, key + QStringLiteral(":figure"), order + 0.05);
            figure.props.x = x0;
            figure.props.y = y + 41;
            figure.fixed.figure = tile.figure;
            figure.fixed.unit = tile.unit;
            figure.fixed.size = size;
            r.items << figure;
            double ty = y + 63;
            if (tile.change || !tile.note.isEmpty()) {
                double tx = x0;
                if (tile.change) {
                    r.items << labelSpec(
                        key + QStringLiteral(":change"), order + 0.15, tx, ty, {tile.changeText},
                        QStringLiteral("dg-metric-change") +
                            (!tile.changeMood.isEmpty() ? QStringLiteral(" is-") + tile.changeMood
                                                        : QString()),
                        QStringLiteral("start"));
                    tx += c.widthOf(tile.changeText, FONT::change) + 8;
                }
                if (!tile.note.isEmpty() && colW - (tx - x0) > 30)
                    r.items << labelSpec(key + QStringLiteral(":note"), order + 0.2, tx, ty,
                                         {truncate(c, tile.note, colW - (tx - x0), FONT::tick)},
                                         QStringLiteral("dg-metric-note"), QStringLiteral("start"));
                ty += 20;
            }
            const auto now = amount(tile.figure);
            if (tile.target && now && finite(now->value) && tile.targetValue > 0) {
                // How far along: a thin track of the accent, filled up to the share reached.
                const double part = now->value / tile.targetValue;
                const QString percent = format(jsRound(part * 100)) + QLatin1Char('%');
                Spec track = box(Type::Rect, key + QStringLiteral(":track"), order + 0.2, x0,
                                 ty - 4, colW, 4, Nodes);
                track.fixed.cls = QStringLiteral("dg-meter-track");
                track.fixed.tone = tone;
                track.fixed.rx = 2;
                r.items << track;
                Spec fill = box(Type::Span, key + QStringLiteral(":fill"), order + 0.25, x0, ty - 2,
                                jmax(3, colW * clamp01(part)), 4);
                fill.fixed.cls = QStringLiteral("dg-meter-fill");
                fill.fixed.tone = tone;
                fill.fixed.rx = 2;
                r.items << fill;
                r.items << labelSpec(key + QStringLiteral(":part"), order + 0.3, x0, ty + 11,
                                     {percent}, QStringLiteral("dg-value"),
                                     QStringLiteral("start"));
                r.items << labelSpec(
                    key + QStringLiteral(":goal"), order + 0.3, x0 + colW, ty + 11,
                    {truncate(c, tile.targetText, colW - c.widthOf(percent, FONT::value) - 10,
                              FONT::tick)},
                    QStringLiteral("dg-metric-note"), QStringLiteral("end"));
                ty += 24;
            }
            if (!tile.trend.isEmpty()) {
                const QVector<double> &v = tile.trend;
                double low = INFINITY, high = -INFINITY;
                for (const double value : v) {
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
                const double sw = jmin(colW - 6, 150), sh = 22, base = ty + 4;
                QVector<QPointF> points;
                for (int j = 0; j < v.size(); ++j)
                    points << QPointF(x0 + sw * j / (v.size() - 1),
                                      base + sh -
                                          (high > low ? (v.at(j) - low) / (high - low) : 0.5) * sh);
                Spec trend = item(Type::Edge, key + QStringLiteral(":trend"), order + 0.3);
                trend.props.pts = smoothPts(points);
                trend.fixed.style = QStringLiteral("solid");
                trend.fixed.head = QStringLiteral("none");
                trend.fixed.tone = tone;
                trend.fixed.cls = QStringLiteral("dg-stroke dg-spark");
                trend.fixed.draw = 700;
                r.items << trend;
                r.items << dot(key + QStringLiteral(":now"), order + 0.6, points.last().x(),
                               points.last().y(), 3, QStringLiteral("dg-point"), tone);
            }
        }
        y += tall + 18;
    }
    r.width = W;
    r.height = y - 18;
    r.flush = true;
    return finish(c, std::move(r));
}

/* Ranges */

// Values against what is normal for them. A row is a name, the value and the range: 130-170, or
// <5.2, or >30. A bound may be said in words, up to 15 or не менее 4,2; the longer sayings come
// first, so that `не более` is not read as `более`.
const Re &boundHigh()
{
    static const Re re = js("^(?:<=?|≤|до|не более|не больше|не выше|менее|меньше|ниже|макс\\.?|"
                            "max\\.?|under|below|up to|at most|less than|no more than|"
                            "not more than)\\s*(.+)$",
                            I);
    return re;
}

const Re &boundLow()
{
    static const Re re = js("^(?:>=?|≥|от|не менее|не меньше|не ниже|более|больше|выше|мин\\.?|"
                            "min\\.?|over|above|from|at least|more than|no less than|"
                            "not less than)\\s*(.+)$",
                            I);
    return re;
}

const Re &rangeRe()
{
    static const Re re = js("^([+\\-−]?\\d[\\d.,]*?)\\s*(?:-|–|—|\\.{2,}|\\s(?:to|до)\\s)\\s*"
                            "([+\\-−]?\\d[\\d.,]*)(?![\\d.,])",
                            I);
    return re;
}

struct RangeRow {
    QString label, text;
    double value = 0;
    std::optional<double> low, high;
};

struct Ranges {
    QString title;
    QVector<RangeRow> rows;
};

std::optional<Ranges> parseRanges(Ctx &c, const QStringList &lines)
{
    static const Re norm = js("^(?:norm|normal|норма|ref\\.?|reference|референс)[:\\s]*", I),
                    fromRe = js("^(?:от|from)\\s+", I), leadMinus = js("^-(?=\\s*\\d)"),
                    unitRe(QStringLiteral("^[^\\d(]+$"));
    const auto valueOf = [](const QString &text) -> std::optional<double> {
        const auto a = amount(text);
        return a ? std::optional<double>(a->value) : std::nullopt;
    };
    Ranges data;
    for (int li = 1; li < lines.size(); ++li) {
        if (!c.budget())
            return std::nullopt;
        const QString line = bare(lines.at(li));
        QRegularExpressionMatch m;
        if (!line.contains(QLatin1Char('|')) && (m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        const QStringList cells = rowCells(line);
        const auto value = amount(at(cells, 1));
        const QString range = at(cells, 2).remove(norm), both = QString(range).remove(fromRe);
        if (at(cells, 0).isEmpty() || !value)
            continue;
        std::optional<double> low, high;
        QString unit;
        if ((m = rangeRe().match(both)).hasMatch()) {
            low = valueOf(m.captured(1));
            high = valueOf(m.captured(2));
            unit = both.mid(m.capturedLength(0)).trimmed();
        } else if ((m = boundHigh().match(range)).hasMatch()) {
            high = valueOf(m.captured(1));
            const auto a = amount(m.captured(1));
            unit = a ? a->after : QString();
        } else if ((m = boundLow().match(range)).hasMatch()) {
            low = valueOf(m.captured(1));
            const auto a = amount(m.captured(1));
            unit = a ? a->after : QString();
        }
        if (!low && !high)
            continue;
        if (low && high && *low > *high)
            std::swap(low, high);
        // A unit written only beside the range belongs to the value as well.
        const QString text = unquote(cells.at(1)).replace(leadMinus, QString(QChar(0x2212)));
        RangeRow row;
        row.label = unquote(cells.at(0));
        row.value = value->value;
        row.text = value->before.isEmpty() && value->after.isEmpty() && unit.size() <= 12 &&
                           unitRe.match(unit).hasMatch()
                       ? text + QLatin1Char(' ') + unit
                       : text;
        row.low = low;
        row.high = high;
        data.rows << row;
    }
    if (data.rows.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> rangesScene(Ctx &c, const Ranges &data)
{
    const double W = clamp(c.width, 300, RANGES.max);
    const int tone = first(c.tones);
    Result r;
    r.kind = QStringLiteral("ranges");
    double top = 0;
    if (!data.title.isEmpty()) {
        titled(c, r, data.title, W - 80);
        top = RANGES.head;
    }
    double widest = -INFINITY, valueW = -INFINITY;
    for (const RangeRow &row : data.rows) {
        widest = jmax(widest, c.widthOf(row.label, FONT::row));
        valueW = jmax(valueW, c.widthOf(row.text, FONT::amount));
    }
    const double labelW = jmin(jmin(W * 0.34, 220), widest);
    valueW += 16;
    const double x0 = labelW + 20, x1 = W - valueW - 14, span = x1 - x0, za = RANGES.zone[0],
                 zb = RANGES.zone[1];
    // Every normal range lies on the same stretch of its track, so the rows read as one column:
    // left of it is low, right of it is high.
    const auto place = [&](const RangeRow &row) {
        const double value = row.value;
        if (row.low && row.high) {
            const double low = *row.low, high = *row.high;
            return clamp(za + (value - low) / orOne(high - low) * (zb - za), 0.02, 0.98);
        }
        if (row.high) {
            const double high = *row.high;
            return value <= high
                       ? clamp(zb * value / orOne(high), 0.02, zb)
                       : clamp(zb + (value - high) / orOne(std::abs(high)) * (zb - za), zb, 0.98);
        }
        const double low = *row.low;
        return value >= low ? clamp(za + (value - low) / orOne(std::abs(low)) * (zb - za), za, 0.98)
                            : clamp(za * value / orOne(low), 0.02, za);
    };
    double y = top + 2;
    for (int i = 0; i < data.rows.size(); ++i) {
        if (!c.budget())
            return std::nullopt;
        const RangeRow &row = data.rows.at(i);
        const double cy = y + 16, order = 0.3 + i * 0.3, t = place(row);
        const QString key = QStringLiteral("rg:%1").arg(i);
        const bool out = (row.low && row.value < *row.low) || (row.high && row.value > *row.high);
        const double from = !row.low ? 0 : za, to = !row.high ? 1 : zb;
        r.items << box(Type::Hit, key + QStringLiteral(":hit"), order, -8, y - 2, W + 16,
                       RANGES.row - 4, Back);
        r.tips.insert(key + QStringLiteral(":hit"), silent());
        r.items << labelSpec(key + QStringLiteral(":name"), order, 0, cy,
                             {truncate(c, row.label, labelW, FONT::row)},
                             QStringLiteral("dg-row-name"), QStringLiteral("start"));
        r.items << lineSpec(key + QStringLiteral(":track"), order, x0, cy, x1, cy,
                            QStringLiteral("dg-range-track"), true, Back);
        Spec zone = box(Type::Span, key + QStringLiteral(":zone"), order + 0.1, x0 + span * from,
                        cy, span * (to - from), 6, Edges);
        zone.fixed.cls = QStringLiteral("dg-range-zone");
        zone.fixed.rx = 3;
        r.items << zone;
        if (row.low)
            r.items << labelSpec(key + QStringLiteral(":low"), order + 0.15, x0 + span * za,
                                 cy + 15, {format(*row.low)}, QStringLiteral("dg-tick"),
                                 QStringLiteral("middle"), Back);
        if (row.high)
            r.items << labelSpec(key + QStringLiteral(":high"), order + 0.15, x0 + span * zb,
                                 cy + 15, {format(*row.high)}, QStringLiteral("dg-tick"),
                                 QStringLiteral("middle"), Back);
        r.items << dot(
            key + QStringLiteral(":mark"), order + 0.3, x0 + span * t, cy, 5,
            out ? QStringLiteral("dg-range-mark is-out") : QStringLiteral("dg-range-mark"), tone);
        r.items << labelSpec(key + QStringLiteral(":value"), order + 0.2, W, cy, {row.text},
                             QStringLiteral("dg-row-value"), QStringLiteral("end"));
        // Out of range says so twice: the mark changes colour, and an arrow by the number tells
        // which way.
        if (out)
            r.items << labelSpec(key + QStringLiteral(":way"), order + 0.35,
                                 W - c.widthOf(row.text, FONT::amount) - 6, cy,
                                 {row.high && row.value > *row.high ? u8("↑") : u8("↓")},
                                 QStringLiteral("dg-range-way"), QStringLiteral("end"));
        y += RANGES.row;
    }
    r.width = W;
    r.height = y - 8;
    r.flush = true;
    return finish(c, std::move(r));
}

/* Plans: what goes where, and what follows what */

struct BoardCard {
    QString head, text;
    QStringList meta;
};

struct BoardColumn {
    QString title, sub;
    QVector<BoardCard> cards;
};

struct Board {
    QString title;
    QVector<BoardColumn> columns;
};

// Columns with cards: the days of a week with what to do, stages with their tasks, options side
// by side. A line at the left edge opens a column: its name and, after · or a colon, what it is
// about. The lines indented under it are its cards: a text, then notes after |. Mermaid's kanban
// is read the same way.
std::optional<Board> parseBoard(Ctx &c, const QStringList &lines)
{
    static const Re bulletRe = js("^\\s*[-*•]\\s"), extraRe = js("@\\{(.*)\\}\\s*$"),
                    pairRe = js("[\\w-]+\\s*:\\s*(?:'([^']*)'|\"([^\"]*)\"|([^,}]+))"),
                    idRe(QStringLiteral("^[\\w-]*\\[(.*)\\]$")),
                    besideRe = js("^(.+?)\\s+(?:·|—|–|\\|)\\s+(.+)$"),
                    colonRe = js("^([^:]+):\\s+(.+)$"), headRe = js("^([^:|]+):\\s+(.+)$");
    Board data;
    std::optional<int> base;
    std::optional<bool> plain;
    bool card = false; // The last column's last card takes the lines indented under it.
    int deep = 0;
    for (int li = 1; li < lines.size(); ++li) {
        if (!c.budget())
            return std::nullopt;
        const QString &raw = lines.at(li);
        const int indent = indentOf(raw);
        const bool bullet = bulletRe.match(raw).hasMatch();
        QString line = bare(raw);
        QStringList extra;
        QRegularExpressionMatch m;
        if (line.isEmpty())
            continue;
        if (data.columns.isEmpty() && (m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if (!base)
            base = indent;
        // Mermaid writes a card as id[Text]@{ assigned: 'anna', priority: 'High' }.
        if ((m = extraRe.match(line)).hasMatch()) {
            for (auto it = pairRe.globalMatch(m.captured(1)); it.hasNext();) {
                const auto e = it.next();
                const QString v = (e.capturedStart(1) >= 0   ? e.captured(1)
                                   : e.capturedStart(2) >= 0 ? e.captured(2)
                                                             : e.captured(3))
                                      .trimmed();
                if (!v.isEmpty())
                    extra << v;
            }
            line = line.left(m.capturedStart(0)).trimmed();
        }
        if ((m = idRe.match(line)).hasMatch())
            line = m.captured(1);
        // In a list written flat, without indents, the bullets tell the cards from the columns
        // they belong to.
        if (indent <= *base && !(bullet && plain.value_or(false) && !data.columns.isEmpty())) {
            m = besideRe.match(line);
            if (!m.hasMatch()) {
                m = colonRe.match(line);
                if (!within(m, 1, 24))
                    m = QRegularExpressionMatch();
            }
            const bool parts = m.hasMatch();
            data.columns << BoardColumn{cleanLabel(parts ? m.captured(1) : line),
                                        parts ? cleanLabel(m.captured(2)) : QString(),
                                        {}};
            if (!plain)
                plain = !bullet;
            card = false;
            continue;
        }
        if (data.columns.isEmpty())
            continue;
        BoardColumn &column = data.columns.last();
        if (card && indent > deep) {
            column.cards.last().meta << cleanLabel(line);
            continue;
        }
        const QStringList cells = cellsOf(line);
        auto head = headRe.match(cells.at(0));
        if (!within(head, 1, 22))
            head = QRegularExpressionMatch();
        BoardCard next;
        next.head = head.hasMatch() ? cleanLabel(head.captured(1)) : QString();
        next.text = cleanLabel(head.hasMatch() ? head.captured(2) : cells.at(0));
        for (int k = 1; k < cells.size(); ++k) {
            if (!cells.at(k).isEmpty())
                next.meta << cleanLabel(cells.at(k));
        }
        next.meta += extra;
        deep = indent;
        column.cards << next;
        card = true;
    }
    if (data.columns.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> boardScene(Ctx &c, const Board &data)
{
    const double room = std::max(260.0, c.width - PAD * 2);
    const int n = int(data.columns.size());
    Result r;
    r.kind = QStringLiteral("board");
    double top = 0;
    if (!data.title.isEmpty()) {
        titled(c, r, data.title, room - 80);
        top = BOARD.head;
    }
    // As many columns in a row as fit at a width that reads; more wrap into rows of equal length.
    const int fit =
        std::max(1, std::min(n, int(std::floor((room + BOARD.gap) / (BOARD.min + BOARD.gap)))));
    const int rows = (n + fit - 1) / fit, per = (n + rows - 1) / rows;
    const double colW = std::min(BOARD.max, (room - BOARD.gap * (per - 1)) / per),
                 inner = colW - BOARD.pad * 2;
    double y = top;
    for (int row = 0; row < rows; ++row) {
        double tall = 0;
        for (int k = 0; k < per && row * per + k < n; ++k) {
            const int ci = row * per + k;
            const BoardColumn &column = data.columns.at(ci);
            if (!c.budget(int(column.cards.size()) + 1))
                return std::nullopt;
            const double x0 = k * (colW + BOARD.gap), order = 0.2 + ci * 0.3;
            const QString col = QStringLiteral("bc:%1").arg(ci);
            const QString name = truncate(c, column.title, colW, FONT::strong);
            const double nameW = c.widthOf(name, FONT::strong);
            r.items << labelSpec(col, order, x0, y + 9, {name}, QStringLiteral("dg-col-name"),
                                 QStringLiteral("start"));
            double cy = y + 23;
            if (!column.sub.isEmpty()) {
                // What a column is about stands beside its name when there is room, and under it
                // when there is not.
                const bool beside =
                    colW - nameW - 8 >= std::min(64.0, c.widthOf(column.sub, FONT::tick));
                r.items << labelSpec(
                    col + QStringLiteral(":sub"), order + 0.03, beside ? x0 + nameW + 8 : x0,
                    beside ? y + 9.5 : y + 26,
                    {truncate(c, column.sub, beside ? colW - nameW - 8 : colW, FONT::tick)},
                    QStringLiteral("dg-col-sub"), QStringLiteral("start"));
                if (!beside)
                    cy += 16;
            }
            r.items << lineSpec(col + QStringLiteral(":rule"), order + 0.05, x0, cy, x0 + colW, cy,
                                QStringLiteral("dg-line"), true, Back);
            cy += 10;
            for (int j = 0; j < column.cards.size(); ++j) {
                const BoardCard &card = column.cards.at(j);
                const QString key = QStringLiteral("bk:%1:%2").arg(ci).arg(j);
                const double at = order + 0.1 + j * 0.06;
                const QStringList text = wrap(c, card.text, inner, FONT::row);
                QStringList meta;
                for (const QString &line : card.meta)
                    meta += wrap(c, line, inner, FONT::tick);
                const double h = BOARD.pad * 2 - 2 + (!card.head.isEmpty() ? 16 : 0) +
                                 text.size() * 17 + (!meta.isEmpty() ? 3 + meta.size() * 15 : 0);
                Spec plate = box(Type::WBox, key, at, x0, cy, colW, h, Edges);
                plate.fixed.cls = QStringLiteral("dg-plate");
                plate.fixed.rx = 8;
                r.items << plate;
                double ty = cy + BOARD.pad - 1;
                if (!card.head.isEmpty()) {
                    r.items << labelSpec(key + QStringLiteral(":h"), at + 0.02, x0 + BOARD.pad,
                                         ty + 6, {caps(c, card.head, inner)},
                                         QStringLiteral("dg-eyebrow"), QStringLiteral("start"));
                    ty += 16;
                }
                r.items << block(key + QStringLiteral(":t"), at + 0.03, x0 + BOARD.pad, ty, text,
                                 QStringLiteral("dg-row-name"), 17);
                ty += text.size() * 17;
                if (!meta.isEmpty())
                    r.items << block(key + QStringLiteral(":m"), at + 0.04, x0 + BOARD.pad, ty + 3,
                                     meta, QStringLiteral("dg-row-note"), 15);
                for (const QString &part : {key, key + QStringLiteral(":h"),
                                            key + QStringLiteral(":t"), key + QStringLiteral(":m")})
                    r.tips.insert(part, silent({key}));
                cy += h + 8;
            }
            tall = std::max(tall, cy - (!column.cards.isEmpty() ? 8 : 10) - y);
        }
        y += tall + 24;
    }
    r.width = per * colW + BOARD.gap * (per - 1);
    r.height = y - 24;
    return finish(c, std::move(r));
}

// Mermaid's journey: its sections become columns and its tasks cards, each with how it goes, out
// of five, and who is in it.
std::optional<Board> parseJourney(Ctx &c, const QStringList &lines)
{
    static const Re sectionRe = js("^section\\s+(.+)$", I), colon = js("\\s*:\\s*");
    Board data;
    for (int li = 1; li < lines.size(); ++li) {
        if (!c.budget())
            return std::nullopt;
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if ((m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if ((m = sectionRe.match(line)).hasMatch()) {
            data.columns << BoardColumn{cleanLabel(m.captured(1)), QString(), {}};
            continue;
        }
        const QStringList parts = line.split(colon);
        // clamp(NaN) stays NaN upstream: an unread score draws no rating.
        const double rounded = jsRound(number(at(parts, 1)));
        if (parts.at(0).isEmpty())
            continue;
        if (data.columns.isEmpty())
            data.columns << BoardColumn{};
        QString rating;
        if (!std::isnan(rounded)) {
            const int score = int(clamp(rounded, 0, 5));
            rating = QString(score, QChar(0x25cf)) + QString(5 - score, QChar(0x25cb));
        }
        QStringList said;
        for (const QString &part : {rating, parts.mid(2).join(QStringLiteral(", "))}) {
            if (!part.isEmpty())
                said << part;
        }
        BoardCard card;
        card.text = cleanLabel(parts.at(0));
        if (!said.isEmpty())
            card.meta << said.join(sep());
        data.columns.last().cards << card;
    }
    if (std::none_of(data.columns.begin(), data.columns.end(),
                     [](const BoardColumn &column) { return !column.cards.isEmpty(); }))
        return std::nullopt;
    return data;
}

/* Steps */

struct StepNote {
    QString text;
    bool warn = false;
};

struct Step {
    QString text;
    bool done = false;
    QStringList meta;
    QVector<StepNote> notes;
};

struct Steps {
    QString title;
    QVector<Step> steps;
};

// A procedure: steps in order. A line at the left edge is a step, and after | what it takes:
// time, tools, a torque. The lines indented under it are remarks; one that starts with ! is a
// warning. A step that starts with [x] is done.
std::optional<Steps> parseSteps(Ctx &c, const QStringList &lines)
{
    static const Re warnRe = js("^(?:!+|\\x{26a0}\\x{fe0f}?|warning:|caution:|внимание:|"
                                "осторожно:)\\s*",
                                I),
                    numbered = js("^\\d{1,2}[.)]\\s+"),
                    boxRe = js("^(?:\\[([ xXvV✓✔])\\]|([✓✔✅]))\\s*");
    Steps data;
    std::optional<int> base;
    for (int li = 1; li < lines.size(); ++li) {
        if (!c.budget())
            return std::nullopt;
        const QString &raw = lines.at(li);
        const int indent = indentOf(raw);
        const QString line = bare(raw);
        QRegularExpressionMatch m;
        if (line.isEmpty())
            continue;
        if (data.steps.isEmpty() && !line.contains(QLatin1Char('|')) &&
            (m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if (!base)
            base = indent;
        if (indent > *base && !data.steps.isEmpty()) {
            const auto warn = warnRe.match(line);
            data.steps.last().notes
                << StepNote{cleanLabel(line.mid(warn.hasMatch() ? warn.capturedLength(0) : 0)),
                            warn.hasMatch()};
            continue;
        }
        const QStringList cells = cellsOf(QString(line).remove(numbered));
        const auto box = boxRe.match(cells.at(0));
        const QString text =
            cleanLabel(box.hasMatch() ? cells.at(0).mid(box.capturedLength(0)) : cells.at(0));
        if (text.isEmpty())
            continue;
        Step step;
        step.text = text;
        step.done =
            box.hasMatch() && (box.capturedStart(1) >= 0 ? box.captured(1) : QStringLiteral("x")) !=
                                  QLatin1String(" ");
        for (int k = 1; k < cells.size(); ++k) {
            if (!cells.at(k).isEmpty())
                step.meta << cleanLabel(cells.at(k));
        }
        data.steps << step;
    }
    if (data.steps.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> stepsScene(Ctx &c, const Steps &data)
{
    const double W = clamp(c.width, 280, STEPS.max), R = STEPS.mark, textX = R * 2 + 14;
    const int tone = first(c.tones);
    Result r;
    r.kind = QStringLiteral("steps");
    double top = 0;
    if (!data.title.isEmpty()) {
        titled(c, r, data.title, W - 80);
        top = STEPS.head;
    }
    double y = top + 2;
    for (int i = 0; i < data.steps.size(); ++i) {
        const Step &step = data.steps.at(i);
        if (!c.budget(int(step.notes.size()) + 1))
            return std::nullopt;
        const double order = 0.3 + i * 0.45, cy = y + R;
        const QString key = QStringLiteral("st:%1").arg(i);
        // What a step takes stands at the right of its first line; a long list of it goes under
        // the step instead.
        const QString meta = step.meta.join(sep());
        const bool aside = !meta.isEmpty() && c.widthOf(meta, FONT::tick) <= W * 0.42;
        const QStringList lines = wrap(
            c, step.text, W - textX - (aside ? c.widthOf(meta, FONT::tick) + 18 : 0), FONT::step);
        // A step keeps its number until it is done; then a tick takes its place and the step
        // steps back.
        r.items << dot(key + QStringLiteral(":mark"), order, R, cy, R,
                       step.done ? QStringLiteral("dg-step-mark is-done")
                                 : QStringLiteral("dg-step-mark"),
                       tone, Nodes);
        if (step.done) {
            Spec tick = item(Type::WGlyph, key + QStringLiteral(":tick"), order + 0.05);
            tick.props.x = R;
            tick.props.y = cy;
            tick.props.s = 15;
            tick.fixed.icon = QStringLiteral("check");
            tick.fixed.cls = QStringLiteral("is-done");
            tick.fixed.tone = tone;
            r.items << tick;
        } else {
            r.items << labelSpec(key + QStringLiteral(":n"), order + 0.05, R, cy,
                                 {QString::number(i + 1)}, QStringLiteral("dg-step-n"));
        }
        r.items << block(key + QStringLiteral(":t"), order + 0.1, textX, cy - 7, lines,
                         step.done ? QStringLiteral("dg-step-text is-done")
                                   : QStringLiteral("dg-step-text"),
                         18);
        if (aside)
            r.items << labelSpec(key + QStringLiteral(":meta"), order + 0.15, W, cy, {meta},
                                 QStringLiteral("dg-step-meta"), QStringLiteral("end"));
        double ty = cy - 7 + lines.size() * 18;
        if (!meta.isEmpty() && !aside) {
            const QStringList under = wrap(c, meta, W - textX, FONT::tick);
            r.items << block(key + QStringLiteral(":meta"), order + 0.15, textX, ty + 2, under,
                             QStringLiteral("dg-step-meta"), 15);
            ty += 2 + under.size() * 15;
        }
        for (int j = 0; j < step.notes.size(); ++j) {
            const StepNote &note = step.notes.at(j);
            const double shift = note.warn ? 20 : 0;
            const QStringList text = wrap(c, note.text, W - textX - shift, FONT::note);
            if (note.warn) {
                Spec warn =
                    item(Type::WGlyph, key + QStringLiteral(":w%1").arg(j), order + 0.2 + j * 0.05);
                warn.props.x = textX + 7;
                warn.props.y = ty + 12;
                warn.props.s = 15;
                warn.fixed.icon = QStringLiteral("warn");
                warn.fixed.cls = QStringLiteral("is-warn");
                r.items << warn;
            }
            r.items << block(
                key + QStringLiteral(":o%1").arg(j), order + 0.2 + j * 0.05, textX + shift, ty + 4,
                text, note.warn ? QStringLiteral("dg-step-warn") : QStringLiteral("dg-step-note"),
                16);
            ty += 4 + text.size() * 16;
        }
        const double bottom = std::max(ty, cy + R);
        if (i < data.steps.size() - 1)
            r.items << lineSpec(key + QStringLiteral(":rail"), order + 0.2, R, cy + R + 4, R,
                                bottom + STEPS.gap - 4, QStringLiteral("dg-rail"), true, Back);
        y = bottom + STEPS.gap;
    }
    r.width = W;
    r.height = y - STEPS.gap;
    r.flush = true;
    return finish(c, std::move(r));
}

/* Money and flows */

struct FallRow {
    QString label;
    std::optional<double> value;
    bool total = false, isSigned = false;
};

struct Waterfall {
    QString title, unit;
    QVector<FallRow> rows;
};

// How a number comes about: where it starts, what adds to it and what takes away, and where it
// lands. The first row is the start, a signed or a plain number after it is a change, and
// `total` puts the running sum.
std::optional<Waterfall> parseWaterfall(Ctx &c, const QStringList &lines)
{
    static const Re unitLine = js("^unit(?:\\s*:\\s*|\\s+)(.+)$", I),
                    totalRe = js("^(?:=|total|sum|subtotal|итого|итог|всего)\\s*(.*)$", I);
    Waterfall data;
    QStringList units; // A Set: in the order first seen.
    for (int li = 1; li < lines.size(); ++li) {
        if (!c.budget())
            return std::nullopt;
        const QString line = bare(lines.at(li));
        QRegularExpressionMatch m;
        if (!line.contains(QLatin1Char('|'))) {
            if ((m = titleLine().match(line)).hasMatch()) {
                data.title = unquote(m.captured(1));
                continue;
            }
            if ((m = unitLine.match(line)).hasMatch()) {
                data.unit = unquote(m.captured(1));
                continue;
            }
        }
        const QStringList cells = rowCells(line);
        const auto total = totalRe.match(at(cells, 1));
        const auto a = amount(total.hasMatch() ? total.captured(1) : at(cells, 1));
        if (!at(cells, 0).isEmpty() && (a || total.hasMatch()))
            data.rows << FallRow{unquote(cells.at(0)),
                                 a ? std::optional<double>(a->value) : std::nullopt,
                                 total.hasMatch(), a && a->isSigned};
        if (!at(cells, 0).isEmpty() && a) {
            const QString unit = a->before.isEmpty() ? a->after : a->before;
            if (!units.contains(unit))
                units << unit;
        }
    }
    // The unit every number carries stands once, over the scale.
    if (data.unit.isEmpty() && units.size() == 1)
        data.unit = units.at(0);
    if (data.rows.size() <= 1)
        return std::nullopt;
    return data;
}

std::optional<Result> waterfallScene(Ctx &c, const Waterfall &data)
{
    const double W = clamp(c.width, 300, CHART.max);
    const int n = int(data.rows.size());
    Result r;
    r.kind = QStringLiteral("waterfall");
    struct Fall {
        QString label, kind;
        double from = 0, to = 0;
    };
    QVector<Fall> bars;
    double level = 0;
    for (int i = 0; i < n; ++i) {
        const FallRow &row = data.rows.at(i);
        // A bar stands on the floor when it is the start, when it is called a total, and when a
        // model wrote the sum out itself: a last plain number that is the sum so far, or a row
        // named as one.
        const bool sum = i > 0 && !row.isSigned && row.value &&
                         (sumRow(row.label, *row.value, level) ||
                          (i == n - 1 && std::abs(*row.value - level) <= std::abs(level) * 0.005));
        const bool whole = row.total || sum || (i == 0 && !row.isSigned);
        const double from = whole ? 0 : level,
                     to = whole ? row.value.value_or(level) : level + row.value.value_or(NaN);
        level = to;
        bars << Fall{row.label,
                     whole        ? QStringLiteral("total")
                     : to >= from ? QStringLiteral("up")
                                  : QStringLiteral("down"),
                     from, to};
    }
    double lo = 0, hi = 0;
    for (const Fall &bar : bars) {
        lo = jmin(jmin(lo, bar.from), bar.to);
        hi = jmax(jmax(hi, bar.from), bar.to);
    }
    if (hi == lo)
        hi = lo + 1;
    const double step = niceStep(hi - lo);
    // Upstream's scale is NaN here; a bounded loop of ticks needs a real step.
    if (!finite(step) || step <= 0 || !finite(lo) || !finite(hi))
        return c.fail(QStringLiteral("This diagram could not be laid out.")), std::nullopt;
    lo = std::floor(lo / step + 1e-9) * step;
    hi = std::ceil(hi / step - 1e-9) * step;
    QVector<double> ticks;
    for (double v = lo; v <= hi + step * 1e-6 && ticks.size() < 1000; v += step)
        ticks << fixed10(v);
    double head = 0;
    if (!data.title.isEmpty()) {
        titled(c, r, data.title, W - 80);
        head = CHART.head;
    }
    if (!data.unit.isEmpty())
        r.items << labelSpec(QStringLiteral("yt"), 0, 0, head + 8, {data.unit},
                             QStringLiteral("dg-unit"), QStringLiteral("start"), Back);
    const double top = head + (!data.unit.isEmpty() ? 18 : 0) + 18, plotH = CHART.plot;
    double widest = -INFINITY;
    for (const double t : ticks)
        widest = jmax(widest, c.widthOf(format(t), FONT::tick));
    const double left = std::ceil(widest) + 10, plotW = W - left - 4, band = plotW / n;
    const auto y = [&](double v) { return top + plotH - (v - lo) / (hi - lo) * plotH; };
    for (const double t : ticks) {
        r.items << lineSpec(QStringLiteral("g:") + jsNum(t), 0, left, y(t), left + plotW, y(t),
                            t == 0 ? QStringLiteral("dg-grid is-zero") : QStringLiteral("dg-grid"),
                            false, Back);
        r.items << labelSpec(QStringLiteral("t:") + jsNum(t), 0, left - 8, y(t), {format(t)},
                             QStringLiteral("dg-tick"), QStringLiteral("end"), Back);
    }
    const double barW = std::min(34.0, band * 0.56);
    QVector<QStringList> names;
    int most = 0;
    for (const Fall &bar : bars) {
        if (!c.budget())
            return std::nullopt;
        names << wrap(c, bar.label, band - 8, FONT::tick).mid(0, 2);
        most = std::max(most, int(names.last().size()));
    }
    const auto signedText = [](double v) {
        return (v >= 0 ? QStringLiteral("+") : QString(QChar(0x2212))) + format(std::abs(v));
    };
    Probe probe;
    probe.x0 = left;
    probe.x1 = left + plotW;
    probe.y0 = top;
    probe.y1 = top + plotH;
    for (int i = 0; i < n; ++i) {
        const Fall &bar = bars.at(i);
        const bool total = bar.kind == QLatin1String("total");
        const double x = left + band * (i + 0.5) - barW / 2, order = 0.3 + i * 0.35;
        const QString idx = QString::number(i);
        Spec column = item(Type::Bar, QStringLiteral("wb:") + idx, order);
        column.props.x = x;
        column.props.top = y(bar.to);
        column.props.w = barW;
        column.props.base = y(bar.from);
        if (total)
            column.fixed.tone = first(c.tones);
        else
            column.fixed.cls = QStringLiteral("is-") + bar.kind;
        column.fixed.r = 3;
        r.items << column;
        Spec value = labelSpec(QStringLiteral("wv:") + idx, order + 0.4, x + barW / 2,
                               y(jmax(bar.from, bar.to)) - 6,
                               {total ? format(bar.to) : signedText(bar.to - bar.from)},
                               QStringLiteral("dg-value"), QStringLiteral("middle"), Labels);
        value.fixed.baseline = QStringLiteral("above");
        r.items << value;
        // A hairline carries each level over to the next bar, so the eye follows the sum.
        if (i < n - 1)
            r.items << lineSpec(QStringLiteral("wl:") + idx, order + 0.3, x + barW, y(bar.to),
                                left + band * (i + 1.5) - barW / 2, y(bar.to),
                                QStringLiteral("dg-bridge"), true, Back);
        // A word longer than its bar's place is cut like the rest: it must not run into the name
        // beside it.
        QStringList cut;
        for (const QString &line : names.at(i))
            cut << truncate(c, line, band - 4, FONT::tick);
        Spec name = block(QStringLiteral("x:") + idx, 0, left + band * (i + 0.5), top + plotH + 9,
                          cut, QStringLiteral("dg-tick"), 14, QStringLiteral("middle"));
        name.layer = Back;
        r.items << name;
        probe.xs << left + band * (i + 0.5);
        probe.tips << tipOf(
            bar.label,
            total ? QVector<TipRow>{tipRow(QStringLiteral("Total"), format(bar.to))}
                  : QVector<TipRow>{tipRow(QStringLiteral("Change"), signedText(bar.to - bar.from),
                                           QStringLiteral("is-") + bar.kind),
                                    tipRow(QStringLiteral("Total"), format(bar.to))});
        probe.keys << QStringList{QStringLiteral("wb:") + idx};
    }
    r.probe = probe;
    r.width = W;
    r.height = top + plotH + 14 + most * 14;
    r.flush = true;
    return finish(c, std::move(r));
}

struct FunnelRow {
    QString label, text;
    double value = 0;
};

struct Funnel {
    QString title;
    QVector<FunnelRow> rows;
};

// Stages that each keep a part of the one before: visitors, sign-ups, payments.
std::optional<Funnel> parseFunnel(Ctx &c, const QStringList &lines)
{
    Funnel data;
    for (int li = 1; li < lines.size(); ++li) {
        if (!c.budget())
            return std::nullopt;
        const QString line = bare(lines.at(li));
        QRegularExpressionMatch m;
        if (!line.contains(QLatin1Char('|')) && (m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        const QStringList cells = rowCells(line);
        const auto a = amount(at(cells, 1));
        if (!at(cells, 0).isEmpty() && a && a->value >= 0)
            data.rows << FunnelRow{unquote(cells.at(0)), withUnit(a->value, a->before, a->after),
                                   a->value};
    }
    if (data.rows.size() <= 1)
        return std::nullopt;
    return data;
}

std::optional<Result> funnelScene(Ctx &c, const Funnel &data)
{
    const double W = clamp(c.width, 300, 640);
    const QVector<FunnelRow> &rows = data.rows;
    Result r;
    r.kind = QStringLiteral("funnel");
    double top = 0;
    if (!data.title.isEmpty()) {
        titled(c, r, data.title, W - 80);
        top = CHART.head;
    }
    double most = -INFINITY;
    for (const FunnelRow &row : rows)
        most = jmax(most, row.value);
    most = orOne(most);
    const double firstValue = orOne(rows.at(0).value);
    const auto part = [](double value, double of) {
        return format(jsRound(value / orOne(of) * 1000) / 10) + QLatin1Char('%');
    };
    double widest = -INFINITY, valueW = -INFINITY, shareW = -INFINITY;
    for (const FunnelRow &row : rows) {
        widest = jmax(widest, c.widthOf(row.label, FONT::row));
        valueW = jmax(valueW, c.widthOf(row.text, FONT::amount));
        shareW = jmax(shareW, c.widthOf(part(row.value, firstValue), FONT::tick));
    }
    shareW += 12;
    const double labelW = jmin(W * 0.3, widest);
    const double x0 = labelW + 18, x1 = W - valueW - shareW - 18, cx = (x0 + x1) / 2;
    const int tone = first(c.tones);
    double y = top + 2;
    for (int i = 0; i < rows.size(); ++i) {
        if (!c.budget())
            return std::nullopt;
        const FunnelRow &row = rows.at(i);
        const double cy = y + 10, w = jmax(6, (x1 - x0) * row.value / most), order = 0.3 + i * 0.4;
        const QString key = QStringLiteral("fn:%1").arg(i);
        // One colour for every stage: where a stage stands and how wide it is already say the
        // rest.
        Spec bar = box(Type::Span, key, order, cx - w / 2, cy, w, 20);
        bar.fixed.cls = QStringLiteral("dg-funnel-bar");
        bar.fixed.tone = tone;
        bar.fixed.rx = 4;
        bar.fixed.mid = true;
        r.items << bar;
        r.items << labelSpec(key + QStringLiteral(":name"), order, 0, cy,
                             {truncate(c, row.label, labelW, FONT::row)},
                             QStringLiteral("dg-row-name"), QStringLiteral("start"));
        r.items << labelSpec(key + QStringLiteral(":value"), order + 0.2, W - shareW, cy,
                             {row.text}, QStringLiteral("dg-row-value"), QStringLiteral("end"));
        r.items << labelSpec(key + QStringLiteral(":share"), order + 0.25, W, cy,
                             {part(row.value, firstValue)}, QStringLiteral("dg-row-share"),
                             QStringLiteral("end"));
        if (i)
            r.items << labelSpec(key + QStringLiteral(":rate"), order - 0.1, cx, y - 9,
                                 {u8("↓ ") + part(row.value, rows.at(i - 1).value)},
                                 QStringLiteral("dg-funnel-rate"), QStringLiteral("middle"), Back);
        r.tips.insert(
            key, tipOf(row.label, {tipRow(QStringLiteral("Value"), row.text),
                                   tipRow(QStringLiteral("Share"), part(row.value, firstValue))}));
        y += 38;
    }
    r.width = W;
    r.height = y - 18;
    r.flush = true;
    return finish(c, std::move(r));
}

struct SankeyLink {
    QString from, to;
    double value = 0;
};

struct Sankey {
    QString title;
    QVector<SankeyLink> links;
};

// Where something comes from and where it goes: income into spending, traffic into orders.
// Mermaid's lines of source,target,value; `A -> B: 10` and `A | B | 10` are read too.
std::optional<Sankey> parseSankey(Ctx &c, const QStringList &lines)
{
    static const Re arrowRe = js("^(.+?)\\s*(?:-->|->|→)\\s*(.+?)\\s*[:|,]\\s*([^:|,]+)$");
    Sankey data;
    for (int li = 1; li < lines.size(); ++li) {
        if (!c.budget())
            return std::nullopt;
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if ((m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        const auto arrow = arrowRe.match(line);
        const QStringList cells =
            arrow.hasMatch() ? QStringList{arrow.captured(1), arrow.captured(2), arrow.captured(3)}
            : line.contains(QLatin1Char('|')) ? cellsOf(line)
                                              : splitList(line);
        const auto a = cells.size() >= 3 ? amount(cells.at(2)) : std::nullopt;
        if (a && a->value > 0)
            data.links << SankeyLink{unquote(cells.at(0)), unquote(cells.at(1)), a->value};
    }
    if (data.links.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> sankeyScene(Ctx &c, const Sankey &data)
{
    const double W = clamp(c.width, 320, SANKEY.max);
    const int tone = first(c.tones);
    Result r;
    r.kind = QStringLiteral("sankey");
    struct Node {
        QString name;
        QVector<int> ins, outs; // Indices into links.
        int layer = 0;
        double value = 0, y = 0, h = 0, pull = 0;
    };
    struct Link {
        QString from, to;
        double value = 0, y0 = 0, y1 = 0;
        int i = 0, a = 0, b = 0;
    };
    QVector<Node> nodes; // A Map: in the order first named.
    QHash<QString, int> index;
    const auto node = [&](const QString &name) {
        const auto found = index.constFind(name);
        if (found != index.constEnd())
            return *found;
        index.insert(name, int(nodes.size()));
        Node made;
        made.name = name;
        nodes << made;
        return int(nodes.size()) - 1;
    };
    QVector<Link> links;
    for (int i = 0; i < data.links.size(); ++i) {
        const SankeyLink &link = data.links.at(i);
        const int a = node(link.from), b = node(link.to);
        if (a != b) {
            Link made;
            made.from = link.from;
            made.to = link.to;
            made.value = link.value;
            made.i = i;
            made.a = a;
            made.b = b;
            links << made;
        }
    }
    if (links.isEmpty())
        return std::nullopt;
    for (int l = 0; l < links.size(); ++l) {
        nodes[links.at(l).a].outs << l;
        nodes[links.at(l).b].ins << l;
    }
    const int size = int(nodes.size());
    // A node stands one step right of the farthest thing that flows into it; a loop stops pushing
    // after a full round.
    for (int round = 0; round < size; ++round) {
        if (!c.budget(int(links.size())) || c.cancelled())
            return std::nullopt;
        bool moved = false;
        for (const Link &link : links) {
            const int la = nodes.at(link.a).layer;
            if (nodes.at(link.b).layer <= la && la + 1 < size) {
                nodes[link.b].layer = la + 1;
                moved = true;
            }
        }
        if (!moved)
            break;
    }
    int depth = 0;
    for (const Node &n : nodes)
        depth = std::max(depth, n.layer + 1);
    QVector<QVector<int>> layers(depth);
    for (int k = 0; k < size; ++k) {
        Node &n = nodes[k];
        double ins = 0, outs = 0;
        for (const int l : n.ins)
            ins += links.at(l).value;
        for (const int l : n.outs)
            outs += links.at(l).value;
        n.value = jmax(ins, outs);
        layers[n.layer] << k;
    }
    int widest = 0;
    for (const auto &layer : layers)
        widest = std::max(widest, int(layer.size()));
    const double H = clamp(widest * SANKEY.lane, 190, 420);
    double scale = INFINITY;
    for (const auto &layer : layers) {
        double sum = 0;
        for (const int k : layer)
            sum += nodes.at(k).value;
        scale = jmin(scale, (H - SANKEY.gap * (layer.size() - 1)) / sum);
    }
    const auto stack = [&](const QVector<int> &layer) {
        double sum = 0;
        for (const int k : layer)
            sum += jmax(2, nodes.at(k).value * scale);
        double y = (H - sum - SANKEY.gap * (layer.size() - 1)) / 2;
        for (const int k : layer) {
            Node &n = nodes[k];
            n.y = y;
            n.h = jmax(2, n.value * scale);
            y += n.h + SANKEY.gap;
        }
    };
    for (const auto &layer : layers)
        stack(layer);
    // Each node moves toward the middle of what it is tied to, a few passes each way, so bands
    // cross as little as they can.
    for (int sweep = 0; sweep < 6; ++sweep) {
        if (!c.budget(int(links.size()) + size) || c.cancelled())
            return std::nullopt;
        const bool forward = sweep % 2 == 0;
        for (int s = 0; s < depth - 1; ++s) {
            QVector<int> &layer = layers[forward ? s + 1 : depth - 2 - s];
            for (const int k : layer) {
                Node &n = nodes[k];
                double weight = 0, sum = 0;
                for (const int l : forward ? n.ins : n.outs) {
                    const Node &other = nodes.at(forward ? links.at(l).a : links.at(l).b);
                    weight += links.at(l).value;
                    sum += (other.y + other.h / 2) * links.at(l).value;
                }
                n.pull = weight ? sum / weight : n.y + n.h / 2;
            }
            std::stable_sort(layer.begin(), layer.end(), [&](int p, int q) {
                return sortKey(nodes.at(p).pull) < sortKey(nodes.at(q).pull);
            });
            stack(layer);
        }
    }
    double top = 0;
    if (!data.title.isEmpty()) {
        titled(c, r, data.title, W - 80);
        top = CHART.head + 4;
    }
    const auto x = [&](int layer) {
        return depth > 1 ? layer * (W - SANKEY.node) / (depth - 1) : 0.0;
    };
    for (int k = 0; k < size; ++k) {
        QVector<int> outs = nodes.at(k).outs, ins = nodes.at(k).ins;
        std::stable_sort(outs.begin(), outs.end(), [&](int p, int q) {
            return sortKey(nodes.at(links.at(p).b).y) < sortKey(nodes.at(links.at(q).b).y);
        });
        std::stable_sort(ins.begin(), ins.end(), [&](int p, int q) {
            return sortKey(nodes.at(links.at(p).a).y) < sortKey(nodes.at(links.at(q).a).y);
        });
        double out = nodes.at(k).y, into = nodes.at(k).y;
        for (const int l : outs) {
            links[l].y0 = out;
            out += links.at(l).value * scale;
        }
        for (const int l : ins) {
            links[l].y1 = into;
            into += links.at(l).value * scale;
        }
        nodes[k].outs = outs;
        nodes[k].ins = ins;
    }
    for (const Link &link : links) {
        const QString key = QStringLiteral("sk:%1").arg(link.i);
        const Node &a = nodes.at(link.a), &b = nodes.at(link.b);
        const double h = link.value * scale, x0 = x(a.layer) + SANKEY.node, x1 = x(b.layer);
        Spec ribbon = item(Type::Ribbon, key, 0.4 + a.layer * 0.8 + link.y0 / H * 0.5, Edges);
        ribbon.props.band = {x0, top + link.y0, top + link.y0 + h,
                             x1, top + link.y1, top + link.y1 + h};
        ribbon.fixed.tone = tone;
        r.items << ribbon;
        r.tips.insert(key, tipOf(link.from + u8(" → ") + link.to,
                                 {tipRow(QStringLiteral("Value"), format(link.value)),
                                  tipRow(QStringLiteral("Share"),
                                         format(jsRound(link.value / a.value * 1000) / 10) +
                                             QLatin1Char('%'))}));
    }
    // Names stand beside their nodes, inside the drawing: to the right of all but the last column.
    const double room = depth > 1 ? (W - SANKEY.node) / (depth - 1) - SANKEY.node - 16 : W - 20;
    const auto shown = [&](const Node &n) {
        return truncate(c, n.name, jmax(40, room - c.widthOf(format(n.value), FONT::tick) - 8),
                        FONT::legend);
    };
    // The names of the last column stand in the same gap as those of the column before it. Where
    // two would meet, the one before gives way: it is cut short, then loses its number, then is
    // left to the pointer.
    struct Facing {
        double y, left;
    };
    QVector<Facing> facing;
    if (depth > 2) {
        for (const int k : layers.at(depth - 1)) {
            const Node &n = nodes.at(k);
            facing << Facing{top + n.y + n.h / 2, x(depth - 1) - 8 -
                                                      c.widthOf(format(n.value), FONT::tick) - 6 -
                                                      c.widthOf(shown(n), FONT::legend)};
        }
    }
    for (const Node &n : nodes) {
        if (!c.budget(int(facing.size()) + 1))
            return std::nullopt;
        const QString key = QStringLiteral("sn:") + n.name;
        const double nx = x(n.layer), cy = top + n.y + n.h / 2, order = 0.2 + n.layer * 0.8;
        const bool last = n.layer == depth - 1 && depth > 1;
        Spec rect = box(Type::Rect, key, order, nx, top + n.y, SANKEY.node, n.h, Nodes);
        rect.fixed.cls = QStringLiteral("dg-sankey-node");
        rect.fixed.tone = tone;
        rect.fixed.rx = 2;
        r.items << rect;
        const QString text = format(n.value);
        const double from = nx + SANKEY.node + 8, textW = c.widthOf(text, FONT::tick);
        double span = INFINITY;
        if (n.layer == depth - 2) {
            double nearest = INFINITY;
            for (const Facing &f : facing) {
                if (std::abs(f.y - cy) < 16)
                    nearest = jmin(nearest, f.left - 10);
            }
            span = nearest - from;
        }
        if (span < 30)
            continue;
        const bool valued = span >= textW + 6 + 30;
        const QString name =
            finite(span)
                ? truncate(c, n.name,
                           jmin(jmax(40, room - textW - 8), valued ? span - textW - 6 : span),
                           FONT::legend)
                : shown(n);
        const double nameW = c.widthOf(name, FONT::legend);
        r.items << labelSpec(key + QStringLiteral(":name"), order + 0.3,
                             last ? nx - 8 - textW - 6 : from, cy, {name},
                             QStringLiteral("dg-sankey-name"),
                             last ? QStringLiteral("end") : QStringLiteral("start"));
        if (valued)
            r.items << labelSpec(key + QStringLiteral(":value"), order + 0.35,
                                 last ? nx - 8 : from + nameW + 6, cy, {text},
                                 QStringLiteral("dg-sankey-value"),
                                 last ? QStringLiteral("end") : QStringLiteral("start"));
    }
    r.width = W;
    r.height = top + H;
    r.flush = true;
    return finish(c, std::move(r));
}
} // namespace

std::optional<Result> metricsKind(Ctx &c, const Lines &lines)
{
    const auto data = parseMetrics(c, lines.lines);
    return data ? metricsScene(c, *data) : std::nullopt;
}

std::optional<Result> barsKind(Ctx &c, const Lines &lines)
{
    const auto bars = parseBars(c, lines.lines);
    return bars ? ledger(c, bars->data, &bars->unit) : std::nullopt;
}

std::optional<Result> ledgerScene(Ctx &c, const LedgerData &data)
{
    return ledger(c, data, nullptr);
}

std::optional<Result> rangesKind(Ctx &c, const Lines &lines)
{
    const auto data = parseRanges(c, lines.lines);
    return data ? rangesScene(c, *data) : std::nullopt;
}

std::optional<Result> boardKind(Ctx &c, const Lines &lines)
{
    const auto data = parseBoard(c, lines.lines);
    return data ? boardScene(c, *data) : std::nullopt;
}

std::optional<Result> stepsKind(Ctx &c, const Lines &lines)
{
    const auto data = parseSteps(c, lines.lines);
    return data ? stepsScene(c, *data) : std::nullopt;
}

std::optional<Result> journeyKind(Ctx &c, const Lines &lines)
{
    const auto data = parseJourney(c, lines.lines);
    return data ? boardScene(c, *data) : std::nullopt;
}

std::optional<Result> waterfallKind(Ctx &c, const Lines &lines)
{
    const auto data = parseWaterfall(c, lines.lines);
    return data ? waterfallScene(c, *data) : std::nullopt;
}

std::optional<Result> funnelKind(Ctx &c, const Lines &lines)
{
    const auto data = parseFunnel(c, lines.lines);
    return data ? funnelScene(c, *data) : std::nullopt;
}

std::optional<Result> sankeyKind(Ctx &c, const Lines &lines)
{
    const auto data = parseSankey(c, lines.lines);
    return data ? sankeyScene(c, *data) : std::nullopt;
}
} // namespace diagram
