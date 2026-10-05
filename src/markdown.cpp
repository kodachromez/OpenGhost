#include "markdown.h"

#include "highlight.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

// A port of openghost/markdown.js. Where it builds HTML strings, this builds
// the display tree; the parsing rules, bounds and live hold-back are the same.
// Not ported: calculation grids and sheets (such blocks show as code).
namespace markdown
{
namespace
{
using Rx = QRegularExpression;

// Each thread compiles its own copy once; matching never shares state.
#define PATTERN(name, pattern)                                                                     \
    const Rx &name()                                                                               \
    {                                                                                              \
        thread_local const Rx rx(QStringLiteral(pattern));                                         \
        return rx;                                                                                 \
    }
PATTERN(listItem, R"(^([ \t]*)([-*+]|\d{1,9}[.)])(?:([ \t]+)(.*))?$)")
PATTERN(heading, R"(^ {0,3}(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$)")
PATTERN(fence, R"(^([ \t]*)(`{3,}|~{3,})[ \t]*([^\s`]*)([^`]*)$)")
PATTERN(fenceClose, R"(^[ \t]*(`{3,}|~{3,})[ \t]*$)")
PATTERN(anyFence, R"(^[ \t]*(`{3,}|~{3,})(.*)$)")
PATTERN(rule, R"(^ {0,3}([-*_])(?:[ \t]*\1){2,}[ \t]*$)")
PATTERN(setext, R"(^ {0,3}(=+|-+)[ \t]*$)")
PATTERN(tableRule, R"(^[ \t]*\|?[ \t]*:?-+:?[ \t]*(?:\|[ \t]*:?-+:?[ \t]*)*\|?[ \t]*$)")
PATTERN(quote, R"(^ {0,3}>[ \t]?)")
// markdown.js CALLOUT is case-insensitive: GitHub writes [!NOTE].
PATTERN(callout, R"((?i)^\[!(note|tip|important|warning|caution)\][ \t]*(.*)$)")
PATTERN(mathBlock, R"(^[ \t]*(\$\$|\\\[)(.*)$)")
PATTERN(boldLine, R"(^(\*\*|__)(?=\S)(?:(?!\1).)+?(?<=\S)\1:?$)")
PATTERN(flowArrow, R"([ \t]+(?:→|->|-->|⟶|=>|⇒|➜|➔|➝)[ \t]+)")
PATTERN(
    partial,
    R"(^[ \t]*(?:#{1,6}[ \t]*|[-*+][ \t]*|\d{1,9}[.)]?[ \t]*|(?:>[ \t]*)+(?:[—–―][ \t]*)?|`{1,2}|`{3,}[^`]*|~{1,2}|~{3,}[^~]*|[-*_=](?:[ \t]*[-*_=])*[ \t]*|\$\$?|\\\[?|\\)$)")
PATTERN(pipe, R"(^[ \t]*\|)")
PATTERN(art, R"([\x{2500}-\x{259f}])")
PATTERN(
    bareDiagram,
    R"(^((flowchart|graph)\s+(TD|TB|LR|RL|BT)|(sequenceDiagram|stateDiagram(-v2)?|xychart(-beta)?|quadrantChart|radar-beta|erDiagram|classDiagram|mindmap|gantt|timeline|candlestick|wireframe|gitGraph|sankey-beta|treemap-beta|kanban|journey)\b.*|pie(\s+(showData|title\b.*))?)\s*$)")
PATTERN(
    diagramHeader,
    R"((?i)^(graph|flowchart|stateDiagram(-v2)?|sequenceDiagram|pie|xychart(-beta)?|candlestick|candles|ohlc|timeline|gantt|mindmap|quadrantChart|radar(-beta)?|erDiagram|classDiagram(-v2)?|wireframe|mockup|files|folder|metrics|bars|ledger|ranges?|plan|board|kanban|steps|waterfall|funnel|sankey(-beta)?|heatmap|calendar|scatter|treemap(-beta)?|gitGraph|journey|array|cells|bracket|nutrition|food|meals?|facts|passport|checklist|checks|changes|diff|outline|contents|matches|fixtures|words|vocab|vocabulary|glossary|gloss|interlinear|forms|conjugation|declension|paradigm|recipe|cooking|parts|build|components|bom|settings|setup|toggles|route|itinerary|trip)\b)")
// A folder is a card like an attachment and stands in the column of the
// text; every other drawing has the chat's whole width (COLUMN_DIAGRAM).
PATTERN(columnDiagram, R"((?i)^(files|folder)\b)")
// A folder drawn with the files block, written without a fence: the word
// alone on a line, then its title, path or an entry with | right under it.
PATTERN(bareFiles, R"((?i)^[ \t]*(files|folder)[ \t]*$)")
PATTERN(filesLine, R"((?i)^[ \t]*((title|path|view|more)[ \t:]+\S|[^|\n]+\|))")
PATTERN(cite, R"(^[ \t]*(?:—|–|―|--)[ \t]+(\S.*)$)")
PATTERN(citeInline, R"(^(.*[»”"])[ \t]+(?:—|–|―|--?)[ \t]+(\S.*)$)")
PATTERN(linkTail,
        R"(\(\s*<?([^\s<>()]*(?:\([^\s<>()]*\)[^\s<>()]*)*)>?(?:\s+(?:"[^"]*"|'[^']*'))?\s*\))")
PATTERN(task, R"(^\[([ xX])\](?:[ \t]+(.*))?$)")
PATTERN(plainLink, R"(\[([^\]]*)\]\([^)]*\)|[*_`~])")
#undef PATTERN

bool matches(const Rx &rx, const QString &text) { return rx.match(text).hasMatch(); }

bool isSpaceAt(const QString &s, qsizetype i)
{
    return i < 0 || i >= s.size() || s.at(i).isSpace();
}

bool isPunct(QChar c)
{
    const ushort u = c.unicode();
    return (u >= '!' && u <= '/') || (u >= ':' && u <= '@') || (u >= '[' && u <= '`') ||
           (u >= '{' && u <= '~') || (u >= 0xa1 && u <= 0xbf) || (u >= 0x2010 && u <= 0x2027) ||
           (u >= 0x2030 && u <= 0x205e) || u == 0xab || u == 0xbb;
}

bool isPunctAt(const QString &s, qsizetype i) { return i >= 0 && i < s.size() && isPunct(s.at(i)); }

bool isDigitAt(const QString &s, qsizetype i)
{
    return i >= 0 && i < s.size() && s.at(i) >= QLatin1Char('0') && s.at(i) <= QLatin1Char('9');
}

bool escapable(QChar c)
{
    const ushort u = c.unicode();
    return (u >= '!' && u <= '/') || (u >= ':' && u <= '@') || (u >= '[' && u <= '`') ||
           (u >= '{' && u <= '~');
}

int indentOf(const QString &line)
{
    int n = 0;
    for (const QChar c : line) {
        if (c == QLatin1Char(' '))
            n++;
        else if (c == QLatin1Char('\t'))
            n += 4 - n % 4;
        else
            break;
    }
    return n;
}

QString dedent(const QString &line, int count)
{
    int n = 0, k = 0;
    while (k < line.size() && n < count) {
        if (line.at(k) == QLatin1Char(' '))
            n++;
        else if (line.at(k) == QLatin1Char('\t'))
            n += 4 - n % 4;
        else
            break;
        k++;
    }
    return line.mid(k);
}

QString trimStart(const QString &s)
{
    qsizetype i = 0;
    while (i < s.size() && s.at(i).isSpace())
        i++;
    return s.mid(i);
}

QString trimEnd(const QString &s)
{
    qsizetype i = s.size();
    while (i > 0 && s.at(i - 1).isSpace())
        i--;
    return s.left(i);
}

bool blank(const QString &s) { return s.trimmed().isEmpty(); }

// ---- Inline text -------------------------------------------------------

// One inline parse and every link label in it share one allowance of
// scanning work, past which the rest is literal (markdown.js INLINE).
struct Budget {
    int depth = InlineDepth;
    qint64 work = 0;
};

// Emphasis, strong and strike tags a delimiter opens or closes.
enum Tag : quint8 { TagStrong, TagEmphasis, TagStrike };

struct Token {
    enum Type { Text, Delim } type = Text;
    Inline text; // Text: display text with its own runs (code, links, math).
    // Delimiter state (markdown.js emphasis()).
    QChar delim;
    int count = 0;
    bool open = false, close = false;
    QVector<Tag> pre, post; // pre: closings, in order; post: openings, in order.
};

class InlineParser
{
  public:
    InlineParser(const QString &src, bool live, Budget &budget) : s(src), live(live), budget(budget)
    {
    }
    Inline parse();

  private:
    const QString &s;
    bool live;
    Budget &budget;
    QVector<Token> tokens;
    Inline pending; // Plain text not yet a token.
    QHash<qsizetype, qsizetype> closes;
    bool noParenClose = false, noBracketClose = false;

    void flush()
    {
        if (!pending.text.isEmpty()) {
            Token t;
            t.text = std::move(pending);
            tokens << std::move(t);
            pending = {};
        }
    }
    void text(const QString &piece) { pending.text += piece; }
    void put(Inline value)
    {
        flush();
        Token t;
        t.text = std::move(value);
        tokens << std::move(t);
    }
    void emitStyled(const QString &body, quint8 format, int ref = -1)
    {
        Inline value;
        value.text = body;
        if (!body.isEmpty())
            value.runs << Run{0, int(body.size()), format, ref};
        put(std::move(value));
    }
    void emitMath(const QString &source)
    {
        Inline value;
        value.text = Object;
        value.math << source;
        value.runs << Run{0, 1, Math, 0};
        put(std::move(value));
    }
    qsizetype findRun(QChar c, int length, qsizetype from);
    qsizetype closeBracket(qsizetype i);
    bool link(qsizetype &i);
    bool autolink(qsizetype &i);
    bool inlineMath(qsizetype &i);
    Inline label(const QString &text, bool labelLive);
    Inline assemble();
};

// Appends `part` to `out`, shifting its runs and references; `extra` is
// OR-ed into every character's format (a link around its label).
void append(Inline &out, const Inline &part, quint8 extra = 0, int ref = -1)
{
    const int base = int(out.text.size()), links = int(out.links.size()),
              math = int(out.math.size());
    out.text += part.text;
    out.links += part.links;
    out.math += part.math;
    int at = 0;
    auto add = [&](int start, int length, quint8 format, int runRef) {
        if (length <= 0 || !format)
            return;
        if (!out.runs.isEmpty()) {
            Run &last = out.runs.last();
            if (last.start + last.length == start && last.format == format && last.ref == runRef) {
                last.length += length;
                return;
            }
        }
        out.runs << Run{start, length, format, runRef};
    };
    for (const Run &run : part.runs) {
        add(base + at, run.start - at, extra, ref);
        int runRef = run.ref;
        if (run.format & Link && runRef >= 0)
            runRef += links;
        else if (run.format & Math)
            runRef += math;
        if (extra & Link && !(run.format & Math))
            runRef = ref;
        add(base + run.start, run.length, run.format | extra, runRef);
        at = run.start + run.length;
    }
    add(base + at, int(part.text.size()) - at, extra, ref);
}

qsizetype InlineParser::findRun(QChar c, int length, qsizetype from)
{
    for (qsizetype i = s.indexOf(c, from); i >= 0; i = s.indexOf(c, i)) {
        int run = 1;
        while (i + run < s.size() && s.at(i + run) == c)
            run++;
        budget.work -= i + run - from;
        from = i + run;
        if (run == length)
            return i;
        if (budget.work < 0)
            return -1;
        i += run;
    }
    budget.work -= s.size() - from;
    return -1;
}

// The `]` closing the `[` at i, or -1. One scan records every bracket it
// passes, so no later bracket scans the same suffix again.
qsizetype InlineParser::closeBracket(qsizetype i)
{
    const auto known = closes.constFind(i);
    if (known != closes.constEnd())
        return known.value();
    QVector<qsizetype> open{i};
    qsizetype j = i + 1;
    for (; j < s.size(); j++) {
        const QChar c = s.at(j);
        if (c == QLatin1Char('\\'))
            j++;
        else if (c == QLatin1Char('['))
            open << j;
        else if (c == QLatin1Char(']')) {
            closes.insert(open.takeLast(), j);
            if (open.isEmpty())
                break;
        }
    }
    budget.work -= std::min(j, s.size()) - i;
    for (qsizetype k : open)
        closes.insert(k, -1);
    return open.isEmpty() ? j : -1;
}

Inline InlineParser::label(const QString &text, bool labelLive)
{
    if (budget.depth <= 0) {
        Inline plain;
        plain.text = text;
        return plain;
    }
    budget.depth--;
    Inline value = InlineParser(text, labelLive, budget).parse();
    budget.depth++;
    return value;
}

QString bareUrl(QString text)
{
    text = text.trimmed();
    if (text.startsWith(QLatin1String("http://"), Qt::CaseInsensitive))
        text.remove(0, 7);
    else if (text.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        text.remove(0, 8);
    if (text.endsWith(QLatin1Char('/')))
        text.chop(1);
    return text;
}

bool safeUrl(const QString &href)
{
    return href.startsWith(QLatin1String("http:"), Qt::CaseInsensitive) ||
           href.startsWith(QLatin1String("https:"), Qt::CaseInsensitive) ||
           href.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive);
}

// link-chip.js label(): a short host name.
QString chipLabel(const QString &url)
{
    thread_local const Rx authority(
        QStringLiteral(
            R"(^(?:[a-z][\w+.-]*:\/\/)?(?:[^@\/?#\s]*@)?(\[[^\]]*\]|[^\/?#:\s]+)(?::(\d+))?)"),
        Rx::CaseInsensitiveOption);
    static const QSet<QString> shorteners = {"youtu.be", "bit.ly",  "goo.gl",  "t.co",
                                             "g.co",     "amzn.to", "lnkd.in", "fb.me",
                                             "wa.me",    "vk.cc",   "clck.ru"};
    static const QSet<QString> suffixes = {
        "co.uk",  "org.uk", "ac.uk",  "gov.uk", "com.au", "net.au", "org.au", "co.jp",  "ne.jp",
        "or.jp",  "com.br", "com.cn", "com.tr", "com.ua", "co.kr",  "co.in",  "co.nz",  "co.za",
        "com.mx", "com.ar", "com.sg", "com.hk", "com.tw", "org.ru", "msk.ru", "spb.ru", "co.il"};
    const auto m = authority.match(url);
    QString name = url, port;
    if (m.hasMatch()) {
        name = m.captured(1).toLower();
        if (name.endsWith(QLatin1Char('.')))
            name.chop(1);
        name.remove(Rx(QStringLiteral("^www\\d?\\.")));
        port = m.captured(2);
    }
    QString text = name;
    if (!port.isEmpty()) {
        text = name + QLatin1Char(':') + port;
    } else if (name.contains(QLatin1Char('.')) && !matches(Rx(QStringLiteral("^[\\d.]+$")), name) &&
               !name.startsWith(QLatin1Char('[')) && !shorteners.contains(name)) {
        const QStringList bits = name.split(QLatin1Char('.'));
        const int cut =
            bits.size() > 2 && suffixes.contains(bits.mid(bits.size() - 2).join('.')) ? 2 : 1;
        const QString rest = bits.mid(0, bits.size() - cut).join(QLatin1Char('.'));
        if (rest.size() > 2)
            text = rest;
    }
    return text.size() > 28 ? QStringLiteral("…") + text.right(27) : text;
}

bool InlineParser::link(qsizetype &i)
{
    const qsizetype n = s.size();
    const qsizetype j = closeBracket(i);
    if (j < 0) {
        if (!live)
            return false;
        i = n; // A label still streaming hides the rest.
        return true;
    }
    const QString labelText = s.mid(i + 1, j - i - 1);
    if (j + 1 >= n || s.at(j + 1) != QLatin1Char('(')) {
        if (live && j + 1 == n) {
            i = n;
            return true;
        }
        return false;
    }
    const auto m = linkTail().match(s, j + 1, Rx::NormalMatch, Rx::AnchorAtOffsetMatchOption);
    if (!m.hasMatch()) {
        if (live && s.indexOf(QLatin1Char(')'), j + 1) < 0) {
            // The destination is still streaming: a link without a target yet.
            Inline value;
            append(value, label(labelText, true), Link, -1);
            put(std::move(value));
            i = n;
            return true;
        }
        return false;
    }
    const QString href = m.captured(1);
    const qsizetype end = m.capturedEnd(0);
    Inline body = label(labelText, false);
    if (!safeUrl(href)) {
        put(std::move(body));
        i = end;
        return true;
    }
    Inline value;
    value.links << href;
    const bool http = href.startsWith(QLatin1String("http"), Qt::CaseInsensitive);
    if (http && bareUrl(labelText) == bareUrl(href)) {
        Inline chip;
        chip.text = chipLabel(href);
        append(value, chip, Link | Chip, 0);
    } else {
        append(value, body, Link, 0);
    }
    put(std::move(value));
    i = end;
    return true;
}

bool InlineParser::autolink(qsizetype &i)
{
    static const QString before = QStringLiteral("@/.");
    if (i > 0) {
        const QChar p = s.at(i - 1);
        if (p.isLetterOrNumber() || p == QLatin1Char('_') || before.contains(p))
            return false;
    }
    qsizetype start = i;
    if (s.mid(i, 7).compare(QLatin1String("http://"), Qt::CaseInsensitive) == 0)
        start += 7;
    else if (s.mid(i, 8).compare(QLatin1String("https://"), Qt::CaseInsensitive) == 0)
        start += 8;
    else if (s.mid(i, 4) == QLatin1String("www."))
        start += 4;
    else
        return false;
    static const QString stops = QStringLiteral("<>\"'`");
    qsizetype end = start;
    const qsizetype limit = std::min(s.size(), i + 2048);
    while (end < limit && !s.at(end).isSpace() && !stops.contains(s.at(end)))
        end++;
    if (end == start)
        return false;
    if (live && end >= s.size()) {
        i = s.size();
        return true;
    }
    QString url = s.mid(i, end - i);
    // link-chip.js trim(): trailing punctuation and unmatched `)`.
    static const QString trail = QStringLiteral(".,;:!?»\"'…*_~");
    const int opens = int(url.count(QLatin1Char('(')));
    int closesCount = int(url.count(QLatin1Char(')')));
    qsizetype cut = url.size();
    for (; cut; cut--) {
        const QChar c = url.at(cut - 1);
        if (c == QLatin1Char(')') && opens < closesCount)
            closesCount--;
        else if (!trail.contains(c))
            break;
    }
    url.truncate(cut);
    if (url.size() < 8)
        return false;
    Inline value;
    value.links << (url.startsWith(QLatin1String("www.")) ? QStringLiteral("https://") + url : url);
    Inline chip;
    chip.text = chipLabel(url);
    append(value, chip, Link | Chip, 0);
    put(std::move(value));
    i += url.size();
    return true;
}

bool InlineParser::inlineMath(qsizetype &i)
{
    if (i + 1 >= s.size() || s.at(i + 1) == QLatin1Char('$') || isSpaceAt(s, i + 1))
        return false;
    for (qsizetype j = s.indexOf(QLatin1Char('$'), i + 1); j > 0;
         j = s.indexOf(QLatin1Char('$'), j + 1)) {
        budget.work -= j - i;
        if (budget.work < 0)
            return false;
        if (s.at(j - 1) == QLatin1Char('\\'))
            continue;
        const QString body = s.mid(i + 1, j - i - 1);
        if (isSpaceAt(s, j - 1) || isDigitAt(s, j + 1) || body.contains(QLatin1Char('\n')))
            return false;
        static const QString marks = QStringLiteral("\\^_={}");
        const bool marked =
            std::any_of(body.cbegin(), body.cend(), [](QChar c) { return marks.contains(c); });
        const bool letter = body.size() == 1 && body.at(0).unicode() < 128 && body.at(0).isLetter();
        if (!marked && !letter)
            return false;
        emitMath(body);
        i = j + 1;
        return true;
    }
    return false;
}

Inline InlineParser::parse()
{
    const qsizetype n = s.size();
    for (qsizetype i = 0; i < n;) {
        if (budget.work < 0) {
            text(s.mid(i));
            break;
        }
        const QChar c = s.at(i);
        if (c == QLatin1Char('\\')) {
            const QChar next = i + 1 < n ? s.at(i + 1) : QChar();
            if (next == QLatin1Char('(') || next == QLatin1Char('[')) {
                const bool paren = next == QLatin1Char('(');
                bool &absent = paren ? noParenClose : noBracketClose;
                const QString delim = paren ? QStringLiteral("\\)") : QStringLiteral("\\]");
                const qsizetype close = absent ? -1 : s.indexOf(delim, i + 2);
                if (close < 0)
                    absent = true;
                if (close >= 0 || live) {
                    emitMath(s.mid(i + 2, (close >= 0 ? close : n) - i - 2));
                    i = close >= 0 ? close + 2 : n;
                    continue;
                }
            }
            if (next.isNull()) {
                if (!live)
                    text(c);
                i++;
                continue;
            }
            if (escapable(next)) {
                text(next);
                i += 2;
                continue;
            }
            text(c);
            i++;
            continue;
        }
        if (c == QLatin1Char('`')) {
            int run = 1;
            while (i + run < n && s.at(i + run) == QLatin1Char('`'))
                run++;
            const qsizetype close = findRun(QLatin1Char('`'), run, i + run);
            if (close >= 0 || live) {
                QString body = s.mid(i + run, (close >= 0 ? close : n) - i - run);
                if (body.size() >= 3 && body.startsWith(QLatin1Char(' ')) &&
                    body.endsWith(QLatin1Char(' ')) && !body.trimmed().isEmpty())
                    body = body.mid(1, body.size() - 2);
                emitStyled(body, Code);
                i = close >= 0 ? close + run : n;
                continue;
            }
            text(s.mid(i, run));
            i += run;
            continue;
        }
        if (c == QLatin1Char('$') && inlineMath(i))
            continue;
        if (c == QLatin1Char('[') && link(i))
            continue;
        if ((c == QLatin1Char('h') || c == QLatin1Char('w')) &&
            (s.mid(i, 4) == QLatin1String("http") || s.mid(i, 4) == QLatin1String("www.")) &&
            autolink(i))
            continue;
        if (c == QLatin1Char('*') || c == QLatin1Char('_') || c == QLatin1Char('~')) {
            int run = 1;
            while (i + run < n && s.at(i + run) == c)
                run++;
            if (live && i + run == n) {
                i = n;
                continue;
            }
            if ((c == QLatin1Char('~') && run != 2) ||
                (isDigitAt(s, i - 1) && isDigitAt(s, i + run))) {
                text(s.mid(i, run));
                i += run;
                continue;
            }
            const qsizetype before = i - 1, after = i + run;
            const bool left =
                !isSpaceAt(s, after) &&
                (!isPunctAt(s, after) || isSpaceAt(s, before) || isPunctAt(s, before));
            const bool right =
                !isSpaceAt(s, before) &&
                (!isPunctAt(s, before) || isSpaceAt(s, after) || isPunctAt(s, after));
            flush();
            Token t;
            t.type = Token::Delim;
            t.delim = c;
            t.count = run;
            if (c == QLatin1Char('_')) {
                t.open = left && (!right || isPunctAt(s, before));
                t.close = right && (!left || isPunctAt(s, after));
            } else {
                t.open = left;
                t.close = right;
            }
            tokens << std::move(t);
            i += run;
            continue;
        }
        text(c);
        i++;
    }
    flush();
    return assemble();
}

// markdown.js emphasis(): pairs delimiter runs into tags, then flattens the
// tokens into display text and runs.
Inline InlineParser::assemble()
{
    QVector<int> stack;
    for (int k = 0; k < tokens.size(); ++k) {
        Token &t = tokens[k];
        if (t.type != Token::Delim)
            continue;
        const bool tilde = t.delim == QLatin1Char('~');
        if (t.close) {
            for (int si = int(stack.size()) - 1; si >= 0 && t.count;) {
                Token &o = tokens[stack.at(si)];
                const int use = tilde ? 2 : o.count >= 2 && t.count >= 2 ? 2 : 1;
                if (o.delim != t.delim || o.count < use || t.count < use) {
                    si--;
                    continue;
                }
                const Tag tag = tilde ? TagStrike : use == 2 ? TagStrong : TagEmphasis;
                o.post.prepend(tag);
                t.pre << tag;
                o.count -= use;
                t.count -= use;
                stack.resize(si + 1);
                if (!o.count) {
                    stack.removeLast();
                    si--;
                }
            }
        }
        if (t.open && t.count)
            stack << k;
    }
    QVector<Tag> tail;
    if (live) {
        for (int si = int(stack.size()) - 1; si >= 0; si--) {
            Token &o = tokens[stack.at(si)];
            const bool tilde = o.delim == QLatin1Char('~');
            while (o.count >= (tilde ? 2 : 1)) {
                const int use = tilde ? 2 : std::min(2, o.count);
                const Tag tag = tilde ? TagStrike : use == 2 ? TagStrong : TagEmphasis;
                o.post.prepend(tag);
                tail << tag;
                o.count -= use;
            }
        }
    }
    Inline out;
    int depth[3] = {0, 0, 0};
    auto format = [&depth]() -> quint8 {
        return (depth[TagStrong] ? Strong : 0) | (depth[TagEmphasis] ? Emphasis : 0) |
               (depth[TagStrike] ? Strike : 0);
    };
    auto apply = [&depth](const QVector<Tag> &tags, int step) {
        for (Tag tag : tags)
            depth[tag] = std::max(0, depth[tag] + step);
    };
    for (const Token &t : tokens) {
        if (t.type == Token::Text) {
            append(out, t.text, format());
            continue;
        }
        apply(t.pre, -1);
        if (t.count) {
            Inline literal;
            literal.text = QString(t.count, t.delim);
            append(out, literal, format());
        }
        apply(t.post, +1);
    }
    Q_UNUSED(tail);
    return out;
}

Inline inlineOf(const QString &src, bool live)
{
    Budget budget;
    budget.work = qint64(src.size()) * (InlineDepth + 1);
    return InlineParser(src, live, budget).parse();
}

// ---- Blocks -------------------------------------------------------------

struct RawItem {
    QStringList body;
    qint8 task = -1;
};

struct Raw {
    enum Type { Code, Math, Heading, Rule, Quote, List, Table, Para, Flow } type = Para;
    int from = 0, to = 0; // Line range.
    QString lang, info, body;
    bool closed = false;
    int level = 0;
    bool pseudo = false;
    QString text;
    QStringList lines; // Quote, paragraph, table header, flow parts.
    bool ordered = false;
    int start = 1;
    QVector<RawItem> items;
    QVector<quint8> align;
    QVector<QStringList> rows;
};

bool isTable(const QStringList &lines, int i)
{
    return i + 1 < lines.size() && lines.at(i).contains(QLatin1Char('|')) &&
           lines.at(i + 1).contains(QLatin1Char('|')) &&
           lines.at(i + 1).contains(QLatin1Char('-')) && matches(tableRule(), lines.at(i + 1));
}

bool startsBlock(const QStringList &lines, int i)
{
    const QString &line = lines.at(i);
    return matches(heading(), line) || matches(fence(), line) || matches(rule(), line) ||
           matches(quote(), line) || matches(mathBlock(), line) || matches(listItem(), line) ||
           isTable(lines, i);
}

QStringList cells(const QString &row)
{
    QStringList out;
    QString cell;
    bool code = false;
    QString body = row.trimmed();
    if (body.startsWith(QLatin1Char('|')))
        body.remove(0, 1);
    if (body.endsWith(QLatin1Char('|')) &&
        !(body.size() >= 2 && body.at(body.size() - 2) == QLatin1Char('\\')))
        body.chop(1);
    for (qsizetype i = 0; i < body.size(); i++) {
        const QChar c = body.at(i);
        if (c == QLatin1Char('\\') && i + 1 < body.size() && body.at(i + 1) == QLatin1Char('|')) {
            cell += QLatin1Char('|');
            i++;
            continue;
        }
        if (c == QLatin1Char('`'))
            code = !code;
        if (c == QLatin1Char('|') && !code) {
            out << cell.trimmed();
            cell.clear();
            continue;
        }
        cell += c;
    }
    out << cell.trimmed();
    return out;
}

QStringList flowParts(const QString &line)
{
    if (line.size() > 180 || !matches(flowArrow(), line))
        return {};
    QStringList parts = line.split(flowArrow());
    for (auto &part : parts)
        part = part.trimmed();
    if (parts.size() < 3 || std::any_of(parts.cbegin(), parts.cend(), [](const QString &part) {
            return part.isEmpty() || part.size() > 42;
        }))
        return {};
    return parts;
}

int parseList(const QStringList &lines, int i, Raw &block)
{
    const auto first = listItem().match(lines.at(i));
    const int base = indentOf(first.captured(1));
    const bool ordered = first.captured(2).at(0).isDigit();
    block.type = Raw::List;
    block.ordered = ordered;
    block.start = ordered ? first.captured(2).chopped(1).toInt() : 1;
    while (i < lines.size()) {
        const auto m = listItem().match(lines.at(i));
        if (!m.hasMatch() || indentOf(m.captured(1)) > base ||
            m.captured(2).at(0).isDigit() != ordered)
            break;
        const int gap =
            m.capturedLength(3) ? (m.capturedLength(3) > 4 ? 1 : int(m.capturedLength(3))) : 1;
        const int column = indentOf(m.captured(1)) + int(m.capturedLength(2)) + gap;
        RawItem item;
        item.body << m.captured(4);
        int j = i + 1;
        for (; j < lines.size(); j++) {
            const QString &line = lines.at(j);
            if (blank(line)) {
                item.body << QString();
                continue;
            }
            const int indent = indentOf(line);
            if (indent > base) {
                item.body << dedent(line, std::min(indent, column));
                continue;
            }
            if (matches(listItem(), line))
                break;
            if (!blank(item.body.last()) && !startsBlock(lines, j)) {
                item.body << line.trimmed();
                continue;
            }
            break;
        }
        while (item.body.size() > 1 && blank(item.body.last()))
            item.body.removeLast();
        const auto box = task().match(item.body.first());
        if (box.hasMatch()) {
            item.task = box.captured(1) != QLatin1String(" ") ? 1 : 0;
            item.body.first() = box.captured(2);
        }
        block.items << item;
        i = j;
    }
    return i;
}

QVector<Raw> parseRaw(const QStringList &lines, int openLine, bool top)
{
    QVector<Raw> blocks;
    const int n = int(lines.size());
    int i = 0;
    while (i < n) {
        const QString &line = lines.at(i);
        const int from = i;
        if (blank(line)) {
            i++;
            continue;
        }
        Raw block;
        block.from = from;
        auto add = [&](Raw &&b, int to) {
            b.to = to;
            blocks << std::move(b);
        };
        auto m = fence().match(line);
        if (m.hasMatch() && indentOf(m.captured(1)) < 4) {
            const QString mark = m.captured(2);
            const int pad = indentOf(m.captured(1));
            QStringList body;
            bool closed = false;
            for (i++; i < n; i++) {
                const auto close = fenceClose().match(lines.at(i));
                if (close.hasMatch() && close.captured(1).at(0) == mark.at(0) &&
                    close.capturedLength(1) >= mark.size()) {
                    closed = true;
                    i++;
                    break;
                }
                body << dedent(lines.at(i), pad);
            }
            block.type = Raw::Code;
            block.lang = m.captured(3).toLower();
            block.info = m.captured(4).trimmed();
            block.body = body.join(QLatin1Char('\n'));
            block.closed = closed;
            add(std::move(block), i);
            continue;
        }
        if (top && matches(bareFiles(), line) && i + 1 < n &&
            matches(filesLine(), lines.at(i + 1))) {
            QStringList body;
            for (; i < n && !blank(lines.at(i)); i++)
                body << lines.at(i);
            block.type = Raw::Code;
            block.lang = QStringLiteral("files");
            block.body = body.join(QLatin1Char('\n'));
            block.closed = i < n;
            add(std::move(block), i);
            continue;
        }
        if ((m = mathBlock().match(line)).hasMatch()) {
            const QString close =
                m.captured(1) == QLatin1String("$$") ? QStringLiteral("$$") : QStringLiteral("\\]");
            QStringList body;
            bool closed = false;
            const QString rest = m.captured(2);
            const qsizetype at = rest.indexOf(close);
            if (at >= 0) {
                body << rest.left(at);
                closed = true;
                i++;
            } else {
                body << rest;
                for (i++; i < n; i++) {
                    const qsizetype k = lines.at(i).indexOf(close);
                    if (k >= 0) {
                        body << lines.at(i).left(k);
                        closed = true;
                        i++;
                        break;
                    }
                    body << lines.at(i);
                }
            }
            block.type = Raw::Math;
            block.body = body.join(QLatin1Char('\n')).trimmed();
            block.closed = closed;
            add(std::move(block), i);
            continue;
        }
        if ((m = heading().match(line)).hasMatch()) {
            i++;
            if (!m.captured(2).isEmpty()) {
                block.type = Raw::Heading;
                block.level = int(m.capturedLength(1));
                block.text = m.captured(2);
                add(std::move(block), i);
            }
            continue;
        }
        if (matches(rule(), line)) {
            i++;
            block.type = Raw::Rule;
            add(std::move(block), i);
            continue;
        }
        if (matches(quote(), line)) {
            for (; i < n && matches(quote(), lines.at(i)); i++) {
                QString body = lines.at(i);
                body.remove(0, quote().match(body).capturedLength(0));
                block.lines << body;
            }
            block.type = Raw::Quote;
            add(std::move(block), i);
            continue;
        }
        if (matches(listItem(), line)) {
            i = parseList(lines, i, block);
            add(std::move(block), i);
            continue;
        }
        if (isTable(lines, i)) {
            block.type = Raw::Table;
            block.lines = cells(line);
            for (const QString &c : cells(lines.at(i + 1)))
                block.align << quint8(
                    c.endsWith(QLatin1Char(':')) ? (c.startsWith(QLatin1Char(':')) ? 1 : 2) : 0);
            for (i += 2; i < n && lines.at(i).contains(QLatin1Char('|')) && !blank(lines.at(i));
                 i++)
                block.rows << cells(lines.at(i));
            add(std::move(block), i);
            continue;
        }
        QStringList para{line.trimmed()};
        int setextLevel = 0;
        for (i++; i < n; i++) {
            const QString &next = lines.at(i);
            if (blank(next))
                break;
            const auto st = setext().match(next);
            if (st.hasMatch() && para.size() == 1) {
                const bool equals = st.captured(1).at(0) == QLatin1Char('=');
                static const QString endings = QStringLiteral(".!?,;:");
                const bool fits =
                    equals ? para.first().size() <= 100
                           : para.first().size() <= 60 &&
                                 !(para.first().size() && endings.contains(para.first().back()));
                if (fits) {
                    setextLevel = equals ? 1 : 2;
                    i++;
                    break;
                }
            }
            if (startsBlock(lines, i))
                break;
            para << next.trimmed();
        }
        if (setextLevel) {
            block.type = Raw::Heading;
            block.level = setextLevel;
            block.text = para.first();
            add(std::move(block), i);
            continue;
        }
        if (top) {
            if (matches(boldLine(), para.first()) && para.first().size() <= 100 &&
                from != openLine) {
                Raw pseudo;
                pseudo.type = Raw::Heading;
                pseudo.from = from;
                pseudo.level = 4;
                pseudo.pseudo = true;
                pseudo.text = para.takeFirst();
                add(std::move(pseudo), from + 1);
                if (para.isEmpty())
                    continue;
            }
            const QStringList parts =
                para.size() == 1 && i - 1 != openLine ? flowParts(para.first()) : QStringList();
            if (!parts.isEmpty()) {
                Raw flow;
                flow.type = Raw::Flow;
                flow.from = i - 1;
                flow.lines = parts;
                add(std::move(flow), i);
                continue;
            }
        }
        block.type = Raw::Para;
        block.from = i - int(para.size());
        block.lines = para;
        add(std::move(block), i);
    }
    return blocks;
}

bool insideFence(const QStringList &lines, int end)
{
    QString open;
    for (int k = 0; k < end; k++) {
        const auto m = anyFence().match(lines.at(k));
        if (!m.hasMatch())
            continue;
        if (open.isEmpty())
            open = m.captured(1);
        else if (m.captured(1).at(0) == open.at(0) && m.capturedLength(1) >= open.size() &&
                 blank(m.captured(2)))
            open.clear();
    }
    return !open.isEmpty();
}

void holdBack(QStringList &lines)
{
    const int last = int(lines.size()) - 1;
    if (matches(partial(), lines.at(last)))
        lines[last].clear();
    int end = last;
    if (blank(lines.at(end)))
        end--;
    int start = end;
    while (start >= 0 && matches(pipe(), lines.at(start)))
        start--;
    start++;
    if (start > end || insideFence(lines, start))
        return;
    const bool confirmed = start + 1 < last && matches(tableRule(), lines.at(start + 1));
    if (!confirmed) {
        for (int k = start; k <= end; k++)
            lines[k].clear();
    }
}

// ---- Rendering into the display tree ---------------------------------

quint64 mix(quint64 h, quint64 v) { return (h ^ v) * 1099511628211ull + 0x9e3779b97f4a7c15ull; }

quint64 hashInline(quint64 h, const Inline &text)
{
    h = mix(h, qHash(text.text));
    for (const Run &run : text.runs)
        h = mix(h, (quint64(run.start) << 32) ^ (quint64(run.length) << 8) ^ run.format ^
                       (quint64(uint(run.ref)) << 40));
    for (const QString &link : text.links)
        h = mix(h, qHash(link));
    for (const QString &math : text.math)
        h = mix(h, qHash(math));
    return h;
}

void seal(Block &b)
{
    quint64 h = mix(quint64(b.kind) + 1, quint64(b.tone + 2));
    h = hashInline(h, b.text);
    h = hashInline(h, b.role);
    h = mix(h, (quint64(b.level) << 8) ^ (b.flag ? 2 : 0) ^ (b.open ? 4 : 0) ^ (b.clipped ? 8 : 0));
    h = mix(h, qHash(b.source));
    h = mix(h, qHash(b.lang));
    for (const Run &run : b.tokens)
        h = mix(h, (quint64(run.start) << 32) ^ (quint64(run.length) << 8) ^ run.format);
    for (const Inline &cell : b.cells)
        h = hashInline(h, cell);
    for (quint8 a : b.align)
        h = mix(h, a);
    h = mix(h, (quint64(b.columns) << 16) ^ quint64(b.headingIndex + 1));
    for (const auto &child : b.children)
        h = mix(h, child->hash);
    for (const Item &item : b.items) {
        h = mix(h, quint64(item.task + 7));
        for (const auto &child : item.blocks)
            h = mix(h, child->hash);
    }
    h = mix(h, quint64(b.omitted));
    b.hash = h;
    // Displayed pieces, exactly as Renderer charged them: the block, its
    // table cells or flow steps, a citation, and each item with its blocks.
    b.pieces = 1;
    if (b.kind == Kind::Table || b.kind == Kind::Flow)
        b.pieces += int(b.cells.size());
    if (b.kind == Kind::Quote && (!b.text.text.isEmpty() || !b.role.text.isEmpty()))
        b.pieces += 2;
    for (const auto &child : b.children)
        b.pieces += child->pieces;
    for (const Item &item : b.items) {
        b.pieces += 1;
        for (const auto &child : item.blocks)
            b.pieces += child->pieces;
    }
}

struct State {
    const Options *options = nullptr;
    int heading = -1;   // Headings seen at the top level, minus one.
    int tone = -1;      // The current heading's tone; -1 before the first.
    int inherited = -1; // The tone a nested block shows (its container's).
    int depth = 0;
    int firstTone() const { return options->tones[0]; }
};

// Builds display blocks from raw ones, charging each piece to one budget
// shared by every nesting level before building it. Once a charge does not
// fit, the renderer is exhausted: the block being built is incomplete and
// nothing more is built, so an oversized message costs at most the budget.
// `end`: the block reaches the end of the text, which is still streaming
// (Options::live) or cut off (Options::clipped); false when neither.
class Renderer
{
  public:
    Renderer(const Options &options, int budget) : m_options(options), m_budget(budget) {}
    BlockPtr render(const Raw &raw, const State &state, bool end);
    Blocks renderAll(const QVector<Raw> &raws, const State &state, bool end);
    bool exhausted() const { return m_exhausted; }
    // The previous parse's last block, when the next top-level block starts
    // where it did in text that only grew: a growing fence's highlighting
    // continues from it.
    BlockPtr grown;
    Inline text(const QString &source, bool end)
    {
        Inline value = inlineOf(source, end && m_options.live);
        if (m_options.finish)
            m_options.finish(value);
        return value;
    }

  private:
    const Options &m_options;
    int m_budget;
    bool m_exhausted = false;
    bool take(qint64 pieces)
    {
        if (m_exhausted || pieces > m_budget) {
            m_exhausted = true;
            return false;
        }
        m_budget -= int(pieces);
        return true;
    }
    BlockPtr code(const Raw &raw, const State &state, bool end);
    BlockPtr list(const Raw &raw, const State &state, bool end);
    BlockPtr quote(const Raw &raw, const State &state, bool end);
    BlockPtr table(const Raw &raw, const State &state, bool end);
    BlockPtr flat(const QStringList &lines, const State &state, bool end);
};

Blocks Renderer::renderAll(const QVector<Raw> &raws, const State &state, bool end)
{
    Blocks out;
    for (int k = 0; k < raws.size() && !m_exhausted; ++k) {
        BlockPtr block = render(raws.at(k), state, end && k == raws.size() - 1);
        if (!block)
            break;
        out << std::move(block);
    }
    return out;
}

BlockPtr finished(Block &&b)
{
    seal(b);
    return std::make_shared<const Block>(std::move(b));
}

QString firstLine(const QString &body)
{
    for (const QString &line : body.split(QLatin1Char('\n'))) {
        if (!blank(line))
            return line;
    }
    return {};
}

// The line of a diagram that names its kind: the first past the block
// between two lines of --- Mermaid lets a diagram open with, and past its %%
// remarks and settings (markdown.js diagramHeader).
QString diagramHeaderOf(const QString &body)
{
    const QStringList lines = body.split(QLatin1Char('\n'));
    qsizetype i = 0;
    while (i < lines.size() && blank(lines.at(i)))
        ++i;
    if (i >= lines.size())
        return {};
    if (lines.at(i).trimmed() == QLatin1String("---")) {
        qsizetype close = i + 1;
        while (close < lines.size() && lines.at(close).trimmed() != QLatin1String("---"))
            ++close;
        if (close >= lines.size())
            return {};
        i = close + 1;
    }
    for (; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1String("%%")))
            return line;
    }
    return {};
}

const QHash<QString, QString> &diagramKinds()
{
    static const QHash<QString, QString> kinds = {
        {"flowchart", "flowchart"},
        {"graph", "flowchart"},
        {"statediagram", "stateDiagram-v2"},
        {"statediagram-v2", "stateDiagram-v2"},
        {"state", "stateDiagram-v2"},
        {"sequencediagram", "sequenceDiagram"},
        {"sequence", "sequenceDiagram"},
        {"pie", "pie"},
        {"xychart", "xychart-beta"},
        {"xychart-beta", "xychart-beta"},
        {"candlestick", "candlestick"},
        {"candles", "candlestick"},
        {"ohlc", "candlestick"},
        {"timeline", "timeline"},
        {"gantt", "gantt"},
        {"mindmap", "mindmap"},
        {"quadrantchart", "quadrantChart"},
        {"quadrant", "quadrantChart"},
        {"radar", "radar-beta"},
        {"radar-beta", "radar-beta"},
        {"erdiagram", "erDiagram"},
        {"er", "erDiagram"},
        {"classdiagram", "classDiagram"},
        {"classdiagram-v2", "classDiagram"},
        {"wireframe", "wireframe"},
        {"mockup", "wireframe"},
        {"files", "files"},
        {"folder", "files"},
        {"metrics", "metrics"},
        {"bars", "bars"},
        {"ledger", "bars"},
        {"ranges", "ranges"},
        {"range", "ranges"},
        {"plan", "plan"},
        {"board", "plan"},
        {"kanban", "kanban"},
        {"steps", "steps"},
        {"waterfall", "waterfall"},
        {"funnel", "funnel"},
        {"sankey", "sankey-beta"},
        {"sankey-beta", "sankey-beta"},
        {"heatmap", "heatmap"},
        {"calendar", "heatmap"},
        {"scatter", "scatter"},
        {"treemap", "treemap-beta"},
        {"treemap-beta", "treemap-beta"},
        {"gitgraph", "gitGraph"},
        {"journey", "journey"},
        {"array", "array"},
        {"cells", "array"},
        {"bracket", "bracket"},
        {"nutrition", "nutrition"},
        {"facts", "facts"},
        {"checklist", "checklist"},
        {"changes", "changes"},
        {"outline", "outline"},
        {"matches", "matches"},
        {"words", "words"},
        {"vocab", "words"},
        {"gloss", "gloss"},
        {"forms", "forms"},
        {"recipe", "recipe"},
        {"parts", "parts"},
        {"settings", "settings"},
        {"route", "route"},
    };
    return kinds;
}

BlockPtr Renderer::code(const Raw &raw, const State &state, bool end)
{
    static const QSet<QString> plainLangs = {"", "text", "txt", "plain", "plaintext"};
    static const QSet<QString> artLangs = {"",      "text",    "txt", "plain", "plaintext",
                                           "ascii", "diagram", "art", "tree"};
    Block b;
    const QString &lang = raw.lang;
    const QString kindName = diagramKinds().value(lang);
    const QString kind =
        kindName.isEmpty() ? QString() : (kindName + QLatin1Char(' ') + raw.info).trimmed();
    const bool bare =
        plainLangs.contains(lang) && matches(bareDiagram(), firstLine(raw.body).trimmed());
    if (lang == QLatin1String("mermaid") || lang == QLatin1String("mmd") || !kind.isEmpty() ||
        bare) {
        const bool open = !raw.closed && end && m_options.live;
        QString body = raw.body;
        if (open) {
            const qsizetype cut = body.lastIndexOf(QLatin1Char('\n'));
            body = cut < 0 ? QString() : body.left(cut);
        }
        const QString header = diagramHeaderOf(body);
        b.kind = Kind::Diagram;
        b.source = body;
        b.lang = kind;
        b.open = open;
        b.clipped = !raw.closed && end && m_options.clipped;
        // A figure stands in the column of the text and grows past it when it
        // needs the room; only a folder keeps to the column.
        const bool column = matches(columnDiagram(), kind.isEmpty() ? header : kind);
        b.flag = !column && !state.depth &&
                 (!kind.isEmpty() || (header.isEmpty() ? open : matches(diagramHeader(), header)));
        b.headingIndex = std::max(0, state.heading);
        b.tone = state.tone >= 0 ? state.tone : state.firstTone();
        return finished(std::move(b));
    }
    const bool art = artLangs.contains(lang) && matches(markdown::art(), firstLine(raw.body));
    b.kind = Kind::Code;
    b.source = raw.body;
    b.flag = art;
    b.open = !raw.closed && end && m_options.live;
    b.clipped = !raw.closed && end && m_options.clipped;
    b.lang = !lang.isEmpty() ? lang : art ? QStringLiteral("diagram") : QStringLiteral("code");
    // A fence that grew (the same kind, label and art, its source extended)
    // is highlighted on from where its last highlighting can resume, so a
    // long streamed fence is not scanned whole for every delta (P5-10).
    highlight::Resumable previous;
    const BlockPtr before = std::exchange(grown, nullptr);
    if (!state.depth && before && before->kind == Kind::Code && before->lang == b.lang &&
        before->flag == art && raw.body.startsWith(before->source))
        previous = {before->tokens, before->resume};
    const highlight::Resumable tokens =
        art ? highlight::art(raw.body, previous) : highlight::code(raw.body, lang, previous);
    b.tokens = tokens.runs;
    b.resume = tokens.resume;
    b.tone = state.tone >= 0 ? state.tone : art ? state.firstTone() : -1;
    return finished(std::move(b));
}

BlockPtr Renderer::list(const Raw &raw, const State &state, bool end)
{
    Block b;
    b.kind = Kind::List;
    b.flag = raw.ordered;
    b.level = raw.start;
    b.tone = state.depth ? state.inherited : state.tone;
    State inner = state;
    inner.depth++;
    inner.inherited = b.tone;
    const int shown = std::min(int(raw.items.size()), MaxItems);
    b.omitted = int(raw.items.size()) - shown;
    for (int k = 0; k < shown && take(1); ++k) {
        const RawItem &item = raw.items.at(k);
        Item out;
        out.task = item.task;
        out.blocks = renderAll(parseRaw(item.body, -1, false), inner, end && k == shown - 1);
        b.items << out;
    }
    return finished(std::move(b));
}

BlockPtr Renderer::quote(const Raw &raw, const State &state, bool end)
{
    static const QHash<QString, int> calloutTones = {
        {"note", 2}, {"tip", 6}, {"important", 0}, {"warning", 4}, {"caution", 3}};
    Block b;
    b.kind = Kind::Quote;
    State inner = state;
    inner.depth++;
    const auto m =
        raw.lines.isEmpty() ? QRegularExpressionMatch() : callout().match(raw.lines.first());
    if (m.hasMatch()) {
        b.lang = m.captured(1).toLower();
        b.tone = calloutTones.value(b.lang);
        inner.inherited = b.tone;
        QStringList body{m.captured(2)};
        body += raw.lines.mid(1);
        b.children = renderAll(parseRaw(body, -1, false), inner, end);
        return finished(std::move(b));
    }
    if (state.depth) {
        b.tone = state.inherited;
        inner.inherited = b.tone;
        b.children = renderAll(parseRaw(raw.lines, -1, false), inner, end);
        return finished(std::move(b));
    }
    // A top-level pull quote: an attribution line and wrapping quote marks.
    b.flag = true;
    b.tone = state.tone >= 0 ? state.tone : state.firstTone();
    inner.inherited = b.tone;
    QStringList lines = raw.lines;
    auto trim = [&lines] {
        while (!lines.isEmpty() && blank(lines.last()))
            lines.removeLast();
    };
    trim();
    QString citation;
    QRegularExpressionMatch c;
    if (lines.size() > 1 && (c = cite().match(lines.last())).hasMatch()) {
        citation = c.captured(1);
        lines.removeLast();
        trim();
    } else if (!lines.isEmpty() && (c = citeInline().match(lines.last())).hasMatch()) {
        citation = c.captured(2);
        lines.last() = c.captured(1);
    }
    const auto first =
        std::find_if(lines.cbegin(), lines.cend(), [](const QString &l) { return !blank(l); });
    if (first != lines.cend()) {
        static const QHash<QChar, QChar> pairs = {{QChar(0xab), QChar(0xbb)},
                                                  {QChar(0x201c), QChar(0x201d)},
                                                  {QChar(0x201e), QChar(0x201c)},
                                                  {QLatin1Char('"'), QLatin1Char('"')}};
        const int at = int(first - lines.cbegin());
        const QString head = trimStart(lines.at(at));
        const QString tail = trimEnd(lines.last());
        const QChar close = head.isEmpty() ? QChar() : pairs.value(head.at(0));
        if (!close.isNull() && tail.endsWith(close) && (at < lines.size() - 1 || tail.size() > 1)) {
            lines[at] = head.mid(1);
            lines.last() = trimEnd(lines.last()).chopped(1);
        }
    }
    b.children = renderAll(parseRaw(lines, -1, false), inner, end && citation.isEmpty());
    if (!citation.isEmpty()) {
        const qsizetype at = citation.indexOf(Rx(QStringLiteral(",\\s")));
        const QString name = at > 0 ? citation.left(at) : citation;
        const QString role = at > 0 ? citation.mid(at + 1).trimmed() : QString();
        b.text = text(name, end && role.isEmpty());
        b.role = text(role, end);
        if (!b.text.text.isEmpty() || !b.role.text.isEmpty())
            take(2);
    }
    return finished(std::move(b));
}

bool wideTable(const Raw &raw)
{
    const int columns = int(raw.lines.size());
    if (columns >= 5)
        return true;
    if (columns < 3)
        return false;
    auto plain = [](const QString &text) {
        QString out = text;
        out.replace(plainLink(), QStringLiteral("\\1"));
        return int(out.trimmed().size());
    };
    int sum = 0;
    for (int k = 0; k < columns; ++k) {
        int longest = plain(raw.lines.at(k));
        for (int r = 0; r < std::min(int(raw.rows.size()), MaxRows); ++r) {
            const QStringList &row = raw.rows.at(r);
            longest = std::max(longest, k < row.size() ? plain(row.at(k)) : 0);
        }
        sum += std::min(80, longest);
    }
    return sum > 130;
}

BlockPtr Renderer::table(const Raw &raw, const State &state, bool end)
{
    // Every row is padded to the header's width: charge the whole grid
    // before building any of it, and refuse a table too wide to show.
    const int rows = std::min(int(raw.rows.size()), MaxRows);
    if (raw.lines.size() > MaxColumns || !take(qint64(raw.lines.size()) * (rows + 1))) {
        m_exhausted = true;
        return nullptr;
    }
    Block b;
    b.kind = Kind::Table;
    b.columns = int(raw.lines.size());
    b.align = raw.align;
    b.align.resize(b.columns);
    b.flag = !state.depth && wideTable(raw);
    b.tone = state.tone >= 0 ? state.tone : state.firstTone();
    for (int k = 0; k < b.columns; ++k)
        b.cells << text(raw.lines.at(k), end && raw.rows.isEmpty() && k == b.columns - 1);
    b.omitted = int(raw.rows.size()) - rows;
    for (int r = 0; r < rows; ++r) {
        const QStringList &row = raw.rows.at(r);
        const bool lastRow = end && r == rows - 1;
        const int lastCell = std::min(int(row.size()), b.columns) - 1;
        for (int k = 0; k < b.columns; ++k)
            b.cells << text(k < row.size() ? row.at(k) : QString(), lastRow && k == lastCell);
    }
    return finished(std::move(b));
}

BlockPtr Renderer::flat(const QStringList &lines, const State &state, bool end)
{
    Block b;
    b.kind = Kind::Paragraph;
    b.tone = state.depth ? -1 : state.tone;
    b.text = text(lines.join(QLatin1Char('\n')), end);
    return finished(std::move(b));
}

QStringList listLines(const Raw &raw)
{
    QStringList out;
    for (int k = 0; k < raw.items.size(); ++k) {
        const RawItem &item = raw.items.at(k);
        const QString marker =
            raw.ordered ? QString::number(raw.start + k) + QLatin1Char('.') : QStringLiteral("-");
        const QString box = item.task < 0 ? QString()
                            : item.task   ? QStringLiteral("[x] ")
                                          : QStringLiteral("[ ] ");
        out << marker + QLatin1Char(' ') + box + item.body.join(QLatin1Char('\n'));
    }
    return out;
}

BlockPtr Renderer::render(const Raw &raw, const State &state, bool end)
{
    if (!take(1))
        return nullptr;
    Block b;
    switch (raw.type) {
    case Raw::Heading:
        b.kind = Kind::Heading;
        b.level = std::min(raw.level, 6);
        b.flag = raw.pseudo;
        b.tone = state.tone >= 0 ? state.tone : state.firstTone();
        b.text = text(raw.text, end);
        return finished(std::move(b));
    case Raw::Para:
        b.kind = Kind::Paragraph;
        b.tone = state.depth ? -1 : state.tone;
        b.text = text(raw.lines.join(QLatin1Char('\n')), end);
        return finished(std::move(b));
    case Raw::Flow:
        if (!take(raw.lines.size()))
            return nullptr;
        b.kind = Kind::Flow;
        b.headingIndex = std::max(0, state.heading);
        for (const QString &part : raw.lines)
            b.cells << text(part, false);
        return finished(std::move(b));
    case Raw::List:
        return state.depth < MaxDepth ? list(raw, state, end) : flat(listLines(raw), state, end);
    case Raw::Quote:
        return state.depth < MaxDepth ? quote(raw, state, end) : flat(raw.lines, state, end);
    case Raw::Code:
        return code(raw, state, end);
    case Raw::Table:
        return table(raw, state, end);
    case Raw::Math:
        b.kind = Kind::Math;
        b.source = raw.body;
        b.open = !raw.closed && end && m_options.live;
        b.clipped = !raw.closed && end && m_options.clipped;
        return finished(std::move(b));
    case Raw::Rule:
        b.kind = Kind::Rule;
        return finished(std::move(b));
    }
    return finished(std::move(b));
}

QString normalized(const QString &text)
{
    if (!text.contains(QLatin1Char('\r')) && !text.startsWith(QChar(0xfeff)))
        return text;
    QString out = text;
    if (out.startsWith(QChar(0xfeff)))
        out.remove(0, 1);
    out.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    out.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return out;
}
} // namespace

bool renderable(const QString &text)
{
    if (text.size() * 3 <= RenderBytes)
        return true;
    qsizetype bytes = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const ushort u = text.at(i).unicode();
        if (u < 0x80)
            bytes += 1;
        else if (u < 0x800)
            bytes += 2;
        else if (QChar::isHighSurrogate(u) && i + 1 < text.size() &&
                 text.at(i + 1).isLowSurrogate()) {
            bytes += 4;
            ++i;
        } else
            bytes += 3;
        if (bytes > RenderBytes)
            return false;
    }
    return true;
}

Inline inlineText(const QString &source, bool live) { return inlineOf(source, live); }

void expandMath(Inline &text, int ref)
{
    for (int k = 0; k < text.runs.size(); ++k) {
        Run &run = text.runs[k];
        if (!(run.format & Math) || run.ref != ref)
            continue;
        const QString shown = QLatin1Char('$') + text.math.value(ref) + QLatin1Char('$');
        const int grow = int(shown.size()) - run.length;
        text.text.replace(run.start, run.length, shown);
        run.length = int(shown.size());
        run.format = quint8((run.format & ~Math) | Code);
        run.ref = -1;
        for (int j = k + 1; j < text.runs.size(); ++j)
            text.runs[j].start += grow;
        return;
    }
}

std::array<int, 7> tonesFor(const QString &seed)
{
    // stream-view.js seeded() and shuffle().
    quint32 h = 2166136261u;
    for (qsizetype i = 0; i < seed.size(); i += 5)
        h = (h ^ seed.at(i).unicode()) * 16777619u;
    auto random = [&h]() {
        h = ((h ^ (h >> 15)) * 2246822507u) ^ ((h ^ (h >> 13)) * 3266489909u);
        h ^= h >> 16;
        return double(h) / 4294967296.0;
    };
    std::array<int, 7> out{0, 1, 2, 3, 4, 5, 6};
    for (int i = 6; i > 0; --i)
        std::swap(out[i], out[int(random() * (i + 1))]);
    return out;
}

Document parse(const QString &source, const Options &options, const Document &previous)
{
    Document doc;
    doc.text = normalized(source);
    doc.live = options.live;
    doc.clipped = options.clipped;
    // Blocks before the previous last one are final while text only grows.
    int keep = 0, resume = 0, headings = 0;
    const bool grew = !previous.blocks.isEmpty() && previous.live == options.live &&
                      previous.clipped == options.clipped && doc.text.startsWith(previous.text);
    if (grew && previous.blocks.size() > 1) {
        keep = int(previous.blocks.size()) - 1;
        resume = previous.starts.at(keep);
        headings = previous.headings;
    }
    QStringList lines = QStringView(doc.text).mid(resume).toString().split(QLatin1Char('\n'));
    const int openLine = options.live ? int(lines.size()) - 1 : -1;
    if (options.live)
        holdBack(lines);
    QVector<int> offsets;
    offsets.reserve(lines.size() + 1);
    int at = resume;
    for (const QString &line : std::as_const(lines)) {
        offsets << at;
        at += int(line.size()) + 1;
    }
    const QVector<Raw> raws = parseRaw(lines, openLine, true);
    doc.blocks = previous.blocks.mid(0, keep);
    doc.starts = previous.starts.mid(0, keep);
    int budget = MaxPieces;
    for (const auto &block : std::as_const(doc.blocks))
        budget -= block->pieces;
    State state;
    state.options = &options;
    state.heading = headings - 1;
    state.tone = state.heading >= 0 ? options.tones[state.heading % 7] : -1;
    Renderer renderer(options, budget);
    for (int k = 0; k < raws.size(); ++k) {
        if (options.cancel && options.cancel->load(std::memory_order_relaxed))
            break;
        const Raw &raw = raws.at(k);
        if (raw.type == Raw::Heading) {
            state.heading++;
            state.tone = options.tones[state.heading % 7];
        }
        state.inherited = state.tone;
        if (k == 0 && grew && offsets.value(raw.from, at) == previous.starts.last())
            renderer.grown = previous.blocks.last();
        BlockPtr block =
            renderer.render(raw, state, (options.live || options.clipped) && k == raws.size() - 1);
        renderer.grown = nullptr;
        // Past the budget the rest is plain text, from this block's start;
        // the incomplete block is dropped.
        if (renderer.exhausted()) {
            doc.rest = doc.text.mid(offsets.value(raw.from, at));
            break;
        }
        doc.blocks << block;
        doc.starts << offsets.value(raw.from, at);
    }
    // Headings before the last block: where a later parse resumes.
    doc.headings = 0;
    for (qsizetype k = 0; k + 1 < doc.blocks.size(); ++k)
        doc.headings += doc.blocks.at(k)->kind == Kind::Heading;
    return doc;
}
} // namespace markdown
