#include "diagram_engine.h"

#include "cssfont.h"
#include "filekinds.h"

#include <QFontInfo>

#include <QDate>
#include <QDateTime>

#include <algorithm>
#include <numeric>

// diagram.js helpers, in its order, and the kinds' table (KINDS, clean,
// compile).
namespace diagram
{
Tones series() { return {1, 2, 3, 4, 5, 6, 7}; }

int layerOf(const Spec &spec)
{
    if (spec.layer != DefaultLayer)
        return spec.layer;
    switch (spec.type) {
    case Type::View:
        return Front;
    case Type::Node:
    case Type::Bar:
    case Type::Arc:
    case Type::Note:
    case Type::Candle:
    case Type::Span:
    case Type::Poly:
    case Type::FSeg:
    case Type::FRow:
        return Nodes;
    case Type::Edge:
    case Type::Ribbon:
    case Type::WBox:
        return Edges;
    case Type::Line:
    case Type::Area:
    case Type::Frame:
    case Type::Cluster:
    case Type::Column:
    case Type::Rect:
    case Type::Hit:
    case Type::FCard:
        return Back;
    case Type::Label:
    case Type::Legend:
    case Type::Dot:
    case Type::Badge:
    case Type::Chip:
    case Type::Figure:
    case Type::WGlyph:
    case Type::FHead:
        return Labels;
    }
    return Labels;
}

/* Context */

Ctx::Ctx(const CompileOptions &options)
    : options(options), probe(1, 1, QImage::Format_ARGB32_Premultiplied)
{
    probe.setDotsPerMeterX(2835);
    probe.setDotsPerMeterY(2835);
    tones = series();
    width = room = std::max(260.0, finite(options.width) ? options.width : 640.0);
}

const Face &Ctx::face(double size, int weight, double spacing, bool mono, bool tabular)
{
    const qint64 key = qint64(std::lround(size * 100)) * 100000 + weight * 10 + (spacing ? 1 : 0) +
                       (mono ? qint64(1) << 50 : 0) + (tabular ? qint64(1) << 51 : 0) +
                       (qint64(std::lround(spacing * 1000)) << 32);
    auto found = faces.find(key);
    if (found != faces.end())
        return found->second;
    QFont font;
    if (mono)
        font.setFamilies({options.mono.isEmpty() ? QStringLiteral("monospace") : options.mono});
    else if (!options.family.isEmpty())
        font.setFamilies({options.family});
    font.setPixelSize(std::max(1, int(std::lround(size * Face::Scale))));
    // The face Chromium picks: 650 is Bold where there is no 650.
    font.setWeight(static_cast<QFont::Weight>(
        clamp(cssfont::nearest(QFontInfo(font).family(), int(clamp(weight, 1, 1000))), 1, 1000)));
    if (spacing)
        font.setLetterSpacing(QFont::AbsoluteSpacing, spacing * size * Face::Scale);
    if (tabular)
        font.setFeature(QFont::Tag("tnum"), 1);
    font.setKerning(true);
    return faces.emplace(key, Face{font, QFontMetricsF(font, &probe)}).first->second;
}

double Ctx::textWidth(const QString &text, double size, int weight, bool mono)
{
    // The same key as upstream's cache: font and text.
    QString key = QString::number(size) + QLatin1Char('/') + QString::number(weight) +
                  (mono ? QLatin1String("m/") : QLatin1String("/")) + text;
    const auto found = widths.constFind(key);
    if (found != widths.constEnd())
        return *found;
    const double w = face(size, weight, 0, mono).advance(text);
    if (widths.size() > 4000)
        widths.clear();
    widths.insert(key, w);
    return w;
}

bool Ctx::fail(const QString &why)
{
    if (error.isEmpty())
        error = why;
    return false;
}

bool Ctx::budget(int units)
{
    spent += units;
    if (spent <= 40'000'000)
        return true;
    return fail(QStringLiteral("This diagram takes too long to lay out."));
}

/* Helpers */

double clamp(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }
double easeOut(double t) { return 1 - std::pow(1 - t, 3); }
double easeInOut(double t) { return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; }

QStringList splitWhitespace(const QString &text)
{
    static const Re space(QStringLiteral("\\s+"));
    return text.split(space, Qt::SkipEmptyParts);
}

QString cutUnits(const QString &text, int n)
{
    if (n > 0 && n < text.size() && text.at(n - 1).isHighSurrogate())
        --n;
    return text.left(std::max(0, n));
}

QString trimEnd(QString s)
{
    int n = int(s.size());
    while (n > 0 && s.at(n - 1).isSpace())
        --n;
    s.truncate(n);
    return s;
}

QString codePoint(qint64 code)
{
    if (code < 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
        return QString(QChar(0xFFFD));
    const char32_t point = char32_t(code);
    return QString::fromUcs4(&point, 1);
}

QString replaceEach(const QString &text, const Re &re, int group)
{
    QString out;
    qsizetype at = 0;
    for (auto it = re.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        out += QStringView(text).mid(at, m.capturedStart() - at);
        out += m.captured(group);
        at = m.capturedEnd();
    }
    out += QStringView(text).mid(at);
    return out;
}

double capsWidth(Ctx &c, const QString &text)
{
    return c.widthOf(text, FONT::eyebrow) + text.size() * FONT::eyebrow.size * TRACK;
}

QString caps(Ctx &c, const QString &text, double max)
{
    QString s = text.toUpper();
    if (capsWidth(c, s) <= max)
        return s;
    while (s.size() > 1 && capsWidth(c, trimEnd(s) + QChar(0x2026)) > max)
        s = cutUnits(s, int(s.size()) - 1);
    return trimEnd(s) + QChar(0x2026);
}

QStringList wrap(Ctx &c, const QString &text, double max, double size, int weight)
{
    QStringList lines;
    for (const auto &paragraph : text.split(QLatin1Char('\n'))) {
        QString line;
        for (const auto &word : splitWhitespace(paragraph)) {
            const QString next = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
            if (!line.isEmpty() && c.textWidth(next, size, weight) > max) {
                lines << line;
                line = word;
            } else {
                line = next;
            }
        }
        lines << line;
    }
    return lines;
}

QStringList wrap(Ctx &c, const QString &text, double max, Font font)
{
    return wrap(c, text, max, font.size, font.weight);
}

QString cleanLabel(const QString &source)
{
    static const Re quoted(QStringLiteral("^\"([\\s\\S]*)\"$")),
        ticked(QStringLiteral("^`([\\s\\S]*)`$"));
    static const Re br(QStringLiteral("<br\\s*/?>"), I), tag(QStringLiteral("</?[a-z][^>]*>"), I);
    static const Re bold(QStringLiteral("\\*\\*(.+?)\\*\\*")), under(QStringLiteral("__(.+?)__")),
        code(QStringLiteral("`([^`]+)`"));
    static const Re entity(QStringLiteral("#(\\d+);")), icon(QStringLiteral("fa:fa-[\\w-]+\\s*"));
    QString s = source.trimmed();
    auto m = quoted.match(s);
    if (m.hasMatch())
        s = m.captured(1);
    m = ticked.match(s);
    if (m.hasMatch())
        s = m.captured(1);
    s.replace(br, QStringLiteral("\n"));
    s.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    s.replace(tag, QString());
    s = replaceEach(s, bold, 1);
    s = replaceEach(s, under, 1);
    s = replaceEach(s, code, 1);
    s.replace(QStringLiteral("#quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("#amp;"), QStringLiteral("&"));
    s.replace(QStringLiteral("#lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("#gt;"), QStringLiteral(">"));
    {
        QString out;
        qsizetype at = 0;
        for (auto it = entity.globalMatch(s); it.hasNext();) {
            const auto e = it.next();
            out += QStringView(s).mid(at, e.capturedStart() - at);
            bool ok = false;
            const qint64 value = e.captured(1).left(12).toLongLong(&ok);
            out += codePoint(ok && e.captured(1).size() <= 12 ? value : -1);
            at = e.capturedEnd();
        }
        out += QStringView(s).mid(at);
        s = std::move(out);
    }
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    s.replace(icon, QString());
    s = s.trimmed();
    return s.size() > MaxLabel ? cutUnits(s, MaxLabel) + QChar(0x2026) : s;
}

QStringList splitStatements(const QString &line)
{
    QStringList out;
    int depth = 0, start = 0;
    bool quote = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('"'))
            quote = !quote;
        else if (quote)
            continue;
        else if (ch == QLatin1Char('[') || ch == QLatin1Char('(') || ch == QLatin1Char('{'))
            ++depth;
        else if (ch == QLatin1Char(']') || ch == QLatin1Char(')') || ch == QLatin1Char('}'))
            depth = std::max(0, depth - 1);
        else if (ch == QLatin1Char(';') && !depth) {
            out << line.mid(start, i - start);
            start = i + 1;
        }
    }
    out << line.mid(start);
    return out;
}

QStringList splitList(const QString &text)
{
    QStringList out;
    bool quote = false;
    QString part;
    for (const QChar ch : text) {
        if (ch == QLatin1Char('"'))
            quote = !quote;
        if (ch == QLatin1Char(',') && !quote) {
            out << part.trimmed();
            part.clear();
            continue;
        }
        part += ch;
    }
    if (!part.trimmed().isEmpty())
        out << part.trimmed();
    return out;
}

QString unquote(const QString &text)
{
    static const Re single(QStringLiteral("^'([\\s\\S]*)'$"));
    const QString s = text.trimmed();
    const auto m = single.match(s);
    return cleanLabel(m.hasMatch() ? m.captured(1) : s);
}

double parseFloatJs(const QString &text)
{
    static const Re number(QStringLiteral(
        "^\\s*([+-]?(?:Infinity|\\d+\\.?\\d*(?:[eE][+-]?\\d+)?|\\.\\d+(?:[eE][+-]?\\d+)?))"));
    const auto m = number.match(text);
    if (!m.hasMatch())
        return NaN;
    const QString s = m.captured(1);
    if (s.endsWith(QLatin1String("Infinity")))
        return s.startsWith(QLatin1Char('-')) ? -INFINITY : INFINITY;
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : NaN;
}

double number(const QString &text)
{
    static const Re space(QStringLiteral("\\s"));
    QString s = text.trimmed();
    s.remove(space);
    if (s.contains(QLatin1Char('.')))
        s.remove(QLatin1Char(','));
    else {
        const int comma = int(s.indexOf(QLatin1Char(',')));
        if (comma >= 0)
            s[comma] = QLatin1Char('.');
    }
    return parseFloatJs(s);
}

std::optional<Amount> amount(const QString &text)
{
    static const Re re(
        QString::fromUtf8("^([^\\d+\\-−–]*?)([+\\-−–]?)\\s*(\\d{1,3}(?:[   ,.']\\d{3})+"
                          "(?:[.,]\\d+)?|\\d+(?:[.,]\\d+)?)\\s*(.*)$"));
    static const Re spaces(QString::fromUtf8("[   ']")),
        thousands(QStringLiteral("^[1-9]\\d{0,2}(,\\d{3})+$")),
        dots(QStringLiteral("^\\d{1,3}(\\.\\d{3}){2,}$"));
    const auto m = re.match(text.trimmed());
    if (!m.hasMatch() || m.captured(1).trimmed().size() > 3)
        return std::nullopt;
    QString digits = m.captured(3);
    digits.remove(spaces);
    const int comma = int(digits.lastIndexOf(QLatin1Char(','))),
              dot = int(digits.lastIndexOf(QLatin1Char('.')));
    if (comma >= 0 && dot >= 0) {
        if (comma > dot) {
            digits.remove(QLatin1Char('.'));
            digits.replace(digits.indexOf(QLatin1Char(',')), 1, QLatin1Char('.'));
        } else {
            digits.remove(QLatin1Char(','));
        }
    } else if (comma >= 0) {
        if (thousands.match(digits).hasMatch())
            digits.remove(QLatin1Char(','));
        else
            digits.replace(digits.indexOf(QLatin1Char(',')), 1, QLatin1Char('.'));
    } else if (dots.match(digits).hasMatch()) {
        digits.remove(QLatin1Char('.'));
    }
    const QString sign = m.captured(2);
    const double value =
        parseFloatJs(digits) * (!sign.isEmpty() && sign != QLatin1String("+") ? -1 : 1);
    if (!finite(value))
        return std::nullopt;
    Amount a;
    a.value = value;
    a.before = m.captured(1).trimmed();
    a.isSigned = !sign.isEmpty();
    a.after = m.captured(4).trimmed();
    return a;
}

QString bare(const QString &raw)
{
    static const Re bullet(QString::fromUtf8("^[-*•]\\s+"));
    QString s = raw.trimmed();
    s.remove(bullet);
    return s;
}

QStringList cellsOf(const QString &line)
{
    static const Re bar(QStringLiteral("\\s*\\|\\s*"));
    QStringList out;
    for (const QString &cell : line.split(bar))
        out << cell.trimmed();
    return out;
}

int indentOf(const QString &raw)
{
    int n = 0;
    for (const QChar ch : raw) {
        if (ch == QLatin1Char('\t'))
            n += 2;
        else if (ch.isSpace())
            n += 1;
        else
            break;
    }
    return n;
}

QStringList rowCells(const QString &line)
{
    if (line.contains(QLatin1Char('|')))
        return cellsOf(line);
    static const Re named(QStringLiteral("^([^:]{1,60}):\\s+(.+)$")),
        note(QStringLiteral("\\s*\\(([^()]*)\\)"));
    const auto m = named.match(line);
    if (!m.hasMatch())
        return {line};
    QStringList notes;
    QString value;
    const QString rest = m.captured(2);
    qsizetype at = 0;
    for (auto it = note.globalMatch(rest); it.hasNext();) {
        const auto n = it.next();
        value += QStringView(rest).mid(at, n.capturedStart() - at);
        notes << n.captured(1).trimmed();
        at = n.capturedEnd();
    }
    value += QStringView(rest).mid(at);
    QStringList out{m.captured(1).trimmed(), value.trimmed()};
    out += notes;
    return out;
}

namespace
{
const Re &totalWord()
{
    // The word ends where its letters do (no \b for Cyrillic).
    static const Re re(QString::fromUtf8("^(total|sum|subtotal|result|итого|"
                                         "итог|всего)(?![\\p{L}\\d])"),
                       I);
    return re;
}
} // namespace

bool sumRow(const QString &label, double value, double sum)
{
    static const Re tail(QStringLiteral("[:.\\s]+$"));
    const auto m = totalWord().match(label);
    if (!m.hasMatch())
        return false;
    QString name = label;
    name.remove(tail);
    return m.capturedLength(0) == name.size() ||
           (finite(value) && sum != 0 && std::abs(value - sum) <= std::abs(sum) * 0.005);
}

QString sumName(const QString &word, const QStringList &lines)
{
    static const Re cyrillic(QString::fromUtf8("[а-яё]"), I);
    if (word.compare(QLatin1String("total"), Qt::CaseInsensitive) == 0 &&
        cyrillic.match(lines.join(QLatin1Char(' '))).hasMatch())
        return QString::fromUtf8("Итого");
    return word.isEmpty() ? word : word.left(1).toUpper() + word.mid(1);
}

QString withUnit(double value, const QString &before, const QString &after)
{
    static const Re tight(QString::fromUtf8("^[%°]"));
    QString out = value < 0 ? QString(QChar(0x2212)) : QString();
    out += before + format(std::abs(value));
    if (!after.isEmpty())
        out += tight.match(after).hasMatch() ? after : QLatin1Char(' ') + after;
    return out;
}

QString formatFixed(double value, int maxFrac, int minFrac, bool group)
{
    if (!finite(value))
        return value != value ? QStringLiteral("NaN")
               : value > 0    ? QString(QChar(0x221e))
                              : QStringLiteral("-") + QChar(0x221e);
    maxFrac = std::clamp(maxFrac, 0, 20);
    const double scale = std::pow(10.0, maxFrac);
    const double rounded = std::round(std::abs(value) * scale) / scale;
    QString digits = QString::number(rounded, 'f', maxFrac);
    if (maxFrac > minFrac) {
        int end = int(digits.size());
        const int dot = int(digits.indexOf(QLatin1Char('.')));
        if (dot >= 0) {
            while (end > dot + 1 + minFrac && digits.at(end - 1) == QLatin1Char('0'))
                --end;
            if (end == dot + 1)
                --end;
            digits.truncate(end);
        }
    }
    if (group) {
        int dot = int(digits.indexOf(QLatin1Char('.')));
        if (dot < 0)
            dot = int(digits.size());
        for (int i = dot - 3; i > 0; i -= 3)
            digits.insert(i, QLatin1Char(','));
    }
    const bool negative = value < 0 && rounded != 0;
    return negative ? QLatin1Char('-') + digits : digits;
}

QString format(double value)
{
    QString out;
    if (value && finite(value) && std::abs(value) < 1) {
        // maximumSignificantDigits: 3.
        const int fraction = std::clamp(2 - int(std::floor(std::log10(std::abs(value)))), 0, 20);
        out = formatFixed(value, fraction);
    } else {
        out = formatFixed(value, 2);
    }
    // Intl writes a negative zero with its sign.
    if (value == 0 && std::signbit(value))
        out = QLatin1Char('-') + out;
    if (out.startsWith(QLatin1Char('-')))
        out.replace(0, 1, QChar(0x2212));
    return out;
}

QString minus(const QString &text)
{
    static const Re hyphen(QString::fromUtf8("(^|[\\s(<>≤≥~≈])-(?=\\d)"));
    QString out;
    qsizetype at = 0;
    for (auto it = hyphen.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        out += QStringView(text).mid(at, m.capturedStart() - at);
        out += m.captured(1) + QChar(0x2212);
        at = m.capturedEnd();
    }
    out += QStringView(text).mid(at);
    return out;
}

QString truncate(Ctx &c, const QString &text, double max, double size, int weight)
{
    if (c.textWidth(text, size, weight) <= max)
        return text;
    int lo = 0, hi = int(text.size());
    while (lo < hi) {
        const int mid = (lo + hi + 1) >> 1;
        if (c.textWidth(trimEnd(cutUnits(text, mid)) + QChar(0x2026), size, weight) <= max)
            lo = mid;
        else
            hi = mid - 1;
    }
    return trimEnd(cutUnits(text, lo)) + QChar(0x2026);
}

double textMax(Ctx &c, const QStringList &lines, double size, int weight)
{
    double w = 0;
    for (const auto &line : lines)
        w = std::max(w, c.textWidth(line, size, weight));
    return w;
}

void spread(QVector<Spreading> &labels, double min, double lo, double hi)
{
    QVector<int> sorted(labels.size());
    std::iota(sorted.begin(), sorted.end(), 0);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [&](int a, int b) { return labels[a].y < labels[b].y; });
    const int n = int(sorted.size());
    for (int i = 1; i < n; ++i)
        labels[sorted[i]].y = std::max(labels[sorted[i]].y, labels[sorted[i - 1]].y + min);
    const double over = n ? labels[sorted[n - 1]].y - hi : 0;
    if (over > 0)
        for (auto &label : labels)
            label.y -= over;
    for (int i = n - 2; i >= 0; --i)
        labels[sorted[i]].y = std::min(labels[sorted[i]].y, labels[sorted[i + 1]].y - min);
    if (n && labels[sorted[0]].y < lo) {
        const double shift = lo - labels[sorted[0]].y;
        for (auto &label : labels)
            label.y += shift;
    }
}

ChipRow chips(Ctx &c, const QVector<ChipEntry> &entries, double x, double y, double maxWidth)
{
    ChipRow out;
    double cx = x, cy = y;
    for (int i = 0; i < entries.size(); ++i) {
        const auto &entry = entries[i];
        const double w = 18 + c.widthOf(entry.text, FONT::small) + 16;
        if (cx > x && cx + w - 16 > x + maxWidth) {
            cx = x;
            cy += 22;
        }
        Spec chip = item(Type::Chip, QStringLiteral("chip:%1").arg(i), 0.2 + i * 0.1, Labels);
        chip.props.x = cx;
        chip.props.y = cy;
        chip.fixed.text = entry.text;
        chip.fixed.tone = entry.tone;
        chip.fixed.mark = entry.mark.isEmpty() ? QStringLiteral("dot") : entry.mark;
        out.items << chip;
        cx += w;
    }
    out.height = cy - y + 22;
    double right = 0;
    for (const auto &chip : out.items)
        right = std::max(right, chip.props.x + 18 + c.widthOf(chip.fixed.text, FONT::small));
    out.width = right - x;
    return out;
}

Title titleOf(Ctx &c, const QString &text, double max)
{
    const QString shown = truncate(c, text, max, FONT::title);
    Title out;
    out.item = labelSpec(QStringLiteral("title"), 0, 0, 10, {shown},
                         QStringLiteral("dg-title-text"), QStringLiteral("start"));
    out.width = c.widthOf(shown, FONT::title);
    return out;
}

double niceCeil(double value)
{
    if (value <= 0 || !finite(value))
        return 1;
    const double mag = std::pow(10.0, std::floor(std::log10(value))), norm = value / mag;
    return (norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 2.5 ? 2.5 : norm <= 5 ? 5 : 10) * mag;
}

int decimalsFor(double step)
{
    return finite(step) && step > 0 ? int(clamp(std::ceil(-std::log10(step) + 0.001), 0, 8)) : 0;
}

double niceStep(double span)
{
    const double rough = span / 5;
    const double mag = std::pow(10.0, std::floor(std::log10(rough ? std::abs(rough) : 1)));
    const double norm = rough / mag;
    return (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * mag;
}

/* Cubic paths */

int segmentCount(const Pts &pts) { return pts.size() < 2 ? 0 : int((pts.size() - 2) / 6); }

namespace
{
std::pair<std::array<double, 8>, std::array<double, 8>> splitCubic(const double *p, double t)
{
    const auto mix = [t](double a, double b) { return a + (b - a) * t; };
    const double x0 = p[0], y0 = p[1], x1 = p[2], y1 = p[3], x2 = p[4], y2 = p[5], x3 = p[6],
                 y3 = p[7];
    const double ax = mix(x0, x1), ay = mix(y0, y1), bx = mix(x1, x2), by = mix(y1, y2),
                 cx = mix(x2, x3), cy = mix(y2, y3);
    const double dx = mix(ax, bx), dy = mix(ay, by), ex = mix(bx, cx), ey = mix(by, cy),
                 fx = mix(dx, ex), fy = mix(dy, ey);
    return {{x0, y0, ax, ay, dx, dy, fx, fy}, {fx, fy, ex, ey, cx, cy, x3, y3}};
}
} // namespace

Pts resample(const Pts &pts, int count)
{
    const int n = segmentCount(pts);
    if (n >= count || n <= 0)
        return pts;
    Pts out{pts[0], pts[1]};
    for (int s = 0; s < n; ++s) {
        std::array<double, 8> seg;
        std::copy(pts.begin() + s * 6, pts.begin() + s * 6 + 8, seg.begin());
        const int parts = count / n + (s < count % n ? 1 : 0);
        for (int j = parts; j > 1; --j) {
            const auto [head, rest] = splitCubic(seg.data(), 1.0 / j);
            for (int k = 2; k < 8; ++k)
                out << head[k];
            seg = rest;
        }
        for (int k = 2; k < 8; ++k)
            out << seg[k];
    }
    return out;
}

Pts straight(double x1, double y1, double x2, double y2)
{
    return {x1 + (x2 - x1) / 3,
            y1 + (y2 - y1) / 3,
            x1 + (x2 - x1) * 2 / 3,
            y1 + (y2 - y1) * 2 / 3,
            x2,
            y2};
}

Pts polyline(const QVector<QPointF> &points)
{
    if (points.isEmpty())
        return {};
    Pts out{points.at(0).x(), points.at(0).y()};
    for (int i = 1; i < points.size(); ++i)
        out += straight(points.at(i - 1).x(), points.at(i - 1).y(), points.at(i).x(),
                        points.at(i).y());
    return out;
}

Pts smoothPts(const QVector<QPointF> &points)
{
    if (points.size() < 3)
        return polyline(points);
    Pts pts{points.at(0).x(), points.at(0).y()};
    const int n = int(points.size());
    for (int i = 0; i < n - 1; ++i) {
        const QPointF p0 = i > 0 ? points.at(i - 1) : points.at(i), p1 = points.at(i),
                      p2 = points.at(i + 1), p3 = i + 2 < n ? points.at(i + 2) : p2;
        const double lo = std::min(p1.y(), p2.y()), hi = std::max(p1.y(), p2.y());
        pts << p1.x() + (p2.x() - p0.x()) / 6 << clamp(p1.y() + (p2.y() - p0.y()) / 6, lo, hi)
            << p2.x() - (p3.x() - p1.x()) / 6 << clamp(p2.y() - (p3.y() - p1.y()) / 6, lo, hi)
            << p2.x() << p2.y();
    }
    return pts;
}

double roughLength(const Pts &pts)
{
    double length = 0;
    for (int i = 2; i + 5 < pts.size(); i += 6)
        length += std::hypot(pts[i + 4] - pts[i - 2], pts[i + 5] - pts[i - 1]);
    return length;
}

Pts shiftPts(Pts pts, double dx, double dy)
{
    for (int i = 0; i < pts.size(); ++i)
        pts[i] += i % 2 ? dy : dx;
    return pts;
}

/* Dates (UTC milliseconds, as Date.UTC) */

double utc(double year, double month, double day, double hour, double minute)
{
    if (!finite(year) || !finite(month) || !finite(day) || !finite(hour) || !finite(minute) ||
        std::abs(year) > 200000 || std::abs(month) > 1e6 || std::abs(day) > 1e8)
        return NaN;
    // Date.UTC reads years 0-99 as 1900-1999.
    year = std::trunc(year);
    if (year >= 0 && year <= 99)
        year += 1900;
    const QDate date = QDate(int(year), 1, 1).addMonths(int(month)).addDays(qint64(day) - 1);
    if (!date.isValid())
        return NaN;
    return double(date.toJulianDay() - 2440588) * DAY + hour * 3600000 + minute * 60000;
}

double today()
{
    const QDate d = QDateTime::currentDateTimeUtc().date();
    return utc(d.year(), d.month() - 1, d.day());
}

double parseDate(const QString &text, const QString &fmt)
{
    static const Re iso(
        QStringLiteral("^(\\d{4})-(\\d{1,2})-(\\d{1,2})(?:[ T](\\d{1,2}):(\\d{2}))?$")),
        dmy(QStringLiteral("^(\\d{1,2})[./-](\\d{1,2})[./-](\\d{4})$")),
        ym(QStringLiteral("^(\\d{4})-(\\d{1,2})$")), y(QStringLiteral("^(\\d{4})$")),
        hm(QStringLiteral("^(\\d{1,2}):(\\d{2})$")), mmFmt(QStringLiteral("^MM"), I),
        yFmt(QStringLiteral("^Y+$"), I);
    const QString s = text.trimmed();
    QRegularExpressionMatch m;
    const auto n = [&](int i) { return m.captured(i).toDouble(); };
    if ((m = iso.match(s)).hasMatch())
        return utc(n(1), n(2) - 1, n(3), m.capturedStart(4) >= 0 ? n(4) : 0,
                   m.capturedStart(5) >= 0 ? n(5) : 0);
    if ((m = dmy.match(s)).hasMatch())
        return mmFmt.match(fmt).hasMatch() ? utc(n(3), n(1) - 1, n(2)) : utc(n(3), n(2) - 1, n(1));
    if ((m = ym.match(s)).hasMatch())
        return utc(n(1), n(2) - 1, 1);
    if ((m = y.match(s)).hasMatch() && yFmt.match(fmt).hasMatch())
        return utc(n(1), 0, 1);
    if ((m = hm.match(s)).hasMatch())
        return utc(1970, 0, 1, n(1), n(2));
    return NaN;
}

double parseDuration(const QString &text)
{
    static const Re duration(QStringLiteral("^(\\d+(?:[.,]\\d+)?)\\s*(ms|min|mo|s|m|h|d|w|M|y)$"));
    const auto m = duration.match(text.trimmed());
    if (!m.hasMatch())
        return NaN;
    const QString u = m.captured(2);
    const double unit = u == QLatin1String("ms")                               ? 1
                        : u == QLatin1String("s")                              ? 1e3
                        : u == QLatin1String("m") || u == QLatin1String("min") ? 6e4
                        : u == QLatin1String("h")                              ? 36e5
                        : u == QLatin1String("d")                              ? DAY
                        : u == QLatin1String("w")                              ? DAY * 7
                        : u == QLatin1String("y")                              ? DAY * 365.25
                                                                               : DAY * 30.44;
    return number(m.captured(1)) * unit;
}

/* Spec builders */

Spec item(Type type, const QString &key, double order, int layer)
{
    Spec spec;
    spec.type = type;
    spec.key = key;
    spec.order = order;
    spec.layer = layer;
    return spec;
}

Spec labelSpec(const QString &key, double order, double x, double y, const QStringList &lines,
               const QString &cls, const QString &anchor, int layer)
{
    Spec spec = item(Type::Label, key, order, layer);
    spec.props.x = x;
    spec.props.y = y;
    spec.fixed.lines = lines;
    spec.fixed.cls = cls;
    spec.fixed.anchor = anchor;
    return spec;
}

Spec lineSpec(const QString &key, double order, double x1, double y1, double x2, double y2,
              const QString &cls, bool draw, int layer)
{
    Spec spec = item(Type::Line, key, order, layer);
    spec.props.x1 = x1;
    spec.props.y1 = y1;
    spec.props.x2 = x2;
    spec.props.y2 = y2;
    spec.fixed.cls = cls;
    spec.fixed.draw = draw ? 1 : 0;
    return spec;
}

/* Files */

FileKind describeFile(const QString &name)
{
    const filekinds::Described d = filekinds::describe(name);
    return {d.kind, d.label, d.glyph, d.tone};
}

QColor fileTone(const QString &name) { return filekinds::tone(name); }

QString formatSize(double bytes) { return filekinds::formatSize(bytes); }

/* Kinds */

namespace
{
// Kinds whose first line may be followed by a `title` line of their own.
const Re &titled()
{
    static const Re re(
        QStringLiteral("^(pie|xychart|candlestick|candles|ohlc|timeline|gantt|quadrantChart|radar|"
                       "journey|kanban|sankey|treemap|metrics|bars|ledger|ranges?|plan|board|"
                       "steps|waterfall|funnel|heatmap|calendar|scatter|array|cells|bracket|"
                       "nutrition|food|meals?|facts|passport|checklist|checks|changes|diff|"
                       "outline|contents|matches|fixtures|words|vocab|vocabulary|glossary|gloss|"
                       "interlinear|forms|conjugation|declension|paradigm|recipe|cooking|parts|"
                       "build|components|bom|settings|setup|toggles|route|itinerary|trip)"),
        I);
    return re;
}

enum class Place { Wide, Column, Plain };
struct KindRow {
    Re test;
    std::function<std::optional<Result>(Ctx &, const Lines &)> build;
    Place place;
};

const QVector<KindRow> &kinds()
{
    static const QVector<KindRow> table = [] {
        const auto re = [](const char *p) { return Re(QString::fromLatin1(p), I); };
        return QVector<KindRow>{
            {re("^(graph|flowchart)\\b"),
             [](Ctx &c, const Lines &l) { return flowKind(c, l, QStringLiteral("flow")); },
             Place::Wide},
            {re("^stateDiagram(-v2)?\\b"),
             [](Ctx &c, const Lines &l) { return flowKind(c, l, QStringLiteral("state")); },
             Place::Wide},
            {re("^sequenceDiagram\\b"), sequenceKind, Place::Wide},
            {re("^pie\\b"), pieKind, Place::Column},
            {re("^xychart(-beta)?\\b"), xyKind, Place::Column},
            {re("^(candlestick|candles|ohlc)\\b"), candleKind, Place::Wide},
            {re("^timeline\\b"), timelineKind, Place::Wide},
            {re("^gantt\\b"), ganttKind, Place::Wide},
            {re("^mindmap\\b"), mindKind, Place::Wide},
            {re("^quadrantChart\\b"), quadrantKind, Place::Column},
            {re("^radar(-beta)?\\b"), radarKind, Place::Column},
            {re("^erDiagram\\b"),
             [](Ctx &c, const Lines &l) { return flowKind(c, l, QStringLiteral("er")); },
             Place::Wide},
            {re("^classDiagram(-v2)?\\b"),
             [](Ctx &c, const Lines &l) { return flowKind(c, l, QStringLiteral("class")); },
             Place::Wide},
            {re("^(wireframe|mockup)\\b"), wireframeKind, Place::Plain},
            {re("^(files|folder)\\b"), filesKind, Place::Plain},
            {re("^metrics\\b"), metricsKind, Place::Column},
            {re("^(bars|ledger)\\b"), barsKind, Place::Column},
            {re("^ranges?\\b"), rangesKind, Place::Column},
            {re("^(plan|board|kanban)\\b"), boardKind, Place::Wide},
            {re("^steps\\b"), stepsKind, Place::Column},
            {re("^journey\\b"), journeyKind, Place::Wide},
            {re("^waterfall\\b"), waterfallKind, Place::Column},
            {re("^funnel\\b"), funnelKind, Place::Column},
            {re("^sankey(-beta)?\\b"), sankeyKind, Place::Column},
            {re("^(heatmap|calendar)\\b"), heatKind, Place::Column},
            {re("^scatter\\b"), scatterKind, Place::Column},
            {re("^treemap(-beta)?\\b"), treemapKind, Place::Column},
            {re("^gitGraph\\b"), gitKind, Place::Column},
            {re("^(array|cells)\\b"), arrayKind, Place::Column},
            {re("^bracket\\b"), bracketKind, Place::Wide},
            {re("^(nutrition|food|meals?)\\b"), nutritionKind, Place::Column},
            {re("^(facts|passport)\\b"), factsKind, Place::Column},
            {re("^(checklist|checks)\\b"), checklistKind, Place::Column},
            {re("^(changes|diff)\\b"), changesKind, Place::Column},
            {re("^(outline|contents)\\b"), outlineKind, Place::Column},
            {re("^(matches|fixtures)\\b"), matchesKind, Place::Column},
            {re("^(words|vocab|vocabulary|glossary)\\b"), wordsKind, Place::Column},
            {re("^(gloss|interlinear)\\b"), glossKind, Place::Column},
            {re("^(forms|conjugation|declension|paradigm)\\b"), formsKind, Place::Column},
            {re("^(recipe|cooking)\\b"), recipeKind, Place::Column},
            {re("^(parts|build|components|bom)\\b"), partsKind, Place::Column},
            {re("^(settings|setup|toggles)\\b"), settingsKind, Place::Column},
            {re("^(route|itinerary|trip)\\b"), routeKind, Place::Column},
        };
    }();
    return table;
}

const KindRow *kindOf(const QString &head)
{
    for (const auto &row : kinds()) {
        if (row.test.match(head).hasMatch())
            return &row;
    }
    return nullptr;
}
} // namespace

int kindCount() { return int(kinds().size()); }

Lines clean(const QString &source)
{
    static const Re front(
        QStringLiteral("^\\s*---[ \\t]*\\r?\\n([\\s\\S]*?)\\r?\\n---[ \\t]*(?:\\r?\\n|$)")),
        named(QStringLiteral("^[ \\t]*title[ \\t]*:[ \\t]*(.+?)[ \\t]*$"), Re::MultilineOption),
        directive(QStringLiteral("%%\\{[\\s\\S]*?\\}%%")), comment(QStringLiteral("%%.*$")),
        titleWord(QStringLiteral("\\btitle\\b"), I), titleLine(QStringLiteral("^\\s*title\\b"), I);
    Lines out;
    const auto f = front.match(source);
    const auto name = f.hasMatch() ? named.match(f.captured(1)) : QRegularExpressionMatch();
    QString body = f.hasMatch() ? source.mid(f.capturedLength(0)) : source;
    body.remove(directive);
    for (QString line : body.split(QLatin1Char('\n'))) {
        line.remove(comment);
        line = trimEnd(line);
        if (!line.trimmed().isEmpty())
            out.lines << line;
    }
    if (name.hasMatch() && !out.lines.isEmpty()) {
        const QString title = unquote(name.captured(1)), head = out.lines.first().trimmed();
        if (!titled().match(head).hasMatch()) {
            out.title = title;
        } else if (!titleWord.match(head).hasMatch() &&
                   std::none_of(out.lines.begin(), out.lines.end(), [&](const QString &line) {
                       return titleLine.match(line).hasMatch();
                   })) {
            out.lines.insert(1, QStringLiteral("title ") + title);
        }
    }
    return out;
}

bool knownHeader(const QString &line) { return kindOf(line.trimmed()) != nullptr; }

std::optional<Result> compileResult(Ctx &c, const QString &source)
{
    Lines lines = clean(source);
    if (lines.lines.isEmpty())
        return c.fail(QStringLiteral("The diagram is empty.")), std::nullopt;
    if (lines.lines.size() > MaxLines)
        return c.fail(QStringLiteral("This diagram has more than %1 lines.").arg(MaxLines)),
               std::nullopt;
    const QString hint = c.options.kind.trimmed();
    if (!hint.isEmpty() && !kindOf(lines.lines.first().trimmed()))
        lines.lines.prepend(hint);
    const KindRow *kind = kindOf(lines.lines.first().trimmed());
    if (!kind)
        return c.fail(QStringLiteral("Unknown diagram type.")), std::nullopt;
    // Laid out in the sizes it draws in: a figure for the text's column and
    // the room past it; a scheme the room whole; files and page sketches in
    // their own sizes.
    const double zoom = kind->place == Place::Plain
                            ? 1
                            : (finite(c.options.zoom) && c.options.zoom > 0 ? c.options.zoom : 1);
    const double width = std::max(1.0, finite(c.options.width) ? c.options.width : 640) / zoom;
    const double column = kind->place == Place::Column && c.options.column > 0
                              ? std::min(width, c.options.column / zoom)
                              : width;
    c.width = column;
    c.room = width;
    auto result = kind->build(c, lines);
    if (!result) {
        if (c.error.isEmpty())
            c.fail(QStringLiteral("This diagram could not be read."));
        return std::nullopt;
    }
    if (c.cancelled())
        return c.fail(QStringLiteral("Drawing was cancelled.")), std::nullopt;
    result->zoom = zoom;
    if (!finite(result->width) || !finite(result->height) || result->width < 0 ||
        result->height < 0)
        return c.fail(QStringLiteral("This diagram could not be laid out.")), std::nullopt;
    if (result->items.size() > MaxItems)
        return c.fail(QStringLiteral("This diagram is too large to draw.")), std::nullopt;
    return result;
}
} // namespace diagram
