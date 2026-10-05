#include "highlight.h"

#include <QSet>

#include <algorithm>

// A port of openghost/highlight.js: one scan per source, a token at a time,
// in the same order its regular expression tries alternatives.
namespace highlight
{
namespace
{
using markdown::Run;

thread_local qint64 t_work = 0;

// Every character a scan looks at goes through at() or find(), so work()
// counts them.
QChar at(const QString &s, qsizetype i)
{
    ++t_work;
    return s.at(i);
}

template <typename Needle> qsizetype find(const QString &s, Needle needle, qsizetype from)
{
    const qsizetype found = s.indexOf(needle, from);
    t_work += std::max<qsizetype>(0, (found < 0 ? s.size() : found) - from);
    return found;
}

// A resume point needs this many characters after it: no decision before a
// line start looks further ahead (a word's "(" lookahead, "<!--").
constexpr qsizetype Ahead = 8;

bool resumable(const QString &s, qsizetype i)
{
    return i > 0 && i + Ahead <= s.size() && s.at(i - 1) == QLatin1Char('\n');
}

QSet<QString> words(const char *list)
{
    QSet<QString> out;
    for (const auto &word : QString::fromLatin1(list).split(QLatin1Char(' '), Qt::SkipEmptyParts))
        out.insert(word);
    return out;
}

const QSet<QString> &keywords()
{
    static const QSet<QString> set = words(
        "abstract and as assert async await break case catch class const constructor continue def "
        "default defer del delete do done elif else end enum esac except export extends extern fi "
        "final finally fn for foreach from func function get go goto if impl implements import in "
        "instanceof interface is lambda let loop match mod module mut namespace new nil not of "
        "operator or override package pass private protected pub public raise readonly ref require "
        "return select set static struct super switch then throw throws trait try type typeof "
        "unless unsafe until use using var virtual when where while with yield echo local begin "
        "rescue ensure self Self this true false True False None null undefined NULL nullptr void");
    return set;
}

const QSet<QString> &sqlWords()
{
    static const QSet<QString> set = words(
        "select from where insert into update delete create alter drop table index view values set "
        "join left right inner outer full cross on group by order having limit offset as distinct "
        "union all and or not null is in exists like between case when then else end primary key "
        "foreign references default unique constraint returning with asc desc count sum avg min "
        "max");
    return set;
}

const QSet<QString> &types()
{
    static const QSet<QString> set = words(
        "int integer float double long short char byte bool boolean string str number bigint "
        "symbol object any unknown never usize isize u8 u16 u32 u64 u128 i8 i16 i32 i64 i128 f32 "
        "f64 uint list dict tuple set map vec option result");
    return set;
}

const QSet<QString> &hashLangs()
{
    static const QSet<QString> set = words(
        "py python rb ruby sh bash shell zsh fish console yaml yml toml r perl pl ps1 powershell "
        "pwsh dockerfile docker makefile make ini conf nginx elixir ex julia nim cmake coffee "
        "graphql gql tcl properties env gitignore");
    return set;
}

const QSet<QString> &dashLangs()
{
    static const QSet<QString> set =
        words("sql mysql postgres postgresql psql sqlite plsql lua haskell hs elm ada");
    return set;
}

const QSet<QString> &markupLangs()
{
    static const QSet<QString> set = words("html xml svg xhtml vue svelte astro plist");
    return set;
}

const QSet<QString> &plainLangs()
{
    static const QSet<QString> set = words("text txt plain plaintext md markdown log output csv");
    return set;
}

bool isWord(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_';
}

bool isDigit(QChar c) { return c.unicode() >= '0' && c.unicode() <= '9'; }

bool isHex(QChar c)
{
    const ushort u = c.unicode();
    return isDigit(c) || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
}

// JavaScript's \s.
bool isJsSpace(QChar c)
{
    const ushort u = c.unicode();
    return c.isSpace() || u == 0xfeff;
}

class Runs
{
  public:
    void add(qsizetype start, qsizetype length, Token token)
    {
        if (length <= 0 || token == Plain)
            return;
        if (!runs.isEmpty() && runs.last().format == token &&
            runs.last().start + runs.last().length == start) {
            runs.last().length += int(length);
            return;
        }
        runs << Run{int(start), int(length), quint8(token), -1};
    }
    QVector<Run> runs;
};

enum class Family { Hash, Dash, Slash };

qsizetype comment(const QString &s, qsizetype i, Family family)
{
    const qsizetype n = s.size();
    auto toLineEnd = [&](qsizetype from) {
        const qsizetype end = find(s, QLatin1Char('\n'), from);
        return (end < 0 ? n : end) - i;
    };
    auto block = [&]() {
        const qsizetype end = find(s, QLatin1String("*/"), i + 2);
        return (end < 0 ? n : end + 2) - i;
    };
    const QChar c = at(s, i), next = i + 1 < n ? at(s, i + 1) : QChar();
    switch (family) {
    case Family::Hash:
        return c == QLatin1Char('#') ? toLineEnd(i) : 0;
    case Family::Dash:
        if (c == QLatin1Char('-') && next == QLatin1Char('-'))
            return toLineEnd(i);
        return c == QLatin1Char('/') && next == QLatin1Char('*') ? block() : 0;
    case Family::Slash:
        if (c == QLatin1Char('/') && next == QLatin1Char('/'))
            return toLineEnd(i);
        return c == QLatin1Char('/') && next == QLatin1Char('*') ? block() : 0;
    }
    return 0;
}

qsizetype string(const QString &s, qsizetype i)
{
    const qsizetype n = s.size();
    const QChar quote = at(s, i);
    if (quote == QLatin1Char('`')) {
        qsizetype j = i + 1;
        while (j < n) {
            if (at(s, j) == QLatin1Char('\\') && j + 1 < n)
                j += 2;
            else if (at(s, j) == QLatin1Char('\\'))
                break;
            else if (at(s, j) == quote)
                return j + 1 - i;
            else
                j++;
        }
        return j - i;
    }
    if (quote != QLatin1Char('"') && quote != QLatin1Char('\''))
        return 0;
    qsizetype j = i + 1;
    while (j < n) {
        const QChar c = at(s, j);
        if (c == QLatin1Char('\\')) {
            if (j + 1 < n && at(s, j + 1) != QLatin1Char('\n') && at(s, j + 1) != QLatin1Char('\r'))
                j += 2;
            else
                break;
        } else if (c == quote) {
            return j + 1 - i;
        } else if (c == QLatin1Char('\n')) {
            break;
        } else {
            j++;
        }
    }
    return j - i;
}

// \b(?:0[xX][\da-fA-F_]+|0[bB][01_]+|\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d+)?)\b
qsizetype number(const QString &s, qsizetype i)
{
    const qsizetype n = s.size();
    if (!isDigit(at(s, i)) || (i > 0 && isWord(at(s, i - 1))))
        return 0;
    auto boundary = [&](qsizetype end) { return end >= n || !isWord(at(s, end)); };
    if (at(s, i) == QLatin1Char('0') && i + 1 < n) {
        const QChar x = at(s, i + 1);
        if (x == QLatin1Char('x') || x == QLatin1Char('X') || x == QLatin1Char('b') ||
            x == QLatin1Char('B')) {
            const bool hex = x == QLatin1Char('x') || x == QLatin1Char('X');
            auto digit = [hex](QChar d) {
                return d == QLatin1Char('_') ||
                       (hex ? isHex(d) : d == QLatin1Char('0') || d == QLatin1Char('1'));
            };
            qsizetype j = i + 2;
            while (j < n && digit(at(s, j)))
                j++;
            if (j > i + 2 && boundary(j))
                return j - i;
        }
    }
    qsizetype whole = i + 1;
    while (whole < n && (isDigit(at(s, whole)) || at(s, whole) == QLatin1Char('_')))
        whole++;
    qsizetype fraction = -1;
    if (whole + 1 < n && at(s, whole) == QLatin1Char('.') && isDigit(at(s, whole + 1))) {
        fraction = whole + 2;
        while (fraction < n && (isDigit(at(s, fraction)) || at(s, fraction) == QLatin1Char('_')))
            fraction++;
    }
    auto exponent = [&](qsizetype from) -> qsizetype {
        if (from >= n || (at(s, from) != QLatin1Char('e') && at(s, from) != QLatin1Char('E')))
            return -1;
        qsizetype j = from + 1;
        if (j < n && (at(s, j) == QLatin1Char('+') || at(s, j) == QLatin1Char('-')))
            j++;
        const qsizetype digits = j;
        while (j < n && isDigit(at(s, j)))
            j++;
        return j > digits ? j : -1;
    };
    for (const qsizetype base : {fraction, whole}) {
        if (base < 0)
            continue;
        const qsizetype e = exponent(base);
        if (e >= 0 && boundary(e))
            return e - i;
        if (boundary(base))
            return base - i;
    }
    return 0;
}

bool upperType(const QString &word)
{
    // /^[A-Z][a-z0-9]\w*$/
    if (word.size() < 2 || word.at(0).unicode() < 'A' || word.at(0).unicode() > 'Z')
        return false;
    const ushort second = word.at(1).unicode();
    return (second >= 'a' && second <= 'z') || (second >= '0' && second <= '9');
}

// Each scan starts at `i` (0, or a resume point of a source this one
// extends) and records its own last resume point in `resume`.
void source(const QString &s, Family family, bool sql, Runs &out, qsizetype i, int &resume)
{
    const qsizetype n = s.size();
    while (i < n) {
        if (resumable(s, i))
            resume = int(i);
        if (const qsizetype length = comment(s, i, family)) {
            out.add(i, length, Comment);
            i += length;
            continue;
        }
        if (const qsizetype length = string(s, i)) {
            out.add(i, length, String);
            i += length;
            continue;
        }
        if (const qsizetype length = number(s, i)) {
            out.add(i, length, Number);
            i += length;
            continue;
        }
        const QChar c = at(s, i);
        if ((c.unicode() < 128 && c.isLetter()) || c == QLatin1Char('_') || c == QLatin1Char('$')) {
            qsizetype j = i + 1;
            while (j < n && (isWord(at(s, j)) || at(s, j) == QLatin1Char('$')))
                j++;
            const QString word = s.mid(i, j - i), lower = word.toLower();
            Token token = Plain;
            if (sql ? sqlWords().contains(lower) : keywords().contains(word)) {
                token = Keyword;
            } else if (types().contains(lower) || upperType(word)) {
                token = Type;
            } else {
                qsizetype k = j;
                while (k < n && k < j + 4 && isJsSpace(at(s, k)) && at(s, k) != QLatin1Char('('))
                    k++;
                if (k < n && k < j + 4 && at(s, k) == QLatin1Char('('))
                    token = Function;
                else if (i > 0 &&
                         (at(s, i - 1) == QLatin1Char('.') || at(s, i - 1) == QLatin1Char('@')))
                    token = Property;
            }
            out.add(i, j - i, token);
            i = j;
            continue;
        }
        i++;
    }
}

void markup(const QString &s, Runs &out, qsizetype i, int &resume)
{
    const qsizetype n = s.size();
    while (i < n) {
        if (resumable(s, i))
            resume = int(i);
        if (at(s, i) != QLatin1Char('<')) {
            i++;
            continue;
        }
        if (s.mid(i, 4) == QLatin1String("<!--")) {
            const qsizetype end = find(s, QLatin1String("-->"), i + 4);
            const qsizetype stop = end < 0 ? n : end + 3;
            out.add(i, stop - i, Comment);
            i = stop;
            continue;
        }
        qsizetype j = i + 1;
        if (j < n && at(s, j) == QLatin1Char('/'))
            j++;
        const QChar first = j < n ? at(s, j) : QChar();
        if (!((first.unicode() < 128 && first.isLetter()) || first == QLatin1Char('!') ||
              first == QLatin1Char('?'))) {
            i++;
            continue;
        }
        const qsizetype close = find(s, QLatin1Char('>'), j);
        const qsizetype end = close < 0 ? n : close + 1; // The whole tag.
        qsizetype name = j;
        while (name < end && !isJsSpace(at(s, name)) && at(s, name) != QLatin1Char('>') &&
               at(s, name) != QLatin1Char('/'))
            name++;
        out.add(j, name - j, Keyword);
        // The attributes stop before a closing "/>" or ">".
        qsizetype stop = end;
        if (close >= 0) {
            stop = close;
            if (stop > name && at(s, stop - 1) == QLatin1Char('/'))
                stop--;
        }
        // `word`: the end of a run of neither space nor "=" found not to
        // end in "=": no attribute name starts inside it, so it is not
        // scanned again from each of its characters (audit P5-10).
        const auto quote = [](QChar c) { return c == QLatin1Char('"') || c == QLatin1Char('\''); };
        for (qsizetype k = name, word = name; k < stop;) {
            const QChar c = at(s, k);
            if (quote(c)) {
                qsizetype q = k + 1;
                while (q < stop && at(s, q) != c)
                    q++;
                const qsizetype after = q < stop ? q + 1 : stop;
                out.add(k, after - k, String);
                k = after;
                continue;
            }
            qsizetype e = std::max(k, word);
            while (e < stop && !isJsSpace(at(s, e)) && at(s, e) != QLatin1Char('='))
                e++;
            if (e > k && e < stop && at(s, e) == QLatin1Char('=')) {
                out.add(k, e - k, Property);
                k = e;
                continue;
            }
            // Only a quote inside the run can start anything before its end.
            word = e;
            for (k++; k < e && !quote(at(s, k));)
                k++;
        }
        i = end;
    }
}

void diff(const QString &s, Runs &out, qsizetype start, int &resume)
{
    while (start <= s.size()) {
        if (resumable(s, start))
            resume = int(start);
        qsizetype end = find(s, QLatin1Char('\n'), start);
        if (end < 0)
            end = s.size();
        const QStringView line = QStringView(s).mid(start, end - start);
        if (line.startsWith(QLatin1String("+++")) || line.startsWith(QLatin1String("---")) ||
            line.startsWith(QLatin1String("@@")))
            out.add(start, line.size(), Comment);
        else if (line.startsWith(QLatin1Char('+')))
            out.add(start, line.size(), Inserted);
        else if (line.startsWith(QLatin1Char('-')))
            out.add(start, line.size(), Deleted);
        start = end + 1;
    }
}

bool arrowChar(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x2190 && u <= 0x21ff) || (u >= 0x25b2 && u <= 0x25c5) || u == 0x25ba ||
           u == 0x25bc || (u >= 0x27f5 && u <= 0x27ff);
}

bool boxChar(QChar c) { return c.unicode() >= 0x2500 && c.unicode() <= 0x259f; }

// markdown.js ART_ARROWS at i: the arrow's length, or 0.
qsizetype arrow(const QString &s, qsizetype i)
{
    const qsizetype n = s.size();
    qsizetype j = i;
    while (j < n && arrowChar(at(s, j)))
        j++;
    if (j > i)
        return j - i;
    auto dashes = [&](qsizetype from) {
        qsizetype k = from;
        while (k < n && at(s, k) == QLatin1Char('-'))
            k++;
        return k;
    };
    const QChar c = at(s, i);
    if (c == QLatin1Char('<') || c == QLatin1Char('-')) {
        const qsizetype from = c == QLatin1Char('<') ? i + 1 : i;
        const qsizetype k = dashes(from);
        if (k > from && k < n && at(s, k) == QLatin1Char('>'))
            return k + 1 - i;
        if (c == QLatin1Char('<') && k > from)
            return k - i;
    }
    if (c == QLatin1Char('=')) {
        qsizetype k = i;
        while (k < n && at(s, k) == QLatin1Char('='))
            k++;
        if (k < n && at(s, k) == QLatin1Char('>'))
            return k + 1 - i;
    }
    return 0;
}

// Where a scan of `text` continues from `previous`: its resume point, or 0.
qsizetype resumeAt(const Resumable &previous, const QString &text)
{
    return previous.resume > 0 && previous.resume <= text.size() ? previous.resume : 0;
}

// The runs of `previous` before `from`, where the scan continues.
Runs kept(const Resumable &previous, qsizetype from)
{
    Runs out;
    for (const Run &run : previous.runs) {
        if (run.start >= from)
            break;
        out.runs << run;
        out.runs.last().length = int(std::min<qsizetype>(run.length, from - run.start));
    }
    return out;
}
} // namespace

qint64 work() { return t_work; }

Resumable code(const QString &text, const QString &language, const Resumable &previous)
{
    const QString lang = language.toLower();
    if (lang.isEmpty() || plainLangs().contains(lang))
        return {};
    const qsizetype from = resumeAt(previous, text);
    Runs out = kept(previous, from);
    int resume = int(from);
    if (lang == QLatin1String("diff") || lang == QLatin1String("patch"))
        diff(text, out, from, resume);
    else if (markupLangs().contains(lang))
        markup(text, out, from, resume);
    else
        source(text,
               hashLangs().contains(lang)   ? Family::Hash
               : dashLangs().contains(lang) ? Family::Dash
                                            : Family::Slash,
               dashLangs().contains(lang) && lang != QLatin1String("lua"), out, from, resume);
    return {out.runs, resume};
}

QVector<Run> code(const QString &text, const QString &lang) { return code(text, lang, {}).runs; }

Resumable art(const QString &text, const Resumable &previous)
{
    qsizetype i = resumeAt(previous, text);
    Runs out = kept(previous, i);
    const qsizetype n = text.size();
    int resume = int(i);
    while (i < n) {
        if (resumable(text, i))
            resume = int(i);
        if (const qsizetype length = arrow(text, i)) {
            out.add(i, length, ArtArrow);
            i += length;
            continue;
        }
        if (boxChar(at(text, i))) {
            qsizetype j = i + 1;
            while (j < n && boxChar(at(text, j)) && !arrow(text, j))
                j++;
            out.add(i, j - i, ArtLine);
            i = j;
            continue;
        }
        // No arrow starts later in a run of "-" or "=" either: each would
        // end where this one did (audit P5-10).
        if (const QChar c = at(text, i); c == QLatin1Char('-') || c == QLatin1Char('=')) {
            while (i < n && at(text, i) == c)
                i++;
            continue;
        }
        i++;
    }
    return {out.runs, resume};
}

QVector<Run> art(const QString &text) { return art(text, {}).runs; }
} // namespace highlight
