#include "diagram_engine.h"

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QSet>

#include <algorithm>
#include <numeric>

// Mermaid charts: sequence, pie, XY chart, candlestick, timeline, Gantt, mindmap, quadrant and
// radar (diagram.js parseSequence … radarScene).
namespace diagram
{
namespace
{
/* Shared */

constexpr double Pi = 3.14159265358979323846;

const Re &titleLine()
{
    static const Re re(QStringLiteral("^title(?:\\s*:\\s*|\\s+)(.+)$"), I);
    return re;
}

bool defined(const QRegularExpressionMatch &m, int i) { return m.capturedStart(i) >= 0; }

// String(number): the shortest digits that read back, as upstream's keys hold numbers.
QString jsNum(double v)
{
    if (v != v)
        return QStringLiteral("NaN");
    if (std::isinf(v))
        return v > 0 ? QStringLiteral("Infinity") : QStringLiteral("-Infinity");
    if (v == 0)
        return QStringLiteral("0");
    const double a = std::abs(v);
    if (a >= 1e21 || a < 1e-6) {
        const QString s = QString::number(v, 'e', QLocale::FloatingPointShortest);
        const int e = int(s.indexOf(QLatin1Char('e')));
        if (e < 0)
            return s;
        QString exp = s.mid(e + 1);
        const bool negative = exp.startsWith(QLatin1Char('-'));
        if (exp.startsWith(QLatin1Char('-')) || exp.startsWith(QLatin1Char('+')))
            exp.remove(0, 1);
        while (exp.size() > 1 && exp.at(0) == QLatin1Char('0'))
            exp.remove(0, 1);
        return s.left(e) + (negative ? QStringLiteral("e-") : QStringLiteral("e+")) + exp;
    }
    return QString::number(v, 'f', QLocale::FloatingPointShortest);
}

// +v.toFixed(10).
double fixed10(double v)
{
    if (!finite(v) || std::abs(v) >= 1e21)
        return v;
    return QString::number(v, 'f', 10).toDouble();
}

// Math.max(...list), Math.min(...list): ∓Infinity when empty, NaN when any is.
double maxOf(const QVector<double> &list, double init = -INFINITY)
{
    double out = init;
    for (const double v : list) {
        if (v != v)
            return NaN;
        out = std::max(out, v);
    }
    return out;
}
double minOf(const QVector<double> &list, double init = INFINITY)
{
    double out = init;
    for (const double v : list) {
        if (v != v)
            return NaN;
        out = std::min(out, v);
    }
    return out;
}

// [...].filter(Boolean).join(sep).
QString joinSet(const QStringList &parts, const QString &sep)
{
    QStringList kept;
    for (const auto &part : parts) {
        if (!part.isEmpty())
            kept << part;
    }
    return kept.join(sep);
}

// String.replace with a regex without the g flag: the first match only.
QString replaceFirst(const QString &text, const Re &re, const QString &with = {})
{
    const auto m = re.match(text);
    if (!m.hasMatch())
        return text;
    return text.left(m.capturedStart(0)) + with + text.mid(m.capturedEnd(0));
}

// Intl.NumberFormat's compact notation (en): 1.2K, 3.4M, 1,500,000T.
QString compact(double value, int maxFrac)
{
    if (!finite(value))
        return formatFixed(value, maxFrac);
    static const double tiers[] = {1, 1e3, 1e6, 1e9, 1e12};
    static const char *const units[] = {"", "K", "M", "B", "T"};
    const double a = std::abs(value), scale = std::pow(10.0, maxFrac);
    int tier = 0;
    while (tier < 4 && a >= tiers[tier + 1])
        ++tier;
    double shown = std::round(a / tiers[tier] * scale) / scale;
    // Rounded up to a thousand of one unit, it is one of the next.
    if (shown >= 1000 && tier < 4) {
        ++tier;
        shown = std::round(a / tiers[tier] * scale) / scale;
    }
    const QString digits = formatFixed(shown, maxFrac, 0, shown >= 10000);
    return (value < 0 && shown != 0 ? QStringLiteral("-") : QString()) + digits +
           QLatin1String(units[tier]);
}

/* Dates, as Intl.DateTimeFormat writes them in UTC (en, 24 h) */

constexpr double MaxTime = 8.64e15; // JS Date's range.

// QDate counts no year 0 (1 BC is -1); JS's getUTCFullYear does (1 BC is 0), and Intl writes a
// year before 1 without its sign or era (1 BC is 1).
struct Utc {
    QDate date;
    int hour = 0, minute = 0;
    int fullYear() const { return date.year() < 0 ? date.year() + 1 : date.year(); }
    int shownYear() const { return std::abs(date.year()); }
};

Utc asUtc(double t)
{
    if (!finite(t) || std::abs(t) > MaxTime)
        return {};
    const double days = std::floor(t / DAY), rest = t - days * DAY;
    return {QDate::fromJulianDay(qint64(days) + 2440588), int(rest / 3600000),
            int(std::fmod(rest, 3600000) / 60000)};
}

// Date.UTC(year, month, 1): years astronomical, 0 to 99 read as 1900 to 1999, months past 11
// carried into the year.
double dateUtc(double year, double month)
{
    year += std::floor(month / 12);
    month -= 12 * std::floor(month / 12);
    if (year >= 0 && year <= 99)
        year += 1900;
    if (!finite(year) || std::abs(year) > 300000)
        return NaN;
    // Days from 1970-01-01 of a proleptic Gregorian date (days_from_civil).
    qint64 y = qint64(year);
    const int m = int(month) + 1;
    y -= m <= 2;
    const qint64 era = (y >= 0 ? y : y - 399) / 400;
    const qint64 yoe = y - era * 400;
    const qint64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5;
    const qint64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return double(era * 146097 + doe - 719468) * DAY;
}

const char *const MonthShort[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char *const MonthLong[] = {"January",   "February", "March",    "April",
                                 "May",       "June",     "July",     "August",
                                 "September", "October",  "November", "December"};

QString two(int v) { return QStringLiteral("%1").arg(v, 2, 10, QLatin1Char('0')); }

// { day: 'numeric', month: 'short' }: Jan 5.
QString dayMonth(double t)
{
    const QDate d = asUtc(t).date;
    if (!d.isValid())
        return {};
    return QStringLiteral("%1 %2").arg(QLatin1String(MonthShort[d.month() - 1])).arg(d.day());
}
// { hour: '2-digit', minute: '2-digit' } in en-US: 09:30 AM, 12:00 PM.
QString clock(int hour, int minute)
{
    const int h = hour % 12 == 0 ? 12 : hour % 12;
    return two(h) + QLatin1Char(':') + two(minute) +
           (hour < 12 ? QLatin1String(" AM") : QLatin1String(" PM"));
}
QString hourMinute(double t)
{
    const Utc u = asUtc(t);
    return u.date.isValid() ? clock(u.hour, u.minute) : QString();
}
// { month: 'short' }: Jan.
QString monthName(double t)
{
    const QDate d = asUtc(t).date;
    return d.isValid() ? QString(QLatin1String(MonthShort[d.month() - 1])) : QString();
}
// { month: 'short', year: 'numeric' }: Jan 2024.
QString monthYear(double t)
{
    const Utc u = asUtc(t);
    if (!u.date.isValid())
        return {};
    return QStringLiteral("%1 %2")
        .arg(QLatin1String(MonthShort[u.date.month() - 1]))
        .arg(u.shownYear());
}
// { year: 'numeric' }.
QString yearOf(double t)
{
    const Utc u = asUtc(t);
    return u.date.isValid() ? QString::number(u.shownYear()) : QString();
}
// { month: 'long', year: 'numeric'[, day: 'numeric'][, hour, minute] }:
// January 5, 2024 at 09:30 AM.
QString longDate(double t, bool day, bool time = false)
{
    const Utc u = asUtc(t);
    if (!u.date.isValid())
        return {};
    QString out = QLatin1String(MonthLong[u.date.month() - 1]);
    out += day ? QStringLiteral(" %1, %2").arg(u.date.day()).arg(u.shownYear())
               : QStringLiteral(" %1").arg(u.shownYear());
    if (time)
        out += QStringLiteral(" at ") + clock(u.hour, u.minute);
    return out;
}
// { day: '2-digit', month: '2-digit', year: '2-digit' }: 01/05/24.
QString shortDay(double t)
{
    const Utc u = asUtc(t);
    if (!u.date.isValid())
        return {};
    return two(u.date.month()) + QLatin1Char('/') + two(u.date.day()) + QLatin1Char('/') +
           two(u.shownYear() % 100);
}
// { month: '2-digit', year: 'numeric' }: 01/2024.
QString shortMonth(double t)
{
    const Utc u = asUtc(t);
    return u.date.isValid()
               ? two(u.date.month()) + QLatin1Char('/') + QString::number(u.shownYear())
               : QString();
}

Spec dotSpec(const QString &key, double order, double x, double y, double r, const QString &cls,
             int tone, int layer = DefaultLayer)
{
    Spec spec = item(Type::Dot, key, order, layer);
    spec.props.x = x;
    spec.props.y = y;
    spec.props.r = r;
    spec.fixed.cls = cls;
    spec.fixed.tone = tone;
    return spec;
}

Corner cornerAt(double x, double h)
{
    Corner corner;
    corner.x = x;
    corner.h = h;
    return corner;
}

// An edge needs a segment: a lone point gets one of no length.
Pts drawable(Pts pts)
{
    if (pts.size() == 2)
        pts += straight(pts[0], pts[1], pts[0], pts[1]);
    return pts;
}

/* Sequence diagrams */

struct SeqActor {
    QString id, label;
    int index = 0, tone = NoTone;
    double w = 0, x = 0;
};

struct SeqFrame {
    QString kind, label, key;
    int depth = 0;
    double top = 0, bottom = NaN, x = 0, w = 0;
};

struct SeqEvent {
    enum Type { Message, Note, Open, Divide, Close } type = Message;
    int frame = -1, from = -1, to = -1;
    bool dashed = false;
    QString head, text, side, label;
    QVector<int> actors;
    QStringList lines;
    double width = 0, x = 0, y = 0, w = 0, h = 0;
    int row = 0, number = 0;
};

struct SeqData {
    QVector<SeqActor> actors;
    QVector<SeqFrame> frames;
    QVector<SeqEvent> events;
    bool numbered = false;
    QString title;
};

std::optional<SeqData> parseSequence(Ctx &c, const QStringList &lines)
{
    static const Re autonumber(QStringLiteral("^autonumber\\b"), I),
        ignored(QStringLiteral("^(activate|deactivate|accTitle|accDescr|create|destroy|link|links|"
                               "properties|details)\\b"),
                I),
        participant(QStringLiteral("^(participant|actor)\\s+(.+?)(?:\\s+as\\s+(.+))?$"), I),
        quotes(QStringLiteral("^\"|\"$")), box(QStringLiteral("^box\\b"), I),
        open(QStringLiteral("^(loop|alt|opt|par|critical|break|rect)\\b\\s*(.*)$"), I),
        divide(QStringLiteral("^(else|and|option)\\b\\s*(.*)$"), I),
        end(QStringLiteral("^end$"), I),
        note(QStringLiteral("^note\\s+(right of|left of|over)\\s+([^:]+?)\\s*:\\s*(.*)$"), I),
        message(QStringLiteral("^(.+?)\\s*(--?)(>>|>|x|\\))\\s*[+-]?\\s*(.+?)\\s*:\\s*(.*)$"));
    SeqData data;
    QHash<QString, int> actorAt;
    QVector<int> stack; // Frames; -1 for a box (or a frame nested past MaxFrameDepth).
    const auto actor = [&](QString id, const QString *label = nullptr) {
        id = id.trimmed();
        int at = actorAt.value(id, -1);
        if (at < 0) {
            if (data.actors.size() >= MaxActors)
                return c.fail(QStringLiteral("This diagram has more than %1 participants.")
                                  .arg(MaxActors)),
                       -1;
            at = int(data.actors.size());
            SeqActor a;
            a.id = a.label = id;
            data.actors << a;
            actorAt.insert(id, at);
        }
        if (label && !label->isEmpty())
            data.actors[at].label = cleanLabel(*label);
        return at;
    };
    const auto add = [&](const SeqEvent &e) {
        if (data.events.size() >= MaxEvents)
            return c.fail(QStringLiteral("This diagram has more than %1 steps.").arg(MaxEvents));
        data.events << e;
        return true;
    };
    for (int li = 1; li < lines.size() && c.error.isEmpty(); ++li) {
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if (line.isEmpty())
            continue;
        if (autonumber.match(line).hasMatch()) {
            data.numbered = true;
            continue;
        }
        if ((m = titleLine().match(line)).hasMatch()) {
            data.title = cleanLabel(m.captured(1));
            continue;
        }
        if (ignored.match(line).hasMatch())
            continue;
        if ((m = participant.match(line)).hasMatch()) {
            QString id = m.captured(2);
            id.remove(quotes);
            const QString label = m.captured(3);
            actor(id, defined(m, 3) ? &label : nullptr);
            continue;
        }
        if (box.match(line).hasMatch()) {
            stack << -1;
            continue;
        }
        if ((m = open.match(line)).hasMatch()) {
            const int depth =
                int(std::count_if(stack.begin(), stack.end(), [](int f) { return f >= 0; }));
            if (depth >= MaxFrameDepth) {
                stack << -1;
                continue;
            }
            SeqFrame frame;
            frame.kind = m.captured(1).toLower();
            frame.label =
                frame.kind == QLatin1String("rect") ? QString() : cleanLabel(m.captured(2));
            frame.depth = depth;
            data.frames << frame;
            stack << int(data.frames.size()) - 1;
            SeqEvent e;
            e.type = SeqEvent::Open;
            e.frame = int(data.frames.size()) - 1;
            add(e);
            continue;
        }
        if ((m = divide.match(line)).hasMatch()) {
            int frame = -1;
            for (int i = int(stack.size()) - 1; i >= 0 && frame < 0; --i)
                frame = stack[i];
            if (frame >= 0) {
                SeqEvent e;
                e.type = SeqEvent::Divide;
                e.frame = frame;
                e.label = cleanLabel(m.captured(2));
                add(e);
            }
            continue;
        }
        if (end.match(line).hasMatch()) {
            const int frame = stack.isEmpty() ? -1 : stack.takeLast();
            if (frame >= 0) {
                SeqEvent e;
                e.type = SeqEvent::Close;
                e.frame = frame;
                add(e);
            }
            continue;
        }
        if ((m = note.match(line)).hasMatch()) {
            SeqEvent e;
            e.type = SeqEvent::Note;
            e.side = m.captured(1).toLower();
            for (const auto &id : m.captured(2).split(QLatin1Char(','))) {
                const int a = actor(id);
                if (a < 0)
                    break;
                e.actors << a;
            }
            e.text = cleanLabel(m.captured(3));
            if (!e.actors.isEmpty())
                add(e);
            continue;
        }
        if ((m = message.match(line)).hasMatch()) {
            SeqEvent e;
            e.from = actor(m.captured(1));
            e.to = actor(m.captured(4));
            if (e.from < 0 || e.to < 0)
                break;
            e.dashed = m.captured(2) == QLatin1String("--");
            e.head = m.captured(3);
            e.text = cleanLabel(m.captured(5));
            add(e);
        }
    }
    if (!c.error.isEmpty())
        return std::nullopt;
    while (!stack.isEmpty()) {
        const int frame = stack.takeLast();
        if (frame >= 0) {
            SeqEvent e;
            e.type = SeqEvent::Close;
            e.frame = frame;
            data.events << e;
        }
    }
    if (data.actors.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> sequenceScene(Ctx &c, SeqData data)
{
    auto &actors = data.actors;
    auto &frames = data.frames;
    for (int i = 0; i < actors.size(); ++i) {
        actors[i].index = i;
        actors[i].tone = first(c.tones);
        actors[i].w = std::max(84.0, std::ceil(c.textWidth(actors[i].label)) + 28);
    }
    const int n = int(actors.size());
    QVector<double> gaps(std::max(0, n - 1));
    for (int i = 0; i < n - 1; ++i)
        gaps[i] = std::max(SEQ.gap, (actors[i].w + actors[i + 1].w) / 2 + 28);
    const auto need = [&](int i, int j, double width) {
        if (i == j) {
            if (i < n - 1)
                gaps[i] = std::max(gaps[i], width + SEQ.self + 20);
            return;
        }
        const int a = std::min(i, j), b = std::max(i, j);
        double span = 0;
        for (int k = a; k < b; ++k)
            span += gaps[k];
        if (span < width) {
            for (int k = a; k < b; ++k)
                gaps[k] += (width - span) / (b - a);
        }
    };
    for (auto &e : data.events) {
        if (!c.budget(n))
            return std::nullopt;
        if (e.type == SeqEvent::Message) {
            e.lines = wrap(c, e.text, 260, FONT::message);
            e.width = textMax(c, e.lines, FONT::message) + (data.numbered ? 56 : 34);
            need(e.from, e.to, e.width);
        } else if (e.type == SeqEvent::Note) {
            e.lines = wrap(c, e.text, 210, FONT::note);
            e.width = textMax(c, e.lines, FONT::note) + 22;
            if (e.actors.size() > 1)
                need(e.actors.first(), e.actors.last(), e.width - 40);
        }
    }
    double x = SEQ.pad + actors[0].w / 2;
    for (int i = 0; i < n; ++i) {
        actors[i].x = x;
        x += i < gaps.size() ? gaps[i] : 0;
    }
    const double right = actors[n - 1].x + actors[n - 1].w / 2 + SEQ.pad;
    // A title stands over the people of the exchange.
    const double top = !data.title.isEmpty() ? CHART.head + 6 : 0;
    double y = top + SEQ.head + 20;
    int number = 0, row = 0;
    for (auto &e : data.events) {
        e.row = row++;
        if (e.type == SeqEvent::Message) {
            e.y = y + e.lines.size() * 15 + 5;
            e.number = data.numbered ? ++number : 0;
            y = e.y + (e.from == e.to ? SEQ.self + 14 : 18);
        } else if (e.type == SeqEvent::Note) {
            e.h = e.lines.size() * 16 + 12;
            QVector<double> xs;
            for (const int a : e.actors)
                xs << actors[a].x;
            if (e.side == QLatin1String("over")) {
                e.w = e.actors.size() > 1 ? maxOf(xs) - minOf(xs) + 60 : std::max(e.width, 90.0);
                e.x = e.actors.size() > 1 ? minOf(xs) - 30 : xs[0] - e.w / 2;
            } else if (e.side == QLatin1String("right of")) {
                e.x = xs[0] + 12;
                e.w = e.width;
            } else {
                e.x = xs[0] - 12 - e.width;
                e.w = e.width;
            }
            e.y = y;
            y += e.h + 12;
        } else if (e.type == SeqEvent::Open) {
            frames[e.frame].top = y;
            y += 28;
        } else if (e.type == SeqEvent::Divide) {
            e.y = y;
            y += 26;
        } else {
            frames[e.frame].bottom = y;
            y += 12;
        }
    }
    const double bottom = y + 6;
    double minX = 0, maxX = right;
    for (const auto &e : data.events) {
        if (e.type == SeqEvent::Note) {
            minX = std::min(minX, e.x - 4);
            maxX = std::max(maxX, e.x + e.w + 4);
        }
    }
    const auto sx = [&](double v) { return v - minX; };
    Result out;
    out.kind = QStringLiteral("sequence");
    auto &items = out.items;
    double titleW = 0;
    if (!data.title.isEmpty()) {
        const Title title = titleOf(c, data.title, std::max(240.0, maxX - minX - 80));
        items << title.item;
        titleW = title.width;
        out.corner = cornerAt(title.width + 16, 28);
    }
    for (int i = 0; i < n; ++i) {
        const auto &a = actors[i];
        Spec node = item(Type::Node, QStringLiteral("a:") + a.id, i * 0.3);
        node.props.x = sx(a.x);
        node.props.y = top + SEQ.head / 2;
        node.props.w = a.w;
        node.props.h = SEQ.head;
        node.fixed.shape = QStringLiteral("actor");
        node.fixed.lines = QStringList{a.label};
        node.fixed.tone = a.tone;
        node.fixed.id = a.id;
        items << node;
        Spec life = lineSpec(QStringLiteral("life:") + a.id, i * 0.3 + 0.4, sx(a.x), top + SEQ.head,
                             sx(a.x), bottom, QStringLiteral("dg-life"), true, Back);
        life.fixed.tone = a.tone;
        items << life;
    }
    const double base = 1 + n * 0.3;
    int message = 0, note = 0, divider = 0;
    for (const auto &e : data.events) {
        const double order = base + e.row * 0.75;
        if (e.type == SeqEvent::Message) {
            const int k = message++;
            const double x1 = sx(actors[e.from].x), x2 = sx(actors[e.to].x);
            const bool self = e.from == e.to;
            const double dir = x2 > x1 ? 1 : x2 < x1 ? -1 : 1;
            const QString head = e.head == QLatin1String(">>")  ? QStringLiteral("arrow")
                                 : e.head == QLatin1String(")") ? QStringLiteral("open")
                                 : e.head == QLatin1String("x") ? QStringLiteral("cross")
                                                                : QStringLiteral("none");
            const double cut = head == QLatin1String("arrow") || head == QLatin1String("open")
                                   ? ARROW.length + 1
                                   : 2;
            Spec edge = item(Type::Edge, QStringLiteral("m:%1").arg(k), order);
            edge.props.pts = self ? polyline({{x1, e.y},
                                              {x1 + SEQ.self, e.y},
                                              {x1 + SEQ.self, e.y + SEQ.self},
                                              {x1 + cut + 1, e.y + SEQ.self}})
                                  : polyline({{x1 + dir * 2, e.y}, {x2 - dir * cut, e.y}});
            edge.fixed.style = e.dashed ? QStringLiteral("dashed") : QStringLiteral("solid");
            edge.fixed.head = head;
            edge.fixed.both = false;
            edge.fixed.tone = actors[e.from].tone;
            edge.fixed.grow = true;
            edge.fixed.cls = QStringLiteral("dg-message");
            items << edge;
            Spec label = labelSpec(QStringLiteral("ml:%1").arg(k), order + 0.2,
                                   self ? x1 + SEQ.self + 8 : (x1 + x2) / 2, e.y - 7, e.lines,
                                   QStringLiteral("dg-message-label"),
                                   self ? QStringLiteral("start") : QStringLiteral("middle"));
            label.fixed.baseline = QStringLiteral("above");
            label.fixed.lineHeight = 15;
            items << label;
            if (e.number) {
                Spec badge = item(Type::Badge, QStringLiteral("mb:%1").arg(k), order);
                badge.props.x = x1 + dir * 14;
                badge.props.y = e.y;
                badge.fixed.n = QString::number(e.number);
                badge.fixed.tone = actors[e.from].tone;
                items << badge;
            }
        } else if (e.type == SeqEvent::Note) {
            Spec spec = item(Type::Note, QStringLiteral("note:%1").arg(note++), order);
            spec.props.x = sx(e.x);
            spec.props.y = e.y;
            spec.props.w = e.w;
            spec.props.h = e.h;
            spec.fixed.lines = e.lines;
            items << spec;
        } else if (e.type == SeqEvent::Open) {
            SeqFrame &fr = frames[e.frame];
            const double inset = 8 + fr.depth * 8;
            fr.key = QStringLiteral("f:%1").arg(e.row);
            fr.x = sx(inset);
            fr.w = right - inset * 2;
            Spec spec = item(Type::Frame, fr.key, order);
            spec.props.x = fr.x;
            spec.props.y = fr.top;
            spec.props.w = fr.w;
            spec.props.h = std::max(20.0, (finite(fr.bottom) ? fr.bottom : bottom) - fr.top);
            spec.fixed.kind = fr.kind.toUpper();
            spec.fixed.label = fr.label;
            items << spec;
        } else if (e.type == SeqEvent::Divide) {
            const SeqFrame &fr = frames[e.frame];
            const int k = divider++;
            items << lineSpec(QStringLiteral("d:%1").arg(k), order, fr.x, e.y, fr.x + fr.w, e.y,
                              QStringLiteral("dg-divider"), false, Back);
            if (!e.label.isEmpty())
                items << labelSpec(QStringLiteral("dl:%1").arg(k), order, fr.x + 10, e.y + 13,
                                   {e.label}, QStringLiteral("dg-frame-label"),
                                   QStringLiteral("start"), Back);
        }
    }
    out.width = std::max(maxX - minX, titleW);
    out.height = bottom;
    return out;
}

/* Pie charts */

struct PieItem {
    QString label;
    double value = 0;
    bool other = false;
};

struct PieData {
    QString title;
    QVector<PieItem> items;
    bool showData = false;
};

std::optional<PieData> parsePie(const QStringList &lines)
{
    static const Re titleHead(QStringLiteral("\\btitle\\s+(.+)$"), I),
        showHead(QStringLiteral("\\bshowData\\b"), I), showLine(QStringLiteral("^showData$"), I),
        slice(QStringLiteral(
            "^(?:\"([^\"]*)\"|'([^']*)'|([^:]+?))\\s*:\\s*([-+]?[\\d\\s.,]+)\\s*%?\\s*$"));
    PieData data;
    const auto head = titleHead.match(lines.value(0));
    QString title = head.hasMatch() ? head.captured(1) : QString();
    data.showData = showHead.match(lines.value(0)).hasMatch();
    for (int li = 1; li < lines.size(); ++li) {
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if ((m = titleLine().match(line)).hasMatch()) {
            title = m.captured(1);
            continue;
        }
        if (showLine.match(line).hasMatch()) {
            data.showData = true;
            continue;
        }
        if ((m = slice.match(line)).hasMatch()) {
            const double value = number(m.captured(4));
            if (value > 0 && finite(value) && data.items.size() < MaxSlices) {
                const int g = defined(m, 1) ? 1 : defined(m, 2) ? 2 : 3;
                data.items << PieItem{cleanLabel(m.captured(g)), value, false};
            }
        }
    }
    if (data.items.isEmpty())
        return std::nullopt;
    data.title = unquote(title);
    return data;
}

std::optional<Result> pieScene(Ctx &c, const PieData &data)
{
    // Past seven slices the smallest fold into one: more colours than that stop telling slices
    // apart.
    QVector<PieItem> entries = data.items;
    if (entries.size() > PIE.max) {
        QVector<int> sorted(entries.size());
        std::iota(sorted.begin(), sorted.end(), 0);
        std::stable_sort(sorted.begin(), sorted.end(),
                         [&](int a, int b) { return entries[a].value > entries[b].value; });
        QSet<int> kept;
        for (int i = 0; i < PIE.max - 1; ++i)
            kept.insert(sorted[i]);
        QVector<PieItem> folded;
        double rest = 0;
        for (int i = 0; i < entries.size(); ++i) {
            if (kept.contains(i))
                folded << entries[i];
            else
                rest += entries[i].value;
        }
        folded << PieItem{QStringLiteral("Other"), rest, true};
        entries = folded;
    }
    double total = 0;
    for (const auto &e : entries)
        total += e.value;
    if (!finite(total) || total <= 0)
        return std::nullopt;
    const bool percents = std::abs(total - 100) < 0.5, valued = data.showData && !percents;
    const auto percent = [&](double value) {
        return formatFixed(value / total * 100, percents || value / total < 0.1 ? 1 : 0) +
               QLatin1Char('%');
    };
    struct Row {
        QString label, value, share;
    };
    QVector<Row> rows;
    for (const auto &e : entries)
        rows << Row{e.label, valued ? format(e.value) : percent(e.value),
                    valued ? percent(e.value) : QString()};
    // The ring stands on the left edge of the text, the legend is a small table beside it; in a
    // narrow place it goes under.
    const double room = std::max(240.0, c.width), R = PIE.radius, size = (R + PIE.width / 2) * 2,
                 cc = size / 2;
    double valueW = -INFINITY, shareW = -INFINITY, labelW = -INFINITY;
    for (const auto &r : rows) {
        valueW = std::max(valueW, c.widthOf(r.value, FONT::amount));
        shareW = std::max(shareW, c.widthOf(r.share, FONT::tick));
        labelW = std::max(labelW, c.widthOf(r.label, FONT::legend));
    }
    shareW = valued ? shareW + 16 : 0;
    const double fixedW = PIE.swatch + 10 + 24 + valueW + shareW, natural = fixedW + labelW;
    const bool beside = size + PIE.legend + std::min(natural, 220.0) <= room;
    const double legendW =
        std::min(beside ? room - size - PIE.legend : room, std::max(natural, PIE.table));
    const double legendH = rows.size() * PIE.row;
    Result out;
    out.kind = QStringLiteral("pie");
    auto &items = out.items;
    double top = 0;
    if (!data.title.isEmpty()) {
        const Title title = titleOf(c, data.title, room - 80);
        items << title.item;
        top = PIE.head;
        out.corner = cornerAt(title.width + 16, 28);
    }
    const double W = beside ? size + PIE.legend + legendW : std::max(size, legendW);
    const double H = top + (beside ? std::max(size, legendH) : size + 18 + legendH);
    const double donutY = top + (beside ? (H - top - size) / 2 : 0);
    const double legendX = beside ? size + PIE.legend : 0,
                 legendY = beside ? top + (H - top - legendH) / 2 : top + size + 18;
    int lead = 0;
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i].value > entries[lead].value)
            lead = i;
    }
    double start = 0;
    for (int i = 0; i < entries.size(); ++i) {
        const auto &e = entries[i];
        const double share = e.value / total * 100;
        const int tone = e.other ? int(Mute) : slot(series(), i);
        Spec arc = item(Type::Arc, QStringLiteral("s:%1").arg(i), start / 100 * 4);
        arc.props.start = start;
        arc.props.sweep = share;
        arc.fixed.tone = tone;
        arc.fixed.cx = cc;
        arc.fixed.cy = donutY + cc;
        arc.fixed.r = R;
        arc.fixed.width = PIE.width;
        arc.fixed.gap = entries.size() > 1 ? PIE.gap : 0;
        arc.fixed.index = i;
        arc.fixed.active = i == lead;
        items << arc;
        Spec legend = item(Type::Legend, QStringLiteral("r:%1").arg(i), 0.6 + i * 0.25);
        legend.props.x = legendX;
        legend.props.y = legendY + PIE.row * (i + 0.5);
        legend.fixed.label = truncate(c, rows[i].label, legendW - fixedW, FONT::legend);
        legend.fixed.value = rows[i].value;
        legend.fixed.share = rows[i].share;
        legend.fixed.tone = tone;
        legend.fixed.w = legendW;
        legend.fixed.valueX = legendW - shareW;
        legend.fixed.index = i;
        legend.fixed.active = i == lead;
        items << legend;
        start += share;
    }
    const auto centerLabel = [&](const QString &label) {
        return truncate(c, label, 2 * R - PIE.width - 22, FONT::tick);
    };
    Spec value = labelSpec(QStringLiteral("c:v"), 3, cc, donutY + cc - 6,
                           {percent(entries[lead].value)}, QStringLiteral("dg-center-value"));
    value.fixed.pop = true;
    items << value;
    items << labelSpec(QStringLiteral("c:l"), 3.2, cc, donutY + cc + 13,
                       {centerLabel(entries[lead].label)}, QStringLiteral("dg-center-label"));
    out.width = W;
    out.height = H;
    out.flush = true;
    PieFocus pie;
    pie.top = lead;
    pie.count = int(entries.size());
    for (const auto &e : entries) {
        pie.values << percent(e.value);
        pie.labels << centerLabel(e.label);
    }
    out.pie = pie;
    return out;
}

/* XY charts */

struct XYSeries {
    QString type, name;
    QVector<double> values;
};
struct XYGoal {
    QString label;
    double value = 0;
};
struct XYZone {
    QString label, a, b;
    bool along = false;
};
struct XYMark {
    QString label, at;
};
struct XYData {
    QString title, xTitle, yTitle;
    QStringList labels;
    std::optional<double> min, max;
    std::optional<std::pair<double, double>> range;
    QVector<XYSeries> series;
    QVector<XYGoal> goals;
    QVector<XYZone> zones;
    QVector<XYMark> marks;
    bool stacked = false, horizontal = false, log = false;
};

constexpr int MaxNotes = 64; // Goals, zones and marks, each.

// Besides Mermaid's own lines, a chart takes `area` series, `goal "Name" 2200` for a level to
// reach, `zone "Name" 135 --> 160` for a band of values, and `stacked` or `horizontal`, in the
// header or on a line of their own.
std::optional<XYData> parseXY(const QStringList &lines)
{
    static const Re stackedWord(QStringLiteral("\\bstacked\\b"), I),
        horizontalWord(QStringLiteral("\\bhorizontal\\b"), I),
        logWord(QStringLiteral("\\blog\\b"), I),
        rangeOf(QStringLiteral("([-\\d.]+(?:e[-+]?\\d+)?)\\s*-->\\s*([-\\d.]+(?:e[-+]?\\d+)?)"), I),
        stackedLine(QStringLiteral("^stacked$"), I),
        horizontalLine(QStringLiteral("^horizontal$"), I),
        logLine(QStringLiteral("^log(arithmic)?$"), I),
        xAxis(QStringLiteral("^x-axis\\s*(.*)$"), I), list(QStringLiteral("\\[(.*)\\]")),
        yAxis(QStringLiteral("^y-axis\\s*(.*)$"), I),
        logTail(QStringLiteral("(?:^|\\s)log(?:arithmic)?\\s*$"), I),
        goal(QStringLiteral("^(?:goal|target|limit)\\b\\s*(?:\"([^\"]*)\"\\s*)?:?\\s*"
                            "([-+]?[\\d.]+(?:e[-+]?\\d+)?)\\s*$"),
             I),
        zone(QString::fromUtf8("^(x-?zone|span|period|zone|band)\\b\\s*(?:\"([^\"]*)\"\\s*)?:?"
                               "\\s*(.+?)\\s*(?:-->|->|\\.{2,}|–|—|\\sto\\s)\\s*(.+?)\\s*$"),
             I),
        valueBand(QStringLiteral("^(zone|band)$"), I),
        mark(QStringLiteral("^(?:mark|marker|event)\\b\\s*(?:\"([^\"]*)\"\\s*)?:?\\s*(.+?)\\s*$"),
             I),
        series(QStringLiteral("^(bar|line|area)\\b\\s*(?:\"([^\"]*)\"\\s*)?\\[(.*)\\]\\s*$"), I);
    XYData data;
    const QString head = lines.value(0);
    data.stacked = stackedWord.match(head).hasMatch();
    data.horizontal = horizontalWord.match(head).hasMatch();
    data.log = logWord.match(head).hasMatch();
    bool hasLabels = false;
    int values = 0;
    // A bound that cannot be read is no bound (upstream would carry NaN into the scale).
    const auto bound = [](double v) { return finite(v) ? std::optional<double>(v) : std::nullopt; };
    for (int li = 1; li < lines.size(); ++li) {
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if ((m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if (stackedLine.match(line).hasMatch()) {
            data.stacked = true;
            continue;
        }
        if (horizontalLine.match(line).hasMatch()) {
            data.horizontal = true;
            continue;
        }
        if (logLine.match(line).hasMatch()) {
            data.log = true;
            continue;
        }
        if ((m = xAxis.match(line)).hasMatch()) {
            const QString rest = m.captured(1);
            const auto l = list.match(rest), range = rangeOf.match(rest);
            const QString name = replaceFirst(replaceFirst(rest, list), rangeOf).trimmed();
            if (!name.isEmpty())
                data.xTitle = unquote(name);
            if (l.hasMatch()) {
                data.labels.clear();
                for (const auto &label : splitList(l.captured(1))) {
                    if (data.labels.size() >= MaxValues)
                        break;
                    data.labels << unquote(label);
                }
                hasLabels = true;
            } else if (range.hasMatch()) {
                data.range = std::make_pair(number(range.captured(1)), number(range.captured(2)));
            }
            continue;
        }
        if ((m = yAxis.match(line)).hasMatch()) {
            QString rest = m.captured(1);
            const auto range = rangeOf.match(rest);
            if (range.hasMatch()) {
                data.min = bound(number(range.captured(1)));
                data.max = bound(number(range.captured(2)));
                rest = replaceFirst(rest, rangeOf);
            }
            // The word log after the name asks for a scale in powers of ten.
            if (logTail.match(rest).hasMatch()) {
                data.log = true;
                rest = replaceFirst(rest, logTail);
            }
            if (!rest.trimmed().isEmpty())
                data.yTitle = unquote(rest.trimmed());
            continue;
        }
        if ((m = goal.match(line)).hasMatch()) {
            const double value = number(m.captured(2));
            if (finite(value) && data.goals.size() < MaxNotes)
                data.goals << XYGoal{cleanLabel(m.captured(1)), value};
            continue;
        }
        // A band lies between two values of the scale, or between two places on the axis below:
        // numbers, dates or labels. Written as x-zone it is a stretch of that axis.
        if ((m = zone.match(line)).hasMatch()) {
            if (data.zones.size() < MaxNotes)
                data.zones << XYZone{cleanLabel(m.captured(2)), unquote(m.captured(3)),
                                     unquote(m.captured(4)),
                                     !valueBand.match(m.captured(1)).hasMatch()};
            continue;
        }
        // A mark is one place on the axis below worth pointing at.
        if ((m = mark.match(line)).hasMatch()) {
            if (data.marks.size() < MaxNotes)
                data.marks << XYMark{cleanLabel(m.captured(1)), unquote(m.captured(2))};
            continue;
        }
        if ((m = series.match(line)).hasMatch()) {
            QVector<double> list;
            bool all = true;
            for (const auto &part : splitList(m.captured(3))) {
                const double v = number(part);
                all = all && finite(v);
                list << v;
            }
            if (!list.isEmpty() && all && data.series.size() < MaxSeries &&
                list.size() <= MaxValues && values + list.size() <= MaxValues * 4) {
                values += int(list.size());
                data.series << XYSeries{m.captured(1).toLower(), m.captured(2), list};
            }
        }
    }
    if (data.series.isEmpty())
        return std::nullopt;
    int count = 0;
    for (const auto &s : data.series)
        count = std::max(count, int(s.values.size()));
    if (!hasLabels) {
        const auto [from, to] = data.range ? *data.range : std::make_pair(1.0, double(count));
        data.labels.clear();
        for (int i = 0; i < count; ++i)
            data.labels << format(count > 1 ? from + (to - from) * i / (count - 1) : from);
    }
    while (data.labels.size() < count)
        data.labels << QString();
    return data;
}

// Where the labels of an axis stand when they are numbers or dates in rising order: at their own
// values, so that uneven steps stay uneven. Other labels stand at even steps.
std::optional<QVector<double>> stampsOf(const QStringList &labels)
{
    static const Re numeric(QString::fromUtf8("^[+\\-−]?\\d+(?:[.,]\\d+)?$")),
        space(QStringLiteral("\\s"));
    if (labels.size() < 2)
        return std::nullopt;
    QVector<double> numbers;
    bool all = true;
    for (const auto &text : labels) {
        QString bareText = text;
        bareText.remove(space);
        QString minusless = text;
        const int at = int(minusless.indexOf(QChar(0x2212)));
        if (at >= 0)
            minusless[at] = QLatin1Char('-');
        const double v = numeric.match(bareText).hasMatch() ? number(minusless) : NaN;
        all = all && finite(v);
        numbers << v;
    }
    QVector<double> stamps = numbers;
    if (!all) {
        stamps.clear();
        for (const auto &text : labels)
            stamps << parseDate(text);
    }
    for (int i = 0; i < stamps.size(); ++i) {
        if (!finite(stamps[i]) || (i && !(stamps[i] > stamps[i - 1])))
            return std::nullopt;
    }
    return stamps;
}

// Labels that are all days, or all months, written as dates: an axis says them short, the way a
// calendar does, and the pointer gets the whole date.
struct Dated {
    QStringList axis, full;
};
std::optional<Dated> datesOf(const QStringList &labels)
{
    static const Re day(
        QStringLiteral("^(?:\\d{4}-\\d{1,2}-\\d{1,2}|\\d{1,2}[./-]\\d{1,2}[./-]\\d{4})$")),
        month(QStringLiteral("^\\d{4}-\\d{1,2}$"));
    const auto every = [&](const Re &re) {
        return std::all_of(labels.begin(), labels.end(), [&](const QString &text) {
            return re.match(text.trimmed()).hasMatch();
        });
    };
    const bool isDay = every(day), isMonth = !isDay && every(month);
    if ((!isDay && !isMonth) || labels.size() < 2)
        return std::nullopt;
    QVector<double> times;
    QSet<int> years;
    for (const auto &text : labels) {
        const double t = parseDate(text);
        if (!finite(t))
            return std::nullopt;
        times << t;
        years.insert(asUtc(t).fullYear());
    }
    const bool year = years.size() == 1;
    Dated out;
    for (const double t : times) {
        out.axis << (isDay  ? (year ? dayMonth(t) : shortDay(t))
                     : year ? monthName(t)
                            : shortMonth(t));
        out.full << longDate(t, isDay);
    }
    return out;
}

// A bar chart turned on its side is a ledger: its categories become the rows.
LedgerData ledgerOf(const XYData &data)
{
    QVector<const XYSeries *> bars;
    for (const auto &s : data.series) {
        if (s.type == QLatin1String("bar"))
            bars << &s;
    }
    const QString unit = data.yTitle;
    const auto text = [unit](double value) { return withUnit(value, {}, unit); };
    LedgerData out;
    out.title = data.title;
    out.total = std::nullopt;
    for (const auto *s : bars)
        out.names << s->name;
    out.amount = text;
    for (int i = 0; i < data.labels.size(); ++i) {
        LedgerRow row;
        row.label = data.labels[i];
        for (const auto *s : bars) {
            const double v = i < s->values.size() ? s->values[i] : NaN;
            row.values << LedgerValue{v, finite(v) ? text(v) : QString()};
        }
        out.rows << row;
    }
    return out;
}

std::optional<Result> chartScene(Ctx &c, const XYData &data)
{
    const double width = c.width, room = c.room;
    if (data.horizontal &&
        std::all_of(data.series.begin(), data.series.end(),
                    [](const XYSeries &s) { return s.type == QLatin1String("bar"); }))
        return ledgerScene(c, ledgerOf(data));
    Result out;
    out.kind = QStringLiteral("chart");
    auto &items = out.items;
    const Tones &tones = c.tones;
    QVector<int> bars, lines; // Indexes of data.series.
    for (int i = 0; i < data.series.size(); ++i)
        (data.series[i].type == QLatin1String("bar") ? bars : lines) << i;
    QVector<int> toneOf;
    for (int i = 0; i < data.series.size(); ++i)
        toneOf << slot(ring(tones, int(data.series.size())), i);
    const QStringList &labels = data.labels;
    const int count = int(labels.size());
    const bool stacked = data.stacked && bars.size() > 1;
    const auto valueAt = [&](int s, int i) {
        const auto &vs = data.series[s].values;
        return i < vs.size() ? vs[i] : NaN;
    };
    // `values[i] || 0`: a missing value is nothing.
    const auto orZero = [](double v) { return v == v ? v : 0.0; };
    QVector<double> sums;
    if (stacked) {
        for (int i = 0; i < count; ++i) {
            double sum = 0;
            for (const int s : bars)
                sum += std::max(0.0, orZero(valueAt(s, i)));
            sums << sum;
        }
    }
    QVector<double> values;
    for (const auto &s : data.series)
        values += s.values;
    values += sums;
    const double least = minOf(values), most = maxOf(values);
    const auto stamps =
        bars.isEmpty() ? stampsOf(labels) : std::optional<QVector<double>>(std::nullopt);
    const auto dated = datesOf(labels);
    const QStringList shown = dated ? dated->axis : labels;
    // Where a band or a mark lies. Written with numbers, a band is a stretch of the scale of
    // values, unless it only makes sense along the axis below. Written with dates or labels of
    // the axis, it lies along the axis.
    static const Re numericRe(QStringLiteral("^[-+\\x{2212}]?[\\d.,\\s]+(?:e[-+]?\\d+)?$"), I);
    const auto numeric = [&](QString text) {
        if (!numericRe.match(text).hasMatch())
            return NaN;
        const int at = int(text.indexOf(QChar(0x2212)));
        if (at >= 0)
            text[at] = QLatin1Char('-');
        return number(text);
    };
    // parseDate's null is NaN here.
    const auto timed = [&](const QString &text) { return stamps && dated ? parseDate(text) : NaN; };
    struct Zone {
        QString label;
        enum Kind { Values, Times, Places } kind = Values;
        double from = 0, to = 0; // Values, or the two places of Times on the axis's scale.
        int i0 = 0, i1 = 0;      // Places: labels of the axis.
    };
    struct Mark {
        QString label;
        bool place = false;
        double x = 0;
        int i = 0;
    };
    QVector<Zone> zones;
    QVector<Mark> marks;
    const bool numbered = stamps && !dated;
    for (const auto &zone : data.zones) {
        const double na = numeric(zone.a), nb = numeric(zone.b), ta = timed(zone.a),
                     tb = timed(zone.b);
        const int ia = int(labels.indexOf(zone.a)), ib = int(labels.indexOf(zone.b));
        Zone z;
        z.label = zone.label;
        if (finite(ta) && finite(tb)) {
            z.kind = Zone::Times;
            z.from = std::min(ta, tb);
            z.to = std::max(ta, tb);
            zones << z;
        } else if (finite(na) && finite(nb)) {
            const double from = std::min(na, nb), to = std::max(na, nb);
            const bool onAxis =
                numbered ? from >= stamps->first() - 1e-9 && to <= stamps->at(count - 1) + 1e-9
                         : ia >= 0 && ib >= 0;
            const bool asValues = to > least && from < most && !(from <= least && to >= most);
            if (zone.along ? !numbered && (ia < 0 || ib < 0) : !onAxis || asValues) {
                z.kind = Zone::Values;
                z.from = from;
                z.to = to;
            } else if (numbered) {
                z.kind = Zone::Times;
                z.from = from;
                z.to = to;
            } else {
                z.kind = Zone::Places;
                z.i0 = std::min(ia, ib);
                z.i1 = std::max(ia, ib);
            }
            zones << z;
        } else if (ia >= 0 && ib >= 0) {
            z.kind = Zone::Places;
            z.i0 = std::min(ia, ib);
            z.i1 = std::max(ia, ib);
            zones << z;
        }
    }
    for (const auto &mark : data.marks) {
        const double n = numeric(mark.at), t = timed(mark.at);
        const int i = int(labels.indexOf(mark.at));
        if (finite(t))
            marks << Mark{mark.label, false, t, 0};
        else if (numbered && finite(n))
            marks << Mark{mark.label, false, n, 0};
        else if (i >= 0)
            marks << Mark{mark.label, true, 0, i};
    }
    // What the scale has to hold: every value, the tops of the stacks, the goals and the bands of
    // values. Bars stand on zero; lines alone take the range of their values, so a change shows
    // instead of lying flat far above the floor.
    QVector<double> all = values;
    for (const auto &g : data.goals)
        all << g.value;
    for (const auto &z : zones) {
        if (z.kind == Zone::Values)
            all << z.from << z.to;
    }
    const double lowest = minOf(all), highest = maxOf(all);
    if (!finite(lowest) || !finite(highest))
        return std::nullopt;
    // Values that run over four orders of magnitude are read on a scale of powers of ten, asked
    // for or not: on an even scale all but the largest would lie on the floor.
    const bool log = bars.isEmpty() && lowest > 0 && (data.log || highest / lowest >= 1e4);
    const auto trueMinus = [](QString s) {
        if (s.startsWith(QLatin1Char('-')))
            s.replace(0, 1, QChar(0x2212));
        return s;
    };
    const auto brief = [&](double v) {
        return std::abs(v) >= 1e6 ? trueMinus(compact(v, 1)) : format(v);
    };
    enum class TickText { Brief, Log, Compact } tickKind = TickText::Brief;
    double lo = 0, hi = 0;
    QVector<double> ticks;
    if (log) {
        lo = std::floor(std::log10(data.min && *data.min > 0 ? *data.min : lowest) + 1e-9);
        hi = std::ceil(std::log10(data.max && *data.max > 0 ? *data.max : highest) - 1e-9);
        // A top under the floor (a max below every value) is taken as a decade over it.
        if (!(hi > lo))
            hi = lo + 1;
        const double each = std::ceil((hi - lo) / 6);
        for (double d = lo; d <= hi && ticks.size() < 64; d += each)
            ticks << std::pow(10.0, d);
        tickKind = TickText::Log;
    } else {
        lo = data.min
                 ? *data.min
                 : (!bars.isEmpty() || lowest < 0 || lowest < highest * 0.5 ? std::min(0.0, lowest)
                                                                            : lowest);
        hi = data.max ? *data.max : highest;
        if (hi == lo)
            hi = lo + 1;
        const double step = niceStep(hi - lo);
        if (!finite(step) || step <= 0)
            return std::nullopt;
        if (!data.min)
            lo = std::floor(lo / step + 1e-9) * step;
        if (!data.max)
            hi = std::ceil(hi / step - 1e-9) * step;
        for (double v = std::ceil(lo / step - 1e-9) * step;
             v <= hi + step * 1e-6 && ticks.size() < 200; v += step)
            ticks << fixed10(v);
        if (std::max(std::abs(lo), std::abs(hi)) >= 1e4)
            tickKind = TickText::Compact;
    }
    // A scale upside down (a min over the max) has nothing to stand on.
    if (ticks.isEmpty() || !(hi > lo) || !finite(hi - lo))
        return std::nullopt;
    const auto tickText = [&](double v) {
        switch (tickKind) {
        case TickText::Log:
            return std::abs(v) >= 1e4 ? compact(v, 1) : format(v);
        case TickText::Compact:
            return trueMinus(compact(v, 1));
        case TickText::Brief:
            break;
        }
        return brief(v);
    };
    // What stands to the right of the plot: the number each line ends on, and the names of the
    // levels to reach. A long name of a level would take the plot's room, so it goes inside.
    QStringList ends, goals;
    for (const int s : lines)
        ends << brief(data.series[s].values.last());
    for (const auto &g : data.goals)
        goals << joinSet({g.label, brief(g.value)}, QStringLiteral(" "));
    const auto aside = [&](const QString &text) { return c.widthOf(text, FONT::value) <= 96; };
    // Bands and marks are tinted with the one colour of the drawing; among several series they
    // stay neutral.
    const QString zoneCls =
        data.series.size() > 1 ? QStringLiteral("dg-zone is-plain") : QStringLiteral("dg-zone");
    // A scale of powers of ten is said to be one, beside the name of what it measures.
    const QString yName =
        joinSet({data.yTitle, log ? QStringLiteral("log") : QString()}, QString::fromUtf8(" · "));
    double tickW = -INFINITY;
    for (const double t : ticks)
        tickW = std::max(tickW, c.widthOf(tickText(t), FONT::tick));
    const double left = std::ceil(tickW) + 10;
    QStringList besides = ends;
    for (const auto &g : goals) {
        if (aside(g))
            besides << g;
    }
    double besideW = -INFINITY;
    for (const auto &text : besides)
        besideW = std::max(besideW, c.widthOf(text, FONT::value));
    const double right = besides.isEmpty() ? 4 : std::ceil(besideW) + 14;
    // The column of text is room enough for most charts. Bars ask for more when they would grow
    // thin or lose the names under them, and a long run of points when they would crowd.
    double labelW = -INFINITY;
    for (const auto &l : shown)
        labelW = std::max(labelW, c.widthOf(l, FONT::tick));
    const int group = stacked ? 1 : std::max(1, int(bars.size()));
    const double each = !bars.isEmpty() ? std::max(group * 15.0 + (group - 1) * 2 + 16,
                                                   count <= 16 ? labelW + 12 : 0.0)
                        : count > 48    ? 9
                                        : 0;
    const double want = left + right + count * each;
    const double W = clamp(want > width * 1.08 ? std::min(want, room) : width, 300, CHART.max);

    double head = 0;
    if (!data.title.isEmpty()) {
        const Title title = titleOf(c, data.title, W - 80);
        items << title.item;
        head = CHART.head;
        out.corner = cornerAt(title.width + 16, 28);
    }
    if (data.series.size() > 1 &&
        std::any_of(data.series.begin(), data.series.end(),
                    [](const XYSeries &s) { return !s.name.isEmpty(); })) {
        QVector<ChipEntry> entries;
        for (int i = 0; i < data.series.size(); ++i) {
            const auto &s = data.series[i];
            entries << ChipEntry{s.name.isEmpty() ? QString::number(i + 1) : s.name, toneOf[i],
                                 s.type == QLatin1String("bar") ? QStringLiteral("bar")
                                                                : QStringLiteral("line")};
        }
        const ChipRow row = chips(c, entries, 0, head + 9, W);
        items += row.items;
        if (!out.corner)
            out.corner = cornerAt(row.width + 16, 20);
        head += row.height + 2;
    }
    // The unit stands once, over the scale, instead of a title turned on its side.
    if (!yName.isEmpty())
        items << labelSpec(QStringLiteral("yt"), 0, 0, head + 8,
                           {truncate(c, yName, W * 0.6, FONT::tick)}, QStringLiteral("dg-unit"),
                           QStringLiteral("start"), Back);
    // Names of bands and marks stand over the plot and need a line of room there.
    const bool named =
        std::any_of(zones.begin(), zones.end(),
                    [](const Zone &z) { return z.kind != Zone::Values && !z.label.isEmpty(); }) ||
        std::any_of(marks.begin(), marks.end(), [](const Mark &m) { return !m.label.isEmpty(); });
    const double top = head + (!yName.isEmpty() ? 18 : 0) + (named ? 26 : 16), plotH = CHART.plot;
    const double plotW = W - left - right, band = plotW / count;
    const auto share = [&](double v) {
        return log ? (clamp(std::log10(std::max(v, std::pow(10.0, lo))), lo, hi) - lo) / (hi - lo)
                   : (clamp(v, lo, hi) - lo) / (hi - lo);
    };
    const auto y = [&](double v) { return top + plotH - share(v) * plotH; };
    const double zero = log ? top + plotH : y(clamp(0, lo, hi));
    const double floorAt = log ? std::pow(10.0, lo) : lo <= 0 && hi >= 0 ? 0 : lo;
    for (const double t : ticks) {
        items << lineSpec(QStringLiteral("g:") + jsNum(t), 0, left, y(t), left + plotW, y(t),
                          t == floorAt ? QStringLiteral("dg-grid is-zero")
                                       : QStringLiteral("dg-grid"),
                          false, Back);
        items << labelSpec(QStringLiteral("t:") + jsNum(t), 0, left - 8, y(t), {tickText(t)},
                           QStringLiteral("dg-tick"), QStringLiteral("end"), Back);
    }
    const double inset = stamps ? std::min(16.0, plotW * 0.04) : 0;
    const auto along = [&](double v) {
        return left + inset +
               clamp((v - stamps->first()) / (stamps->at(count - 1) - stamps->first()), 0, 1) *
                   (plotW - inset * 2);
    };
    const auto xAt = [&](int i) { return stamps ? along(stamps->at(i)) : left + band * (i + 0.5); };
    // Labels at even steps thin out evenly. Those at their own values are kept wherever they
    // clear the one before and the last one, which always stands: it is where the line has come
    // to.
    const double every = std::max(1.0, std::ceil((labelW + 10) / band));
    const double end =
        stamps ? xAt(count - 1) - c.widthOf(shown[count - 1], FONT::tick) / 2 : INFINITY;
    double edge = -INFINITY;
    for (int i = 0; i < count; ++i) {
        const QString &text = shown[i];
        const double x = xAt(i), half = c.widthOf(text, FONT::tick) / 2;
        if (stamps ? i < count - 1 && (x - half < edge + 10 || x + half > end - 10)
                   : std::fmod(double(i), every) != 0)
            continue;
        edge = x + half;
        Spec label = labelSpec(QStringLiteral("x:%1").arg(i), 0, x, top + plotH + 9, {text},
                               QStringLiteral("dg-tick"), QStringLiteral("middle"), Back);
        label.fixed.baseline = QStringLiteral("below");
        items << label;
    }
    if (!data.xTitle.isEmpty())
        items << labelSpec(QStringLiteral("xt"), 0, left + plotW, top + plotH + 34,
                           {truncate(c, data.xTitle, plotW, FONT::tick)}, QStringLiteral("dg-unit"),
                           QStringLiteral("end"), Back);
    const double H = top + plotH + 26 + (!data.xTitle.isEmpty() ? 18 : 0);

    // Bands: across the plot for a stretch of values, down it for a stretch of the axis. A band
    // of values is named inside, at its top; a band of the axis is named over the plot, where no
    // line can run into the name.
    const auto span = [&](const Zone &zone) -> std::pair<double, double> {
        if (zone.kind == Zone::Times)
            return {along(zone.from), along(zone.to)};
        return {xAt(zone.i0) - (stamps ? 0 : band / 2), xAt(zone.i1) + (stamps ? 0 : band / 2)};
    };
    QVector<std::pair<double, double>> over;
    for (int k = 0; k < zones.size(); ++k) {
        const Zone &zone = zones[k];
        if (zone.kind != Zone::Values) {
            const auto [x0, x1] = span(zone);
            const QString text =
                truncate(c, zone.label, std::max(40.0, left + plotW - x0 - 4), FONT::tick);
            Spec rect = item(Type::Rect, QStringLiteral("zone:%1").arg(k), 0.1, Back);
            rect.props.x = x0;
            rect.props.y = top;
            rect.props.w = std::max(1.0, x1 - x0);
            rect.props.h = plotH;
            rect.fixed.cls = zoneCls;
            rect.fixed.tone = first(tones);
            rect.fixed.rx = 0;
            items << rect;
            if (!zone.label.isEmpty()) {
                items << labelSpec(QStringLiteral("zonel:%1").arg(k), 4.4, x0 + 2, top - 9, {text},
                                   QStringLiteral("dg-zone-label"), QStringLiteral("start"),
                                   Labels);
                over << std::make_pair(x0, x0 + 2 + c.widthOf(text, FONT::tick));
            }
            continue;
        }
        const double y0 = y(zone.to), y1 = y(zone.from);
        Spec rect = item(Type::Rect, QStringLiteral("zone:%1").arg(k), 0.1, Back);
        rect.props.x = left;
        rect.props.y = y0;
        rect.props.w = plotW;
        rect.props.h = std::max(1.0, y1 - y0);
        rect.fixed.cls = zoneCls;
        rect.fixed.tone = first(tones);
        rect.fixed.rx = 0;
        items << rect;
        if (!zone.label.isEmpty())
            items << labelSpec(QStringLiteral("zonel:%1").arg(k), 4.4, left + 6, y0 + 10,
                               {zone.label}, QStringLiteral("dg-zone-label"),
                               QStringLiteral("start"), Labels);
    }
    // A mark is a hairline down the plot with its name over it, on the side where there is room;
    // where the name of a band already stands there, the mark's name goes inside the plot.
    for (int k = 0; k < marks.size(); ++k) {
        const Mark &mark = marks[k];
        const double x = mark.place ? xAt(mark.i) : along(mark.x);
        const bool side = x > left + plotW * 0.7;
        const QString text =
            truncate(c, mark.label, (side ? x - left : left + plotW + right - x) - 6, FONT::value);
        const double tw = c.widthOf(text, FONT::value), from = side ? x - 4 - tw : x + 4;
        const bool inside =
            std::any_of(over.begin(), over.end(), [&](const std::pair<double, double> &o) {
                return from < o.second + 8 && o.first < from + tw + 8;
            });
        items << lineSpec(QStringLiteral("mark:%1").arg(k), 4.5, x, top, x, top + plotH,
                          QStringLiteral("dg-goal"), true, Labels);
        if (!mark.label.isEmpty())
            items << labelSpec(QStringLiteral("markl:%1").arg(k), 4.7, x + (side ? -4 : 4),
                               inside ? top + 10 : top - 9, {text}, QStringLiteral("dg-goal-label"),
                               side ? QStringLiteral("end") : QStringLiteral("start"), Labels);
        if (!inside)
            over << std::make_pair(from, from + tw);
    }

    const double gap = group > 1 ? 2 : 0;
    const double barW = std::min(CHART.bar, (band * 0.62 - gap * (group - 1)) / group);
    const bool single = bars.size() == 1 && lines.isEmpty();
    const auto fits = [&](const QString &text) { return c.widthOf(text, FONT::value) <= band - 2; };
    bool labeled = false;
    if (count <= CHART.labeled) {
        if (single) {
            const auto &vs = data.series[bars[0]].values;
            labeled = std::all_of(vs.begin(), vs.end(), [&](double v) { return fits(brief(v)); });
        } else {
            labeled =
                stacked && lines.isEmpty() &&
                std::all_of(sums.begin(), sums.end(), [&](double v) { return fits(brief(v)); });
        }
    }
    for (int s = 0; s < bars.size(); ++s) {
        const auto &series = data.series[bars[s]];
        const int tone = toneOf[bars[s]];
        for (int i = 0; i < series.values.size(); ++i) {
            const double value = series.values[i];
            double x = xAt(i) - barW / 2, at = y(value), base = zero;
            if (stacked) {
                // Parts of one bar are parted by two pixels of the surface, not by an outline.
                double below = 0;
                for (int o = 0; o < s; ++o)
                    below += std::max(0.0, orZero(valueAt(bars[o], i)));
                base = y(below) - (s ? 1 : 0);
                at =
                    std::min(base, y(below + std::max(0.0, value)) + (s < bars.size() - 1 ? 1 : 0));
            } else {
                x += (s - (group - 1) / 2.0) * (barW + gap);
            }
            Spec bar =
                item(Type::Bar, QStringLiteral("b:%1:%2").arg(s).arg(i), i * 0.25 + s * 0.12);
            bar.props.x = x;
            bar.props.top = at;
            bar.props.w = barW;
            bar.props.base = base;
            bar.fixed.tone = tone;
            bar.fixed.r = !stacked || s == bars.size() - 1 ? 3 : 0;
            items << bar;
            if (labeled && single) {
                Spec label =
                    labelSpec(QStringLiteral("v:%1:%2").arg(s).arg(i), i * 0.25 + 1.6, x + barW / 2,
                              value >= 0 ? at - 6 : at + 6, {brief(value)},
                              QStringLiteral("dg-value"), QStringLiteral("middle"), Labels);
                label.fixed.baseline =
                    value >= 0 ? QStringLiteral("above") : QStringLiteral("below");
                items << label;
            }
        }
    }
    if (labeled && stacked) {
        for (int i = 0; i < sums.size(); ++i) {
            Spec label = labelSpec(QStringLiteral("v:sum:%1").arg(i), i * 0.25 + 1.8, xAt(i),
                                   y(sums[i]) - 6, {brief(sums[i])}, QStringLiteral("dg-value"),
                                   QStringLiteral("middle"), Labels);
            label.fixed.baseline = QStringLiteral("above");
            items << label;
        }
    }

    // Where a name or a number already stands inside the plot: the numbers of points keep clear
    // of these.
    struct Note {
        QString key, text, cls;
        double x = 0;
    };
    struct Box {
        double x0 = 0, y0 = 0, w = 0;
    };
    QVector<Note> notes;
    QVector<Spreading> noteYs;
    QVector<Box> taken;
    for (int s = 0; s < lines.size(); ++s) {
        const auto &series = data.series[lines[s]];
        const int tone = toneOf[lines[s]], n = int(series.values.size());
        const double begin = !bars.isEmpty() ? 1 : 0.2;
        QVector<QPointF> points;
        for (int i = 0; i < n; ++i)
            points << QPointF(xAt(i), y(series.values[i]));
        const Pts pts = drawable(smoothPts(points));
        if (series.type == QLatin1String("area") || (bars.isEmpty() && lines.size() == 1)) {
            Spec area = item(Type::Area, QStringLiteral("ar:%1").arg(s), 2, Back);
            area.props.pts = pts;
            area.props.base = zero;
            area.fixed.tone = tone;
            items << area;
        }
        Spec edge = item(Type::Edge, QStringLiteral("ln:%1").arg(s), begin);
        edge.props.pts = pts;
        edge.fixed.style = QStringLiteral("solid");
        edge.fixed.head = QStringLiteral("none");
        edge.fixed.tone = tone;
        edge.fixed.cls = QStringLiteral("dg-stroke");
        edge.fixed.draw = 900;
        items << edge;
        // A sparse line shows its points; a dense one only where it ends and under the pointer.
        for (int i = 0; i < n; ++i) {
            const double r = i == n - 1 ? 4 : n <= CHART.dots ? 2.75 : 0;
            items << dotSpec(QStringLiteral("p:%1:%2").arg(s).arg(i),
                             begin + (n > 1 ? double(i) / (n - 1) : 0) * 4, points[i].x(),
                             points[i].y(), r, QStringLiteral("dg-point"), tone);
        }
        notes << Note{QStringLiteral("end:%1").arg(s), ends[s], QStringLiteral("dg-end"),
                      points[n - 1].x() + 9};
        noteYs << Spreading{points[n - 1].y(), int(noteYs.size())};
    }
    for (int k = 0; k < data.goals.size(); ++k) {
        const auto &goal = data.goals[k];
        const double gy = y(goal.value);
        items << lineSpec(QStringLiteral("goal:%1").arg(k), 4.6, left, gy, left + plotW, gy,
                          QStringLiteral("dg-goal"), true, Labels);
        if (aside(goals[k])) {
            notes << Note{QStringLiteral("goall:%1").arg(k), goals[k],
                          QStringLiteral("dg-goal-label"), left + plotW + 8};
            noteYs << Spreading{gy, int(noteYs.size())};
            continue;
        }
        const QString text =
            truncate(c, joinSet({goal.label, brief(goal.value)}, QString::fromUtf8(" · ")),
                     plotW - 12, FONT::value);
        const bool below = gy < top + 22;
        Spec label = labelSpec(QStringLiteral("goall:%1").arg(k), 5.2, left + plotW - 4,
                               below ? gy + 8 : gy - 7, {text}, QStringLiteral("dg-goal-label"),
                               QStringLiteral("end"), Labels);
        label.fixed.baseline = below ? QStringLiteral("below") : QStringLiteral("above");
        items << label;
        const double tw = c.widthOf(text, FONT::value);
        taken << Box{left + plotW - 4 - tw, below ? gy + 7 : gy - 21, tw};
    }
    spread(noteYs, 14, top, top + plotH);
    for (int k = 0; k < notes.size(); ++k)
        items << labelSpec(notes[k].key, 5.2, notes[k].x, noteYs[k].y, {notes[k].text},
                           notes[k].cls, QStringLiteral("start"));
    if (lines.size() == 1 && bars.isEmpty()) {
        const auto &vs = data.series[lines[0]].values;
        const int n = int(vs.size());
        const int peak = int(std::max_element(vs.begin(), vs.end()) - vs.begin()),
                  trough = int(std::min_element(vs.begin(), vs.end()) - vs.begin());
        // A lone line of a few points carries the number of each: it stands on the side the line
        // leaves free, and gives way where two would meet. A longer line names only its highest
        // and its lowest.
        const auto put = [&](int i, const QString &key) {
            if (i == n - 1)
                return;
            const double px = xAt(i), py = y(vs[i]);
            const QString text = brief(vs[i]);
            const double tw = c.widthOf(text, FONT::value);
            // A point lying on the floor of the plot needs no number: the scale says what the
            // floor is.
            if (key != QLatin1String("ext:hi") && py > top + plotH - 3)
                return;
            const double before = i ? y(vs[i - 1]) : py, after = y(vs[i + 1]);
            // Seen from a point: the line comes down to it, goes up from it, or both, and the
            // number keeps out of its way.
            const bool down = before < py - 2, up = after < py - 2, under = up && (down || !i);
            if (under && py > top + plotH - 20)
                return;
            const QString anchor = under  ? QStringLiteral("middle")
                                   : down ? QStringLiteral("start")
                                   : up   ? QStringLiteral("end")
                                          : QStringLiteral("middle");
            const double x = anchor == QLatin1String("start") ? px + 5
                             : anchor == QLatin1String("end")
                                 ? px - 5
                                 : clamp(px, left + tw / 2, left + plotW - tw / 2);
            const Box box{anchor == QLatin1String("start") ? x
                          : anchor == QLatin1String("end") ? x - tw
                                                           : x - tw / 2,
                          under ? py + 8 : py - 22, tw};
            if (box.x0 < left - 2 || box.x0 + tw > left + plotW + 2 ||
                std::any_of(taken.begin(), taken.end(), [&](const Box &b) {
                    return box.x0 < b.x0 + b.w + 6 && b.x0 < box.x0 + tw + 6 &&
                           std::abs(b.y0 - box.y0) < 14;
                }))
                return;
            taken << box;
            Spec label = labelSpec(key, 5, x, py + (under ? 9 : -9), {text},
                                   QStringLiteral("dg-value"), anchor);
            label.fixed.baseline = under ? QStringLiteral("below") : QStringLiteral("above");
            items << label;
        };
        if (peak != trough) {
            put(peak, QStringLiteral("ext:hi"));
            put(trough, QStringLiteral("ext:lo"));
        }
        if (n <= 8 && !log) {
            for (int i = 0; i < n; ++i) {
                if (i != peak && i != trough)
                    put(i, QStringLiteral("ext:%1").arg(i));
            }
        }
    }
    static const Re plainNumber(QStringLiteral("^[-\\d.,\\s]+$"));
    Probe probe;
    probe.x0 = left;
    probe.x1 = left + plotW;
    probe.y0 = top;
    probe.y1 = top + plotH;
    for (int i = 0; i < count; ++i) {
        probe.xs << xAt(i);
        Tip tip;
        tip.title = dated ? dated->full[i]
                    : !data.xTitle.isEmpty() && plainNumber.match(labels[i]).hasMatch()
                        ? data.xTitle + QLatin1Char(' ') + labels[i]
                        : labels[i];
        for (int s = 0; s < data.series.size(); ++s) {
            const auto &series = data.series[s];
            const double v = valueAt(s, i);
            TipRow row;
            row.tone = toneOf[s];
            row.mark = series.type == QLatin1String("bar") ? QStringLiteral("bar")
                                                           : QStringLiteral("line");
            row.name = series.name.isEmpty() ? data.yTitle : series.name;
            row.value = finite(v) ? format(v) : QString(QChar(0x2013));
            tip.rows << row;
        }
        probe.tips << tip;
        QStringList keys;
        for (int s = 0; s < bars.size(); ++s)
            keys << QStringLiteral("b:%1:%2").arg(s).arg(i);
        for (int s = 0; s < lines.size(); ++s)
            keys << QStringLiteral("p:%1:%2").arg(s).arg(i);
        probe.keys << keys;
    }
    out.probe = probe;
    out.width = W;
    out.height = H;
    out.flush = true;
    return out;
}

/* Candlestick charts */

struct CandleRow {
    QString label, axis, full;
    double o = NaN, h = NaN, l = NaN, c = NaN, v = NaN;
};

struct CandleData {
    QString title;
    QVector<CandleRow> rows;
    QVector<double> ma;
};

// OHLC: the names a header row gives its columns.
QChar ohlcKey(const QString &cell)
{
    const QString k = cell.toLower();
    if (k == QLatin1String("open") || k == QLatin1String("o"))
        return QLatin1Char('o');
    if (k == QLatin1String("high") || k == QLatin1String("h"))
        return QLatin1Char('h');
    if (k == QLatin1String("low") || k == QLatin1String("l"))
        return QLatin1Char('l');
    if (k == QLatin1String("close") || k == QLatin1String("c"))
        return QLatin1Char('c');
    if (k == QLatin1String("volume") || k == QLatin1String("vol") || k == QLatin1String("v"))
        return QLatin1Char('v');
    return {};
}

std::optional<CandleData> parseCandles(const QStringList &lines)
{
    static const Re ma(QStringLiteral("^(?:ma|sma)\\s+([\\d\\s,]+)$"), I),
        maSplit(QStringLiteral("[\\s,]+")), cellSplit(QStringLiteral("\\s*[,;|\\t]\\s*")),
        space(QStringLiteral("\\s+")), colon(QStringLiteral(":$"));
    CandleData data;
    QVector<QChar> order{QLatin1Char('o'), QLatin1Char('h'), QLatin1Char('l'), QLatin1Char('c'),
                         QLatin1Char('v')};
    for (int li = 1; li < lines.size(); ++li) {
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if ((m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if ((m = ma.match(line)).hasMatch()) {
            for (const auto &part : m.captured(1).split(maSplit)) {
                const double p = part.isEmpty() ? 0 : part.toDouble();
                // The same period twice is one line: its key is its period.
                if (p > 1 && p < 400 && !data.ma.contains(p) && data.ma.size() < MaxAverages)
                    data.ma << p;
            }
            continue;
        }
        QStringList cells;
        for (const auto &cell : line.split(cellSplit)) {
            if (!cell.isEmpty())
                cells << cell;
        }
        if (cells.size() < 5) {
            const QStringList words = line.split(space);
            int k = int(words.size());
            while (k > 0 && finite(number(words[k - 1])) && words.size() - k < 5)
                --k;
            cells = QStringList{words.mid(0, k).join(QLatin1Char(' '))} + words.mid(k);
        }
        QVector<QChar> head;
        bool named = true;
        for (int i = 1; i < cells.size(); ++i) {
            head << ohlcKey(cells[i]);
            named = named && !head.last().isNull();
        }
        if (head.size() >= 4 && named) {
            order = head;
            continue;
        }
        QVector<double> values;
        for (int i = 1; i < cells.size(); ++i)
            values << number(cells[i]);
        if (values.size() < 4 ||
            !std::all_of(values.begin(), values.begin() + 4, [](double v) { return finite(v); }))
            continue;
        CandleRow row;
        QString label = cells[0];
        label.remove(colon);
        row.label = unquote(label);
        for (int i = 0; i < order.size(); ++i) {
            const double v = i < values.size() ? values[i] : NaN;
            switch (order[i].toLatin1()) {
            case 'o':
                row.o = v;
                break;
            case 'h':
                row.h = v;
                break;
            case 'l':
                row.l = v;
                break;
            case 'c':
                row.c = v;
                break;
            default:
                row.v = v;
                break;
            }
        }
        // A header without one of the four leaves its value unread: such a row is no candle.
        if (!finite(row.o) || !finite(row.h) || !finite(row.l) || !finite(row.c))
            continue;
        row.h = std::max({row.o, row.h, row.l, row.c});
        row.l = std::min({row.o, row.h, row.l, row.c});
        row.v = finite(row.v) ? std::max(0.0, row.v) : 0;
        if (data.rows.size() < MaxRows)
            data.rows << row;
    }
    if (data.rows.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> candleScene(Ctx &c, CandleData data)
{
    auto &rows = data.rows;
    const int n = int(rows.size());
    const double W = std::max(340.0, std::min(CANDLE.maxWidth, c.width - PAD * 2));
    const auto dir = [](const CandleRow &r) {
        return r.c >= r.o ? QStringLiteral("is-up") : QStringLiteral("is-down");
    };
    double lo = INFINITY, hi = -INFINITY;
    for (const auto &r : rows) {
        lo = std::min(lo, r.l);
        hi = std::max(hi, r.h);
    }
    double room = (hi - lo) * 0.08;
    if (!room)
        room = std::abs(hi) * 0.02;
    if (!room)
        room = 1;
    lo -= room;
    hi += room;
    if (!finite(hi - lo) || !(hi > lo))
        return std::nullopt;
    const double step = niceStep(hi - lo);
    if (!finite(step) || step <= 0)
        return std::nullopt;
    const int digits = decimalsFor(step);
    const auto tickFmt = [digits](double v) { return formatFixed(v, digits, digits); };
    const int priceDigits = std::abs(rows[n - 1].c) >= 1 ? 2 : 6;
    const auto priceFmt = [priceDigits](double v) { return formatFixed(v, priceDigits); };
    const auto pctFmt = [](double v) { return formatFixed(v, 2, 2); };
    const auto signedText = [](double v, const std::function<QString(double)> &fmt) {
        return (v >= 0 ? QStringLiteral("+") : QString(QChar(0x2212))) + fmt(std::abs(v));
    };
    QVector<double> times;
    for (const auto &r : rows)
        times << parseDate(r.label);
    if (std::all_of(times.begin(), times.end(), [](double t) { return finite(t); })) {
        const bool intraday = times[n - 1] - times[0] < 3 * DAY;
        const bool timed = std::any_of(times.begin(), times.end(),
                                       [](double t) { return std::fmod(t, DAY) != 0; });
        for (int i = 0; i < n; ++i) {
            rows[i].axis = intraday ? hourMinute(times[i]) : dayMonth(times[i]);
            rows[i].full = longDate(times[i], true, timed);
        }
    }
    const CandleRow &head = rows[0], &last = rows[n - 1];
    Result out;
    out.kind = QStringLiteral("candles");
    auto &items = out.items;
    double y0 = 0, titleW = 0;
    if (!data.title.isEmpty()) {
        const Title title = titleOf(c, data.title, W - 80);
        items << title.item;
        titleW = title.width;
        y0 = 26;
    }
    const double change = last.c - head.o, pct = head.o ? change / head.o * 100 : 0;
    const QString priceText = priceFmt(last.c);
    const double priceW = c.widthOf(priceText, FONT::figure);
    const QString changeText = signedText(change, priceFmt) + QString::fromUtf8(" · ") +
                               signedText(pct, pctFmt) + QLatin1Char('%');
    items << labelSpec(QStringLiteral("price"), 0.1, 0, y0 + 16, {priceText},
                       QStringLiteral("dg-price"), QStringLiteral("start"));
    items << labelSpec(QStringLiteral("change"), 0.2, priceW + 10, y0 + 19, {changeText},
                       QStringLiteral("dg-change ") +
                           (change >= 0 ? QStringLiteral("is-up") : QStringLiteral("is-down")),
                       QStringLiteral("start"));
    double headW = priceW + 10 + c.widthOf(changeText, FONT::amount);
    if (!data.ma.isEmpty()) {
        QVector<ChipEntry> entries;
        for (int i = 0; i < data.ma.size(); ++i)
            entries << ChipEntry{QStringLiteral("MA ") + jsNum(data.ma[i]), slot(c.tones, i),
                                 QStringLiteral("line")};
        const ChipRow row = chips(c, entries, headW + 22, y0 + 19, INFINITY);
        items += row.items;
        headW += 22 + row.width;
    }
    out.corner = cornerAt(std::max(titleW, headW) + 16, y0 + 32);
    const double top = y0 + CANDLE.head;
    QVector<double> ticks;
    for (double v = std::ceil(lo / step) * step; v <= hi && ticks.size() < 200; v += step)
        ticks << fixed10(v);
    const QString tagText = priceFmt(last.c);
    const double tagW = c.widthOf(tagText, FONT::value) + 12;
    double axisW = tagW;
    for (const double t : ticks)
        axisW = std::max(axisW, c.widthOf(tickFmt(t), FONT::tick));
    axisW = std::ceil(axisW) + 12;
    const double plotW = W - axisW;
    const bool hasVol =
        std::any_of(rows.begin(), rows.end(), [](const CandleRow &r) { return r.v > 0; });
    double maxV = 1e-9;
    for (const auto &r : rows)
        maxV = std::max(maxV, r.v);
    const double volTop = top + CANDLE.price + CANDLE.gap,
                 bottom = hasVol ? volTop + CANDLE.volume : top + CANDLE.price;
    const auto y = [&](double v) { return top + (hi - v) / (hi - lo) * CANDLE.price; };
    const double band = plotW / n, bodyW = clamp(band * CANDLE.body, 1.5, CANDLE.maxBody),
                 ly = y(last.c);
    for (const double t : ticks) {
        items << lineSpec(QStringLiteral("g:") + jsNum(t), 0, 0, y(t), plotW, y(t),
                          QStringLiteral("dg-grid"), false, Back);
        if (std::abs(y(t) - ly) > 13)
            items << labelSpec(QStringLiteral("t:") + jsNum(t), 0, plotW + 10, y(t), {tickFmt(t)},
                               QStringLiteral("dg-tick"), QStringLiteral("start"), Back);
    }
    if (hasVol)
        items << lineSpec(QStringLiteral("vsep"), 0, 0, volTop - CANDLE.gap / 2, plotW,
                          volTop - CANDLE.gap / 2, QStringLiteral("dg-grid is-zero"), false, Back);
    double labelW = -INFINITY;
    for (const auto &r : rows)
        labelW = std::max(labelW, c.widthOf(r.axis.isEmpty() ? r.label : r.axis, FONT::tick));
    const double every = std::max(1.0, std::ceil((labelW + 16) / band));
    for (int i = 0; i < n; ++i) {
        if (std::fmod(double(i), every) != 0 || band * (i + 0.5) + labelW / 2 > W)
            continue;
        Spec label = labelSpec(QStringLiteral("x:%1").arg(i), 0, band * (i + 0.5), bottom + 9,
                               {rows[i].axis.isEmpty() ? rows[i].label : rows[i].axis},
                               QStringLiteral("dg-tick"), QStringLiteral("middle"), Back);
        label.fixed.baseline = QStringLiteral("below");
        items << label;
    }
    for (int i = 0; i < n; ++i) {
        const auto &r = rows[i];
        const double x = band * (i + 0.5), order = 0.5 + i * (4.0 / n);
        Spec candle = item(Type::Candle, QStringLiteral("k:%1").arg(i), order);
        candle.props.x = x;
        candle.props.o = y(r.o);
        candle.props.hi = y(r.h);
        candle.props.lo = y(r.l);
        candle.props.c = y(r.c);
        candle.props.w = bodyW;
        candle.fixed.cls = dir(r);
        items << candle;
        if (hasVol) {
            Spec vol = item(Type::Column, QStringLiteral("vol:%1").arg(i), order + 0.15, Back);
            vol.props.x = x - bodyW / 2;
            vol.props.top = bottom - CANDLE.volume * r.v / maxV;
            vol.props.w = bodyW;
            vol.props.base = bottom;
            vol.fixed.cls = QStringLiteral("dg-vol ") + dir(r);
            vol.fixed.r = 1;
            items << vol;
        }
    }
    for (int k = 0; k < data.ma.size(); ++k) {
        const double p = data.ma[k];
        QVector<QPointF> points;
        for (double i = p - 1; i < n; ++i) {
            double sum = 0;
            for (double j = i - p + 1; j <= i; ++j)
                sum += rows[int(j)].c;
            points << QPointF(band * (i + 0.5), y(sum / p));
        }
        if (points.size() > 1) {
            Spec edge = item(Type::Edge, QStringLiteral("ma:") + jsNum(p), 4.6 + k * 0.2);
            edge.props.pts = smoothPts(points);
            edge.fixed.style = QStringLiteral("solid");
            edge.fixed.head = QStringLiteral("none");
            edge.fixed.tone = slot(c.tones, k);
            edge.fixed.cls = QStringLiteral("dg-stroke is-ma");
            edge.fixed.draw = 800;
            items << edge;
        }
    }
    items << lineSpec(QStringLiteral("last"), 4.8, 0, ly, plotW, ly,
                      QStringLiteral("dg-last ") + dir(last), true, Back);
    Spec tag = labelSpec(QStringLiteral("tag"), 5.2, plotW + 4 + tagW / 2, ly, {tagText},
                         QStringLiteral("dg-last-tag ") + dir(last));
    tag.fixed.pill = true;
    tag.fixed.rx = 4;
    tag.fixed.w = tagW;
    tag.fixed.h = 18;
    tag.fixed.pop = true;
    items << tag;
    const auto tipRow = [](const QString &name, const QString &value, const QString &cls = {}) {
        TipRow row;
        row.name = name;
        row.value = value;
        row.cls = cls;
        return row;
    };
    Probe probe;
    probe.x0 = 0;
    probe.x1 = plotW;
    probe.y0 = top;
    probe.y1 = bottom;
    for (int i = 0; i < n; ++i) {
        const auto &r = rows[i];
        const QString cls = dir(r);
        probe.xs << band * (i + 0.5);
        Tip tip;
        tip.title = r.full.isEmpty() ? r.label : r.full;
        tip.rows << tipRow(QStringLiteral("Open"), priceFmt(r.o))
                 << tipRow(QStringLiteral("High"), priceFmt(r.h))
                 << tipRow(QStringLiteral("Low"), priceFmt(r.l))
                 << tipRow(QStringLiteral("Close"), priceFmt(r.c), cls)
                 << tipRow(QStringLiteral("Change"),
                           signedText(r.o ? (r.c - r.o) / r.o * 100 : 0, pctFmt) + QLatin1Char('%'),
                           cls);
        if (r.v)
            tip.rows << tipRow(QStringLiteral("Volume"), compact(r.v, 2));
        probe.tips << tip;
        probe.keys << QStringList{QStringLiteral("k:%1").arg(i), QStringLiteral("vol:%1").arg(i)};
    }
    out.probe = probe;
    out.width = W;
    out.height = bottom + 28;
    return out;
}

/* Timelines */

struct Period {
    QString label, section;
    QStringList events;
};

struct TimelineData {
    QString title;
    QVector<Period> periods;
};

std::optional<TimelineData> parseTimeline(const QStringList &lines)
{
    static const Re section(QStringLiteral("^section\\s+(.+)$"), I),
        parts(QStringLiteral("\\s*:\\s+"));
    TimelineData data;
    QString current;
    const auto events = [](const QStringList &list) {
        QStringList out;
        for (const auto &e : list) {
            const QString text = unquote(e);
            if (!text.isEmpty() && out.size() < MaxPeriodEvents)
                out << text;
        }
        return out;
    };
    for (int li = 1; li < lines.size(); ++li) {
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if ((m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if ((m = section.match(line)).hasMatch()) {
            current = unquote(m.captured(1));
            continue;
        }
        if (line.startsWith(QLatin1Char(':'))) {
            if (!data.periods.isEmpty()) {
                auto &last = data.periods.last().events;
                last += events(line.mid(1).split(parts));
                if (last.size() > MaxPeriodEvents)
                    last = last.mid(0, MaxPeriodEvents);
            }
            continue;
        }
        if (data.periods.size() >= MaxPeriods)
            continue;
        const QStringList split = line.split(parts);
        data.periods << Period{unquote(split.value(0)), current, events(split.mid(1))};
    }
    if (data.periods.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> timelineScene(Ctx &c, const TimelineData &data)
{
    const int accent = first(c.tones);
    const double room = std::max(280.0, c.width - PAD * 2);
    const auto &periods = data.periods;
    const int n = int(periods.size());
    Result out;
    out.kind = QStringLiteral("timeline");
    auto &items = out.items;
    double top = 0;
    if (!data.title.isEmpty()) {
        const Title title = titleOf(c, data.title, room - 80);
        items << title.item;
        top = CHART.head + 4;
        out.corner = cornerAt(title.width + 16, 28);
    }
    const double colW = std::min(TIMELINE.col, room / n);
    if (colW >= TIMELINE.minCol) {
        const double W = colW * n;
        if (std::any_of(periods.begin(), periods.end(),
                        [](const Period &p) { return !p.section.isEmpty(); })) {
            int k = 0;
            for (int i = 0; i < n;) {
                int j = i;
                while (j + 1 < n && periods[j + 1].section == periods[i].section)
                    ++j;
                if (!periods[i].section.isEmpty()) {
                    const double x0 = i * colW + 12, x1 = (j + 1) * colW - 12;
                    items << labelSpec(QStringLiteral("ts:%1").arg(k), i * 0.6, x0, top + 6,
                                       {caps(c, periods[i].section, x1 - x0)},
                                       QStringLiteral("dg-eyebrow"), QStringLiteral("start"), Back);
                    items << lineSpec(QStringLiteral("tb:%1").arg(k), i * 0.6, x0, top + 19, x1,
                                      top + 19, QStringLiteral("dg-bracket"), true, Back);
                    ++k;
                }
                i = j + 1;
            }
            top += 34;
        }
        QVector<QStringList> labels;
        int most = 0;
        for (const auto &p : periods) {
            labels << wrap(c, p.label, colW - 16, FONT::period).mid(0, 2);
            most = std::max(most, int(labels.last().size()));
        }
        const double labelH = most * 18;
        const double axisY = top + labelH + 15;
        items << lineSpec(QStringLiteral("axis"), 0, colW * 0.5, axisY, W - colW * 0.5, axisY,
                          QStringLiteral("dg-axis-line"), true, Back);
        double bottom = axisY;
        for (int i = 0; i < n; ++i) {
            const double cx = colW * (i + 0.5), order = 0.3 + i * 0.6;
            Spec period = labelSpec(QStringLiteral("tp:%1").arg(i), order, cx, axisY - 15,
                                    labels[i], QStringLiteral("dg-period"));
            period.fixed.baseline = QStringLiteral("above");
            period.fixed.lineHeight = 18;
            items << period;
            items << dotSpec(QStringLiteral("td:%1").arg(i), order + 0.1, cx, axisY, 4,
                             QStringLiteral("dg-tl-dot"), accent);
            double ey = axisY + 20;
            for (int j = 0; j < periods[i].events.size(); ++j) {
                const QStringList lines = wrap(c, periods[i].events[j], colW - 22, FONT::event);
                Spec event = labelSpec(
                    QStringLiteral("te:%1:%2").arg(i).arg(j), order + 0.25 + j * 0.12, cx, ey,
                    lines, j ? QStringLiteral("dg-event") : QStringLiteral("dg-event is-lead"));
                event.fixed.baseline = QStringLiteral("below");
                event.fixed.lineHeight = TIMELINE.line;
                items << event;
                ey += lines.size() * TIMELINE.line + 7;
            }
            bottom = std::max(bottom, ey - 7);
        }
        out.width = W;
        out.height = bottom + 4;
        return out;
    }
    double widest = -INFINITY;
    for (const auto &p : periods)
        widest = std::max(widest, c.widthOf(p.label, FONT::period));
    const double labelW = std::min(170.0, widest);
    const double lineX = labelW + 20, textX = lineX + 20, W = std::min(room, 720.0),
                 textW = W - textX;
    double y = top + 4, dotTop = NaN, dotBottom = 0;
    QString section;
    for (int i = 0; i < n; ++i) {
        const auto &p = periods[i];
        const double order = i * 0.5;
        if (!p.section.isEmpty() && p.section != section) {
            section = p.section;
            items << labelSpec(QStringLiteral("ts:%1").arg(i), order, textX, y + 6,
                               {caps(c, p.section, textW)}, QStringLiteral("dg-eyebrow"),
                               QStringLiteral("start"), Back);
            y += 26;
        }
        const QStringList plines = wrap(c, p.label, labelW, FONT::period);
        Spec period = labelSpec(QStringLiteral("tp:%1").arg(i), order, labelW, y, plines,
                                QStringLiteral("dg-period"), QStringLiteral("end"));
        period.fixed.baseline = QStringLiteral("below");
        period.fixed.lineHeight = 18;
        items << period;
        items << dotSpec(QStringLiteral("td:%1").arg(i), order + 0.1, lineX, y + 8, 4,
                         QStringLiteral("dg-tl-dot"), accent);
        if (!finite(dotTop))
            dotTop = y + 8;
        dotBottom = y + 8;
        double ey = y;
        for (int j = 0; j < p.events.size(); ++j) {
            const QStringList lines = wrap(c, p.events[j], textW, FONT::event);
            Spec event = labelSpec(
                QStringLiteral("te:%1:%2").arg(i).arg(j), order + 0.2 + j * 0.1, textX, ey + 1,
                lines, j ? QStringLiteral("dg-event") : QStringLiteral("dg-event is-lead"),
                QStringLiteral("start"));
            event.fixed.baseline = QStringLiteral("below");
            event.fixed.lineHeight = 18;
            items << event;
            ey += lines.size() * 18 + 4;
        }
        y = std::max(ey, y + plines.size() * 18) + 14;
    }
    if (n > 1)
        items << lineSpec(QStringLiteral("axis"), 0, lineX, dotTop, lineX, dotBottom,
                          QStringLiteral("dg-axis-line"), true, Back);
    out.width = W;
    out.height = y - 14;
    return out;
}

/* Gantt charts */

struct Task {
    QString name, section;
    QStringList tags;
    double start = 0, end = 0;
};

struct GanttData {
    QString title;
    QVector<Task> tasks;
    bool noToday = false;
};

std::optional<GanttData> parseGantt(const QStringList &lines)
{
    static const Re dateFormat(QStringLiteral("^dateFormat\\s+(.+)$"), I),
        section(QStringLiteral("^section\\s+(.+)$"), I),
        todayOff(QStringLiteral("^todayMarker\\s+off\\b"), I),
        skipped(QStringLiteral("^(axisFormat|tickInterval|excludes|includes|weekday|weekend|"
                               "inclusiveEndDates|topAxis|displayMode|todayMarker|accTitle|"
                               "accDescr)\\b"),
                I),
        task(QStringLiteral("^(.+?)\\s*:\\s*(.*)$")),
        tag(QStringLiteral("^(done|active|crit|milestone)$"), I),
        after(QStringLiteral("^after\\s"), I), until(QStringLiteral("^until\\s+(\\S+)"), I),
        space(QStringLiteral("\\s+"));
    GanttData data;
    QHash<QString, int> ids; // Task indexes, looked up only.
    QString fmt = QStringLiteral("YYYY-MM-DD"), current;
    int prev = -1;
    for (int li = 1; li < lines.size(); ++li) {
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if ((m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if ((m = dateFormat.match(line)).hasMatch()) {
            fmt = m.captured(1).trimmed();
            continue;
        }
        if ((m = section.match(line)).hasMatch()) {
            current = unquote(m.captured(1));
            continue;
        }
        if (todayOff.match(line).hasMatch()) {
            data.noToday = true;
            continue;
        }
        if (skipped.match(line).hasMatch())
            continue;
        if (!(m = task.match(line)).hasMatch())
            continue;
        if (data.tasks.size() >= MaxTasks)
            break;
        QStringList tokens, tags;
        for (const auto &token : m.captured(2).split(QLatin1Char(','))) {
            if (!token.trimmed().isEmpty())
                tokens << token.trimmed();
        }
        while (!tokens.isEmpty() && tag.match(tokens.first()).hasMatch())
            tags << tokens.takeFirst().toLower();
        QString id, from, to;
        if (tokens.size() >= 3) {
            id = tokens[0];
            from = tokens[1];
            to = tokens[2];
        } else if (tokens.size() == 2) {
            from = tokens[0];
            to = tokens[1];
        } else {
            to = tokens.isEmpty() ? QStringLiteral("1d") : tokens[0];
        }
        if (!from.isEmpty() && !after.match(from).hasMatch() && !finite(parseDate(from, fmt))) {
            id = from;
            from.clear();
        }
        const double prevEnd = prev >= 0 ? data.tasks[prev].end : NaN;
        double start = NaN;
        if (!from.isEmpty() && after.match(from).hasMatch()) {
            double latest = -INFINITY;
            bool any = false;
            for (const auto &ref : from.mid(6).split(space)) {
                const int at = ids.value(ref, -1);
                if (at >= 0 && finite(data.tasks[at].end)) {
                    latest = std::max(latest, data.tasks[at].end);
                    any = true;
                }
            }
            start = any ? latest : prevEnd;
        } else if (!from.isEmpty()) {
            start = parseDate(from, fmt);
        }
        if (!finite(start))
            start = finite(prevEnd) ? prevEnd : today();
        const bool milestone = tags.contains(QStringLiteral("milestone"));
        double end = NaN;
        if (!to.isEmpty()) {
            const auto u = until.match(to);
            if (u.hasMatch()) {
                const int at = ids.value(u.captured(1), -1);
                end = at >= 0 ? data.tasks[at].start : NaN;
            } else {
                const double duration = parseDuration(to);
                end = finite(duration) ? start + duration : parseDate(to, fmt);
            }
        }
        if (!finite(end) || end < start)
            end = start + (milestone ? 0 : DAY);
        if (milestone)
            end = start;
        if (!id.isEmpty())
            ids.insert(id, int(data.tasks.size()));
        data.tasks << Task{cleanLabel(m.captured(1)), current, tags, start, end};
        prev = int(data.tasks.size()) - 1;
    }
    if (data.tasks.isEmpty())
        return std::nullopt;
    return data;
}

struct TimeTick {
    double t = 0;
    QString label;
};

QVector<TimeTick> timeTicks(double lo, double hi, double plotW)
{
    const double span = hi - lo;
    const int max = std::max(2, int(std::floor(plotW / 78)));
    QVector<TimeTick> out;
    if (span <= 2 * DAY) {
        double hours = 24;
        for (const double h : {1, 2, 3, 6, 12, 24}) {
            if (span / (h * 36e5) <= max) {
                hours = h;
                break;
            }
        }
        const double step = hours * 36e5;
        double t = std::ceil(lo / step) * step;
        for (int k = 0; k < MaxLoop && t <= hi; ++k, t += step)
            out << TimeTick{t, hourMinute(t)};
    } else if (span <= max * DAY * 1.5) {
        const double days = std::max(1.0, std::ceil(span / DAY / max));
        double t = std::ceil(lo / DAY) * DAY;
        for (int k = 0; k < MaxLoop && t <= hi; ++k, t += days * DAY)
            out << TimeTick{t, dayMonth(t)};
    } else if (span <= max * 7 * DAY) {
        const double weeks = std::max(1.0, std::ceil(span / (7 * DAY) / max));
        double t = std::ceil(lo / DAY) * DAY;
        for (int k = 0; k < 7 && asUtc(t).date.isValid() && asUtc(t).date.dayOfWeek() != 1; ++k)
            t += DAY;
        for (int k = 0; k < MaxLoop && t <= hi; ++k, t += weeks * 7 * DAY)
            out << TimeTick{t, dayMonth(t)};
    } else if (span <= max * 186 * DAY) {
        int months = 12;
        for (const int k : {1, 2, 3, 6}) {
            if (span / (k * 30.44 * DAY) <= max) {
                months = k;
                break;
            }
        }
        const Utc d = asUtc(lo);
        if (!d.date.isValid())
            return out;
        for (int k = 1; k < MaxLoop; ++k) {
            const double t = dateUtc(d.fullYear(), d.date.month() - 1 + k);
            if (!finite(t) || t > hi)
                break;
            const int m = asUtc(t).date.month() - 1;
            if (m % months)
                continue;
            out << TimeTick{t, m == 0 || out.isEmpty() ? monthYear(t) : monthName(t)};
        }
    } else {
        const Utc d = asUtc(lo);
        if (!d.date.isValid())
            return out;
        const int years = int(std::min(1e6, std::max(1.0, std::ceil(span / (365.25 * DAY) / max))));
        for (int y = d.fullYear() + 1, k = 0; k < MaxLoop && dateUtc(y, 0) <= hi; y += years, ++k)
            out << TimeTick{dateUtc(y, 0), yearOf(dateUtc(y, 0))};
    }
    return out;
}

std::optional<Result> ganttScene(Ctx &c, const GanttData &data)
{
    const auto &tasks = data.tasks;
    const int accent = first(c.tones);
    double lo = INFINITY, hi = -INFINITY;
    for (const auto &t : tasks) {
        // A date past JS's range cannot be written: upstream's formatter throws on it.
        if (std::abs(t.start) > MaxTime || std::abs(t.end) > MaxTime)
            return std::nullopt;
        lo = std::min(lo, t.start);
        hi = std::max(hi, t.end);
    }
    if (hi - lo < 36e5)
        hi = lo + DAY;
    const double pad = (hi - lo) * 0.02;
    lo -= pad;
    hi += pad;
    if (!(hi > lo) || !finite(hi - lo))
        return std::nullopt;
    const double W = std::max(440.0, std::min(GANTT.maxWidth, c.width - PAD * 2));
    double widest = 96;
    for (const auto &t : tasks)
        widest = std::max(widest, c.widthOf(t.name, FONT::task));
    const double labelW = std::min(GANTT.label, widest + 26);
    const double plotW = W - labelW;
    const auto x = [&](double t) { return labelW + (t - lo) / (hi - lo) * plotW; };
    const bool hourly = hi - lo <= 2 * DAY;
    const auto when = [hourly](double t) { return hourly ? hourMinute(t) : dayMonth(t); };
    // { style: 'unit', unitDisplay: 'short', maximumFractionDigits: 1 }: 1 day, 3 days, 5 hr.
    const auto dur = [](double ms) {
        if (ms >= DAY) {
            const QString n = formatFixed(ms / DAY, 1);
            return n + (n == QLatin1String("1") ? QStringLiteral(" day") : QStringLiteral(" days"));
        }
        return formatFixed(ms / 36e5, 1) + QStringLiteral(" hr");
    };
    Result out;
    out.kind = QStringLiteral("gantt");
    auto &items = out.items;
    double head = 0;
    if (!data.title.isEmpty()) {
        const Title title = titleOf(c, data.title, W - 80);
        items << title.item;
        head = CHART.head;
        out.corner = cornerAt(title.width + 16, 28);
    }
    const double rowsTop = head + GANTT.axis;
    double y = rowsTop;
    std::optional<QString> section;
    int si = 0;
    for (int i = 0; i < tasks.size(); ++i) {
        const Task &t = tasks[i];
        if (!section || t.section != *section) {
            section = t.section;
            if (!section->isEmpty()) {
                if (y > rowsTop)
                    items << lineSpec(QStringLiteral("gr:%1").arg(si), i * 0.3, 0, y + 4, W, y + 4,
                                      QStringLiteral("dg-sep"), false, Back);
                // The name of a section has its line to itself and may run over the plot, past
                // the column of names.
                items << labelSpec(QStringLiteral("gs:%1").arg(si), i * 0.3, 0,
                                   y + GANTT.section / 2 + 6,
                                   {caps(c, *section, std::max(labelW - 16, W * 0.6))},
                                   QStringLiteral("dg-eyebrow is-over"), QStringLiteral("start"));
                ++si;
                y += GANTT.section;
            }
        }
        const double cy = y + GANTT.row / 2, order = 0.4 + i * 0.3;
        const auto has = [&](const char *name) { return t.tags.contains(QLatin1String(name)); };
        const QString cls = QStringLiteral("dg-task") +
                            (has("done") ? QStringLiteral(" is-done") : QString()) +
                            (has("crit") ? QStringLiteral(" is-crit") : QString()) +
                            (has("active") ? QStringLiteral(" is-active") : QString());
        items << labelSpec(QStringLiteral("gn:%1").arg(i), order, 0, cy,
                           {truncate(c, t.name, labelW - 20, FONT::task)},
                           QStringLiteral("dg-task-name") +
                               (has("done") ? QStringLiteral(" is-done") : QString()),
                           QStringLiteral("start"));
        const double x0 = x(t.start), x1 = std::max(x0 + 6, x(t.end));
        const QString key = QStringLiteral("gb:%1").arg(i);
        const bool milestone = has("milestone");
        Spec bar = item(Type::Span, key, order + 0.1);
        bar.props.x = x0;
        bar.props.y = cy;
        bar.props.w = milestone ? 0 : x1 - x0;
        bar.props.h = milestone ? 11 : GANTT.bar;
        bar.fixed.cls = cls;
        bar.fixed.tone = accent;
        bar.fixed.milestone = milestone;
        items << bar;
        const double last = t.end - t.start >= DAY && !hourly ? t.end - 1 : t.end;
        const QString text = milestone ? when(t.start) : dur(t.end - t.start);
        const double tw = c.widthOf(text, FONT::tick);
        const double after = milestone ? x0 + 11 : x1 + 8;
        if (after + tw <= W)
            items << labelSpec(QStringLiteral("gd:%1").arg(i), order + 0.6, after, cy, {text},
                               QStringLiteral("dg-task-meta"), QStringLiteral("start"));
        else if (x0 - 8 - tw >= labelW)
            items << labelSpec(QStringLiteral("gd:%1").arg(i), order + 0.6, x0 - 8, cy, {text},
                               QStringLiteral("dg-task-meta"), QStringLiteral("end"));
        Tip tip;
        tip.title = t.name;
        TipRow row;
        row.name = milestone ? QString() : when(t.start) + QString::fromUtf8(" – ") + when(last);
        row.value = milestone ? when(t.start) : dur(t.end - t.start);
        tip.rows << row;
        tip.hot = QStringList{key, QStringLiteral("gn:%1").arg(i)};
        out.tips.insert(key, tip);
        out.tips.insert(QStringLiteral("gn:%1").arg(i), tip);
        y += GANTT.row;
    }
    for (const auto &tick : timeTicks(lo, hi, plotW)) {
        const double tx = x(tick.t);
        if (tx < labelW || tx + 6 + c.widthOf(tick.label, FONT::tick) > W)
            continue;
        items << lineSpec(QStringLiteral("gt:") + jsNum(tick.t), 0, tx, rowsTop - 8, tx, y,
                          QStringLiteral("dg-grid"), false, Back);
        items << labelSpec(QStringLiteral("gl:") + jsNum(tick.t), 0, tx + 6, head + 11,
                           {tick.label}, QStringLiteral("dg-tick"), QStringLiteral("start"), Back);
    }
    const double now = double(QDateTime::currentMSecsSinceEpoch());
    if (!data.noToday && !hourly && now > lo && now < hi) {
        const double tx = x(now);
        const QString text = caps(c, QStringLiteral("Today"));
        const double tw = capsWidth(c, text);
        items << lineSpec(QStringLiteral("today"), 3, tx, rowsTop - 8, tx, y + 2,
                          QStringLiteral("dg-today"), true, Nodes);
        items << labelSpec(QStringLiteral("today-tag"), 3.4, clamp(tx, labelW + tw / 2, W - tw / 2),
                           y + 13, {text}, QStringLiteral("dg-today-tag"), QStringLiteral("middle"),
                           Labels);
        y += 22;
    }
    out.width = W;
    out.height = y + 4;
    return out;
}

/* Mindmaps */

struct MindNode {
    QString label, path;
    QVector<int> children;
    int parent = -1, depth = 0, tone = NoTone, leaves = 1;
    QStringList lines;
    double w = 0, h = 0, y = 0, ax = 0, dir = 1, order = 0;
};

QString mindLabel(const QString &text)
{
    static const Re cls(QStringLiteral(":::[\\w\\s-]+$")),
        shaped(QStringLiteral("^[\\p{L}\\p{N}_-]*\\s*(\\(\\(|\\)\\)|\\{\\{|\\(|\\)|\\[)"
                              "([\\s\\S]*?)(\\)\\)|\\(\\(|\\}\\}|\\)|\\(|\\])$"));
    const QString s = replaceFirst(text, cls).trimmed();
    const auto m = shaped.match(s);
    return cleanLabel(m.hasMatch() ? m.captured(2) : s);
}

// The tree in one list, the root first.
std::optional<QVector<MindNode>> parseMindmap(const QStringList &lines)
{
    static const Re skip(QStringLiteral("^::icon\\(|^:::|^%%"));
    QVector<MindNode> nodes;
    QVector<int> tops; // The first is the root; the rest join it.
    struct Open {
        int indent, node;
    };
    QVector<Open> stack;
    for (int li = 1; li < lines.size(); ++li) {
        const QString &raw = lines.at(li);
        const QString text = raw.trimmed();
        if (skip.match(text).hasMatch())
            continue;
        if (nodes.size() >= MaxMindNodes)
            break;
        int indent = 0;
        for (const QChar ch : raw) {
            if (ch == QLatin1Char('\t'))
                indent += 4;
            else if (ch.isSpace())
                indent += 1;
            else
                break;
        }
        while (!stack.isEmpty() && stack.last().indent >= indent)
            stack.removeLast();
        // Past MaxMindDepth a node goes beside the deepest one allowed.
        while (stack.size() >= MaxMindDepth)
            stack.removeLast();
        MindNode node;
        node.label = mindLabel(text);
        const int at = int(nodes.size());
        if (stack.isEmpty()) {
            tops << at;
        } else {
            node.parent = stack.last().node;
            nodes[node.parent].children << at;
        }
        nodes << node;
        stack << Open{indent, at};
    }
    if (tops.isEmpty())
        return std::nullopt;
    // The first node read is a top, so the root is node 0.
    for (int i = 1; i < tops.size(); ++i) {
        nodes[0].children << tops[i];
        nodes[tops[i]].parent = 0;
    }
    return nodes;
}

struct MindText {
    double size;
    int weight;
    double line, max, padX, padY;
};
constexpr MindText MIND_TEXT[] = {
    {13.5, 650, 19, 220, 30, 18}, {12.5, 600, 17, 180, 24, 12}, {12.5, 450, 17, 220, 14, 5}};

std::optional<Result> mindScene(Ctx &c, QVector<MindNode> nodes)
{
    const Tones &tones = c.tones;
    const auto toneAt = [&](int i) { return tones.isEmpty() ? 1 : tones.at(i % tones.size()); };
    // Depth first, children in order; the parser bounds the depth.
    std::function<void(int, int, const QString &)> measure = [&](int at, int depth,
                                                                 const QString &path) {
        const MindText &t = MIND_TEXT[std::min(depth, 2)];
        MindNode &node = nodes[at];
        node.depth = depth;
        node.path = path;
        node.lines = wrap(c, node.label, t.max, t.size, t.weight);
        node.w = std::ceil(textMax(c, node.lines, t.size, t.weight) + t.padX);
        node.h = node.lines.size() * t.line + t.padY;
        const QVector<int> children = node.children;
        for (int i = 0; i < children.size(); ++i)
            measure(children[i], depth + 1, path + QLatin1Char('.') + QString::number(i));
        int leaves = 0;
        for (const int child : children)
            leaves += nodes[child].leaves;
        nodes[at].leaves = children.isEmpty() ? 1 : leaves;
    };
    measure(0, 0, QStringLiteral("r"));
    const QVector<int> rootKids = nodes[0].children;
    QVector<int> right, left;
    double acc = 0;
    for (const int child : rootKids) {
        if (acc < nodes[0].leaves / 2.0) {
            right << child;
            acc += nodes[child].leaves;
        } else {
            left << child;
        }
    }
    QVector<int> placed;
    const auto side = [&](const QVector<int> &kids, double dir) {
        if (kids.isEmpty())
            return;
        QVector<double> colW;
        std::function<void(int)> walk = [&](int at) {
            const MindNode &node = nodes[at];
            if (colW.size() <= node.depth)
                colW.resize(node.depth + 1);
            colW[node.depth] = std::max(colW[node.depth], node.w);
            for (const int child : node.children)
                walk(child);
        };
        for (const int kid : kids)
            walk(kid);
        QVector<double> colX{0, nodes[0].w / 2 + MIND.gapX};
        for (int d = 2; d < colW.size(); ++d)
            colX << colX[d - 1] + colW[d - 1] + MIND.gapX * (d == 2 ? 1 : 0.75);
        double y = 0;
        std::function<void(int, int, double)> place = [&](int at, int tone, double order) {
            MindNode &node = nodes[at];
            node.tone = tone;
            node.dir = dir;
            node.ax = dir * colX.value(node.depth);
            node.order = order;
            if (node.children.isEmpty()) {
                node.y = y + node.h / 2;
                y += node.h + MIND.gapY;
                return;
            }
            const QVector<int> children = node.children;
            for (int j = 0; j < children.size(); ++j)
                place(children[j], tone, order + 0.35 + j * 0.12);
            nodes[at].y = (nodes[children.first()].y + nodes[children.last()].y) / 2;
        };
        for (int i = 0; i < kids.size(); ++i) {
            if (i)
                y += MIND.branch;
            const int index = int(rootKids.indexOf(kids[i]));
            place(kids[i], toneAt(index), 0.5 + index * 0.4);
        }
        const double shift = -(y - MIND.gapY) / 2;
        std::function<void(int)> fix = [&](int at) {
            nodes[at].y += shift;
            placed << at;
            for (const int child : nodes[at].children)
                fix(child);
        };
        for (const int kid : kids)
            fix(kid);
    };
    side(right, 1);
    side(left, -1);
    nodes[0].y = 0;
    const MindNode &root = nodes[0];
    double minX = -root.w / 2, maxX = root.w / 2, minY = -root.h / 2, maxY = root.h / 2;
    for (const int at : placed) {
        const MindNode &node = nodes[at];
        const double far = node.ax + node.dir * node.w;
        minX = std::min({minX, node.ax, far});
        maxX = std::max({maxX, node.ax, far});
        minY = std::min(minY, node.y - node.h / 2);
        maxY = std::max(maxY, node.y + node.h / 2);
    }
    const double ox = -minX, oy = -minY;
    Result out;
    out.kind = QStringLiteral("mindmap");
    auto &items = out.items;
    Spec top = item(Type::Node, QStringLiteral("m:r"), 0);
    top.props.x = ox;
    top.props.y = oy;
    top.props.w = root.w;
    top.props.h = root.h;
    top.fixed.shape = QStringLiteral("round");
    top.fixed.lines = root.lines;
    top.fixed.tone = first(tones);
    top.fixed.id = QStringLiteral("r");
    top.fixed.cls = QStringLiteral("is-root");
    items << top;
    for (const int at : placed) {
        const MindNode &node = nodes[at];
        const MindNode &p = nodes[node.parent];
        const double dir = node.dir;
        const bool fromRoot = node.parent == 0;
        const double sx = ox + (fromRoot ? dir * root.w / 2 : p.ax + dir * p.w);
        const double sy = oy + (fromRoot ? clamp(node.y * 0.2, -root.h / 4, root.h / 4) : p.y);
        const double ex = ox + node.ax, ey = oy + node.y, mid = (ex - sx) / 2;
        Spec edge = item(Type::Edge, QStringLiteral("mb:") + node.path, node.order - 0.1);
        edge.props.pts = Pts{sx, sy, sx + mid, sy, ex - mid, ey, ex, ey};
        edge.fixed.style = QStringLiteral("solid");
        edge.fixed.head = QStringLiteral("none");
        edge.fixed.tone = node.tone;
        edge.fixed.cls = QStringLiteral("dg-branch is-d%1").arg(std::min(node.depth, 3));
        edge.fixed.draw = 420;
        items << edge;
        if (node.depth == 1) {
            Spec spec = item(Type::Node, QStringLiteral("m:") + node.path, node.order);
            spec.props.x = ox + node.ax + dir * node.w / 2;
            spec.props.y = ey;
            spec.props.w = node.w;
            spec.props.h = node.h;
            spec.fixed.shape = QStringLiteral("round");
            spec.fixed.lines = node.lines;
            spec.fixed.tone = node.tone;
            spec.fixed.id = node.path;
            spec.fixed.cls = QStringLiteral("is-branch");
            items << spec;
        } else {
            items << dotSpec(QStringLiteral("md:") + node.path, node.order, ex, ey, 3,
                             QStringLiteral("dg-mind-dot"), node.tone);
            Spec label = labelSpec(QStringLiteral("m:") + node.path, node.order + 0.05,
                                   ex + dir * 9, ey, node.lines, QStringLiteral("dg-mind-leaf"),
                                   dir > 0 ? QStringLiteral("start") : QStringLiteral("end"));
            label.fixed.lineHeight = 17;
            items << label;
        }
    }
    out.width = maxX - minX;
    out.height = maxY - minY;
    return out;
}

/* Quadrant charts */

struct QuadPoint {
    QString label;
    double x = 0, y = 0;
};

struct QuadData {
    QString title;
    QStringList x{QString(), QString()}, y{QString(), QString()},
        q{QString(), QString(), QString(), QString()};
    QVector<QuadPoint> points;
};

std::optional<QuadData> parseQuadrant(const QStringList &lines)
{
    static const Re axis(QStringLiteral("^([xy])-axis\\s+(.+?)(?:\\s*-->\\s*(.+))?$"), I),
        quadrant(QStringLiteral("^quadrant-([1-4])\\s+(.+)$"), I),
        point(QStringLiteral(
            "^(.+?)\\s*(?::::[\\w-]+)?\\s*:\\s*\\[\\s*([-\\d.]+)\\s*,\\s*([-\\d.]+)\\s*\\]"));
    QuadData data;
    for (int li = 1; li < lines.size(); ++li) {
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if ((m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if ((m = axis.match(line)).hasMatch()) {
            QStringList &target = m.captured(1).toLower() == QLatin1String("x") ? data.x : data.y;
            target = QStringList{unquote(m.captured(2)),
                                 !m.captured(3).isEmpty() ? unquote(m.captured(3)) : QString()};
            continue;
        }
        if ((m = quadrant.match(line)).hasMatch()) {
            data.q[m.captured(1).toInt() - 1] = unquote(m.captured(2));
            continue;
        }
        if ((m = point.match(line)).hasMatch()) {
            const double x = number(m.captured(2)), y = number(m.captured(3));
            if (finite(x) && finite(y) && data.points.size() < MaxPoints)
                data.points << QuadPoint{unquote(m.captured(1)), x, y};
        }
    }
    double max = 1;
    for (const auto &p : data.points)
        max = std::max({max, p.x, p.y});
    if (max > 1) {
        for (auto &p : data.points) {
            p.x /= max > 10 ? 100 : 10;
            p.y /= max > 10 ? 100 : 10;
        }
    }
    const bool named =
        std::any_of(data.q.begin(), data.q.end(), [](const QString &q) { return !q.isEmpty(); });
    if (data.points.isEmpty() && !named)
        return std::nullopt;
    return data;
}

std::optional<Result> quadrantScene(Ctx &c, const QuadData &data)
{
    const int accent = first(c.tones);
    const double width = c.width;
    Result out;
    out.kind = QStringLiteral("quadrant");
    auto &items = out.items;
    double top = 0;
    if (!data.title.isEmpty()) {
        const Title title = titleOf(c, data.title, std::max(160.0, width - 80));
        items << title.item;
        top = CHART.head + 4;
        out.corner = cornerAt(title.width + 16, 28);
    }
    // Two hairlines cross in the middle; the corner worth going for lies under a faint wash, the
    // rest stay bare.
    const double ax = !data.y[0].isEmpty() || !data.y[1].isEmpty() ? 24 : 0;
    const double S = clamp(width - ax, QUAD.min, QUAD.max), half = S / 2;
    const auto px = [&](double v) { return ax + clamp01(v) * S; };
    const auto py = [&](double v) { return top + (1 - clamp01(v)) * S; };
    Spec wash = item(Type::Rect, QStringLiteral("q:wash"), 0, Back);
    wash.props.x = ax + half;
    wash.props.y = top;
    wash.props.w = half;
    wash.props.h = half;
    wash.fixed.cls = QStringLiteral("dg-quad");
    wash.fixed.rx = 0;
    items << wash;
    items << lineSpec(QStringLiteral("q:h"), 0.1, ax, top + half, ax + S, top + half,
                      QStringLiteral("dg-quad-axis"), true, Back);
    items << lineSpec(QStringLiteral("q:v"), 0.1, ax + half, top + S, ax + half, top,
                      QStringLiteral("dg-quad-axis"), true, Back);
    struct Cell {
        int q;
        double x, y;
        bool start, atTop;
    };
    const Cell cells[] = {{1, ax + half, top, false, true},
                          {2, ax, top, true, true},
                          {3, ax, top + half, true, false},
                          {4, ax + half, top + half, false, false}};
    for (int i = 0; i < 4; ++i) {
        const Cell &cell = cells[i];
        const QString &text = data.q[cell.q - 1];
        if (text.isEmpty())
            continue;
        items << labelSpec(QStringLiteral("ql:%1").arg(cell.q), 0.3 + i * 0.15,
                           cell.start ? cell.x + 10 : cell.x + half - 10,
                           cell.atTop ? cell.y + 14 : cell.y + half - 14,
                           {caps(c, text, half - 24)},
                           QStringLiteral("dg-quad-label") +
                               (cell.q == 1 ? QStringLiteral(" is-lead") : QString()),
                           cell.start ? QStringLiteral("start") : QStringLiteral("end"), Back);
    }
    const double bottom = top + S;
    if (!data.x[0].isEmpty())
        items << labelSpec(QStringLiteral("x0"), 0.5, ax, bottom + 15, {data.x[0]},
                           QStringLiteral("dg-axis-title"), QStringLiteral("start"), Back);
    if (!data.x[1].isEmpty())
        items << labelSpec(QStringLiteral("x1"), 0.5, ax + S, bottom + 15,
                           {data.x[1] + QString::fromUtf8(" →")}, QStringLiteral("dg-axis-title"),
                           QStringLiteral("end"), Back);
    if (!data.y[0].isEmpty()) {
        Spec label = labelSpec(QStringLiteral("y0"), 0.5, 8, bottom, {data.y[0]},
                               QStringLiteral("dg-axis-title"), QStringLiteral("start"), Back);
        label.fixed.rotate = -90;
        items << label;
    }
    if (!data.y[1].isEmpty()) {
        Spec label =
            labelSpec(QStringLiteral("y1"), 0.5, 8, top, {data.y[1] + QString::fromUtf8(" →")},
                      QStringLiteral("dg-axis-title"), QStringLiteral("end"), Back);
        label.fixed.rotate = -90;
        items << label;
    }
    struct Box {
        double x, y, w, h;
    };
    QVector<Box> boxes;
    for (const auto &p : data.points)
        boxes << Box{px(p.x) - 7, py(p.y) - 7, 14, 14};
    const auto hit = [](const Box &a, const Box &b) {
        return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    };
    for (int i = 0; i < data.points.size(); ++i) {
        if (!c.budget(int(boxes.size())))
            return std::nullopt;
        const auto &p = data.points[i];
        const double x = px(p.x), y = py(p.y), tw = c.widthOf(p.label, FONT::point);
        struct Option {
            double x, y;
            const char *anchor;
            double left;
        };
        const Option options[] = {{x + 10, y, "start", x + 10},
                                  {x - 10, y, "end", x - 10 - tw},
                                  {x, y - 15, "middle", x - tw / 2},
                                  {x, y + 16, "middle", x - tw / 2}};
        Option pick = options[0];
        for (const Option &option : options) {
            const Box box{option.left, option.y - 8, tw, 16};
            if (box.x < ax || box.x + box.w > ax + S || box.y < top || box.y + box.h > bottom)
                continue;
            bool clear = true;
            for (int k = 0; k < boxes.size() && clear; ++k)
                clear = k == i || !hit(box, boxes[k]);
            if (!clear)
                continue;
            pick = option;
            break;
        }
        boxes << Box{pick.left, pick.y - 8, tw, 16};
        const QString key = QStringLiteral("qp:%1").arg(i);
        const double order = 1 + i * 0.25;
        items << dotSpec(key, order, x, y, 4.5, QStringLiteral("dg-q-point"), accent);
        items << labelSpec(QStringLiteral("qn:%1").arg(i), order + 0.1, pick.x, pick.y, {p.label},
                           QStringLiteral("dg-q-name"), QLatin1String(pick.anchor));
        Tip tip;
        tip.title = p.label;
        TipRow rx, ry;
        rx.name = data.x[1].isEmpty() ? QStringLiteral("x") : data.x[1];
        rx.value = format(p.x);
        ry.name = data.y[1].isEmpty() ? QStringLiteral("y") : data.y[1];
        ry.value = format(p.y);
        tip.rows << rx << ry;
        tip.hot = QStringList{key, QStringLiteral("qn:%1").arg(i)};
        out.tips.insert(key, tip);
        out.tips.insert(QStringLiteral("qn:%1").arg(i), tip);
    }
    out.width = ax + S;
    out.height = bottom + (!data.x[0].isEmpty() || !data.x[1].isEmpty() ? 26 : 4);
    out.flush = true;
    return out;
}

/* Radar charts */

struct RadarAxis {
    QString id, label;
};
struct RadarCurve {
    QString label;
    QVector<double> data;
};
struct RadarData {
    QString title;
    QVector<RadarAxis> axes;
    QVector<RadarCurve> curves;
    std::optional<double> min, max;
    int levels = RADAR.levels;
    bool circle = false;
};

std::optional<RadarData> parseRadar(const QStringList &lines)
{
    static const Re entry(
        QStringLiteral("^([\\p{L}\\p{N}_-]+)(?:\\s*\\[\\s*\"?([^\"\\]]*)\"?\\s*\\])?$")),
        axis(QStringLiteral("^axis\\s+(.+)$"), I),
        curve(QStringLiteral("^curve\\s+([\\p{L}\\p{N}_-]+)(?:\\s*\\[\\s*\"?([^\"\\]]*)\"?\\s*\\])?"
                             "\\s*\\{(.*)\\}\\s*$")),
        keyValue(QStringLiteral("^([\\p{L}\\p{N}_-]+)\\s*:\\s*(.+)$")),
        bound(QStringLiteral("^(max|min)\\s+([-\\d.]+)$"), I),
        ticks(QStringLiteral("^ticks\\s+(\\d+)$"), I),
        graticule(QStringLiteral("^graticule\\s+circle"), I);
    RadarData data;
    struct Raw {
        QString label;
        QHash<QString, double> values; // Looked up only.
        QVector<double> list;
    };
    QVector<Raw> raws;
    for (int li = 1; li < lines.size(); ++li) {
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if ((m = titleLine().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if ((m = axis.match(line)).hasMatch()) {
            for (const auto &part : splitList(m.captured(1))) {
                const auto e = entry.match(part);
                if (e.hasMatch() && data.axes.size() < MaxAxes)
                    data.axes << RadarAxis{
                        e.captured(1),
                        cleanLabel(!e.captured(2).isEmpty() ? e.captured(2) : e.captured(1))};
            }
            continue;
        }
        if ((m = curve.match(line)).hasMatch()) {
            Raw raw;
            raw.label = cleanLabel(!m.captured(2).isEmpty() ? m.captured(2) : m.captured(1));
            for (const auto &part : splitList(m.captured(3))) {
                const auto kv = keyValue.match(part);
                if (kv.hasMatch())
                    raw.values.insert(kv.captured(1), number(kv.captured(2)));
                else if (raw.list.size() < MaxAxes)
                    raw.list << number(part);
            }
            if (raws.size() < MaxCurves)
                raws << raw;
            continue;
        }
        if ((m = bound.match(line)).hasMatch()) {
            // An unread bound is none (upstream would carry NaN into the scale).
            const double v = number(m.captured(2));
            (m.captured(1).toLower() == QLatin1String("max") ? data.max : data.min) =
                finite(v) ? std::optional<double>(v) : std::nullopt;
            continue;
        }
        if ((m = ticks.match(line)).hasMatch()) {
            data.levels = int(clamp(m.captured(1).toDouble(), 2, 8));
            continue;
        }
        if (graticule.match(line).hasMatch())
            data.circle = true;
    }
    for (const auto &raw : raws) {
        RadarCurve curve;
        curve.label = raw.label;
        for (int i = 0; i < data.axes.size(); ++i) {
            const auto &id = data.axes[i].id;
            const double v = raw.values.contains(id) ? raw.values.value(id)
                             : i < raw.list.size()   ? raw.list[i]
                                                     : 0;
            curve.data << (finite(v) ? v : 0);
        }
        data.curves << curve;
    }
    if (data.axes.size() < 3 || data.curves.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> radarScene(Ctx &c, const RadarData &data)
{
    const int n = int(data.axes.size());
    const double width = c.width;
    Result out;
    out.kind = QStringLiteral("radar");
    auto &items = out.items;
    double widest = -INFINITY;
    for (const auto &a : data.axes)
        widest = std::max(widest, c.widthOf(a.label, FONT::legend));
    const double labelW = std::min(150.0, widest);
    const double R = clamp((std::min(620.0, width) - 2 * (labelW + 16)) / 2, RADAR.min, RADAR.max);
    QVector<double> values;
    for (const auto &curve : data.curves)
        values += curve.data;
    const double lo = data.min ? *data.min : 0;
    const double hi = data.max ? *data.max : niceCeil(std::max(maxOf(values), lo + 1));
    std::optional<ChipRow> legend;
    if (data.curves.size() > 1) {
        QVector<ChipEntry> entries;
        for (int i = 0; i < data.curves.size(); ++i)
            entries << ChipEntry{data.curves[i].label, slot(series(), i), QStringLiteral("line")};
        legend = chips(c, entries, 0, 0, 600);
    }
    double top = 0;
    if (!data.title.isEmpty()) {
        const Title title = titleOf(c, data.title, std::max(160.0, width - 80));
        items << title.item;
        top = CHART.head;
        out.corner = cornerAt(title.width + 16, 28);
    }
    const double W = std::max(2 * (labelW + 16 + R), legend ? legend->width : 0.0), cx = W / 2,
                 cy = top + 22 + R;
    const auto angle = [&](int i) { return -Pi / 2 + i * 2 * Pi / n; };
    const auto at = [&](int i, double r) {
        return QPointF(cx + std::cos(angle(i)) * r, cy + std::sin(angle(i)) * r);
    };
    const auto ringOf = [&](double r) {
        Pts vs;
        if (data.circle) {
            for (int k = 0; k < 64; ++k)
                vs << cx + std::cos(k / 64.0 * 2 * Pi) * r << cy + std::sin(k / 64.0 * 2 * Pi) * r;
        } else {
            for (int i = 0; i < n; ++i) {
                const QPointF p = at(i, r);
                vs << p.x() << p.y();
            }
        }
        return vs;
    };
    // A value past the scale's ends sits on them; an empty scale (max at min) puts all at the
    // centre, where upstream's NaN would draw nothing.
    const auto ratio = [&](double v) {
        const double t = (v - lo) / (hi - lo);
        return t == t ? clamp01(t) : 0.0;
    };
    for (int k = 1; k <= data.levels; ++k) {
        Spec grid = item(Type::Poly, QStringLiteral("rg:%1").arg(k), k * 0.1, Back);
        grid.props.vs = ringOf(R * k / data.levels);
        grid.fixed.cx = cx;
        grid.fixed.cy = cy;
        grid.fixed.cls = QStringLiteral("dg-radar-grid") +
                         (k == data.levels ? QStringLiteral(" is-outer") : QString());
        items << grid;
        if (k < data.levels)
            items << labelSpec(QStringLiteral("rt:%1").arg(k), 0.6, cx + 5,
                               cy - R * k / data.levels - 6,
                               {format(lo + (hi - lo) * k / data.levels)},
                               QStringLiteral("dg-tick is-radar"), QStringLiteral("start"), Front);
    }
    double low = cy + R;
    for (int i = 0; i < n; ++i) {
        const QPointF p = at(i, R), l = at(i, R + 12);
        const double cos = std::cos(angle(i)), sin = std::sin(angle(i));
        items << lineSpec(QStringLiteral("rs:%1").arg(i), 0.2, cx, cy, p.x(), p.y(),
                          QStringLiteral("dg-radar-spoke"), true, Back);
        items << labelSpec(QStringLiteral("ra:%1").arg(i), 0.4 + i * 0.05, l.x(), l.y() + sin * 5,
                           {truncate(c, data.axes[i].label, labelW, FONT::legend)},
                           QStringLiteral("dg-radar-axis"),
                           cos > 0.3    ? QStringLiteral("start")
                           : cos < -0.3 ? QStringLiteral("end")
                                        : QStringLiteral("middle"),
                           Labels);
        low = std::max(low, l.y() + sin * 5 + 8);
    }
    for (int k = 0; k < data.curves.size(); ++k) {
        const auto &curve = data.curves[k];
        // Upstream's `slot(ring(tones, …), c)` calls radarScene's own `ring` (the grid's), which
        // shadows the palette's: the slot is NaN, so curves, their points and tips carry no tone
        // and draw in the drawing's default (--s1). Kept as drawn.
        const int tone = NoTone;
        const double order = 1 + k * 0.5;
        Pts vs;
        QVector<QPointF> points;
        for (int i = 0; i < n; ++i) {
            const QPointF p = at(i, R * ratio(curve.data[i]));
            points << p;
            vs << p.x() << p.y();
        }
        Spec poly = item(Type::Poly, QStringLiteral("rc:%1").arg(k), order);
        poly.props.vs = vs;
        poly.fixed.cx = cx;
        poly.fixed.cy = cy;
        poly.fixed.tone = tone;
        poly.fixed.cls = QStringLiteral("dg-radar-curve");
        items << poly;
        for (int i = 0; i < n; ++i) {
            const QString key = QStringLiteral("rp:%1:%2").arg(k).arg(i);
            items << dotSpec(key, order + 0.6, points[i].x(), points[i].y(), 2.75,
                             QStringLiteral("dg-point"), tone, Labels);
            Tip tip;
            tip.title = data.axes[i].label;
            TipRow row;
            row.tone = tone;
            row.mark = QStringLiteral("line");
            row.name = curve.label;
            row.value = format(curve.data[i]);
            tip.rows << row;
            out.tips.insert(key, tip);
        }
    }
    // The legend clears the lowest label, whatever the number of axes puts there.
    double H = low + 10;
    if (legend) {
        for (auto chip : legend->items) {
            chip.props.x += (W - legend->width) / 2;
            chip.props.y += H + 12;
            chip.order += 2;
            items << chip;
        }
        H += legend->height + 8;
    }
    out.width = W;
    out.height = H;
    out.flush = true;
    return out;
}
} // namespace

std::optional<Result> sequenceKind(Ctx &c, const Lines &lines)
{
    auto data = parseSequence(c, lines.lines);
    if (!data || c.cancelled())
        return std::nullopt;
    // A scheme takes the title its lines came with, unless it names one itself (titled).
    if (data->title.isEmpty())
        data->title = lines.title;
    return sequenceScene(c, std::move(*data));
}

std::optional<Result> pieKind(Ctx &c, const Lines &lines)
{
    const auto data = parsePie(lines.lines);
    return data && !c.cancelled() ? pieScene(c, *data) : std::nullopt;
}

std::optional<Result> xyKind(Ctx &c, const Lines &lines)
{
    const auto data = parseXY(lines.lines);
    return data && !c.cancelled() ? chartScene(c, *data) : std::nullopt;
}

std::optional<Result> candleKind(Ctx &c, const Lines &lines)
{
    auto data = parseCandles(lines.lines);
    return data && !c.cancelled() ? candleScene(c, std::move(*data)) : std::nullopt;
}

std::optional<Result> timelineKind(Ctx &c, const Lines &lines)
{
    const auto data = parseTimeline(lines.lines);
    return data && !c.cancelled() ? timelineScene(c, *data) : std::nullopt;
}

std::optional<Result> ganttKind(Ctx &c, const Lines &lines)
{
    const auto data = parseGantt(lines.lines);
    return data && !c.cancelled() ? ganttScene(c, *data) : std::nullopt;
}

std::optional<Result> mindKind(Ctx &c, const Lines &lines)
{
    auto data = parseMindmap(lines.lines);
    return data && !c.cancelled() ? mindScene(c, std::move(*data)) : std::nullopt;
}

std::optional<Result> quadrantKind(Ctx &c, const Lines &lines)
{
    const auto data = parseQuadrant(lines.lines);
    return data && !c.cancelled() ? quadrantScene(c, *data) : std::nullopt;
}

std::optional<Result> radarKind(Ctx &c, const Lines &lines)
{
    const auto data = parseRadar(lines.lines);
    return data && !c.cancelled() ? radarScene(c, *data) : std::nullopt;
}
} // namespace diagram
