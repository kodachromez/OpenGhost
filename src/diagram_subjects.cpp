#include "diagram_engine.h"

#include <QLocale>

#include <algorithm>

// Kinds made for one subject: facts, checklist, changes, outline, matches,
// words, gloss, forms, recipe, parts, settings and route (diagram.js
// parseFacts … routeScene).
namespace diagram
{
namespace
{
/* Shared by these kinds */

Re rx(const char *pattern, Re::PatternOptions options = Re::NoPatternOption)
{
    return Re(QString::fromUtf8(pattern), options);
}

bool test(const Re &re, const QString &text) { return re.match(text).hasMatch(); }

QString sep() { return QStringLiteral(" · "); }

// `title …` on a line of its own, as every kind here reads it.
const Re &titleRe()
{
    static const Re re = rx("^title(?:\\s*:\\s*|\\s+)(.+)$", I);
    return re;
}

bool hasBar(const QString &line) { return line.contains(QLatin1Char('|')); }

// .filter(Boolean)
QStringList nonEmpty(const QStringList &cells)
{
    QStringList out;
    for (const QString &cell : cells) {
        if (!cell.isEmpty())
            out << cell;
    }
    return out;
}

QStringList mapped(const QStringList &cells, QString (*fn)(const QString &))
{
    QStringList out;
    for (const QString &cell : cells)
        out << fn(cell);
    return out;
}

// .replace(/:$/, '')
QString dropColon(QString text)
{
    if (text.endsWith(QLatin1Char(':')))
        text.chop(1);
    return text;
}

// String.prototype.trimStart: whitespace off the start only.
QString trimStart(const QString &text)
{
    int n = 0;
    while (n < text.size() && text.at(n).isSpace())
        ++n;
    return text.mid(n);
}

// Math.round: halves go up.
double jsRound(double v)
{
    const double f = std::floor(v);
    return v - f >= 0.5 ? f + 1 : f;
}

// A number as JavaScript writes it in a template string.
QString jsNumber(double v)
{
    if (std::isnan(v))
        return QStringLiteral("NaN");
    if (std::isinf(v))
        return v > 0 ? QStringLiteral("Infinity") : QStringLiteral("-Infinity");
    if (v == 0)
        return QStringLiteral("0");
    const double a = std::abs(v);
    if (a >= 1e21 || a < 1e-6)
        return QString::number(v, 'e', QLocale::FloatingPointShortest);
    return QString::number(v, 'f', QLocale::FloatingPointShortest);
}

// Math.max(floor, ...list.map(f)).
template <typename List, typename F> double maxOf(double floor, const List &list, F f)
{
    double m = floor;
    for (const auto &x : list)
        m = std::max(m, double(f(x)));
    return m;
}

Tip silent(const QStringList &hot = {})
{
    Tip tip;
    tip.silent = true;
    tip.hot = hot;
    return tip;
}

// A label of lines set under its top (baseline 'below').
Spec block(const QString &key, double order, double x, double y, const QStringList &lines,
           double lineHeight, const QString &cls, int layer = DefaultLayer)
{
    Spec s = labelSpec(key, order, x, y, lines, cls, QStringLiteral("start"), layer);
    s.fixed.baseline = QStringLiteral("below");
    s.fixed.lineHeight = lineHeight;
    return s;
}

Spec dotSpec(const QString &key, double order, double x, double y, double r, const QString &cls,
             int tone = NoTone, int layer = DefaultLayer)
{
    Spec s = item(Type::Dot, key, order, layer);
    s.props.x = x;
    s.props.y = y;
    s.props.r = r;
    s.fixed.cls = cls;
    s.fixed.tone = tone;
    return s;
}

Spec hitSpec(const QString &key, double order, double x, double y, double w, double h)
{
    Spec s = item(Type::Hit, key, order, Back);
    s.props.x = x;
    s.props.y = y;
    s.props.w = w;
    s.props.h = h;
    return s;
}

// A hairline between rows.
Spec rule(const QString &key, double order, double y, double W)
{
    return lineSpec(key, order, 0, y, W, y, QStringLiteral("dg-grid"), false, Back);
}

Spec glyph(const QString &key, double order, double x, double y, double s, const QString &icon,
           const QString &cls, int tone = NoTone)
{
    Spec g = item(Type::WGlyph, key, order);
    g.props.x = x;
    g.props.y = y;
    g.props.s = s;
    g.fixed.icon = icon;
    g.fixed.cls = cls;
    g.fixed.tone = tone;
    return g;
}

Spec boxSpec(Type type, const QString &key, double order, double x, double y, double w, double h,
             const QString &cls, int tone, double radius, int layer = DefaultLayer)
{
    Spec s = item(type, key, order, layer);
    s.props.x = x;
    s.props.y = y;
    s.props.w = w;
    s.props.h = h;
    s.fixed.cls = cls;
    s.fixed.tone = tone;
    s.fixed.rx = radius;
    return s;
}

// The title on the first line, and the room it keeps at the top left: the
// top the drawing starts at.
double placeTitle(Ctx &c, Result &r, const QString &title, double W, double head)
{
    if (title.isEmpty())
        return 0;
    const Title made = titleOf(c, title, W - 80);
    r.items << made.item;
    r.corner = Corner{made.width + 16, 28, 0};
    return head;
}

Result resultOf(const char *kind)
{
    Result r;
    r.kind = QString::fromLatin1(kind);
    r.flush = true;
    return r;
}

// MOOD: good, bad or warn after a value, the colour of the mark beside it.
const Re &moodRe()
{
    static const Re re = rx("^(good|bad|warn|ok|хорошо|плохо|внимание)$", I);
    return re;
}

// MOODS
QString moodOf(const QString &cell)
{
    const QString word = cell.toLower();
    if (word == QLatin1String("good") || word == QLatin1String("ok") ||
        word == QString::fromUtf8("хорошо"))
        return QStringLiteral("good");
    if (word == QLatin1String("bad") || word == QString::fromUtf8("плохо"))
        return QStringLiteral("bad");
    if (word == QLatin1String("warn") || word == QString::fromUtf8("внимание"))
        return QStringLiteral("warn");
    return {};
}

// The first cell that is a mood, as its mood ('' for none).
QString findMood(const QStringList &cells)
{
    for (const QString &cell : cells) {
        if (test(moodRe(), cell))
            return moodOf(cell);
    }
    return {};
}

// The cells of a row of the later kinds: parted by bars, or written as
// name: value, or as name — value (pairCells).
QStringList pairCells(const QString &line)
{
    static const Re named = rx("^[^:]{1,60}:\\s+\\S"), dashed = rx("^(.+?)\\s+[—–-]\\s+(.+)$");
    if (hasBar(line))
        return cellsOf(line);
    if (test(named, line))
        return rowCells(line);
    const auto m = dashed.match(line);
    return m.hasMatch() ? QStringList{m.captured(1).trimmed(), m.captured(2).trimmed()}
                        : QStringList{line};
}

/* facts: what a thing is, at a glance */

struct FactsRow {
    QString label, value, note, mood;
};
struct FactsFile {
    QString name, meta;
};
struct Facts {
    QString title;
    std::optional<FactsFile> file;
    QVector<FactsRow> rows;
};

std::optional<Facts> parseFacts(const QStringList &lines)
{
    static const Re file = rx("^(?:file|файл|document|документ)(?:\\s*:\\s*|\\s+)(?!\\|)(.+)$", I);
    static const Re colonRe = rx("^([^:|]{1,60}):\\s+(.+)$");
    Facts data;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = bare(lines.at(i));
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (!hasBar(line) && (m = titleRe().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if ((m = file.match(line)).hasMatch() && !data.file) {
            const QStringList cells = cellsOf(m.captured(1));
            data.file = FactsFile{unquote(cells.at(0)),
                                  mapped(nonEmpty(cells.mid(1)), unquote).join(sep())};
            continue;
        }
        // Written with a colon, what stands after it is the value whole: its
        // brackets are part of what it says.
        const auto colon = colonRe.match(line);
        const QStringList cells = hasBar(line)       ? cellsOf(line)
                                  : colon.hasMatch() ? QStringList{colon.captured(1).trimmed(),
                                                                   colon.captured(2).trimmed()}
                                                     : QStringList{line};
        if (cells.value(0).isEmpty() || cells.value(1).isEmpty())
            continue;
        QStringList notes;
        for (const QString &cell : cells.mid(2)) {
            if (!cell.isEmpty() && !test(moodRe(), cell))
                notes << unquote(cell);
        }
        data.rows << FactsRow{unquote(cells.at(0)), unquote(cells.at(1)), notes.join(sep()),
                              findMood(cells.mid(2))};
    }
    if (data.rows.isEmpty() && !data.file)
        return std::nullopt;
    return data;
}

std::optional<Result> factsScene(Ctx &c, const Facts &data)
{
    const double W = clamp(c.width, 280, FACTS.max);
    Result r = resultOf("facts");
    auto &items = r.items;
    const double top = placeTitle(c, r, data.title, W, CHART.head + 2);
    double y = top;
    if (data.file) {
        // The file itself heads the list: a sheet with a turned corner, its
        // name, and what is known of it.
        const FactsFile &file = *data.file;
        const double metaW = !file.meta.isEmpty() ? c.widthOf(file.meta, FONT::tick) + 12 : 0;
        items << glyph(QStringLiteral("ff:icon"), 0.2, 9, y + 13, 26, QStringLiteral("sheet"),
                       QStringLiteral("is-sheet"), first(c.tones));
        items << labelSpec(QStringLiteral("ff:name"), 0.25, 28, y + 13,
                           {truncate(c, file.name, W - 28 - metaW, FONT::strong)},
                           QStringLiteral("dg-row-total"), QStringLiteral("start"));
        if (!file.meta.isEmpty())
            items << labelSpec(QStringLiteral("ff:meta"), 0.3, W, y + 13,
                               {truncate(c, file.meta, W * 0.5, FONT::tick)},
                               QStringLiteral("dg-row-note"), QStringLiteral("end"));
        y += 30;
        if (!data.rows.isEmpty()) {
            items << lineSpec(QStringLiteral("ff:rule"), 0.3, 0, y, W, y, QStringLiteral("dg-line"),
                              true, Back);
            y += 6;
        }
    }
    // Names stand in a column of small capitals; what each is fills the rest
    // of the line and wraps where it must.
    const double labelW =
        std::min(W * 0.36,
                 maxOf(60, data.rows,
                       [&](const FactsRow &row) { return capsWidth(c, row.label.toUpper()); })) +
        20;
    const double valueW = W - labelW;
    for (int i = 0; i < data.rows.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const FactsRow &row = data.rows.at(i);
        const double order = 0.4 + i * 0.18, dot = !row.mood.isEmpty() ? 14 : 0;
        const QString key = QStringLiteral("fr:%1").arg(i);
        const QStringList lines = wrap(c, row.value, valueW - dot, FONT::row).mid(0, 4);
        const QStringList notes =
            !row.note.isEmpty() ? wrap(c, row.note, valueW, FONT::tick).mid(0, 2) : QStringList();
        const double h = std::max(FACTS.row, 14.0 + lines.size() * 18 + notes.size() * 15);
        if (i)
            items << rule(key + QStringLiteral(":rule"), order, y, W);
        items << labelSpec(key + QStringLiteral(":label"), order, 0, y + 14.5,
                           {caps(c, row.label, labelW - 12)}, QStringLiteral("dg-eyebrow"),
                           QStringLiteral("start"), Back);
        if (!row.mood.isEmpty())
            items << dotSpec(key + QStringLiteral(":mood"), order + 0.03, labelW + 4, y + 14.5, 3.5,
                             QStringLiteral("dg-state is-") + row.mood);
        items << block(key + QStringLiteral(":value"), order + 0.05, labelW + dot, y + 7, lines, 18,
                       QStringLiteral("dg-row-name"));
        if (!notes.isEmpty())
            items << block(key + QStringLiteral(":note"), order + 0.08, labelW,
                           y + 7 + lines.size() * 18 + 1, notes, 15, QStringLiteral("dg-row-note"));
        y += h;
    }
    r.width = W;
    r.height = y;
    return r;
}

/* checklist: what holds and what does not */

// STATE: how a row stands, read off its start, and the icon of its mark.
struct CheckState {
    QString state;
    Re test;
    QString icon;
};

const QVector<CheckState> &states()
{
    static const QVector<CheckState> table{
        {QStringLiteral("good"), rx("^(?:\\[\\s*[xXхХvV✓✔+]\\s*\\]|[✓✔✅☑])\\s*"),
         QStringLiteral("check")},
        {QStringLiteral("warn"), rx("^(?:\\[\\s*[!~]\\s*\\]|[⚠❗])\\s*"), QStringLiteral("bang")},
        {QStringLiteral("bad"), rx("^(?:\\[\\s*[-–—✗✘×]\\s*\\]|[✗✘✕❌⛔])\\s*"),
         QStringLiteral("cross")},
        {QStringLiteral("ask"), rx("^(?:\\[\\s*\\?\\s*\\]|[❓?])\\s*"), QStringLiteral("ask")},
        {QStringLiteral("open"), rx("^(?:\\[\\s*\\]|[☐○])\\s*"), QString()},
    };
    return table;
}

const CheckState *stateOf(const QString &line)
{
    for (const auto &state : states()) {
        if (test(state.test, line))
            return &state;
    }
    return nullptr;
}

// STATE_WORD: a last cell that says how the row stands. Only these words
// (upstream's object lookup also answers `constructor` and the like).
QString stateWord(const QString &cell)
{
    const QString w = cell.toLower();
    if (w == QLatin1String("good") || w == QLatin1String("ok") || w == QLatin1String("yes") ||
        w == QString::fromUtf8("да"))
        return QStringLiteral("good");
    if (w == QLatin1String("bad") || w == QLatin1String("no") || w == QString::fromUtf8("нет"))
        return QStringLiteral("bad");
    if (w == QLatin1String("warn") || w == QLatin1String("warning"))
        return QStringLiteral("warn");
    if (w == QLatin1String("unknown"))
        return QStringLiteral("ask");
    if (w == QLatin1String("open"))
        return QStringLiteral("open");
    return {};
}

struct CheckRow {
    QString text, state, aside;
    QStringList notes;
};
struct Checklist {
    QString title;
    QVector<CheckRow> rows;
};

std::optional<Checklist> parseChecklist(const QStringList &lines)
{
    static const Re number = rx("^\\d{1,2}[.)]\\s+(?=\\[|[✓✔✅☑⚠❗✗✘✕❌⛔❓☐○])");
    Checklist data;
    std::optional<int> base;
    for (int i = 1; i < lines.size(); ++i) {
        const QString &raw = lines.at(i);
        const int indent = indentOf(raw);
        // A number before the mark of a row, as in a numbered list, says
        // nothing here and is dropped.
        QString line = bare(raw);
        line.remove(QChar(0xfe0f));
        line.remove(number);
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (data.rows.isEmpty() && !hasBar(line) && (m = titleRe().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if (!base)
            base = indent;
        if (indent > *base && !data.rows.isEmpty() && !stateOf(line)) {
            data.rows.last().notes << cleanLabel(line);
            continue;
        }
        // `[x]` lost its dash to the bullet reader when written as `- [x]`;
        // `[-]` keeps its own.
        const CheckState *state = stateOf(line);
        if (state)
            line.remove(state->test);
        QStringList cells = cellsOf(line);
        const QString word = cells.size() > 1 ? stateWord(cells.last()) : QString();
        if (!word.isEmpty())
            cells.removeLast();
        const QString text = cleanLabel(cells.value(0));
        if (!text.isEmpty())
            data.rows << CheckRow{text,
                                  !word.isEmpty() ? word
                                  : state         ? state->state
                                                  : QStringLiteral("open"),
                                  mapped(nonEmpty(cells.mid(1)), cleanLabel).join(sep()),
                                  {}};
    }
    if (data.rows.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> checklistScene(Ctx &c, const Checklist &data)
{
    const double W = clamp(c.width, 280, STEPS.max), R = CHECK.mark, textX = R * 2 + 12;
    Result r = resultOf("checklist");
    auto &items = r.items;
    double top = placeTitle(c, r, data.title, W, STEPS.head);
    // How many of each, told once over the list when it is long enough to
    // want a count.
    QVector<std::pair<QString, int>> kinds;
    for (const char *name : {"good", "warn", "bad", "ask", "open"}) {
        const QString state = QString::fromLatin1(name);
        const int n = int(std::count_if(data.rows.begin(), data.rows.end(),
                                        [&](const CheckRow &row) { return row.state == state; }));
        if (n)
            kinds.append({state, n});
    }
    if (data.rows.size() >= 4 && kinds.size() > 1) {
        double x = 0;
        for (int k = 0; k < kinds.size(); ++k) {
            const auto &[state, n] = kinds.at(k);
            items << dotSpec(QStringLiteral("ck:tally:") + state, 0.1 + k * 0.05, x + 4, top + 9,
                             3.5, QStringLiteral("dg-state is-") + state);
            items << labelSpec(QStringLiteral("ck:count:") + state, 0.12 + k * 0.05, x + 13,
                               top + 9, {QString::number(n)}, QStringLiteral("dg-value"),
                               QStringLiteral("start"));
            x += 13 + c.widthOf(QString::number(n), FONT::value) + 16;
        }
        top += 26;
    }
    double y = top + 2;
    for (int i = 0; i < data.rows.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const CheckRow &row = data.rows.at(i);
        const double order = 0.3 + i * 0.3, cy = y + R;
        const QString key = QStringLiteral("ck:%1").arg(i);
        QString icon;
        for (const auto &state : states()) {
            if (state.state == row.state) {
                icon = state.icon;
                break;
            }
        }
        const bool aside = !row.aside.isEmpty() && c.widthOf(row.aside, FONT::tick) <= W * 0.4;
        const QStringList lines =
            wrap(c, row.text, W - textX - (aside ? c.widthOf(row.aside, FONT::tick) + 18 : 0),
                 FONT::step);
        items << dotSpec(key + QStringLiteral(":mark"), order, R, cy, R,
                         QStringLiteral("dg-check-mark is-") + row.state, NoTone, Nodes);
        if (!icon.isEmpty())
            items << glyph(key + QStringLiteral(":icon"), order + 0.05, R, cy, 14, icon,
                           QStringLiteral("dg-check-icon is-") + row.state);
        items << block(key + QStringLiteral(":t"), order + 0.1, textX, cy - 7, lines, 18,
                       row.state == QLatin1String("open") ? QStringLiteral("dg-step-text is-done")
                                                          : QStringLiteral("dg-step-text"));
        if (aside)
            items << labelSpec(key + QStringLiteral(":aside"), order + 0.15, W, cy, {row.aside},
                               QStringLiteral("dg-step-meta"), QStringLiteral("end"));
        double ty = cy - 7 + lines.size() * 18;
        QStringList under;
        if (!row.aside.isEmpty() && !aside)
            under << row.aside;
        under += row.notes;
        for (int j = 0; j < under.size(); ++j) {
            const QStringList text = wrap(c, under.at(j), W - textX, FONT::note).mid(0, 3);
            items << block(QStringLiteral("%1:o%2").arg(key).arg(j), order + 0.2 + j * 0.05, textX,
                           ty + 3, text, 16, QStringLiteral("dg-step-note"));
            ty += 3 + text.size() * 16;
        }
        y = std::max(ty, cy + R) + CHECK.gap;
    }
    r.width = W;
    r.height = y - CHECK.gap;
    return r;
}

/* changes: what became different */

// ARROW_SPLIT
const Re &arrowSplit()
{
    static const Re re = rx("\\s*(?:-{1,2}>|=>|→|⟶|➜|➔)\\s*");
    return re;
}

struct ChangeRow {
    QString label, was, now, note, mood;
};
struct Changes {
    QString title;
    QVector<ChangeRow> rows;
};

std::optional<Changes> parseChanges(const QStringList &lines)
{
    static const Re wasWord = rx("^(?:было|was|from)\\s+", I),
                    nowWord = rx("^(?:стало|now|to)\\s+", I);
    Changes data;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = bare(lines.at(i));
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (!hasBar(line) && (m = titleRe().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        const QStringList cells = hasBar(line) ? cellsOf(line) : rowCells(line);
        if (cells.value(0).isEmpty() || cells.size() < 2)
            continue;
        QStringList rest = nonEmpty(cells.mid(1));
        QString was, now;
        int at = -1;
        for (int k = 0; k < rest.size() && at < 0; ++k) {
            if (test(arrowSplit(), rest.at(k)))
                at = k;
        }
        if (at >= 0) {
            const QStringList parts = rest.at(at).split(arrowSplit());
            was = parts.value(0).trimmed();
            now = parts.value(1).trimmed();
            rest.removeAt(at);
        } else if (rest.size() >= 2) {
            was = rest.at(0);
            now = rest.at(1);
            rest = rest.mid(2);
        } else {
            continue;
        }
        QStringList notes;
        for (const QString &cell : rest) {
            if (!test(moodRe(), cell))
                notes << unquote(cell);
        }
        was.remove(wasWord);
        now.remove(nowWord);
        data.rows << ChangeRow{unquote(cells.at(0)), minus(unquote(was)), minus(unquote(now)),
                               notes.join(sep()), findMood(rest)};
    }
    if (data.rows.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> changesScene(Ctx &c, const Changes &data)
{
    const double W = clamp(c.width, 300, LEDGER.max);
    Result r = resultOf("changes");
    auto &items = r.items;
    auto &tips = r.tips;
    const double top = placeTitle(c, r, data.title, W, LEDGER.head);
    // How far the value moved, where both are numbers of one kind: a share
    // of what it was, or the step itself from zero.
    const auto moved = [](const ChangeRow &row) -> QString {
        const auto a = amount(row.was), b = amount(row.now);
        if (!a || !b || a->before != b->before ||
            (a->after != b->after && !a->after.isEmpty() && !b->after.isEmpty()) ||
            a->value == b->value)
            return {};
        const double step = b->value - a->value;
        const QString sign = step > 0 ? QStringLiteral("+") : QString(QChar(0x2212));
        return a->value ? sign + format(jsRound(std::abs(step / a->value) * 1000) / 10) +
                              QLatin1Char('%')
                        : sign + format(std::abs(step));
    };
    // A value said in many words has a line of its own under the name, was
    // and now side by side there: the short values of the other rows keep
    // their narrow columns, and their names the room.
    const auto wordy = [&](const ChangeRow &row) {
        return c.widthOf(row.was, FONT::amount) > W * 0.2 ||
               c.widthOf(row.now, FONT::strong) > W * 0.26;
    };
    QVector<ChangeRow> brief;
    for (const auto &row : data.rows) {
        if (!wordy(row))
            brief << row;
    }
    const double wasW =
        maxOf(0, brief, [&](const ChangeRow &row) { return c.widthOf(row.was, FONT::amount); });
    const double nowW =
        maxOf(0, brief, [&](const ChangeRow &row) { return c.widthOf(row.now, FONT::strong); });
    const double moveW =
        maxOf(0, brief, [&](const ChangeRow &row) { return c.widthOf(moved(row), FONT::change); });
    const double noteW = std::min(W * 0.26, maxOf(0, data.rows, [&](const ChangeRow &row) {
                                      return c.widthOf(row.note, FONT::tick);
                                  }));
    const double fixedW = wasW + 30 + nowW + (moveW ? moveW + 16 : 0) + (noteW ? noteW + 16 : 0);
    const double labelW =
        std::min(maxOf(-INFINITY, data.rows,
                       [&](const ChangeRow &row) { return c.widthOf(row.label, FONT::row); }),
                 std::max(90.0, W - fixedW - 16));
    const double xWas = labelW + 16 + wasW, xNow = xWas + 30;
    const auto arrow = [&](const QString &key, double order, double from, double to, double cy) {
        Spec edge = item(Type::Edge, key + QStringLiteral(":to"), order + 0.06);
        edge.props.pts = polyline({QPointF(from, cy), QPointF(to - ARROW.length, cy)});
        edge.fixed.style = QStringLiteral("solid");
        edge.fixed.head = QStringLiteral("arrow");
        edge.fixed.cls = QStringLiteral("dg-turn");
        edge.fixed.grow = true;
        items << edge;
    };
    double y = top + 2;
    for (int i = 0; i < data.rows.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const ChangeRow &row = data.rows.at(i);
        const bool isLong = wordy(row);
        const double h = LEDGER.row + 4 + (isLong ? 20 : 0), cy = y + LEDGER.row / 2 + 2,
                     order = 0.3 + i * 0.14;
        const QString key = QStringLiteral("ch:%1").arg(i);
        const QString name =
            truncate(c, row.label, isLong ? W - (noteW ? noteW + 16 : 0) : labelW, FONT::row);
        const QString note =
            !row.note.isEmpty()
                ? truncate(c, row.note,
                           isLong ? noteW
                                  : std::max(40.0, W - (xNow + nowW + (moveW ? moveW + 28 : 12))),
                           FONT::tick)
                : QString();
        // The line of a wordy row: what it was, cut to half the width if it
        // must be, the arrow, what it is now.
        const QString was = isLong ? truncate(c, row.was, (W - 34) * 0.46, FONT::amount) : row.was;
        const double wasEnd = c.widthOf(was, FONT::amount);
        const QString now = isLong ? truncate(c, row.now, W - wasEnd - 34, FONT::strong) : row.now;
        Tip tip;
        if (name != row.label || was != row.was || now != row.now) {
            tip.title = row.label;
            tip.rows << TipRow{QString(), row.was + QStringLiteral(" → ") + row.now, QString(),
                               NoTone, QString()};
            tip.hot = QStringList{key};
        } else {
            tip = silent({key});
        }
        items << hitSpec(key, order, -8, y, W + 16, h);
        tips.insert(key, tip);
        if (i)
            items << rule(key + QStringLiteral(":rule"), order, y, W);
        items << labelSpec(key + QStringLiteral(":name"), order, 0, cy, {name},
                           QStringLiteral("dg-row-name"), QStringLiteral("start"));
        const double at = isLong ? cy + 20 : cy,
                     after = isLong ? wasEnd + 30 + c.widthOf(now, FONT::strong) : xNow + nowW;
        items << labelSpec(key + QStringLiteral(":was"), order + 0.04, isLong ? 0 : xWas, at, {was},
                           QStringLiteral("dg-was"),
                           isLong ? QStringLiteral("start") : QStringLiteral("end"));
        arrow(key, order, (isLong ? wasEnd : xWas) + 8, (isLong ? wasEnd + 30 : xNow) - 8, at);
        items << labelSpec(key + QStringLiteral(":now"), order + 0.08, isLong ? wasEnd + 30 : xNow,
                           at, {now}, QStringLiteral("dg-row-total"), QStringLiteral("start"));
        const QString move = moved(row);
        if (!isLong && !move.isEmpty())
            items << labelSpec(
                key + QStringLiteral(":move"), order + 0.1, after + 16, at, {move},
                QStringLiteral("dg-metric-change") +
                    (!row.mood.isEmpty() ? QStringLiteral(" is-") + row.mood : QString()),
                QStringLiteral("start"));
        else if (!row.mood.isEmpty() && after + 16 < W)
            items << dotSpec(key + QStringLiteral(":mood"), order + 0.1, after + 12, at, 3.5,
                             QStringLiteral("dg-state is-") + row.mood);
        if (!note.isEmpty())
            items << labelSpec(key + QStringLiteral(":note"), order + 0.12, W, cy, {note},
                               QStringLiteral("dg-row-note"), QStringLiteral("end"));
        for (const char *part : {":name", ":was", ":now", ":move", ":note"})
            tips.insert(key + QLatin1String(part), tip);
        y += h;
    }
    r.width = W;
    r.height = y;
    return r;
}

/* outline: how a paper is built */

struct OutlineRow {
    int depth = 0;
    QString mark, text, where, note;
};
struct Outline {
    QString title;
    QVector<OutlineRow> rows;
};

std::optional<Outline> parseOutline(const QStringList &lines)
{
    static const Re page =
        rx("^(?:стр\\.?|с\\.|p\\.?|pp\\.?|pages?|страниц[аы]?|§|разд\\.?|sec\\.?)\\s*[\\d–—-]+", I);
    static const Re numbered = rx("^((?:\\d+\\.)*\\d+[.)]?|[IVXLC]+[.)]|[A-ZА-Я][.)])\\s+");
    static const Re digits = rx("\\d+");
    Outline data;
    QVector<int> depths;
    for (int i = 1; i < lines.size(); ++i) {
        const QString &raw = lines.at(i);
        const int indent = indentOf(raw);
        const QString line = bare(raw);
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (data.rows.isEmpty() && !hasBar(line) && (m = titleRe().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        while (!depths.isEmpty() && depths.last() >= indent)
            depths.removeLast();
        const int depth = std::min(3, int(depths.size()));
        depths << indent;
        const QStringList cells = cellsOf(line);
        const auto mark = numbered.match(cells.at(0));
        const QString text =
            cleanLabel(mark.hasMatch() ? cells.at(0).mid(mark.capturedLength(0)) : cells.at(0));
        // A part numbered 2.1 lies one step in even when the model wrote it
        // flush with the rest.
        int dots = 0;
        if (mark.hasMatch()) {
            int runs = 0;
            for (auto it = digits.globalMatch(mark.captured(1)); it.hasNext(); it.next())
                ++runs;
            dots = runs - 1;
        }
        const QStringList rest = nonEmpty(cells.mid(1));
        QString where;
        for (const QString &cell : rest) {
            if (test(page, cell)) {
                where = cell;
                break;
            }
        }
        QStringList notes;
        for (const QString &cell : rest) {
            if (cell != where)
                notes << unquote(cell);
        }
        if (!text.isEmpty())
            data.rows << OutlineRow{std::max(depth, std::min(3, dots)),
                                    mark.hasMatch() ? mark.captured(1) : QString(), text,
                                    unquote(where), notes.join(sep())};
    }
    if (data.rows.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> outlineScene(Ctx &c, const Outline &data)
{
    const double W = clamp(c.width, 280, STEPS.max);
    Result r = resultOf("outline");
    auto &items = r.items;
    const double top = placeTitle(c, r, data.title, W, STEPS.head);
    const double markW = maxOf(
        0, data.rows, [&](const OutlineRow &row) { return c.widthOf(row.mark, FONT::amount); });
    const double whereW = maxOf(
        0, data.rows, [&](const OutlineRow &row) { return c.widthOf(row.where, FONT::tick); });
    double y = top + 2;
    // The part the rows under it hang from; the rail they hang on: whose it
    // is, and its item.
    int opened = -1, railOf = -1, rail = -1;
    for (int i = 0; i < data.rows.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const OutlineRow &row = data.rows.at(i);
        const double order = 0.3 + i * 0.16, x = row.depth * OUTLINE.indent,
                     tx = x + (markW ? markW + 10 : 0);
        const bool head = row.depth == 0;
        const QString key = QStringLiteral("ol:%1").arg(i);
        const double room = W - tx - (whereW ? whereW + 16 : 0);
        const QStringList lines =
            wrap(c, row.text, room, head ? FONT::strong : FONT::row).mid(0, 3);
        const QStringList notes =
            !row.note.isEmpty() ? wrap(c, row.note, room, FONT::note).mid(0, 3) : QStringList();
        // Each part at the left edge starts under a hairline; the parts under
        // it hang on a rail that runs down from it.
        if (head && i) {
            items << rule(key + QStringLiteral(":rule"), order, y + 2, W);
            y += 10;
        }
        if (!row.mark.isEmpty())
            items << labelSpec(key + QStringLiteral(":mark"), order, x, y + 8.5, {row.mark},
                               head ? QStringLiteral("dg-outline-mark is-head")
                                    : QStringLiteral("dg-outline-mark"),
                               QStringLiteral("start"));
        else if (!head)
            items << dotSpec(key + QStringLiteral(":dot"), order, x - 8, y + 8.5, 1.75,
                             QStringLiteral("dg-outline-dot"));
        items << block(key + QStringLiteral(":text"), order + 0.04, tx, y + 1, lines, 18,
                       head ? QStringLiteral("dg-row-total") : QStringLiteral("dg-row-name"));
        if (!row.where.isEmpty())
            items << labelSpec(key + QStringLiteral(":where"), order + 0.08, W, y + 8.5,
                               {row.where}, QStringLiteral("dg-row-note"), QStringLiteral("end"));
        double ty = y + 1 + lines.size() * 18;
        if (!notes.isEmpty()) {
            items << block(key + QStringLiteral(":note"), order + 0.1, tx, ty + 1, notes, 16,
                           QStringLiteral("dg-step-note"));
            ty += 1 + notes.size() * 16;
        }
        // The parts of a part hang on one rail, which runs down beside them
        // all.
        if (row.depth > 0 && opened >= 0) {
            if (rail >= 0 && railOf == opened) {
                items[rail].props.y2 = ty;
            } else {
                railOf = opened;
                rail = int(items.size());
                items << lineSpec(QStringLiteral("ol:%1:rail").arg(opened), order,
                                  OUTLINE.indent - 14, y, OUTLINE.indent - 14, ty,
                                  QStringLiteral("dg-rail"), false, Back);
            }
        }
        if (head)
            opened = i;
        y = ty + OUTLINE.gap;
    }
    r.width = W;
    r.height = y - OUTLINE.gap + 2;
    return r;
}

/* matches: the games of a day */

struct MatchRow {
    bool isHead = false;
    QString head, time, home, away, what, note;
    bool hasScore = false, star = false;
    double score[2] = {0, 0};
};
struct Matches {
    QString title;
    QVector<MatchRow> rows;
};

// SIDES
const Re &sidesRe()
{
    static const Re re =
        rx("^(.+?)\\s+(?:(\\d+)\\s*[:–—-]\\s*(\\d+)|—|–|-|vs\\.?|v|x|:)\\s+(.+)$", I);
    return re;
}

// +digits: a run too long for a double is Infinity, as in JavaScript.
double digitsValue(const QString &text)
{
    bool ok = false;
    const double v = text.toDouble(&ok);
    return ok && finite(v) ? v : INFINITY;
}

std::optional<Matches> parseMatches(const QStringList &lines)
{
    static const Re starRe = rx("^(?:[*★⭐!]+|top|main|главный)$", I),
                    starEnd = rx("\\s+[*★⭐]\\s*$"),
                    timeRe = rx("^(?:\\d{1,2}[:.]\\d{2}|\\d{1,2}\\s?[ap]m|tbd|—|-)$", I);
    Matches data;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = bare(lines.at(i));
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (data.rows.isEmpty() && !hasBar(line) && (m = titleRe().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        QString starred = line;
        if ((m = starEnd.match(line)).hasMatch())
            starred =
                line.left(m.capturedStart()) + QStringLiteral(" | *") + line.mid(m.capturedEnd());
        const QStringList all = cellsOf(starred);
        bool star = false;
        QStringList cells;
        for (int k = 0; k < all.size(); ++k) {
            const bool isStar = test(starRe, all.at(k));
            star = star || isStar;
            if (!isStar && (!all.at(k).isEmpty() || k == 0))
                cells << all.at(k);
        }
        QString time;
        if (!cells.isEmpty() && test(timeRe, cells.at(0)))
            time = cells.takeFirst();
        int at = -1;
        for (int k = 0; k < cells.size() && at < 0; ++k) {
            if (test(sidesRe(), cells.at(k)))
                at = k;
        }
        if (at < 0) {
            if (cells.size() == 1 && time.isEmpty()) {
                // A name that cleans to nothing names nothing (upstream takes
                // it for a match without sides).
                MatchRow head;
                head.isHead = true;
                head.head = cleanLabel(dropColon(cells.at(0)));
                if (!head.head.isEmpty())
                    data.rows << head;
            }
            continue;
        }
        const auto sides = sidesRe().match(cells.at(at));
        QStringList rest;
        for (int k = 0; k < cells.size(); ++k) {
            if (k == at)
                continue;
            const QString cell = cleanLabel(cells.at(k));
            if (!cell.isEmpty())
                rest << cell;
        }
        MatchRow row;
        const int dot = int(time.indexOf(QLatin1Char('.')));
        if (dot >= 0)
            time[dot] = QLatin1Char(':');
        row.time = time;
        row.home = cleanLabel(sides.captured(1));
        row.away = cleanLabel(sides.captured(4));
        if (sides.hasCaptured(2)) {
            row.hasScore = true;
            row.score[0] = digitsValue(sides.captured(2));
            row.score[1] = digitsValue(sides.captured(3));
        }
        row.what = rest.value(0);
        row.note = rest.mid(1).join(sep());
        row.star = star;
        data.rows << row;
    }
    if (std::none_of(data.rows.begin(), data.rows.end(),
                     [](const MatchRow &row) { return !row.isHead; }))
        return std::nullopt;
    return data;
}

QString scoreText(const MatchRow &row)
{
    return jsNumber(row.score[0]) + QStringLiteral(" : ") + jsNumber(row.score[1]);
}

std::optional<Result> matchesScene(Ctx &c, const Matches &data)
{
    const double W = clamp(c.width, 300, LEDGER.max);
    Result r = resultOf("matches");
    auto &items = r.items;
    auto &tips = r.tips;
    QVector<MatchRow> games;
    for (const auto &row : data.rows) {
        if (!row.isHead)
            games << row;
    }
    const double top = placeTitle(c, r, data.title, W, LEDGER.head);
    // The two sides face each other across a middle that holds the score, or
    // a dash while the match is not played.
    const double timeW =
        maxOf(0, games, [&](const MatchRow &row) { return c.widthOf(row.time, FONT::amount); });
    const bool starred =
        std::any_of(games.begin(), games.end(), [](const MatchRow &row) { return row.star; });
    const double x0 = timeW ? timeW + 18 : starred ? 14 : 0;
    // The middle is as wide as the widest score: a basketball score takes
    // more room than a football one.
    const double whatW = std::min(W * 0.3, maxOf(0, games, [&](const MatchRow &row) {
                                      return c.widthOf(row.what, FONT::tick);
                                  }));
    const double mid = maxOf(44, games, [&](const MatchRow &row) {
        return row.hasScore ? c.widthOf(scoreText(row), FONT::amount) + 20 : 0;
    });
    const double sideW =
        std::max(60.0, std::min((W - x0 - mid - (whatW ? whatW + 18 : 0)) / 2,
                                maxOf(-INFINITY, games, [&](const MatchRow &row) {
                                    return std::max(c.widthOf(row.home, FONT::strong),
                                                    c.widthOf(row.away, FONT::strong));
                                })));
    const double cx = x0 + sideW + mid / 2;
    double y = top + 2;
    QString seen;
    for (int i = 0; i < data.rows.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const MatchRow &row = data.rows.at(i);
        const double order = 0.3 + i * 0.14;
        const QString key = QStringLiteral("mt:%1").arg(i);
        if (row.isHead) {
            items << labelSpec(key, order, 0, y + 12, {caps(c, row.head, W)},
                               QStringLiteral("dg-eyebrow"), QStringLiteral("start"), Back);
            y += 26;
            seen.clear();
            continue;
        }
        const double h = !row.note.isEmpty() ? MATCH.noted : MATCH.row,
                     cy = y + (!row.note.isEmpty() ? 14 : h / 2);
        // Math.sign: NaN (Infinity against Infinity) is neither side.
        const double won = row.hasScore ? row.score[0] - row.score[1] : 0;
        items << hitSpec(key, order, -8, y, W + 16, h);
        tips.insert(key, silent());
        // The hour is written once for the matches that share it.
        if (!row.time.isEmpty() && row.time != seen)
            items << labelSpec(key + QStringLiteral(":time"), order, 0, cy, {row.time},
                               QStringLiteral("dg-match-time"), QStringLiteral("start"));
        seen = row.time;
        if (row.star)
            items << dotSpec(key + QStringLiteral(":star"), order + 0.02, x0 - 8, cy, 2.5,
                             QStringLiteral("dg-point"), first(c.tones));
        const auto name = [&](const QString &text, bool lead) {
            return truncate(c, text, sideW, lead ? FONT::strong : FONT::row);
        };
        const bool homeLead = row.star || won > 0, awayLead = row.star || won < 0;
        const QString total = QStringLiteral("dg-row-total"), plain = QStringLiteral("dg-row-name"),
                      quiet = QStringLiteral(" is-quiet");
        items << labelSpec(key + QStringLiteral(":home"), order + 0.04, cx - mid / 2, cy,
                           {name(row.home, homeLead)},
                           (homeLead ? total : plain) + (won < 0 ? quiet : QString()),
                           QStringLiteral("end"));
        items << labelSpec(key + QStringLiteral(":mid"), order + 0.06, cx, cy,
                           {row.hasScore ? scoreText(row) : QString(QChar(0x2013))},
                           row.hasScore ? QStringLiteral("dg-score")
                                        : QStringLiteral("dg-row-share"));
        items << labelSpec(key + QStringLiteral(":away"), order + 0.08, cx + mid / 2, cy,
                           {name(row.away, awayLead)},
                           (awayLead ? total : plain) + (won > 0 ? quiet : QString()),
                           QStringLiteral("start"));
        if (!row.what.isEmpty())
            items << labelSpec(key + QStringLiteral(":what"), order + 0.1, W, cy,
                               {truncate(c, row.what, whatW ? whatW : W * 0.3, FONT::tick)},
                               QStringLiteral("dg-row-note"), QStringLiteral("end"));
        if (!row.note.isEmpty())
            items << labelSpec(key + QStringLiteral(":note"), order + 0.12, cx, cy + 16,
                               {truncate(c, row.note, W - x0, FONT::tick)},
                               QStringLiteral("dg-row-note"));
        for (const char *part : {":time", ":home", ":mid", ":away", ":what", ":note"})
            tips.insert(key + QLatin1String(part), silent({key}));
        y += h;
    }
    r.width = W;
    r.height = y;
    return r;
}

/* words: words to learn */

struct WordRow {
    bool isHead = false;
    QString head, word, sound, speech, meaning, example;
};
struct Words {
    QString title;
    QVector<WordRow> rows;
};

std::optional<Words> parseWords(const QStringList &lines)
{
    // SOUND, SOUND_END (on a cleaned word, which may hold a line break: \z
    // is JavaScript's $) and SPEECH.
    static const Re sound = rx("^(?:\\[[^\\]]+\\]|/[^/]+/)$"),
                    soundEnd = rx("^(.+?)\\s*(\\[[^\\]]+\\]|/[^/\\s][^/]*/)\\z"),
                    speech = rx("^(?:n|v|adj|adv|prep|conj|pron|num|noun|verb|phr|idiom|сущ|гл|"
                                "прил|нареч|предл|союз|мест|числ|част|фраза|m|f|nt|pl|м|ж|ср|"
                                "мн)\\.?$",
                                I);
    Words data;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = bare(lines.at(i));
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (data.rows.isEmpty() && !hasBar(line) && (m = titleRe().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        QStringList cells = nonEmpty(pairCells(line));
        if (cells.size() < 2) {
            // A group's name that cleans to nothing names nothing.
            WordRow head;
            head.isHead = true;
            head.head = cleanLabel(dropColon(line));
            if (!head.head.isEmpty())
                data.rows << head;
            continue;
        }
        // How a word is said may stand in a cell of its own or right after
        // the word.
        WordRow row;
        row.word = cleanLabel(cells.takeFirst());
        for (const QString &cell : cells) {
            if (test(sound, cell)) {
                row.sound = cell;
                break;
            }
        }
        if (row.sound.isEmpty() && (m = soundEnd.match(row.word)).hasMatch()) {
            row.sound = m.captured(2);
            row.word = m.captured(1);
        }
        for (const QString &cell : cells) {
            if (test(speech, cell)) {
                row.speech = cell;
                break;
            }
        }
        QStringList rest;
        for (const QString &cell : cells) {
            if (cell != row.sound && cell != row.speech)
                rest << cleanLabel(cell);
        }
        row.meaning = rest.value(0);
        row.example = rest.mid(1).join(QStringLiteral(" — "));
        if (!row.word.isEmpty())
            data.rows << row;
    }
    if (std::none_of(data.rows.begin(), data.rows.end(),
                     [](const WordRow &row) { return !row.isHead; }))
        return std::nullopt;
    return data;
}

std::optional<Result> wordsScene(Ctx &c, const Words &data)
{
    const double W = clamp(c.width, 300, LEDGER.max);
    Result r = resultOf("words");
    auto &items = r.items;
    auto &tips = r.tips;
    QVector<WordRow> words;
    for (const auto &row : data.rows) {
        if (!row.isHead)
            words << row;
    }
    const double top = placeTitle(c, r, data.title, W, LEDGER.head);
    // The words stand in a column of their own, each with how it is said;
    // what they mean starts on one line for all.
    const auto soundW = [&](const WordRow &row) {
        return !row.sound.isEmpty() ? 8 + c.widthOf(row.sound, FONT::tick) : 0.0;
    };
    const double speechW =
        maxOf(0, words, [&](const WordRow &row) { return c.widthOf(row.speech, FONT::tick); });
    const double col =
        std::min(W * 0.46, maxOf(-INFINITY, words,
                                 [&](const WordRow &row) {
                                     return c.widthOf(row.word, FONT::strong) + soundW(row);
                                 })) +
        24;
    const double room = W - col - (speechW ? speechW + 14 : 0);
    double y = top + 2;
    bool firstRow = true;
    for (int i = 0; i < data.rows.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const WordRow &row = data.rows.at(i);
        const double order = 0.3 + i * 0.12;
        const QString key = QStringLiteral("wd:%1").arg(i);
        if (row.isHead) {
            items << labelSpec(key, order, 0, y + (firstRow ? 9 : 19), {caps(c, row.head, W)},
                               QStringLiteral("dg-eyebrow"), QStringLiteral("start"), Back);
            y += firstRow ? 24 : 34;
            firstRow = true;
            continue;
        }
        const QStringList meaning = wrap(c, row.meaning, room, FONT::row).mid(0, 3);
        const QStringList example = !row.example.isEmpty()
                                        ? wrap(c, row.example, room, FONT::note).mid(0, 3)
                                        : QStringList();
        const double h = std::max(32.0, 14.0 + meaning.size() * 17 +
                                            (!example.isEmpty() ? 2.0 + example.size() * 16 : 0.0));
        const QString word = truncate(c, row.word, col - 24 - soundW(row), FONT::strong);
        if (!firstRow)
            items << rule(key + QStringLiteral(":rule"), order, y, W);
        items << hitSpec(key, order, -8, y, W + 16, h);
        tips.insert(key, silent());
        items << labelSpec(key + QStringLiteral(":word"), order, 0, y + 15.5, {word},
                           QStringLiteral("dg-row-total"), QStringLiteral("start"));
        if (!row.sound.isEmpty())
            items << labelSpec(key + QStringLiteral(":sound"), order + 0.03,
                               c.widthOf(word, FONT::strong) + 8, y + 15.5, {row.sound},
                               QStringLiteral("dg-row-note"), QStringLiteral("start"));
        items << block(key + QStringLiteral(":means"), order + 0.05, col, y + 9.5, meaning, 17,
                       QStringLiteral("dg-row-name"));
        if (!example.isEmpty())
            items << block(key + QStringLiteral(":say"), order + 0.08, col,
                           y + 11 + meaning.size() * 17, example, 16,
                           QStringLiteral("dg-step-note"));
        if (!row.speech.isEmpty())
            items << labelSpec(key + QStringLiteral(":kind"), order + 0.1, W, y + 15.5,
                               {row.speech}, QStringLiteral("dg-row-note"), QStringLiteral("end"));
        for (const char *part : {":word", ":sound", ":means", ":say", ":kind"})
            tips.insert(key + QLatin1String(part), silent({key}));
        y += h;
        firstRow = false;
    }
    r.width = W;
    r.height = y;
    return r;
}

/* gloss: a sentence taken apart */

struct GlossCell {
    bool key = false;
    QStringList lines;
};
struct Gloss {
    QString title, whole;
    QVector<GlossCell> cells;
};

std::optional<Gloss> parseGloss(Ctx &c, const QStringList &lines)
{
    static const Re whole = rx("^(?:=+|→|translation\\s*:|перевод\\s*:)\\s*(.+)$", I),
                    space = rx("\\s+"), keyRe = rx("^\\*[^*]+\\*$");
    Gloss data;
    QVector<QStringList> tiers;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (tiers.isEmpty() && !hasBar(line) && (m = titleRe().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if ((m = whole.match(line)).hasMatch()) {
            data.whole = unquote(m.captured(1));
            continue;
        }
        // A line without bars is the sentence itself when it comes first, and
        // its translation when it comes after.
        if (!hasBar(line) && !tiers.isEmpty()) {
            if (data.whole.isEmpty())
                data.whole = unquote(line);
            continue;
        }
        tiers << (hasBar(line) ? cellsOf(line) : line.split(space));
    }
    if (tiers.isEmpty())
        return std::nullopt;
    int count = 0;
    for (const auto &tier : tiers)
        count = std::max(count, int(tier.size()));
    // Untrusted bound: every word draws two items at least; a sentence wider
    // than a drawing may hold, or too deep to take apart, is refused before
    // the work.
    if (count * 2.0 > MaxItems || double(count) * tiers.size() > MaxItems * 10.0) {
        c.fail(QStringLiteral("This diagram is too large to draw."));
        return std::nullopt;
    }
    for (int i = 0; i < count; ++i) {
        if (!c.budget(int(tiers.size())))
            return std::nullopt;
        const QString word = tiers.at(0).value(i);
        GlossCell cell;
        cell.key = test(keyRe, word);
        for (int t = 0; t < tiers.size(); ++t) {
            const QString text = t          ? tiers.at(t).value(i)
                                 : cell.key ? word.mid(1, word.size() - 2)
                                            : word;
            cell.lines << (text.isEmpty() ? QString() : cleanLabel(text));
        }
        data.cells << cell;
    }
    return data;
}

std::optional<Result> glossScene(Ctx &c, const Gloss &data)
{
    const double W = clamp(c.width, 300, LEDGER.max);
    Result r = resultOf("gloss");
    auto &items = r.items;
    const int depth = int(data.cells.at(0).lines.size());
    const double top = placeTitle(c, r, data.title, W, LEDGER.head);
    // Each word is a small column: the word, a rule under it, then what it
    // means and what it is. The columns run on like the words of a line and
    // turn to the next line where the room ends.
    const Font fonts[] = {FONT::period, FONT::row, FONT::tick};
    const double rowH = 26 + (depth > 1 ? 20 : 0) + std::max(0, depth - 2) * 16.0;
    double x = 0, y = top + 4;
    for (int i = 0; i < data.cells.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const GlossCell &cell = data.cells.at(i);
        double widest = 22;
        for (int t = 0; t < cell.lines.size(); ++t)
            widest = std::max(widest, c.widthOf(cell.lines.at(t), fonts[std::min(t, 2)]));
        const double w = std::min(W, widest), order = 0.3 + i * 0.16;
        const QString key = QStringLiteral("gl:%1").arg(i);
        if (x && x + w > W) {
            x = 0;
            y += rowH + 18;
        }
        items << labelSpec(key + QStringLiteral(":word"), order, x, y + 9,
                           {truncate(c, cell.lines.at(0), W, FONT::period)},
                           QStringLiteral("dg-gloss-word"), QStringLiteral("start"));
        Spec line = lineSpec(key + QStringLiteral(":rule"), order + 0.04, x, y + 22, x + w, y + 22,
                             cell.key ? QStringLiteral("dg-gloss-rule is-key")
                                      : QStringLiteral("dg-gloss-rule"),
                             true, Back);
        line.fixed.tone = first(c.tones);
        items << line;
        for (int t = 0; t + 1 < cell.lines.size(); ++t) {
            const QString &text = cell.lines.at(t + 1);
            if (!text.isEmpty())
                items << labelSpec(
                    QStringLiteral("%1:t%2").arg(key).arg(t), order + 0.08 + t * 0.04, x,
                    y + (t ? 36 + 18 + (t - 1) * 16 : 36),
                    {truncate(c, text, W, fonts[std::min(t + 1, 2)])},
                    t ? QStringLiteral("dg-row-note") : QStringLiteral("dg-gloss-means"),
                    QStringLiteral("start"));
        }
        x += w + 18;
        if (items.size() > MaxItems) {
            c.fail(QStringLiteral("This diagram is too large to draw."));
            return std::nullopt;
        }
    }
    y += rowH;
    if (!data.whole.isEmpty()) {
        const QStringList lines = wrap(c, data.whole, W, FONT::note).mid(0, 4);
        items << block(QStringLiteral("gl:whole"), 0.4 + data.cells.size() * 0.16, 0, y + 12, lines,
                       16, QStringLiteral("dg-step-note"));
        y += 12 + lines.size() * 16;
    }
    r.width = W;
    r.height = y;
    return r;
}

/* forms: the forms of a word */

struct FormRow {
    QString label;
    QStringList forms;
    QString note;
};
struct Forms {
    QString title, stem;
    QStringList cols;
    QVector<FormRow> rows;
};

std::optional<Forms> parseForms(const QStringList &lines)
{
    static const Re bullet = rx("^[-•]\\s+"),
                    colsRe = rx("^(?:cols|columns|series)(?:\\s*:\\s*|\\s+)(.+)$", I),
                    stemRe = rx("^stem(?:\\s*:\\s*|\\s+)(.+)$", I), stemEnd = rx("[-·]$");
    Forms data;
    for (int i = 1; i < lines.size(); ++i) {
        QString line = lines.at(i).trimmed();
        line.remove(bullet);
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (!hasBar(line)) {
            if ((m = titleRe().match(line)).hasMatch())
                data.title = unquote(m.captured(1));
            else if ((m = colsRe.match(line)).hasMatch())
                data.cols = mapped(splitList(m.captured(1)), unquote);
            else if ((m = stemRe.match(line)).hasMatch())
                data.stem = unquote(m.captured(1)).remove(stemEnd);
            continue;
        }
        const QStringList cells = cellsOf(line);
        // A first row that names no one is the row of headings.
        if (cells.at(0).isEmpty() && data.cols.isEmpty() && data.rows.isEmpty()) {
            data.cols = mapped(cells.mid(1), unquote);
            continue;
        }
        const int count = std::max(1, int(data.cols.size()));
        data.rows << FormRow{cleanLabel(cells.at(0)), cells.mid(1, count),
                             mapped(nonEmpty(cells.mid(1 + count)), cleanLabel).join(sep())};
    }
    if (data.rows.isEmpty())
        return std::nullopt;
    return data;
}

// Where a form parts into what stays and what changes.
struct FormPart {
    QString head, end, tail;
};

QVector<FormPart> endingsOf(const QStringList &forms, const QString &stem)
{
    static const Re markedRe = rx("^(.*?)\\*([^*]+)\\*(.*)$");
    QVector<QRegularExpressionMatch> marked;
    bool any = false;
    for (const QString &form : forms) {
        QString f = form;
        f.replace(QStringLiteral("**"), QStringLiteral("*"));
        marked << markedRe.match(f);
        any = any || marked.last().hasMatch();
    }
    // The spaces round a marked part are kept: they say where it stands among
    // the words of the form.
    const QStringList words = mapped(forms, cleanLabel), full = nonEmpty(words);
    const QString lower = stem.toLower();
    const auto split = [](const QString &word, int at) {
        return at ? FormPart{word.left(at), word.mid(at), QString()}
                  : FormPart{word, QString(), QString()};
    };
    QVector<FormPart> out;
    // A form left unmarked among marked ones still parts after the stem, when
    // one was named.
    if (any) {
        for (int i = 0; i < forms.size(); ++i) {
            const auto &m = marked.at(i);
            if (m.hasMatch()) {
                out << FormPart{m.captured(1).remove(QLatin1Char('*')), m.captured(2),
                                m.captured(3).remove(QLatin1Char('*'))};
                continue;
            }
            const int at =
                !stem.isEmpty() && words.at(i).toLower().startsWith(lower) ? int(stem.size()) : 0;
            out << split(words.at(i), at);
        }
        return out;
    }
    int shared = 0;
    // What all the forms open with is a stem when there are enough of them to
    // tell, and it is more than a letter.
    const auto spaced = [](const QString &word) {
        return std::any_of(word.begin(), word.end(), [](QChar ch) { return ch.isSpace(); });
    };
    if (stem.isEmpty() && full.size() >= 3 && std::none_of(full.begin(), full.end(), spaced)) {
        const QString &head = full.at(0);
        const auto same = [&](const QString &word) {
            return shared < word.size() &&
                   QString(word.at(shared)).toLower() == QString(head.at(shared)).toLower();
        };
        while (shared < head.size() && std::all_of(full.begin(), full.end(), same))
            ++shared;
        if (shared < 2 || std::all_of(
                              full.begin(), full.end(),
                              [&](const QString &word) { return word.size() == shared; }))
            shared = 0;
    }
    for (const QString &word : words) {
        const int at =
            !stem.isEmpty() ? (word.toLower().startsWith(lower) ? int(stem.size()) : 0) : shared;
        out << split(word, at);
    }
    return out;
}

std::optional<Result> formsScene(Ctx &c, const Forms &data)
{
    Result r = resultOf("forms");
    auto &items = r.items;
    auto &tips = r.tips;
    int count = 1;
    for (const auto &row : data.rows)
        count = std::max(count, int(row.forms.size()));
    // Untrusted bound: a row draws three items and its forms more; a table
    // larger than a drawing may hold is refused before the work.
    if (double(count) * data.rows.size() > MaxItems * 4.0) {
        c.fail(QStringLiteral("This diagram is too large to draw."));
        return std::nullopt;
    }
    QVector<QVector<FormPart>> columns;
    for (int k = 0; k < count; ++k) {
        if (!c.budget(int(data.rows.size())))
            return std::nullopt;
        QStringList forms;
        for (const auto &row : data.rows)
            forms << row.forms.value(k);
        columns << endingsOf(forms, data.stem);
    }
    const auto formW = [&](const FormPart &part) {
        return c.widthOf(part.head, FONT::step) + c.widthOf(part.end, FONT::period) +
               c.widthOf(part.tail, FONT::step);
    };
    const double labelW = maxOf(-INFINITY, data.rows, [&](const FormRow &row) {
        return c.widthOf(row.label, FONT::legend);
    });
    QVector<double> colW;
    for (int k = 0; k < count; ++k)
        colW << maxOf(!data.cols.value(k).isEmpty() ? capsWidth(c, data.cols.value(k).toUpper())
                                                    : 0,
                      columns.at(k), formW);
    const double noteW =
        maxOf(0, data.rows, [&](const FormRow &row) { return c.widthOf(row.note, FONT::tick); });
    double need = labelW + 22 + (noteW ? std::min(noteW, 200.0) : 0);
    for (const double w : colW)
        need += w + 28;
    // A table of many columns may ask for more than the column of text: it
    // takes what it needs, and is shown smaller where even the whole width is
    // not enough.
    const double W = need > c.width ? std::min(need, CHART.max) : clamp(c.width, 280, LEDGER.max);
    const double top = placeTitle(c, r, data.title, W, LEDGER.head);
    QVector<double> xs;
    double at = std::min(labelW, W * 0.3) + 22;
    for (const double w : colW) {
        xs << at;
        at += w + 28;
    }
    double y = top + 2;
    if (std::any_of(data.cols.begin(), data.cols.end(),
                    [](const QString &name) { return !name.isEmpty(); })) {
        for (int k = 0; k < std::min(count, int(data.cols.size())); ++k) {
            if (!data.cols.at(k).isEmpty())
                items << labelSpec(QStringLiteral("fm:col:%1").arg(k), 0.2, xs.at(k), y + 9,
                                   {caps(c, data.cols.at(k), colW.at(k) + 20)},
                                   QStringLiteral("dg-eyebrow"), QStringLiteral("start"), Back);
        }
        y += 24;
    }
    for (int i = 0; i < data.rows.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const FormRow &row = data.rows.at(i);
        const double cy = y + 15, order = 0.3 + i * 0.12;
        const QString key = QStringLiteral("fm:%1").arg(i);
        if (i)
            items << rule(key + QStringLiteral(":rule"), order, y, W);
        items << hitSpec(key, order, -8, y, W + 16, 30);
        tips.insert(key, silent());
        items << labelSpec(key + QStringLiteral(":who"), order, 0, cy,
                           {truncate(c, row.label, std::min(labelW, W * 0.3), FONT::legend)},
                           QStringLiteral("dg-metric-name"), QStringLiteral("start"));
        for (int k = 0; k < count; ++k) {
            // A space at the end of a part is measured but not drawn: the next
            // part starts past it.
            const FormPart &part = columns.at(k).at(i);
            const double headW = c.widthOf(part.head, FONT::step),
                         endW = c.widthOf(part.end, FONT::period),
                         lead = c.widthOf(part.tail, FONT::step) -
                                c.widthOf(trimStart(part.tail), FONT::step);
            const QString h = QStringLiteral("%1:h%2").arg(key).arg(k),
                          e = QStringLiteral("%1:e%2").arg(key).arg(k),
                          t = QStringLiteral("%1:t%2").arg(key).arg(k);
            if (!part.head.trimmed().isEmpty())
                items << labelSpec(h, order + 0.04, xs.at(k), cy, {trimEnd(part.head)},
                                   QStringLiteral("dg-form"), QStringLiteral("start"));
            if (!part.end.isEmpty()) {
                // What changes is set strong and underlined in the colour of
                // the drawing.
                items << labelSpec(e, order + 0.06, xs.at(k) + headW, cy, {part.end},
                                   QStringLiteral("dg-form-end"), QStringLiteral("start"));
                Spec under = lineSpec(QStringLiteral("%1:u%2").arg(key).arg(k), order + 0.08,
                                      xs.at(k) + headW, cy + 10, xs.at(k) + headW + endW, cy + 10,
                                      QStringLiteral("dg-form-rule"), true, Back);
                under.fixed.tone = first(c.tones);
                items << under;
            }
            if (!part.tail.trimmed().isEmpty())
                items << labelSpec(t, order + 0.07, xs.at(k) + headW + endW + lead, cy,
                                   {part.tail.trimmed()}, QStringLiteral("dg-form"),
                                   QStringLiteral("start"));
            for (const QString &piece : {h, e, t})
                tips.insert(piece, silent({key}));
        }
        const double free = W - (xs.at(count - 1) + colW.at(count - 1) + 20);
        if (!row.note.isEmpty() && free > 40)
            items << labelSpec(key + QStringLiteral(":note"), order + 0.1, W, cy,
                               {truncate(c, row.note, free, FONT::tick)},
                               QStringLiteral("dg-row-note"), QStringLiteral("end"));
        tips.insert(key + QStringLiteral(":who"), silent({key}));
        tips.insert(key + QStringLiteral(":note"), silent({key}));
        y += 30;
        if (items.size() > MaxItems) {
            c.fail(QStringLiteral("This diagram is too large to draw."));
            return std::nullopt;
        }
    }
    r.width = W;
    r.height = y;
    return r;
}

/* recipe: a dish to cook */

// TAKES: how long. 9 min, 1 ч 20 мин, 0:45; a teaspoon, ч. л., is not an
// hour.
const Re &takesRe()
{
    static const Re re = rx("\\d\\s*(?:мин(?:ут[аыу]?)?|час(?:а|ов)?|ч(?!\\.?\\s*л)|"
                            "сек(?:унд[аыу]?)?|min(?:ute)?s?|hours?|hrs?|h|sec(?:ond)?s?|s)"
                            "(?![\\p{L}])|\\d+:\\d{2}",
                            I);
    return re;
}

struct RecipeEntry {
    bool step = false;
    QString name, amount, text, time, note, warn;
};
struct Recipe {
    QString title, partsName, stepsName;
    QStringList about;
    QVector<RecipeEntry> parts, steps;
};

std::optional<Recipe> parseRecipe(const QStringList &lines)
{
    // GOES_IN and TO_DO: the words that name what goes in and what to do.
    static const Re goesIn = rx("^(?:ингредиент|продукт|состав|понадобит|что нужно|нужно|"
                                "ingredients?|you(?:'ll| will)? need|shopping|for the)",
                                I),
                    toDo = rx("^(?:шаги|шаг|приготовлен|как готовить|способ|порядок|готовим|"
                              "steps?|method|directions|instructions|preparation|how to)",
                              I),
                    aboutRe = rx("^(?:about|meta|info)(?:\\s*:\\s*|\\s+)(.+)$", I),
                    digit = rx("\\d"), warnRe = rx("^!+\\s*"), numberedRe = rx("^\\d{1,2}[.)]\\s+");
    Recipe data;
    const QStringList rows = lines.mid(1);
    QString into;
    std::optional<int> base;
    bool nested = false;
    int set = 0;
    // The row a remark set in under it goes to: its list and place.
    QVector<RecipeEntry> *lastList = nullptr;
    int lastAt = -1;
    for (int i = 0; i < rows.size(); ++i) {
        const QString &raw = rows.at(i);
        const int indent = indentOf(raw);
        const QString line = bare(raw);
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (!hasBar(line) && (m = titleRe().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if ((m = aboutRe.match(line)).hasMatch()) {
            data.about += mapped(nonEmpty(cellsOf(m.captured(1))), cleanLabel);
            continue;
        }
        if (!base)
            base = indent;
        // A line at the left edge that names what follows opens a part of the
        // recipe: what goes in, or what to do. It is known by its word, or,
        // in another language, by the rows set in under it.
        const bool plain = !hasBar(line) && indent <= *base && line.size() <= 40 &&
                           !test(digit, line) && !line.startsWith(QLatin1Char('!'));
        const bool named = plain && (test(goesIn, line) || test(toDo, line));
        const bool opens = plain && i + 1 < rows.size() && indentOf(rows.at(i + 1)) > indent &&
                           (nested || !lastList);
        if (named || opens) {
            into = test(goesIn, line) ? QStringLiteral("parts")
                   : test(toDo, line) ? QStringLiteral("steps")
                   : !data.parts.isEmpty() || into == QLatin1String("parts")
                       ? QStringLiteral("steps")
                       : QStringLiteral("parts");
            (into == QLatin1String("parts") ? data.partsName : data.stepsName) =
                cleanLabel(dropColon(line));
            lastList = nullptr;
            continue;
        }
        // A line set in under a row is a remark on it; one that opens with !
        // is what to mind at that step.
        const auto warn = warnRe.match(line);
        if (lastList && (warn.hasMatch() || indent > set)) {
            RecipeEntry &last = (*lastList)[lastAt];
            const QString text =
                cleanLabel(warn.hasMatch() ? line.mid(warn.capturedLength(0)) : line);
            if (warn.hasMatch() && last.step)
                last.warn = text;
            else
                last.note = !last.note.isEmpty() ? last.note + sep() + text : text;
            continue;
        }
        const bool numbered = test(numberedRe, line);
        const QStringList cells = nonEmpty(pairCells(QString(line).remove(numberedRe)));
        if (cells.isEmpty())
            continue;
        const QString kind =
            !into.isEmpty() ? into
            : numbered || cells.size() == 1 || test(takesRe(), cells.at(1)) || !amount(cells.at(1))
                ? QStringLiteral("steps")
                : QStringLiteral("parts");
        RecipeEntry entry;
        if (kind == QLatin1String("parts")) {
            entry.name = cleanLabel(cells.at(0));
            entry.amount = cleanLabel(cells.value(1));
            entry.note = mapped(cells.mid(2), cleanLabel).join(sep());
        } else {
            QString mind;
            for (const QString &cell : cells) {
                if (cell.startsWith(QLatin1Char('!'))) {
                    mind = cell;
                    break;
                }
            }
            QStringList rest;
            for (const QString &cell : cells.mid(1)) {
                if (cell != mind)
                    rest << cleanLabel(cell);
            }
            for (const QString &cell : rest) {
                if (test(takesRe(), cell)) {
                    entry.time = cell;
                    break;
                }
            }
            QStringList notes;
            for (const QString &cell : rest) {
                if (cell != entry.time)
                    notes << cell;
            }
            entry.step = true;
            entry.text = cleanLabel(cells.at(0));
            entry.note = notes.join(sep());
            entry.warn = !mind.isEmpty() ? cleanLabel(QString(mind).remove(warnRe)) : QString();
        }
        lastList = kind == QLatin1String("parts") ? &data.parts : &data.steps;
        *lastList << entry;
        lastAt = int(lastList->size()) - 1;
        set = indent;
        nested = nested || indent > *base;
    }
    if (data.parts.isEmpty() && data.steps.isEmpty())
        return std::nullopt;
    return data;
}

std::optional<Result> recipeScene(Ctx &c, const Recipe &data)
{
    const double W = clamp(c.width, 300, LEDGER.max);
    Result r = resultOf("recipe");
    auto &items = r.items;
    auto &tips = r.tips;
    double top = placeTitle(c, r, data.title, W, LEDGER.head);
    // What the dish asks for, at a glance: one quiet line, its parts set
    // apart by dots.
    if (!data.about.isEmpty()) {
        double x = 0;
        for (int k = 0; k < data.about.size(); ++k) {
            const QString &text = data.about.at(k);
            const double w = c.widthOf(text, FONT::amount);
            if (x + w > W)
                continue;
            if (k)
                items << dotSpec(QStringLiteral("rc:dot:%1").arg(k), 0.2, x - 9, top + 9, 1.5,
                                 QStringLiteral("dg-outline-dot"));
            items << labelSpec(QStringLiteral("rc:about:%1").arg(k), 0.2 + k * 0.05, x, top + 9,
                               {text}, QStringLiteral("dg-row-value"), QStringLiteral("start"));
            x += w + 18;
        }
        top += 28;
    }
    const double amountW = maxOf(0, data.parts, [&](const RecipeEntry &part) {
        return c.widthOf(part.amount, FONT::amount);
    });
    const double nameW = maxOf(
        0, data.parts, [&](const RecipeEntry &part) { return c.widthOf(part.name, FONT::row); });
    // What goes in stands in a column on the left and what to do beside it;
    // in a narrow place one goes under the other.
    const bool beside = !data.parts.isEmpty() && !data.steps.isEmpty() && W >= 520;
    const double leftW = beside ? clamp(nameW + amountW + 36, 180, W * 0.42) : W,
                 x1 = beside ? leftW + 36 : 0;
    const auto column = [&](const QString &name, double x, double y) {
        if (name.isEmpty())
            return y;
        items << labelSpec(QStringLiteral("rc:head:%1:%2").arg(jsNumber(x), jsNumber(jsRound(y))),
                           0.25, x, y + 9, {caps(c, name, W - x)}, QStringLiteral("dg-eyebrow"),
                           QStringLiteral("start"), Back);
        return y + 26;
    };
    double ya = column(data.partsName, 0, top + 2);
    for (int i = 0; i < data.parts.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const RecipeEntry &part = data.parts.at(i);
        const QString key = QStringLiteral("rc:p%1").arg(i);
        const double order = 0.3 + i * 0.1, h = !part.note.isEmpty() ? 40 : 26, cy = ya + 13;
        const QString name = truncate(c, part.name, leftW - amountW - 20, FONT::row);
        const double from = c.widthOf(name, FONT::row) + 8,
                     to = leftW - c.widthOf(part.amount, FONT::amount) - 8;
        items << hitSpec(key, order, -8, ya, leftW + 16, h);
        tips.insert(key, silent());
        items << labelSpec(key + QStringLiteral(":name"), order, 0, cy, {name},
                           QStringLiteral("dg-row-name"), QStringLiteral("start"));
        // A row of dots leads the eye from the name to its amount, as on a
        // menu.
        if (!part.amount.isEmpty() && to - from > 12)
            items << lineSpec(key + QStringLiteral(":lead"), order + 0.03, from, cy + 4, to, cy + 4,
                              QStringLiteral("dg-leader"), false, Back);
        if (!part.amount.isEmpty())
            items << labelSpec(key + QStringLiteral(":amount"), order + 0.05, leftW, cy,
                               {part.amount}, QStringLiteral("dg-row-value"),
                               QStringLiteral("end"));
        if (!part.note.isEmpty())
            items << labelSpec(key + QStringLiteral(":note"), order + 0.07, 0, cy + 16,
                               {truncate(c, part.note, leftW, FONT::tick)},
                               QStringLiteral("dg-row-note"), QStringLiteral("start"));
        for (const char *piece : {":name", ":amount", ":note"})
            tips.insert(key + QLatin1String(piece), silent({key}));
        ya += h;
    }
    double yb =
        column(data.stepsName, x1,
               beside ? top + 2 : ya + (!data.parts.isEmpty() && !data.steps.isEmpty() ? 18 : 0));
    const double R = 9, textX = x1 + R * 2 + 12;
    for (int i = 0; i < data.steps.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const RecipeEntry &step = data.steps.at(i);
        const QString key = QStringLiteral("rc:s%1").arg(i);
        const double order = 0.4 + i * 0.3, cy = yb + R,
                     timeW = !step.time.isEmpty() ? c.widthOf(step.time, FONT::tick) + 16 : 0;
        const QStringList lines = wrap(c, step.text, W - textX - timeW, FONT::step);
        items << dotSpec(key + QStringLiteral(":mark"), order, x1 + R, cy, R,
                         QStringLiteral("dg-step-mark"), first(c.tones), Nodes);
        items << labelSpec(key + QStringLiteral(":n"), order + 0.05, x1 + R, cy,
                           {QString::number(i + 1)}, QStringLiteral("dg-step-n"));
        items << block(key + QStringLiteral(":t"), order + 0.1, textX, cy - 7, lines, 18,
                       QStringLiteral("dg-step-text"));
        if (!step.time.isEmpty())
            items << labelSpec(key + QStringLiteral(":time"), order + 0.15, W, cy, {step.time},
                               QStringLiteral("dg-step-meta"), QStringLiteral("end"));
        double ty = cy - 7 + lines.size() * 18;
        if (!step.note.isEmpty()) {
            const QStringList text = wrap(c, step.note, W - textX, FONT::note).mid(0, 3);
            items << block(key + QStringLiteral(":note"), order + 0.2, textX, ty + 3, text, 16,
                           QStringLiteral("dg-step-note"));
            ty += 3 + text.size() * 16;
        }
        if (!step.warn.isEmpty()) {
            const QStringList text = wrap(c, step.warn, W - textX - 20, FONT::note).mid(0, 3);
            items << glyph(key + QStringLiteral(":wi"), order + 0.22, textX + 7, ty + 12, 15,
                           QStringLiteral("warn"), QStringLiteral("is-warn"));
            items << block(key + QStringLiteral(":warn"), order + 0.24, textX + 20, ty + 4, text,
                           16, QStringLiteral("dg-step-warn"));
            ty += 4 + text.size() * 16;
        }
        const double bottom = std::max(ty, cy + R);
        if (i < data.steps.size() - 1)
            items << lineSpec(key + QStringLiteral(":rail"), order + 0.2, x1 + R, cy + R + 4,
                              x1 + R, bottom + STEPS.gap - 4, QStringLiteral("dg-rail"), true,
                              Back);
        yb = bottom + STEPS.gap;
    }
    if (!data.steps.isEmpty())
        yb -= STEPS.gap;
    const double bottom = beside ? std::max(ya, yb) : !data.steps.isEmpty() ? yb : ya;
    if (beside)
        items << lineSpec(QStringLiteral("rc:part"), 0.25, leftW + 18, top + 4, leftW + 18, bottom,
                          QStringLiteral("dg-grid"), true, Back);
    r.width = W;
    r.height = bottom + 2;
    return r;
}

/* parts: what a thing is put together from */

struct Cost {
    double value = 0;
    QString before, after, text;
};
struct PartRow {
    bool isHead = false;
    QString head, slot, name, mood, note;
    QStringList rest;
    std::optional<Cost> cost;
};
struct Parts {
    QString title;
    std::optional<QString> total;
    QVector<PartRow> rows;
};

std::optional<Parts> parseParts(const QStringList &lines)
{
    // TOTAL_WORD (the word ends where its letters do: no \b for Cyrillic) and
    // MONEY.
    static const Re totalWord =
                        rx("^(total|sum|subtotal|result|итого|итог|всего)(?![\\p{L}\\d])", I),
                    lead = rx("^[:\\s]+"), slotted = rx("^[^:]{1,40}:\\s+\\S"),
                    tail = rx("[:.\\s]+$"),
                    money =
                        rx("^(?:[₽$€£¥₴₸]|руб|р\\.|usd|eur|rub|uah|kzt|pln|тг|грн|zł|сом|сум)", I),
                    digit = rx("\\d");
    Parts data;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = bare(lines.at(i));
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (!hasBar(line)) {
            if ((m = titleRe().match(line)).hasMatch()) {
                data.title = unquote(m.captured(1));
                continue;
            }
            if ((m = totalWord.match(line)).hasMatch()) {
                const QString rest = line.mid(m.capturedLength(0)).remove(lead);
                data.total =
                    rest.isEmpty() || amount(rest) ? sumName(m.captured(1), lines) : unquote(rest);
                continue;
            }
            // A line with nothing after its name names a group; one written
            // as slot: part is a row like the rest.
            if (!test(slotted, line)) {
                PartRow head;
                head.isHead = true;
                head.head = cleanLabel(dropColon(line));
                if (!head.head.isEmpty())
                    data.rows << head;
                continue;
            }
        }
        const QStringList cells = pairCells(line);
        if (sumRow(unquote(cells.at(0)), NaN, 0)) {
            data.total = unquote(cells.at(0)).remove(tail);
            continue;
        }
        PartRow row;
        const QStringList after = cells.mid(2);
        QString moodCell;
        bool hasMood = false;
        for (const QString &cell : after) {
            if (test(moodRe(), cell)) {
                moodCell = cell;
                hasMood = true;
                break;
            }
        }
        for (const QString &cell : after) {
            if (!cell.isEmpty() && (!hasMood || cell != moodCell))
                row.rest << cell;
        }
        row.slot = cleanLabel(cells.at(0));
        row.name = cleanLabel(cells.value(1));
        row.mood = hasMood ? moodOf(moodCell) : QString();
        data.rows << row;
    }
    if (std::none_of(data.rows.begin(), data.rows.end(),
                     [](const PartRow &row) { return !row.isHead; }))
        return std::nullopt;
    // The cost of a part is its last number counted in money. Where a sum is
    // asked for and nothing is counted in money, it is the last number with a
    // unit: the weights of a kit, the watts of a build.
    const auto pick = [&](bool inMoney) {
        for (auto &row : data.rows) {
            if (row.isHead)
                continue;
            for (int k = int(row.rest.size()) - 1; k >= 0 && !row.cost; --k) {
                const auto a = amount(row.rest.at(k));
                if (a && !test(digit, a->after) &&
                    (inMoney ? test(money, a->before) || test(money, a->after)
                             : !a->before.isEmpty() || !a->after.isEmpty())) {
                    row.cost = Cost{a->value, a->before, a->after, cleanLabel(row.rest.at(k))};
                    row.rest.removeAt(k);
                }
            }
        }
    };
    pick(true);
    if (data.total && std::none_of(data.rows.begin(), data.rows.end(),
                                   [](const PartRow &row) { return !row.isHead && row.cost; }))
        pick(false);
    for (auto &row : data.rows) {
        if (!row.isHead)
            row.note = mapped(row.rest, cleanLabel).join(sep());
    }
    return data;
}

std::optional<Result> partsScene(Ctx &c, const Parts &data)
{
    const double W = clamp(c.width, 300, LEDGER.max);
    Result r = resultOf("parts");
    auto &items = r.items;
    auto &tips = r.tips;
    QVector<PartRow> parts, priced;
    for (const auto &row : data.rows) {
        if (row.isHead)
            continue;
        parts << row;
        if (row.cost)
            priced << row;
    }
    const double top = placeTitle(c, r, data.title, W, LEDGER.head);
    // What every cost has before or after its number, when it is the same for
    // all, or nothing.
    const auto same = [&](QString Cost::*field) -> QString {
        if (priced.isEmpty())
            return {};
        const QString &head = (*priced.first().cost).*field;
        return std::all_of(priced.begin(), priced.end(),
                           [&](const PartRow &row) { return (*row.cost).*field == head; })
                   ? head
                   : QString();
    };
    const QString sameBefore = same(&Cost::before), sameAfter = same(&Cost::after);
    double sum = 0;
    for (const auto &row : priced)
        sum += row.cost->value;
    const double most = maxOf(1e-9, priced, [](const PartRow &row) { return row.cost->value; });
    const auto said = [&](double value) { return withUnit(value, sameBefore, sameAfter); };
    const auto costOf = [&](const PartRow &row) -> QString {
        if (!row.cost)
            return {};
        return !sameBefore.isEmpty() || !sameAfter.isEmpty() ? said(row.cost->value)
                                                             : row.cost->text;
    };
    const bool totalled = data.total && !priced.isEmpty();
    const double slotW =
        std::min(W * 0.24,
                 maxOf(-INFINITY, parts,
                       [&](const PartRow &row) { return capsWidth(c, row.slot.toUpper()); })) +
        18;
    const double costW = std::max(
        maxOf(0, parts, [&](const PartRow &row) { return c.widthOf(costOf(row), FONT::amount); }),
        totalled ? c.widthOf(said(sum), FONT::strong) : 0.0);
    // A thin bar beside each cost shows how much of the largest it is, where
    // there is room for one.
    const double meter = priced.size() > 1 && W >= 460 ? 56 : 0;
    const double room = W - slotW - (costW ? costW + 16 : 0) - (meter ? meter + 14 : 0);
    double y = top + 2;
    bool opened = true;
    for (int i = 0; i < data.rows.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const PartRow &row = data.rows.at(i);
        const double order = 0.3 + i * 0.12;
        const QString key = QStringLiteral("pt:%1").arg(i);
        if (row.isHead) {
            // The parts are named in small capitals already, so a group of
            // them is named in plain strong letters.
            items << labelSpec(key, order, 0, y + (opened ? 10 : 22),
                               {truncate(c, row.head, W, FONT::strong)},
                               QStringLiteral("dg-row-total"), QStringLiteral("start"), Back);
            y += opened ? 26 : 38;
            opened = true;
            continue;
        }
        const double dot = !row.mood.isEmpty() ? 13 : 0;
        const QStringList notes =
            !row.note.isEmpty() ? wrap(c, row.note, room, FONT::tick).mid(0, 2) : QStringList();
        const double h = 32 + notes.size() * 14, cy = y + 16;
        if (!opened)
            items << rule(key + QStringLiteral(":rule"), order, y, W);
        items << hitSpec(key, order, -8, y, W + 16, h);
        tips.insert(key, silent());
        items << labelSpec(key + QStringLiteral(":slot"), order, 0, cy,
                           {caps(c, row.slot, slotW - 12)}, QStringLiteral("dg-eyebrow"),
                           QStringLiteral("start"), Back);
        if (!row.mood.isEmpty())
            items << dotSpec(key + QStringLiteral(":mood"), order + 0.03, slotW + 4, cy, 3.5,
                             QStringLiteral("dg-state is-") + row.mood);
        items << labelSpec(key + QStringLiteral(":name"), order + 0.04, slotW + dot, cy,
                           {truncate(c, row.name, room - dot, FONT::strong)},
                           QStringLiteral("dg-row-total"), QStringLiteral("start"));
        if (!notes.isEmpty())
            items << block(key + QStringLiteral(":note"), order + 0.06, slotW, cy + 10, notes, 14,
                           QStringLiteral("dg-row-note"));
        if (row.cost) {
            if (meter) {
                const double mx = W - costW - 14 - meter;
                items << boxSpec(Type::Rect, key + QStringLiteral(":track"), order + 0.08, mx,
                                 cy - 2, meter, 4, QStringLiteral("dg-meter-track"), first(c.tones),
                                 2, Nodes);
                items << boxSpec(Type::Span, key + QStringLiteral(":fill"), order + 0.1, mx, cy,
                                 std::max(3.0, meter * clamp01(row.cost->value / most)), 4,
                                 QStringLiteral("dg-meter-fill"), first(c.tones), 2);
            }
            items << labelSpec(key + QStringLiteral(":cost"), order + 0.12, W, cy, {costOf(row)},
                               QStringLiteral("dg-row-value"), QStringLiteral("end"));
        }
        for (const char *part : {":slot", ":name", ":note", ":cost"})
            tips.insert(key + QLatin1String(part), silent({key}));
        y += h;
        opened = false;
    }
    if (totalled) {
        const double order = 0.4 + data.rows.size() * 0.12;
        items << lineSpec(QStringLiteral("pt:sum:rule"), order, 0, y + 6, W, y + 6,
                          QStringLiteral("dg-line"), true, Back);
        items << labelSpec(QStringLiteral("pt:sum:name"), order + 0.1, 0, y + 23,
                           {truncate(c, *data.total, W * 0.6, FONT::strong)},
                           QStringLiteral("dg-row-total"), QStringLiteral("start"));
        items << labelSpec(QStringLiteral("pt:sum:value"), order + 0.2, W, y + 23, {said(sum)},
                           QStringLiteral("dg-row-total"), QStringLiteral("end"));
        y += 36;
    }
    r.width = W;
    r.height = y;
    return r;
}

/* settings: where a setting lives and how to set it */

struct SettingRow {
    bool isHead = false;
    QString head, name, state, value, note;
    QStringList path;
};
struct Settings {
    QString title;
    QVector<SettingRow> rows;
};

std::optional<Settings> parseSettings(const QStringList &lines)
{
    // CRUMB, SET_ON and SET_OFF (on a cleaned value, which may hold a line
    // break: \z is JavaScript's $).
    static const Re crumb = rx("\\s*(?:>|›|→|➜)\\s*|\\s+»\\s+"),
                    on = rx("^(?:on|вкл\\.?|включ[а-яё]*|enabled?|yes|да|true|✓|✔|✅)\\z", I),
                    off = rx("^(?:off|выкл\\.?|выключ[а-яё]*|отключ[а-яё]*|disabled?|no|нет|"
                             "false|✗|✘|❌)\\z",
                             I);
    Settings data;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = bare(lines.at(i));
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (!hasBar(line)) {
            if (data.rows.isEmpty() && (m = titleRe().match(line)).hasMatch()) {
                data.title = unquote(m.captured(1));
                continue;
            }
            if (!test(crumb, line)) {
                // A group's name that cleans to nothing names nothing.
                SettingRow head;
                head.isHead = true;
                head.head = cleanLabel(dropColon(line));
                if (!head.head.isEmpty())
                    data.rows << head;
                continue;
            }
        }
        const QStringList cells = cellsOf(line);
        const QStringList path = nonEmpty(mapped(cells.at(0).split(crumb), cleanLabel));
        if (path.isEmpty())
            continue;
        SettingRow row;
        row.value = cleanLabel(cells.value(1));
        row.name = path.last();
        row.path = path.mid(0, path.size() - 1);
        row.state = test(on, row.value)    ? QStringLiteral("on")
                    : test(off, row.value) ? QStringLiteral("off")
                                           : QString();
        row.note = mapped(nonEmpty(cells.mid(2)), cleanLabel).join(sep());
        data.rows << row;
    }
    if (std::none_of(data.rows.begin(), data.rows.end(),
                     [](const SettingRow &row) { return !row.isHead; }))
        return std::nullopt;
    return data;
}

std::optional<Result> settingsScene(Ctx &c, const Settings &data)
{
    const double W = clamp(c.width, 300, LEDGER.max);
    Result r = resultOf("settings");
    auto &items = r.items;
    auto &tips = r.tips;
    const double top = placeTitle(c, r, data.title, W, LEDGER.head);
    double y = top + 2;
    bool opened = true;
    for (int i = 0; i < data.rows.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const SettingRow &row = data.rows.at(i);
        const double order = 0.3 + i * 0.14;
        const QString key = QStringLiteral("sg:%1").arg(i);
        if (row.isHead) {
            items << labelSpec(key, order, 0, y + (opened ? 9 : 19), {caps(c, row.head, W)},
                               QStringLiteral("dg-eyebrow"), QStringLiteral("start"), Back);
            y += opened ? 24 : 34;
            opened = true;
            continue;
        }
        // The name of the setting, and under it the way to it; at the right,
        // the switch as it should stand or the value.
        const bool hasState = !row.state.isEmpty();
        const QString value = hasState ? QString() : truncate(c, row.value, W * 0.4, FONT::amount);
        const double ctrlW = hasState           ? 30
                             : !value.isEmpty() ? c.widthOf(value, FONT::amount) + 18
                                                : 0;
        const double room = W - ctrlW - (ctrlW ? 16 : 0);
        const QString trail = row.path.join(QStringLiteral(" › "));
        const QStringList notes =
            !row.note.isEmpty() ? wrap(c, row.note, room, FONT::note).mid(0, 2) : QStringList();
        const double h = 30 + (!trail.isEmpty() ? 15 : 0) +
                         (!notes.isEmpty() ? 2.0 + notes.size() * 16 : 0.0) +
                         (!trail.isEmpty() || !notes.isEmpty() ? 5 : 0),
                     cy = y + 15;
        if (!opened)
            items << rule(key + QStringLiteral(":rule"), order, y, W);
        items << hitSpec(key, order, -8, y, W + 16, h);
        tips.insert(key, silent());
        items << labelSpec(key + QStringLiteral(":name"), order, 0, cy,
                           {truncate(c, row.name, room, FONT::strong)},
                           QStringLiteral("dg-row-total"), QStringLiteral("start"));
        if (!trail.isEmpty())
            items << labelSpec(key + QStringLiteral(":way"), order + 0.04, 0, cy + 16,
                               {truncate(c, trail, room, FONT::tick)},
                               QStringLiteral("dg-row-note"), QStringLiteral("start"));
        if (!notes.isEmpty())
            items << block(key + QStringLiteral(":note"), order + 0.06, 0,
                           cy + (!trail.isEmpty() ? 26 : 11), notes, 16,
                           QStringLiteral("dg-step-note"));
        if (hasState) {
            const bool isOn = row.state == QLatin1String("on");
            items << boxSpec(Type::Rect, key + QStringLiteral(":track"), order + 0.08, W - 30,
                             cy - 9, 30, 18,
                             isOn ? QStringLiteral("dg-toggle is-on") : QStringLiteral("dg-toggle"),
                             first(c.tones), 9, Nodes);
            items << dotSpec(
                key + QStringLiteral(":knob"), order + 0.1, isOn ? W - 9 : W - 21, cy, 6,
                isOn ? QStringLiteral("dg-toggle-knob is-on") : QStringLiteral("dg-toggle-knob"));
        } else if (!value.isEmpty()) {
            Spec pill = labelSpec(key + QStringLiteral(":value"), order + 0.08, W - ctrlW / 2, cy,
                                  {value}, QStringLiteral("dg-set-value"));
            pill.fixed.pill = true;
            pill.fixed.w = ctrlW;
            pill.fixed.h = 22;
            pill.fixed.rx = 7;
            pill.fixed.pop = true;
            items << pill;
        }
        for (const char *part : {":name", ":way", ":note", ":value"})
            tips.insert(key + QLatin1String(part), silent({key}));
        y += h;
        opened = false;
    }
    r.width = W;
    r.height = y;
    return r;
}

/* route: a journey */

struct RouteRow {
    bool isHead = false;
    QString head, place, when, note, leg;
};
struct Route {
    QString title;
    QVector<RouteRow> rows;
};

std::optional<Route> parseRoute(const QStringList &lines)
{
    // WAY_ON and DAY_NAME.
    static const Re wayOn = rx("^(?:↓|->|→|=>|~>|\\.\\.\\.|…)\\s*"),
                    dayName = rx("^(?:день|day|jour|tag|d[ií]a|giorno)\\s*\\d+", I),
                    hourFirst = rx("^(?:\\d{1,2}[:.]\\d{2}(?:\\s*[–—-]\\s*\\d{1,2}[:.]\\d{2})?|"
                                   "\\d{1,2}\\s?[ap]m)$",
                                   I);
    Route data;
    int set = 0;
    for (int i = 1; i < lines.size(); ++i) {
        const QString &raw = lines.at(i);
        const int indent = indentOf(raw);
        const QString line = bare(raw);
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (data.rows.isEmpty() && !hasBar(line) && (m = titleRe().match(line)).hasMatch()) {
            data.title = unquote(m.captured(1));
            continue;
        }
        if (!hasBar(line) && test(dayName, line)) {
            RouteRow head;
            head.isHead = true;
            head.head = cleanLabel(dropColon(line));
            data.rows << head;
            continue;
        }
        RouteRow *last = data.rows.isEmpty() ? nullptr : &data.rows.last();
        if (last && !last->isHead && (indent > set || test(wayOn, line))) {
            const QString text =
                mapped(nonEmpty(cellsOf(QString(line).remove(wayOn))), cleanLabel).join(sep());
            last->leg = !last->leg.isEmpty() ? last->leg + sep() + text : text;
            continue;
        }
        QStringList cells = pairCells(line);
        // Written with the hour first, the way a timetable is, a row still
        // gives its place first.
        if (cells.size() > 1 && test(hourFirst, cells.at(0)))
            cells.swapItemsAt(0, 1);
        set = indent;
        RouteRow row;
        row.place = cleanLabel(cells.at(0));
        row.when = cleanLabel(cells.value(1));
        row.note = mapped(nonEmpty(cells.mid(2)), cleanLabel).join(sep());
        data.rows << row;
    }
    if (std::none_of(data.rows.begin(), data.rows.end(),
                     [](const RouteRow &row) { return !row.isHead; }))
        return std::nullopt;
    return data;
}

std::optional<Result> routeScene(Ctx &c, const Route &data)
{
    const double W = clamp(c.width, 300, STEPS.max), textX = 26;
    Result r = resultOf("route");
    auto &items = r.items;
    int stops = 0;
    for (const auto &row : data.rows)
        stops += row.isHead ? 0 : 1;
    const double top = placeTitle(c, r, data.title, W, STEPS.head);
    const double whenW = maxOf(0, data.rows, [&](const RouteRow &row) {
        return row.isHead ? 0 : c.widthOf(row.when, FONT::amount);
    });
    double y = top + 4;
    int k = 0;
    std::optional<double> from;
    for (int i = 0; i < data.rows.size(); ++i) {
        if (c.cancelled())
            return std::nullopt;
        const RouteRow &row = data.rows.at(i);
        const double order = 0.3 + i * 0.3;
        const QString key = QStringLiteral("rt:%1").arg(i);
        if (row.isHead) {
            items << labelSpec(key, order, textX, y + (i ? 12 : 8), {caps(c, row.head, W - textX)},
                               QStringLiteral("dg-eyebrow"), QStringLiteral("start"), Back);
            y += i ? 30 : 24;
            continue;
        }
        const bool end = k == 0 || k == stops - 1;
        const double cy = y + 9, radius = end ? 5 : 4;
        const QStringList notes = !row.note.isEmpty()
                                      ? wrap(c, row.note, W - textX, FONT::note).mid(0, 3)
                                      : QStringList();
        // The line of the way runs from stop to stop; where it starts and
        // where it ends are rings.
        if (from) {
            Spec rail = lineSpec(key + QStringLiteral(":rail"), order - 0.1, 7, *from, 7,
                                 cy - radius - 3, QStringLiteral("dg-route-rail"), true, Back);
            rail.fixed.tone = first(c.tones);
            items << rail;
        }
        items << dotSpec(key + QStringLiteral(":stop"), order, 7, cy, radius,
                         end ? QStringLiteral("dg-route-stop is-end")
                             : QStringLiteral("dg-route-stop"),
                         first(c.tones), Nodes);
        items << labelSpec(
            key + QStringLiteral(":place"), order + 0.05, textX, cy,
            {truncate(c, row.place, W - textX - (whenW ? whenW + 16 : 0), FONT::strong)},
            QStringLiteral("dg-row-total"), QStringLiteral("start"));
        if (!row.when.isEmpty())
            items << labelSpec(key + QStringLiteral(":when"), order + 0.08, W, cy, {row.when},
                               QStringLiteral("dg-match-time"), QStringLiteral("end"));
        double ty = cy + 10;
        if (!notes.isEmpty()) {
            items << block(key + QStringLiteral(":note"), order + 0.1, textX, ty + 2, notes, 16,
                           QStringLiteral("dg-step-note"));
            ty += 2 + notes.size() * 16;
        }
        from = cy + radius + 3;
        if (!row.leg.isEmpty() && k < stops - 1) {
            items << labelSpec(key + QStringLiteral(":leg"), order + 0.15, textX, ty + 16,
                               {truncate(c, row.leg, W - textX, FONT::small)},
                               QStringLiteral("dg-route-leg"), QStringLiteral("start"), Back);
            ty += 26;
        }
        y = ty + 14;
        ++k;
    }
    r.width = W;
    r.height = y - 14;
    return r;
}

template <typename Data>
std::optional<Result> sceneOf(Ctx &c, const std::optional<Data> &data,
                              std::optional<Result> (*scene)(Ctx &, const Data &))
{
    if (!data)
        return std::nullopt;
    return scene(c, *data);
}
} // namespace

std::optional<Result> factsKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseFacts(lines.lines), factsScene);
}

std::optional<Result> checklistKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseChecklist(lines.lines), checklistScene);
}

std::optional<Result> changesKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseChanges(lines.lines), changesScene);
}

std::optional<Result> outlineKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseOutline(lines.lines), outlineScene);
}

std::optional<Result> matchesKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseMatches(lines.lines), matchesScene);
}

std::optional<Result> wordsKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseWords(lines.lines), wordsScene);
}

std::optional<Result> glossKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseGloss(c, lines.lines), glossScene);
}

std::optional<Result> formsKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseForms(lines.lines), formsScene);
}

std::optional<Result> recipeKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseRecipe(lines.lines), recipeScene);
}

std::optional<Result> partsKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseParts(lines.lines), partsScene);
}

std::optional<Result> settingsKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseSettings(lines.lines), settingsScene);
}

std::optional<Result> routeKind(Ctx &c, const Lines &lines)
{
    return sceneOf(c, parseRoute(lines.lines), routeScene);
}

} // namespace diagram
