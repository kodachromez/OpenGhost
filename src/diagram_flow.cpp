#include "diagram_engine.h"

#include <QSet>

#include <algorithm>
#include <deque>

// Flowcharts, state, ER and class diagrams: parsing, the layered layout and its scene
// (diagram.js parseFlow … flowScene).
namespace diagram
{
namespace
{
/* Array.prototype.sort */

// V8's sort (TimSort): a list shorter than 64 is one run made by binary insertion, which this
// follows step for step, so a comparator that is not consistent (a hint missing on one side)
// orders as upstream's does. Longer lists are merged stably, the same for consistent ones.
template <typename T, typename Cmp> void jsSort(QVector<T> &a, Cmp cmp)
{
    const int n = int(a.size());
    if (n < 2)
        return;
    if (n < 64) {
        int run = 2;
        const bool descending = cmp(a[1], a[0]) < 0;
        for (int i = 2; i < n; ++i) {
            const double order = cmp(a[i], a[i - 1]);
            if (descending ? order >= 0 : order < 0)
                break;
            ++run;
        }
        if (descending)
            std::reverse(a.begin(), a.begin() + run);
        for (int start = run; start < n; ++start) {
            const T pivot = a[start];
            int left = 0, right = start;
            while (left < right) {
                const int mid = left + ((right - left) >> 1);
                if (cmp(pivot, a[mid]) < 0)
                    right = mid;
                else
                    left = mid + 1;
            }
            for (int p = start; p > left; --p)
                a[p] = a[p - 1];
            a[left] = pivot;
        }
        return;
    }
    QVector<T> buf(n);
    for (int width = 1; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            const int mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                buf[k++] = cmp(a[j], a[i]) < 0 ? a[j++] : a[i++];
            while (i < mid)
                buf[k++] = a[i++];
            while (j < hi)
                buf[k++] = a[j++];
        }
        std::swap(a, buf);
    }
}

double signOf(double v) { return v > 0 ? 1 : v < 0 ? -1 : v; }

bool lying(const QString &way) { return way == QLatin1String("LR") || way == QLatin1String("RL"); }

QString upright(const QString &way)
{
    const QString d = way.toUpper();
    return d == QLatin1String("TB") ? QStringLiteral("TD") : d;
}

// JSON.stringify of a string, for a card's key.
QString jsonString(const QString &s)
{
    QString out = QStringLiteral("\"");
    for (int i = 0; i < s.size(); ++i) {
        const QChar ch = s.at(i);
        const ushort u = ch.unicode();
        if (ch == QLatin1Char('"'))
            out += QLatin1String("\\\"");
        else if (ch == QLatin1Char('\\'))
            out += QLatin1String("\\\\");
        else if (u == '\b')
            out += QLatin1String("\\b");
        else if (u == '\f')
            out += QLatin1String("\\f");
        else if (u == '\n')
            out += QLatin1String("\\n");
        else if (u == '\r')
            out += QLatin1String("\\r");
        else if (u == '\t')
            out += QLatin1String("\\t");
        else if (u < 0x20 ||
                 (ch.isHighSurrogate() && !(i + 1 < s.size() && s.at(i + 1).isLowSurrogate())) ||
                 (ch.isLowSurrogate() && !(i > 0 && s.at(i - 1).isHighSurrogate())))
            out += QStringLiteral("\\u%1").arg(u, 4, 16, QLatin1Char('0'));
        else
            out += ch;
    }
    return out + QLatin1Char('"');
}

/* Parsing */

// A block may open with its name and go on with what it does (headed).
struct Parts {
    QString head, detail;
    bool soft = false;
};

struct Level;

struct Node {
    QString id, label, shape = QStringLiteral("rect");
    std::optional<QString> raw; // The label as written, when one was.
    int seq = -1;               // -1: not yet given (`seq ??=`).
    bool grouped = false;       // `group !== undefined`.
    QString group;
    std::optional<Parts> parts;
    std::optional<QString> text; // What is edited, when not the label.
    QStringList lines;
    int head = 0;
    double w = 0, h = 0;
    std::shared_ptr<const Card> card;
    Level *cluster = nullptr; // A group standing as a block in its parent's layout.
    // An entity's or a class's members while parsing.
    QVector<CardRow> attrRows;
    QStringList attrs, methods;
    QString stereo;
};

struct Edge {
    QString from, to, label, style = QStringLiteral("solid"), head = QStringLiteral("none");
    bool both = false, lift = false;
    bool hasEnds = false;
    QString endStart, endEnd; // `ends`; empty is null.
};

struct Group {
    QString id, title, dir, parent;
    int seq = 0;
};

struct Graph {
    QString dir = QStringLiteral("TD"), title;
    QVector<Node> nodes;
    QVector<Edge> edges;
    QVector<Group> groups;
};

// graphBuilder: nodes in the order first named, a Map of id to node.
class Builder
{
  public:
    explicit Builder(Ctx &c) : ctx(c) {}
    // The node's index, or -1 past the bound on nodes.
    int touch(const QString &id, const QString *label = nullptr, const QString &shape = {})
    {
        int at = index.value(id, -1);
        if (at < 0) {
            if (nodes.size() >= MaxNodes) {
                ctx.fail(QStringLiteral("This diagram has more than %1 nodes.").arg(MaxNodes));
                return -1;
            }
            Node node;
            node.id = node.label = id;
            at = int(nodes.size());
            nodes << node;
            index.insert(id, at);
        }
        if (label) {
            const QString text = cleanLabel(*label);
            Node &node = nodes[at];
            node.label = text.isEmpty() ? QStringLiteral(" ") : text;
            node.shape = shape;
            node.raw = *label;
        }
        return at;
    }
    bool edge(const Edge &e)
    {
        if (edges.size() >= MaxEdges)
            return ctx.fail(QStringLiteral("This diagram has more than %1 links.").arg(MaxEdges));
        edges << e;
        return true;
    }
    void remove(const QString &id)
    {
        const int at = index.value(id, -1);
        if (at < 0)
            return;
        nodes.remove(at);
        index.clear();
        for (int i = 0; i < nodes.size(); ++i)
            index.insert(nodes.at(i).id, i);
    }
    Ctx &ctx;
    QVector<Node> nodes;
    QVector<Edge> edges;
    QHash<QString, int> index;
};

// The groups of a scheme as upstream holds them: objects in a Map by id, a later one of the
// same id taking the earlier one's place; a stack refers to the objects themselves.
struct GroupMap {
    QVector<Group> objects;
    QVector<int> order; // Objects in the Map, in its order.
    QHash<QString, int> slot;
    bool has(const QString &id) const { return slot.contains(id); }
    int size() const { return int(order.size()); }
    int set(const Group &g)
    {
        objects << g;
        const int object = int(objects.size()) - 1;
        const int at = slot.value(g.id, -1);
        if (at >= 0) {
            order[at] = object;
        } else {
            slot.insert(g.id, int(order.size()));
            order << object;
        }
        return object;
    }
    QVector<Group> values() const
    {
        QVector<Group> out;
        for (const int object : order)
            out << objects.at(object);
        return out;
    }
};

const QSet<QString> &headedShapes()
{
    // HEADED: the shapes a name and its detail are set in.
    static const QSet<QString> set{QStringLiteral("rect"), QStringLiteral("round"),
                                   QStringLiteral("stadium"), QStringLiteral("subroutine"),
                                   QStringLiteral("cylinder")};
    return set;
}

// PATH_SHAPES: the shapes drawn as a path rather than a rect (the painter's).
[[maybe_unused]] const char *const PathShapes[] = {"diamond", "hexagon", "lean", "flag",
                                                   "cylinder"};

// **Name** and the rest, or Name: and the rest: the name set strong and the rest quiet under
// it. `soft` marks the two written on one line: they part only when the line breaks anyway.
std::optional<Parts> headed(const std::optional<QString> &raw)
{
    static const Re quoted(QStringLiteral("^\"([\\s\\S]*)\"$")),
        broken(QStringLiteral("<br\\s*/?>|\\\\n|\\n"), I),
        strong(
            QStringLiteral("^\\*\\*([^*]+)\\*\\*[\\s:\u2014\u2013-]*(?:(?:<br\\s*/?>|\\\\n)\\s*)*"
                           "([\\s\\S]+)$"),
            I),
        named(QStringLiteral(
                  "^([^:<\\\\\\n*]{2,36}):\\s*(?:(?:<br\\s*/?>|\\\\n)\\s*)*(\\S[\\s\\S]*)$"),
              I),
        comment(QStringLiteral("^//")), digitStart(QStringLiteral("^\\d")),
        digitEnd(QStringLiteral("\\d$"));
    QString text = raw.value_or(QString()).trimmed();
    const auto q = quoted.match(text);
    if (q.hasMatch())
        text = q.captured(1);
    text = text.trimmed();
    auto m = strong.match(text);
    if (m.hasMatch())
        return Parts{cleanLabel(m.captured(1)), cleanLabel(m.captured(2)), false};
    m = named.match(text);
    if (!m.hasMatch() || comment.match(m.captured(2)).hasMatch() ||
        (digitStart.match(m.captured(2)).hasMatch() && digitEnd.match(m.captured(1)).hasMatch()))
        return std::nullopt;
    return Parts{cleanLabel(m.captured(1)), cleanLabel(m.captured(2)),
                 !broken.match(text).hasMatch()};
}

struct ShapeDef {
    const char *open;
    const char *closers[2];
    const char *kind;
};
// SHAPES: how a block's label is bracketed, longest first.
const ShapeDef Shapes[] = {
    {"(((", {")))", nullptr}, "circle"}, {"((", {"))", nullptr}, "circle"},
    {"([", {"])", nullptr}, "stadium"},  {"[[", {"]]", nullptr}, "subroutine"},
    {"[(", {")]", nullptr}, "cylinder"}, {"[/", {"/]", "\\]"}, "lean"},
    {"[\\", {"\\]", "/]"}, "lean"},      {"{{", {"}}", nullptr}, "hexagon"},
    {"[", {"]", nullptr}, "rect"},       {"(", {")", nullptr}, "round"},
    {"{", {"}", nullptr}, "diamond"},    {">", {"]", nullptr}, "flag"},
};

struct Cursor {
    QString s;
    int p = 0;
};

QPair<int, QString> closing(const QString &s, int start, const ShapeDef &shape)
{
    const QString open = QLatin1String(shape.open);
    if (open.size() == 1 && qstrlen(shape.closers[0]) == 1) {
        // One bracket: nested ones and quoted text are skipped.
        const QChar close = QLatin1Char(shape.closers[0][0]);
        int depth = 0;
        for (int i = start; i < s.size(); ++i) {
            if (s.at(i) == QLatin1Char('"')) {
                const int q = int(s.indexOf(QLatin1Char('"'), i + 1));
                if (q > 0) {
                    i = q;
                    continue;
                }
            }
            if (s.at(i) == open.at(0))
                ++depth;
            else if (s.at(i) == close && !depth--)
                return {i, QString(close)};
        }
        return {-1, {}};
    }
    int best = -1;
    QString match;
    for (const char *closer : shape.closers) {
        if (!closer)
            continue;
        const QString close = QLatin1String(closer);
        const int at = int(s.indexOf(close, start));
        if (at >= 0 && (best < 0 || at < best)) {
            best = at;
            match = close;
        }
    }
    return {best, match};
}

QRegularExpressionMatch matchAt(const Re &re, const QString &s, int p)
{
    return re.match(s, p, Re::NormalMatch, Re::AnchorAtOffsetMatchOption);
}

int readNode(Cursor &cursor, Builder &graph)
{
    // ID: letters, digits and _, with . or - between them.
    static const Re id(
        QStringLiteral("[\\p{L}\\p{N}_](?:[\\p{L}\\p{N}_]|[.\\-](?=[\\p{L}\\p{N}_]))*"));
    static const Re cls(QStringLiteral(":::[\\w-]+"));
    const auto m = matchAt(id, cursor.s, cursor.p);
    if (!m.hasMatch())
        return -1;
    const QString name = m.captured(0);
    cursor.p += int(name.size());
    QString label, shape;
    bool labelled = false;
    for (const auto &def : Shapes) {
        const QString open = QLatin1String(def.open);
        if (!QStringView(cursor.s).mid(cursor.p).startsWith(open))
            continue;
        const int start = cursor.p + int(open.size());
        const auto [end, close] = closing(cursor.s, start, def);
        if (end < 0)
            continue;
        label = cursor.s.mid(start, end - start);
        shape = QLatin1String(def.kind);
        labelled = true;
        cursor.p = end + int(close.size());
        break;
    }
    const auto k = matchAt(cls, cursor.s, cursor.p);
    if (k.hasMatch())
        cursor.p += int(k.capturedLength());
    return graph.touch(name, labelled ? &label : nullptr, shape);
}

void skipSpace(Cursor &cursor)
{
    while (cursor.p < cursor.s.size() && cursor.s.at(cursor.p).isSpace())
        ++cursor.p;
}

// A & B & C: several blocks on one end of a link. Empty: none read (or the bound on nodes).
QVector<int> readGroup(Cursor &cursor, Builder &graph)
{
    const int first = readNode(cursor, graph);
    if (first < 0)
        return {};
    QVector<int> group{first};
    for (;;) {
        const int save = cursor.p;
        skipSpace(cursor);
        if (cursor.p >= cursor.s.size() || cursor.s.at(cursor.p) != QLatin1Char('&')) {
            cursor.p = save;
            break;
        }
        ++cursor.p;
        skipSpace(cursor);
        const int next = readNode(cursor, graph);
        if (next < 0)
            break;
        group << next;
    }
    return group;
}

std::optional<Edge> readLink(Cursor &cursor)
{
    // LINK_TEXT (-- text -->) and LINK (-->|text|).
    static const Re text(
        QStringLiteral("(<)?(--|==|-\\.)[ \\t]+(.+?)[ "
                       "\\t]+(-{2,}>|={2,}>|\\.-+>|-{3,}|={3,}|\\.-+|--[ox]|==[ox])"));
    static const Re plain(QStringLiteral("(<)?(-{2,}>|={2,}>|-\\.+->|-{3,}|={3,}|-\\.+-|--[ox]|==["
                                         "ox]|~{3,})(?:[ \\t]*\\|([^|]*)\\|)?"));
    auto m = matchAt(text, cursor.s, cursor.p);
    QString token, labelText;
    bool both = false;
    if (m.hasMatch()) {
        both = m.capturedStart(1) >= 0;
        token = m.captured(2) + m.captured(4);
        labelText = m.captured(3);
    } else {
        m = matchAt(plain, cursor.s, cursor.p);
        if (!m.hasMatch())
            return std::nullopt;
        both = m.capturedStart(1) >= 0;
        token = m.captured(2);
        labelText = m.captured(3);
    }
    cursor.p += int(m.capturedLength());
    Edge e;
    e.label = cleanLabel(labelText);
    e.style = token.contains(QLatin1Char('~'))   ? QStringLiteral("hidden")
              : token.contains(QLatin1Char('.')) ? QStringLiteral("dotted")
              : token.contains(QLatin1Char('=')) ? QStringLiteral("thick")
                                                 : QStringLiteral("solid");
    e.head = token.endsWith(QLatin1Char('>'))   ? QStringLiteral("arrow")
             : token.endsWith(QLatin1Char('o')) ? QStringLiteral("circle")
             : token.endsWith(QLatin1Char('x')) ? QStringLiteral("cross")
                                                : QStringLiteral("none");
    e.both = both;
    return e;
}

Group subgraphOf(QString text, int index, const Group *parent)
{
    static const Re cls(QStringLiteral(":::[\\w-]+[ \\t]*$")),
        titled(QStringLiteral("^([^\\s\"[\\]]+)[ \\t]*\\[([\\s\\S]*)\\]$")),
        plain(QStringLiteral("^[\\p{L}\\p{N}_][\\p{L}\\p{N}_.\\-]*$"));
    text.remove(cls);
    text = text.trimmed();
    const auto m = titled.match(text);
    const QString id = m.hasMatch()                   ? m.captured(1)
                       : plain.match(text).hasMatch() ? text
                                                      : QString();
    Group g;
    g.id = id.isEmpty() ? QStringLiteral("__g%1").arg(index) : id;
    g.title = cleanLabel(m.hasMatch() ? m.captured(2) : text);
    g.parent = parent ? parent->id : QString();
    return g;
}

// The group a stack's top stands for: the nearest one kept (-1 marks one past the bounds).
int topOf(const QVector<int> &stack)
{
    for (int i = int(stack.size()) - 1; i >= 0; --i) {
        if (stack.at(i) >= 0)
            return stack.at(i);
    }
    return -1;
}

std::optional<Graph> parseFlow(Ctx &c, const QStringList &lines)
{
    static const Re header(QStringLiteral("^(?:graph|flowchart)[ \\t]+(TB|TD|BT|RL|LR)"), I),
        subgraph(QStringLiteral("^subgraph\\b[ \\t]*(.*)$"), I), end(QStringLiteral("^end\\b"), I),
        direction(QStringLiteral("^direction[ \\t]+(TB|TD|BT|RL|LR)\\b"), I),
        // SKIP.
        skip(QStringLiteral("^(classDef|class|style|linkStyle|click|direction|accTitle|accDescr|"
                            "end|note|%%)\\b"),
             I),
        starred(QStringLiteral("^\\s*\"?\\s*\\*\\*"));
    const auto h = header.match(lines.value(0));
    const QString dir = h.hasMatch() ? h.captured(1).toUpper() : QStringLiteral("TD");
    Builder graph(c);
    GroupMap groups;
    QVector<int> stack; // Group objects; -1 for a subgraph past the bounds.
    int seq = 0;
    const auto claim = [&](const QVector<int> &ids) {
        const int group = topOf(stack);
        for (const int id : ids) {
            Node &node = graph.nodes[id];
            if (node.seq < 0)
                node.seq = seq++;
            if (group >= 0 && !node.grouped) {
                node.grouped = true;
                node.group = groups.objects.at(group).id;
            }
        }
    };
    for (int li = 1; li < lines.size() && c.error.isEmpty(); ++li) {
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if ((m = subgraph.match(line)).hasMatch()) {
            if (groups.size() >= MaxGroups || stack.size() >= MaxGroupDepth) {
                stack << -1;
                continue;
            }
            const int parent = topOf(stack);
            Group group = subgraphOf(m.captured(1), groups.size(),
                                     parent >= 0 ? &groups.objects.at(parent) : nullptr);
            if (groups.has(group.id))
                group.id = QStringLiteral("%1__%2").arg(group.id).arg(groups.size());
            group.seq = seq++;
            stack << groups.set(group);
            continue;
        }
        if (end.match(line).hasMatch()) {
            if (!stack.isEmpty())
                stack.removeLast();
            continue;
        }
        if ((m = direction.match(line)).hasMatch()) {
            if (!stack.isEmpty() && stack.last() >= 0)
                groups.objects[stack.last()].dir = upright(m.captured(1));
            continue;
        }
        if (skip.match(line).hasMatch())
            continue;
        for (const auto &part : splitStatements(line)) {
            Cursor cursor{part.trimmed(), 0};
            QVector<int> left = readGroup(cursor, graph);
            if (!left.isEmpty())
                claim(left);
            while (!left.isEmpty() && c.error.isEmpty()) {
                skipSpace(cursor);
                const auto link = readLink(cursor);
                if (!link)
                    break;
                skipSpace(cursor);
                const QVector<int> right = readGroup(cursor, graph);
                if (right.isEmpty())
                    break;
                claim(right);
                for (const int from : left) {
                    for (const int to : right) {
                        Edge e = *link;
                        e.from = graph.nodes.at(from).id;
                        e.to = graph.nodes.at(to).id;
                        if (!graph.edge(e))
                            break;
                    }
                }
                left = right;
            }
            if (!c.error.isEmpty())
                break;
        }
    }
    if (!c.error.isEmpty())
        return std::nullopt;
    for (const int object : groups.order)
        graph.remove(groups.objects.at(object).id);
    if (graph.nodes.isEmpty() && !groups.size())
        return std::nullopt;
    for (auto &node : graph.nodes) {
        if (!headedShapes().contains(node.shape))
            continue;
        node.parts = headed(node.raw);
        // A name marked with stars is edited with them, so the mark is kept.
        if (node.parts && starred.match(node.raw.value_or(QString())).hasMatch())
            node.text = QStringLiteral("**%1**\n%2").arg(node.parts->head, node.parts->detail);
    }
    Graph out;
    out.dir = dir == QLatin1String("TB") ? QStringLiteral("TD") : dir;
    out.nodes = graph.nodes;
    out.edges = graph.edges;
    out.groups = groups.values();
    return out;
}

// States and the moves between them. A state may hold states of its own, in braces after its
// name: it is drawn as a group, and the start and the end written inside it are its own.
std::optional<Graph> parseState(Ctx &c, const QStringList &lines)
{
    static const Re opensRe(QStringLiteral("\\{\\s*$")), brace(QStringLiteral("[{}]\\s*$")),
        endNote(QStringLiteral("^end\\s+note\\b"), I), noteRe(QStringLiteral("^note\\b"), I),
        ignore(QStringLiteral("^(--|%%|classDef|class|hide|scale)(\\s|$)"), I),
        direction(QStringLiteral("^direction\\s+(LR|RL|TB|TD|BT)"), I),
        named(QStringLiteral("^state\\s+\"([^\"]+)\"\\s+as\\s+([\\p{L}\\p{N}_]+)")),
        state(QStringLiteral("^state\\s+()([\\p{L}\\p{N}_]+)(?:\\s*<<\\s*(\\w+)\\s*>>)?")),
        choiceRe(QStringLiteral("^choice$"), I),
        link(QStringLiteral("^(\\[\\*\\]|[\\p{L}\\p{N}_]+)\\s*-->\\s*(\\[\\*\\]|[\\p{L}\\p{N}_]+)"
                            "\\s*(?::\\s*(.*))?$")),
        described(QStringLiteral("^([\\p{L}\\p{N}_]+)\\s*:\\s*(.+)$")),
        underscores(QStringLiteral("_+"));
    Builder graph(c);
    GroupMap groups;
    QVector<int> stack;
    QHash<QString, QString> names;
    QString dir = QStringLiteral("TD");
    int seq = 0;
    bool note = false;
    const auto claim = [&](int at) -> QString {
        if (at < 0)
            return {};
        const int group = topOf(stack);
        Node &node = graph.nodes[at];
        if (node.seq < 0)
            node.seq = seq++;
        if (group >= 0 && !node.grouped) {
            node.grouped = true;
            node.group = groups.objects.at(group).id;
        }
        return node.id;
    };
    const auto ref = [&](const QString &token, bool to) -> QString {
        if (token != QLatin1String("[*]"))
            return claim(graph.touch(token));
        const int own = topOf(stack);
        const QString id =
            QStringLiteral("__%1%2").arg(to ? QStringLiteral("end") : QStringLiteral("start"),
                                         own >= 0 && !groups.objects.at(own).id.isEmpty()
                                             ? QLatin1Char('@') + groups.objects.at(own).id
                                             : QString());
        const QString space = QStringLiteral(" ");
        return claim(graph.touch(id, &space, to ? QStringLiteral("end") : QStringLiteral("start")));
    };
    for (int li = 1; li < lines.size() && c.error.isEmpty(); ++li) {
        const QString &raw = lines.at(li);
        const bool opens = opensRe.match(raw).hasMatch();
        QString line = raw.trimmed();
        line.remove(brace);
        line = line.trimmed();
        QRegularExpressionMatch m;
        // A note may run over several lines, down to `end note`; none of them is a state.
        if (note) {
            note = !endNote.match(line).hasMatch();
            continue;
        }
        if (noteRe.match(line).hasMatch()) {
            note = !line.contains(QLatin1Char(':'));
            continue;
        }
        if (line.isEmpty()) {
            if (raw.trimmed() == QLatin1String("}") && !stack.isEmpty())
                stack.removeLast();
            continue;
        }
        if (ignore.match(line).hasMatch())
            continue;
        if ((m = direction.match(line)).hasMatch()) {
            const QString way = upright(m.captured(1));
            if (!stack.isEmpty()) {
                if (stack.last() >= 0)
                    groups.objects[stack.last()].dir = way;
            } else {
                dir = way;
            }
            continue;
        }
        if ((m = named.match(line)).hasMatch() || (m = state.match(line)).hasMatch()) {
            const QString id = m.captured(2), name = m.captured(1);
            const bool choice = choiceRe.match(m.captured(3)).hasMatch();
            if (!name.isEmpty())
                names.insert(id, name);
            if (opens) {
                if (groups.size() >= MaxGroups || stack.size() >= MaxGroupDepth) {
                    stack << -1;
                    continue;
                }
                const int parent = topOf(stack);
                Group group;
                group.id = id;
                group.parent = parent >= 0 ? groups.objects.at(parent).id : QString();
                group.seq = seq++;
                stack << groups.set(group);
            } else {
                const QString space = QStringLiteral(" ");
                const QString *label = !name.isEmpty() ? &name : choice ? &space : nullptr;
                claim(graph.touch(id, label,
                                  choice ? QStringLiteral("diamond") : QStringLiteral("round")));
            }
            continue;
        }
        if ((m = link.match(line)).hasMatch()) {
            Edge e;
            e.from = ref(m.captured(1), false);
            e.to = ref(m.captured(2), true);
            if (!c.error.isEmpty())
                break;
            e.label = cleanLabel(m.captured(3));
            e.head = QStringLiteral("arrow");
            graph.edge(e);
            continue;
        }
        if ((m = described.match(line)).hasMatch()) {
            const QString text = m.captured(2);
            claim(graph.touch(m.captured(1), &text, QStringLiteral("round")));
            names.insert(m.captured(1), text);
        }
    }
    if (!c.error.isEmpty())
        return std::nullopt;
    // A state has only its id for a name, and an id can't hold a space: its underscores are
    // read as spaces.
    for (auto &node : graph.nodes) {
        if (node.shape == QLatin1String("rect"))
            node.shape = QStringLiteral("round");
        if (node.label == node.id)
            node.label = QString(node.id).replace(underscores, QStringLiteral(" "));
    }
    // A state that holds others is a group, not a block: it goes by the name it was given.
    QVector<Group> list = groups.values();
    for (auto &group : list) {
        const QString name = names.value(group.id);
        group.title = cleanLabel(
            !name.isEmpty() ? name : QString(group.id).replace(underscores, QStringLiteral(" ")));
        graph.remove(group.id);
    }
    if (graph.nodes.isEmpty() && list.isEmpty())
        return std::nullopt;
    Graph out;
    out.dir = dir;
    out.nodes = graph.nodes;
    out.edges = graph.edges;
    out.groups = list;
    return out;
}

/* Cards: tables of a database, classes of code */

std::shared_ptr<const Card> cardOf(Ctx &c, const QString &title, const QString &sub,
                                   const QVector<CardRow> &rows, int sep = -1)
{
    double leadW = 0, textW = 0, metaW = 0;
    for (const auto &r : rows) {
        if (!r.lead.isEmpty())
            leadW = std::max(
                leadW,
                (r.badge ? c.widthOf(r.lead, FONT::cardBadge) + r.lead.size() * 0.4
                         : c.textWidth(r.lead, FONT::cardMeta.size, FONT::cardMeta.weight, true)) +
                    8);
        textW = std::max(textW, c.widthOf(r.text, FONT::cardText));
        if (!r.meta.isEmpty())
            metaW = std::max(metaW,
                             c.textWidth(r.meta, FONT::cardMeta.size, FONT::cardMeta.weight, true));
    }
    const double titleW = std::max(c.widthOf(title, FONT::cardTitle),
                                   sub.isEmpty() ? 0.0 : c.widthOf(sub, FONT::cardSub));
    auto card = std::make_shared<Card>();
    card->w = std::ceil(std::max(
        {CARD.min, titleW + 36, CARD.padX * 2 + leadW + textW + (metaW ? metaW + 22 : 0)}));
    card->head = sub.isEmpty() ? CARD.head : CARD.stereo;
    const bool split = sep > 0 && sep < rows.size();
    card->h = card->head + (rows.isEmpty() ? 0 : 9 + rows.size() * CARD.row + (split ? 9 : 0));
    card->title = title;
    card->sub = sub;
    card->rows = rows;
    card->sep = split ? sep : -1;
    card->leadW = leadW;
    // JSON.stringify([title, sub, rows, sep]): an ER row carries `badge`, a class row none.
    QString key = QLatin1Char('[') + jsonString(title) + QLatin1Char(',') + jsonString(sub) +
                  QLatin1String(",[");
    for (int i = 0; i < rows.size(); ++i) {
        const auto &r = rows.at(i);
        if (i)
            key += QLatin1Char(',');
        key += QLatin1String("{\"lead\":") + jsonString(r.lead) +
               (r.badge ? QLatin1String(",\"badge\":true") : QLatin1String("")) +
               QLatin1String(",\"text\":") + jsonString(r.text) + QLatin1String(",\"meta\":") +
               jsonString(r.meta) + QLatin1Char('}');
    }
    key += QLatin1String("],") + QString::number(sep) + QLatin1Char(']');
    card->key = key;
    return card;
}

void asCard(Node &node, std::shared_ptr<const Card> card)
{
    node.shape = QStringLiteral("card");
    node.lines.clear();
    node.w = card->w;
    node.h = card->h;
    node.card = std::move(card);
}

QString erCard(const QString &mark)
{
    // ER_CARD: how many stand at an end of a relation.
    if (mark == QLatin1String("|o") || mark == QLatin1String("o|"))
        return QStringLiteral("zero-one");
    if (mark == QLatin1String("||"))
        return QStringLiteral("one");
    if (mark == QLatin1String("}o") || mark == QLatin1String("o{"))
        return QStringLiteral("zero-many");
    if (mark == QLatin1String("}|") || mark == QLatin1String("|{"))
        return QStringLiteral("one-many");
    return {};
}

// A field written as two words is a type and a name, in either order: the word that looks
// more like a type is the type, and when neither does, the type comes first, as Mermaid has it.
struct Typed {
    QString name, type;
};
Typed typedPair(const QString &first, const QString &last)
{
    // PRIMITIVE and TYPE_SHAPE.
    static const Re primitive(
        QStringLiteral("^(?:int|integer|bigint|long|short|float|double|number|decimal|numeric|"
                       "bool|boolean|string|str|text|varchar|char|byte|bytes|void|any|object|json|"
                       "date|datetime|timestamp|time|uuid|list|array|map|set|dict|enum)"
                       "(?:\\(\\d+(?:,\\d+)?\\))?(?:\\[\\])?$"),
        I),
        typeShape(QStringLiteral("^(?:[A-Z][\\w.]*(?:<.*>)?(?:\\[\\])?|\\w+<.*>|\\w+\\[\\])$"));
    const auto kind = [&](const QString &word) {
        return primitive.match(word).hasMatch() ? 2 : typeShape.match(word).hasMatch() ? 1 : 0;
    };
    return kind(last) > kind(first) ? Typed{first, last} : Typed{last, first};
}

std::optional<Graph> parseER(Ctx &c, const QStringList &lines)
{
    static const Re entityRe(
        QStringLiteral("^\"?([^\"[\\]]+?)\"?(?:\\s*\\[\\s*\"?([^\"\\]]*)\"?\\s*\\])?$")),
        attr(QStringLiteral("^(\\S+)\\s+([^\\s\"]+)\\s*((?:PK|FK|UK)(?:\\s*,\\s*(?:PK|FK|UK))*)?"),
             I),
        direction(QStringLiteral("^direction\\s+(LR|RL|TB|TD|BT)"), I),
        rel(QStringLiteral("^(.+?)\\s*(\\|o|\\|\\||\\}o|\\}\\|)(--|\\.\\.)(o\\||\\|\\||o\\{|\\|\\{)"
                           "\\s*(.+?)\\s*(?::\\s*(.*))?$")),
        block(QStringLiteral("^(.+?)\\s*\\{\\s*(\\})?$")),
        bareRe(QStringLiteral("^[\\p{L}\\p{N}_-]+$")), space(QStringLiteral("\\s"));
    Builder graph(c);
    int open = -1;
    QString dir = QStringLiteral("LR");
    const auto entity = [&](const QString &token) {
        const auto m = entityRe.match(token.trimmed());
        const QString id = (m.hasMatch() ? m.captured(1) : token).trimmed();
        const int at = graph.touch(id);
        if (at >= 0 && m.hasMatch() && !m.captured(2).isEmpty())
            graph.nodes[at].label = cleanLabel(m.captured(2));
        return at;
    };
    for (int li = 1; li < lines.size() && c.error.isEmpty(); ++li) {
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if (open >= 0) {
            if (line.startsWith(QLatin1Char('}'))) {
                open = -1;
                continue;
            }
            if ((m = attr.match(line)).hasMatch() &&
                graph.nodes[open].attrRows.size() < MaxMembers) {
                const Typed pair = typedPair(m.captured(1).remove(QLatin1Char('~')), m.captured(2));
                QString keys = m.captured(3);
                keys.remove(space);
                CardRow row;
                row.lead = keys.toUpper();
                row.badge = true;
                row.text = pair.name;
                row.meta = pair.type;
                graph.nodes[open].attrRows << row;
            }
            continue;
        }
        if ((m = direction.match(line)).hasMatch()) {
            dir = upright(m.captured(1));
            continue;
        }
        if ((m = rel.match(line)).hasMatch()) {
            const int a = entity(m.captured(1));
            const int b = a < 0 ? -1 : entity(m.captured(5));
            if (a < 0 || b < 0)
                break;
            Edge e;
            e.from = graph.nodes.at(a).id;
            e.to = graph.nodes.at(b).id;
            e.label = unquote(m.captured(6));
            e.style = m.captured(3) == QLatin1String("..") ? QStringLiteral("dashed")
                                                           : QStringLiteral("solid");
            e.hasEnds = true;
            e.endStart = erCard(m.captured(2));
            e.endEnd = erCard(m.captured(4));
            graph.edge(e);
            continue;
        }
        if ((m = block.match(line)).hasMatch()) {
            const int node = entity(m.captured(1));
            if (m.capturedStart(2) < 0)
                open = node;
            continue;
        }
        if (bareRe.match(line).hasMatch())
            entity(line);
    }
    if (!c.error.isEmpty() || graph.nodes.isEmpty())
        return std::nullopt;
    for (auto &node : graph.nodes)
        asCard(node, cardOf(c, node.label, {}, node.attrRows));
    Graph out;
    out.dir = dir;
    out.nodes = graph.nodes;
    out.edges = graph.edges;
    return out;
}

CardRow memberRow(const QString &text)
{
    static const Re tail(QStringLiteral("[$*]$")), generic(QStringLiteral("~([^~]+)~")),
        vis(QStringLiteral("^[+\\-#~]")), lead(QStringLiteral("^[$*]?\\s*:?\\s*")),
        space(QStringLiteral("\\s+"));
    QString s = text.trimmed();
    s.remove(tail);
    s.replace(generic, QStringLiteral("<\\1>"));
    CardRow row;
    if (vis.match(s).hasMatch()) {
        row.lead = s.left(1);
        s = s.mid(1).trimmed();
    }
    if (s.contains(QLatin1Char('('))) {
        const int close = int(s.lastIndexOf(QLatin1Char(')')));
        row.text = s.left(close + 1);
        row.meta = s.mid(close + 1).remove(lead);
        return row;
    }
    if (s.contains(QLatin1Char(':'))) {
        const int at = int(s.indexOf(QLatin1Char(':')));
        row.text = s.left(at).trimmed();
        row.meta = s.mid(at + 1).trimmed();
        return row;
    }
    const QStringList words = s.split(space);
    if (words.size() < 2) {
        row.text = s;
        return row;
    }
    const Typed pair =
        typedPair(words.mid(0, words.size() - 1).join(QLatin1Char(' ')), words.last());
    row.text = pair.name;
    row.meta = pair.type;
    return row;
}

std::optional<Graph> parseClass(Ctx &c, const QStringList &lines)
{
    static const Re direction(QStringLiteral("^direction\\s+(LR|RL|TB|TD|BT)"), I),
        skipped(QStringLiteral("^(note|namespace|classDef|cssClass|style|click|link|callback)\\b"),
                I),
        classLine(QStringLiteral("^class\\s+([^{]+?)\\s*(\\{)?\\s*(\\})?$")),
        stereoLine(QStringLiteral("^<<(.+)>>\\s+(\\S+)$")),
        // CLASS_REL.
        rel(QStringLiteral("^(\\S+?)\\s*(?:\"([^\"]*)\"\\s*)?(<\\|--|--\\|>|<\\|\\.\\.|\\.\\.\\|>|"
                           "\\*--|--\\*|o--|--o|<--|-->|<\\.\\.|\\.\\.>|--|\\.\\.)\\s*(?:\"([^\"]*)"
                           "\"\\s*)?(\\S+?)\\s*(?::\\s*(.+))?$")),
        member(QStringLiteral("^([\\p{L}\\p{N}_-]+)\\s*:\\s*(.+)$")),
        alias(QStringLiteral("^([\\p{L}\\p{N}_-]+)\\s*\\[\\s*\"?([^\"\\]]*)\"?\\s*\\]$")),
        generic(QStringLiteral("^([^~]+)~(.+)~$")), stereo(QStringLiteral("^<<(.+)>>$")),
        ticks(QStringLiteral("^`|`$")), cssClass(QStringLiteral(":::[\\w-]+$"));
    // CLASS_OPS: the mark at the start, at the end, and the line.
    struct Op {
        const char *op, *start, *end, *style;
    };
    static const Op ops[] = {
        {"<|--", "triangle", nullptr, "solid"},  {"--|>", nullptr, "triangle", "solid"},
        {"<|..", "triangle", nullptr, "dashed"}, {"..|>", nullptr, "triangle", "dashed"},
        {"*--", "diamond", nullptr, "solid"},    {"--*", nullptr, "diamond", "solid"},
        {"o--", "odiamond", nullptr, "solid"},   {"--o", nullptr, "odiamond", "solid"},
        {"<--", "vee", nullptr, "solid"},        {"-->", nullptr, "vee", "solid"},
        {"<..", "vee", nullptr, "dashed"},       {"..>", nullptr, "vee", "dashed"},
        {"--", nullptr, nullptr, "solid"},       {"..", nullptr, nullptr, "dashed"},
    };
    Builder graph(c);
    int open = -1;
    QString dir = QStringLiteral("TD");
    const auto cls = [&](const QString &token) {
        QString id = token.trimmed();
        id.remove(ticks);
        id.remove(cssClass);
        QString label;
        auto m = alias.match(id);
        if (m.hasMatch()) {
            id = m.captured(1);
            label = m.captured(2);
        }
        m = generic.match(id);
        if (m.hasMatch()) {
            id = m.captured(1);
            if (label.isEmpty())
                label = QStringLiteral("%1<%2>").arg(m.captured(1), m.captured(2));
        }
        const int at = graph.touch(id);
        if (at >= 0 && !label.isEmpty())
            graph.nodes[at].label = label;
        return at;
    };
    const auto addMember = [&](int at, const QString &text) {
        const QString s = text.trimmed();
        if (s.isEmpty() || at < 0)
            return;
        Node &node = graph.nodes[at];
        const auto st = stereo.match(s);
        if (st.hasMatch())
            node.stereo = st.captured(1);
        else if (node.attrs.size() + node.methods.size() < MaxMembers)
            (s.contains(QLatin1Char('(')) ? node.methods : node.attrs) << s;
    };
    for (int li = 1; li < lines.size() && c.error.isEmpty(); ++li) {
        const QString line = lines.at(li).trimmed();
        QRegularExpressionMatch m;
        if (open >= 0) {
            if (line.startsWith(QLatin1Char('}')))
                open = -1;
            else
                addMember(open, line);
            continue;
        }
        if ((m = direction.match(line)).hasMatch()) {
            dir = upright(m.captured(1));
            continue;
        }
        if (skipped.match(line).hasMatch() || line == QLatin1String("}"))
            continue;
        if ((m = classLine.match(line)).hasMatch()) {
            const int node = cls(m.captured(1));
            if (m.capturedStart(2) >= 0 && m.capturedStart(3) < 0)
                open = node;
            continue;
        }
        if ((m = stereoLine.match(line)).hasMatch()) {
            const int node = cls(m.captured(2));
            if (node >= 0)
                graph.nodes[node].stereo = m.captured(1);
            continue;
        }
        if ((m = rel.match(line)).hasMatch()) {
            const QString token = m.captured(3);
            const Op *op = nullptr;
            for (const auto &o : ops) {
                if (token == QLatin1String(o.op))
                    op = &o;
            }
            if (!op)
                continue;
            const QString start = op->start ? QLatin1String(op->start) : QString(),
                          end = op->end ? QLatin1String(op->end) : QString();
            const int a = cls(m.captured(1));
            const int b = a < 0 ? -1 : cls(m.captured(5));
            if (a < 0 || b < 0)
                break;
            const bool flip = start.isEmpty() && !end.isEmpty() && end != QLatin1String("vee");
            // How many stand at each end is told with the caption, in the order the line is
            // drawn: 1 : *.
            QStringList count;
            for (const QString &part :
                 {flip ? m.captured(4) : m.captured(2), flip ? m.captured(2) : m.captured(4)}) {
                if (!part.isEmpty())
                    count << part;
            }
            QStringList label;
            for (const QString &part :
                 {cleanLabel(m.captured(6)), count.join(QStringLiteral(" : "))}) {
                if (!part.isEmpty())
                    label << part;
            }
            Edge e;
            e.from = graph.nodes.at(flip ? b : a).id;
            e.to = graph.nodes.at(flip ? a : b).id;
            e.label = label.join(QStringLiteral(" \u00b7 "));
            e.style = QLatin1String(op->style);
            e.hasEnds = true;
            e.endStart = flip ? end : start;
            e.endEnd = flip ? QString() : end;
            graph.edge(e);
            continue;
        }
        if ((m = member.match(line)).hasMatch())
            addMember(cls(m.captured(1)), m.captured(2));
    }
    if (!c.error.isEmpty() || graph.nodes.isEmpty())
        return std::nullopt;
    for (auto &node : graph.nodes) {
        QVector<CardRow> rows;
        for (const auto &text : node.attrs + node.methods)
            rows << memberRow(text);
        asCard(node,
               cardOf(c, node.label,
                      node.stereo.isEmpty() ? QString()
                                            : QStringLiteral("\u00ab%1\u00bb").arg(node.stereo),
                      rows, int(node.attrs.size())));
    }
    Graph out;
    out.dir = dir;
    out.nodes = graph.nodes;
    out.edges = graph.edges;
    return out;
}

/* Layered layout for flowcharts and state diagrams */

// Shapes an arrow meets at their middle: their ports are not spread.
bool roundShape(const QString &s)
{
    return s == QLatin1String("diamond") || s == QLatin1String("circle") ||
           s == QLatin1String("start") || s == QLatin1String("end");
}

void sizeNode(Ctx &c, Node &node)
{
    if (node.card)
        return;
    if (node.shape == QLatin1String("start") || node.shape == QLatin1String("end")) {
        node.lines.clear();
        node.w = node.h = 16;
        return;
    }
    node.head = 0;
    const Parts *parts =
        node.parts && (!node.parts->soft || c.textWidth(node.label) > NODE.maxWidth) ? &*node.parts
                                                                                     : nullptr;
    if (parts) {
        // The name of the block, strong, and under it what it does, set smaller and quiet.
        const QStringList head = wrap(c, parts->head, NODE.maxWidth, FONT::strong),
                          detail = wrap(c, parts->detail, NODE.maxWidth, FONT::detail);
        node.lines = head + detail;
        node.head = int(head.size());
        const double tw =
            std::max(textMax(c, head, FONT::strong), textMax(c, detail, FONT::detail));
        node.w = std::ceil(std::max(NODE.minWidth, tw + NODE.padX * 2) +
                           (node.shape == QLatin1String("subroutine") ? 12 : 0));
        node.h = std::ceil(head.size() * TEXT.line + NODE.part + detail.size() * NODE.detail +
                           NODE.padY * 2 + (node.shape == QLatin1String("cylinder") ? 12 : 0));
        return;
    }
    node.lines = wrap(c, node.label, NODE.maxWidth);
    const double tw = textMax(c, node.lines), th = node.lines.size() * TEXT.line;
    double w = std::max(NODE.minWidth, tw + NODE.padX * 2), h = th + NODE.padY * 2;
    const QString &s = node.shape;
    if (s == QLatin1String("diamond")) {
        const double a = tw / 2 + th / 1.2 + 10;
        w = a * 2;
        h = a * 1.2;
    } else if (s == QLatin1String("circle")) {
        w = h = std::max(tw, th) + 30;
    } else if (s == QLatin1String("hexagon")) {
        w += h * 0.5;
    } else if (s == QLatin1String("cylinder")) {
        h += 12;
    } else if (s == QLatin1String("lean") || s == QLatin1String("flag")) {
        w += 16;
    } else if (s == QLatin1String("subroutine")) {
        w += 12;
    }
    node.w = std::ceil(w);
    node.h = std::ceil(h);
}

struct Place {
    const Node *node = nullptr;
    double cx = 0, cy = 0, rank = 0, order = 0;
};

// An arrow laid out: its path, where its caption stands, and the ids of its ends.
struct Routed {
    Edge edge;
    QString from, to;
    Pts pts;
    QPointF labelAt;
    QStringList labelLines;
    double labelW = 0, labelH = 0;
    bool bulged = false;
};

struct ClusterBox {
    QString id, title;
    double x = 0, y = 0, w = 0, h = 0, rank = 0;
    int depth = 0;
};

struct Layout {
    QVector<Place> places;
    QVector<Routed> edges;
    QVector<ClusterBox> clusters;
    double width = 0, height = 0;
    Hints hints;
};

// A block of a rank, or a bend of a link that passes the rank (a dummy).
struct Vert {
    const Node *node = nullptr;
    int link = -1; // A dummy's link.
    int rank = 0;
    double cross = 0, main = 0, x = 0, y = 0, bary = 0, hint = NaN;
    int order = 0, seq = 0;
    QVector<int> up, down;
};

struct Link {
    Edge edge;
    int u = 0, v = 0, a = 0, b = 0;
    bool back = false;
    QVector<int> chain;
    QStringList labelLines;
    double labelW = 0, labelH = 0;
    int seat = -1; // The bend that carries the caption of an arrow back.
};

void isotonic(QVector<Vert> &verts, const QVector<int> &layer, const QVector<double> &want,
              const QVector<double> &weight, double gap)
{
    const int n = int(layer.size());
    QVector<double> offset(n, 0);
    for (int i = 1; i < n; ++i)
        offset[i] = offset[i - 1] + (verts[layer[i - 1]].cross + verts[layer[i]].cross) / 2 + gap;
    struct Block {
        double sum, weight;
        int from, to;
    };
    QVector<Block> blocks;
    for (int i = 0; i < n; ++i) {
        Block block{(want[i] - offset[i]) * weight[i], weight[i], i, i};
        while (!blocks.isEmpty() &&
               blocks.last().sum / blocks.last().weight > block.sum / block.weight) {
            const Block prev = blocks.takeLast();
            block = {prev.sum + block.sum, prev.weight + block.weight, prev.from, block.to};
        }
        blocks << block;
    }
    for (const auto &block : blocks) {
        for (int i = block.from; i <= block.to; ++i)
            verts[layer[i]].x = block.sum / block.weight + offset[i];
    }
}

qint64 crossings(Ctx &c, const QVector<Vert> &verts, const QVector<QVector<int>> &layers)
{
    qint64 count = 0;
    QVector<QPair<int, int>> links;
    for (int r = 1; r < layers.size(); ++r) {
        links.clear();
        for (const int v : layers[r]) {
            for (const int u : verts[v].up)
                links << qMakePair(verts[u].order, verts[v].order);
        }
        c.budget(int(std::min<qint64>(qint64(links.size()) * links.size() / 2 + 1, 1 << 30)));
        for (int i = 0; i < links.size(); ++i) {
            for (int j = i + 1; j < links.size(); ++j) {
                if (qint64(links[i].first - links[j].first) * (links[i].second - links[j].second) <
                    0)
                    ++count;
            }
        }
    }
    return count;
}

std::optional<Layout> layoutFlow(Ctx &c, const QVector<const Node *> &nodes,
                                 const QVector<Edge> &edges, const QString &dir, const Hints &hints)
{
    const bool side = lying(dir);
    QHash<QString, int> index;
    for (int i = 0; i < nodes.size(); ++i)
        index.insert(nodes[i]->id, i);
    const int n = int(nodes.size());
    QVector<Link> links;
    for (const auto &e : edges) {
        const int u = index.value(e.from, -1), v = index.value(e.to, -1);
        if (e.from == e.to || u < 0 || v < 0)
            continue;
        Link link;
        link.edge = e;
        link.u = u;
        link.v = v;
        links << link;
    }
    QVector<QVector<int>> out(n);
    for (int i = 0; i < links.size(); ++i)
        out[links[i].u] << i;
    // Depth first, in the order of the recursive visit: a link into a block still open is back.
    QVector<char> state(n, 0);
    for (int root = 0; root < n; ++root) {
        if (state[root])
            continue;
        QVector<QPair<int, int>> stack{{root, 0}};
        state[root] = 1;
        while (!stack.isEmpty()) {
            const int u = stack.last().first;
            const int k = stack.last().second++;
            if (k >= out[u].size()) {
                state[u] = 2;
                stack.removeLast();
                continue;
            }
            Link &link = links[out[u][k]];
            if (state[link.v] == 1) {
                link.back = true;
            } else if (!state[link.v]) {
                state[link.v] = 1;
                stack << qMakePair(link.v, 0);
            }
        }
    }

    QVector<QVector<int>> succ(n), pred(n);
    for (int i = 0; i < links.size(); ++i) {
        Link &link = links[i];
        link.a = link.back ? link.v : link.u;
        link.b = link.back ? link.u : link.v;
        succ[link.a] << i;
        pred[link.b] << i;
    }
    QVector<int> rank(n, 0), indegree(n), queue;
    for (int u = 0; u < n; ++u) {
        indegree[u] = int(pred[u].size());
        if (!indegree[u])
            queue << u;
    }
    for (int q = 0; q < queue.size(); ++q) {
        const int u = queue[q];
        for (const int l : succ[u]) {
            const int b = links[l].b;
            rank[b] = std::max(rank[b], rank[u] + 1);
            if (!--indegree[b])
                queue << b;
        }
    }
    for (int u = 0; u < n; ++u) {
        if (pred[u].isEmpty() && !succ[u].isEmpty()) {
            int low = std::numeric_limits<int>::max();
            for (const int l : succ[u])
                low = std::min(low, rank[links[l].b]);
            rank[u] = std::max(0, low - 1);
        }
    }

    QVector<Vert> verts(n);
    for (int i = 0; i < n; ++i) {
        verts[i].node = nodes[i];
        verts[i].rank = rank[i];
        verts[i].cross = side ? nodes[i]->h : nodes[i]->w;
        verts[i].main = side ? nodes[i]->w : nodes[i]->h;
    }
    for (int l = 0; l < links.size(); ++l) {
        Link &link = links[l];
        QVector<int> chain{link.a};
        for (int r = rank[link.a] + 1; r < rank[link.b]; ++r) {
            if (verts.size() >= MaxVerts) {
                c.fail(QStringLiteral("This diagram is too large to lay out."));
                return std::nullopt;
            }
            Vert dummy;
            dummy.link = l;
            dummy.rank = r;
            dummy.cross = 12;
            verts << dummy;
            chain << int(verts.size()) - 1;
        }
        chain << link.b;
        for (int k = 1; k < chain.size(); ++k) {
            verts[chain[k - 1]].down << chain[k];
            verts[chain[k]].up << chain[k - 1];
        }
        link.chain = chain;
    }
    // What an arrow says is measured before the blocks are placed. An arrow back over several
    // ranks carries it on its lane, in the rank at the middle of its way, which keeps room for it.
    for (auto &link : links) {
        if (link.edge.label.isEmpty())
            continue;
        link.labelLines = wrap(c, link.edge.label, 140, FONT::small);
        link.labelW = textMax(c, link.labelLines, FONT::small) + 14;
        link.labelH = link.labelLines.size() * 15 + 6;
        if (!link.back || link.chain.size() < 3)
            continue;
        link.seat = link.chain[link.chain.size() / 2];
        Vert &seat = verts[link.seat];
        seat.cross = std::max(seat.cross, (side ? link.labelH : link.labelW) + 4);
    }

    int depth = 0;
    for (const auto &v : verts)
        depth = std::max(depth, v.rank + 1);
    QVector<QVector<int>> layers(depth);
    QVector<char> placed(verts.size(), 0);
    // Depth first from each block, in the recursive order: a block, then what is under it.
    const auto place = [&](int start) {
        if (placed[start])
            return;
        placed[start] = 1;
        layers[verts[start].rank] << start;
        QVector<QPair<int, int>> stack{{start, 0}};
        while (!stack.isEmpty()) {
            const int v = stack.last().first;
            const int k = stack.last().second++;
            if (k >= verts[v].down.size()) {
                stack.removeLast();
                continue;
            }
            const int w = verts[v].down[k];
            if (placed[w])
                continue;
            placed[w] = 1;
            layers[verts[w].rank] << w;
            stack << qMakePair(w, 0);
        }
    };
    for (int v = 0; v < verts.size(); ++v) {
        if (verts[v].link < 0 && verts[v].up.isEmpty())
            place(v);
    }
    for (int v = 0; v < verts.size(); ++v)
        place(v);
    // The order the blocks had in the last layout, so a small edit doesn't reshuffle them.
    if (!hints.isEmpty()) {
        const auto hintOf = [&](const Vert &v) {
            if (v.link >= 0) {
                const Link &link = links[v.link];
                return (hints.value(nodes[link.a]->id, NaN) + hints.value(nodes[link.b]->id, NaN)) /
                       2;
            }
            return hints.value(v.node->id, NaN);
        };
        for (auto &layer : layers) {
            for (int i = 0; i < layer.size(); ++i) {
                verts[layer[i]].seq = i;
                verts[layer[i]].hint = hintOf(verts[layer[i]]);
            }
            jsSort(layer, [&](int a, int b) {
                const Vert &va = verts[a], &vb = verts[b];
                const double d = finite(va.hint) && finite(vb.hint) ? va.hint - vb.hint : 0;
                return d != 0 && d == d ? d : double(va.seq - vb.seq);
            });
        }
    }
    const auto number = [&] {
        for (const auto &layer : layers) {
            for (int i = 0; i < layer.size(); ++i)
                verts[layer[i]].order = i;
        }
    };
    number();
    auto best = layers;
    qint64 bestCount = crossings(c, verts, layers);
    for (int sweep = 0; sweep < SWEEPS.order && bestCount; ++sweep) {
        if (!c.error.isEmpty() || c.cancelled())
            return std::nullopt;
        const bool down = sweep % 2 == 0;
        for (int k = 1; k < depth; ++k) {
            auto &layer = layers[down ? k : depth - 1 - k];
            for (const int v : layer) {
                const auto &near = down ? verts[v].up : verts[v].down;
                double sum = 0;
                for (const int w : near)
                    sum += verts[w].order;
                verts[v].bary = near.isEmpty() ? verts[v].order : sum / near.size();
            }
            jsSort(layer, [&](int a, int b) {
                const double d = verts[a].bary - verts[b].bary;
                return d != 0 && d == d ? d : double(verts[a].order - verts[b].order);
            });
            for (int i = 0; i < layer.size(); ++i)
                verts[layer[i]].order = i;
        }
        const qint64 count = crossings(c, verts, layers);
        if (count < bestCount) {
            bestCount = count;
            best = layers;
        }
    }
    if (!c.error.isEmpty())
        return std::nullopt;
    layers = best;
    number();

    for (const auto &layer : layers) {
        double x = 0;
        for (const int v : layer) {
            verts[v].x = x + verts[v].cross / 2;
            x += verts[v].cross + FLOW_GAP.node;
        }
        const double shift = (x - FLOW_GAP.node) / 2;
        for (const int v : layer)
            verts[v].x -= shift;
    }
    // A block lines up with the blocks it is joined to, not with the bends of an arrow that runs
    // back past it, and where the two want one place the block has it. So a chain keeps a
    // straight spine, and an arrow back to its start runs down its side.
    const auto solid = [&](const Vert &v, const QVector<int> &near) {
        if (v.link >= 0)
            return near;
        QVector<int> kept;
        for (const int w : near) {
            if (verts[w].link < 0 || !links[verts[w].link].back)
                kept << w;
        }
        return kept.isEmpty() ? near : kept;
    };
    for (int sweep = 0; sweep < SWEEPS.place; ++sweep) {
        const bool down = sweep % 2 == 0, last = sweep == SWEEPS.place - 1;
        for (int k = 0; k < depth; ++k) {
            const auto &layer = layers[down ? k : depth - 1 - k];
            QVector<double> want, weight;
            for (const int v : layer) {
                const Vert &vert = verts[v];
                const QVector<int> near =
                    solid(vert, last   ? vert.up + vert.down
                                : down ? (vert.up.isEmpty() ? vert.down : vert.up)
                                       : (vert.down.isEmpty() ? vert.up : vert.down));
                double sum = 0;
                for (const int w : near)
                    sum += verts[w].x;
                want << (near.isEmpty() ? vert.x : sum / near.size());
                weight << (vert.link >= 0 ? (links[vert.link].back ? 0.001 : 3) : 1);
            }
            isotonic(verts, layer, want, weight, FLOW_GAP.node);
        }
        c.budget(int(verts.size()));
    }

    QVector<double> labelRoom(depth, 0);
    for (const auto &link : links) {
        if (link.edge.label.isEmpty() || link.seat >= 0)
            continue;
        QVector<int> chain = link.chain;
        if (link.back)
            std::reverse(chain.begin(), chain.end());
        const int segment = int((chain.size() - 2) / 2);
        const int r = std::min(verts[chain[segment]].rank, verts[chain[segment + 1]].rank);
        labelRoom[r] = std::max(labelRoom[r], (side ? link.labelW : link.labelH) + FLOW_GAP.label);
    }
    QVector<double> size(depth, 0);
    for (int r = 0; r < depth; ++r) {
        for (const int v : layers[r])
            size[r] = std::max(size[r], verts[v].main);
    }
    QVector<char> drawn(depth, 0);
    for (const auto &link : links) {
        if (link.edge.style != QLatin1String("hidden")) {
            for (int r = rank[link.a]; r < rank[link.b]; ++r)
                drawn[r] = 1;
        }
    }
    const auto gapAfter = [&](int r) {
        return drawn[r] ? std::max(side ? FLOW_GAP.rankSide : FLOW_GAP.rank, labelRoom[r] + 24)
                        : FLOW_GAP.node;
    };
    QVector<double> mainAt(depth);
    double cursor = 0;
    for (int r = 0; r < depth; ++r) {
        mainAt[r] = cursor + size[r] / 2;
        cursor += size[r] + gapAfter(r);
    }
    for (auto &v : verts)
        v.y = mainAt[v.rank];

    double minX = INFINITY, maxX = -INFINITY;
    for (const auto &v : verts) {
        minX = std::min(minX, v.x - v.cross / 2);
        maxX = std::max(maxX, v.x + v.cross / 2);
    }
    const double mainEnd = depth ? cursor - gapAfter(depth - 1) : 0;
    const bool flipMain = dir == QLatin1String("BT") || dir == QLatin1String("RL");
    const auto map = [&](double x, double y) {
        const double cx = x - minX, cy = flipMain ? mainEnd - y : y;
        return side ? QPointF(cy, cx) : QPointF(cx, cy);
    };

    struct Port {
        int v, other;
        double x, y;
    };
    QVector<Port> ports;
    QVector<QVector<int>> portLists; // By key (vert and side), in the order first used.
    QHash<qint64, int> portSlot;
    const auto portFor = [&](int v, int sign, int other) {
        const qint64 key = qint64(v) * 2 + (sign > 0 ? 1 : 0);
        int slot = portSlot.value(key, -1);
        if (slot < 0) {
            slot = int(portLists.size());
            portSlot.insert(key, slot);
            portLists << QVector<int>();
        }
        ports << Port{v, other, verts[v].x, verts[v].y + sign * verts[v].main / 2};
        portLists[slot] << int(ports.size()) - 1;
        return int(ports.size()) - 1;
    };
    struct Route {
        int link = 0;
        QVector<int> chain;
        int out = 0, into = 0;
        int startPort = -1, endPort = -1;
        QPointF start, end;
    };
    QVector<Route> routes;
    for (int l = 0; l < links.size(); ++l) {
        const Link &link = links[l];
        Route route;
        route.link = l;
        route.chain = link.chain;
        if (link.back)
            std::reverse(route.chain.begin(), route.chain.end());
        const auto &chain = route.chain;
        const Vert &first = verts[chain.first()], &last = verts[chain.last()];
        const int sign = verts[chain[1]].y > first.y ? 1 : -1;
        // An arrow back over several ranks runs in a lane beside the blocks. Where the lane lies
        // clear of a block, the arrow leaves it, or comes into it, by the side facing the lane.
        const bool lane = link.back && chain.size() > 2;
        const auto facing = [&](const Vert &v, const Vert &w) {
            return lane && std::abs(w.x - v.x) >= v.cross / 2 + 12 ? int(signOf(w.x - v.x)) : 0;
        };
        route.out = facing(first, verts[chain[1]]);
        route.into = facing(last, verts[chain[chain.size() - 2]]);
        if (route.out)
            route.start = QPointF(first.x + route.out * first.cross / 2, first.y);
        else
            route.startPort = portFor(chain.first(), sign, chain[1]);
        if (route.into)
            route.end = QPointF(last.x + route.into * last.cross / 2, last.y);
        else
            route.endPort = portFor(chain.last(), -sign, chain[chain.size() - 2]);
        routes << route;
    }
    for (auto &list : portLists) {
        const Vert &v = verts[ports[list[0]].v];
        if (list.size() < 2 || v.link >= 0 || roundShape(v.node->shape))
            continue;
        jsSort(list,
               [&](int a, int b) { return verts[ports[a].other].x - verts[ports[b].other].x; });
        const double span = std::min(v.cross * 0.56, (list.size() - 1) * 16.0);
        for (int i = 0; i < list.size(); ++i)
            ports[list[i]].x = v.x - span / 2 + span * i / (list.size() - 1);
    }

    QSet<qint64> twins;
    for (const auto &link : links) {
        if (!link.back)
            twins.insert(qint64(link.u) * 1000000 + link.v);
    }
    Layout layout;
    for (const auto &route : routes) {
        const Link &link = links[route.link];
        const auto &chain = route.chain;
        const QPointF start = route.startPort >= 0
                                  ? QPointF(ports[route.startPort].x, ports[route.startPort].y)
                                  : route.start;
        const QPointF end = route.endPort >= 0
                                ? QPointF(ports[route.endPort].x, ports[route.endPort].y)
                                : route.end;
        // Past a tall rank an arrow runs straight for the whole height of it, so its bends fall
        // between the ranks. `spans` tells which points belong to each stop of the way.
        double way = signOf(verts[chain.last()].y - verts[chain.first()].y);
        if (!way)
            way = 1;
        QVector<QPair<int, int>> spans{{0, 0}};
        QVector<QPointF> points{start};
        for (int k = 1; k + 1 < chain.size(); ++k) {
            const Vert &v = verts[chain[k]];
            const double half = size[v.rank] > TALL ? size[v.rank] / 2 : 0;
            spans << qMakePair(int(points.size()), int(points.size()) + (half ? 1 : 0));
            if (half)
                points << QPointF(v.x, v.y - way * half) << QPointF(v.x, v.y + way * half);
            else
                points << QPointF(v.x, v.y);
        }
        spans << qMakePair(int(points.size()), int(points.size()));
        points << end;
        bool bulged = false, corner = false;
        QPointF bulge;
        if (link.back && chain.size() == 2 && twins.contains(qint64(link.v) * 1000000 + link.u)) {
            const auto edgeX = [&](const Vert &v) {
                return roundShape(v.node->shape) ? v.x : v.x + v.cross / 2 - 12;
            };
            const Vert &v0 = verts[chain[0]];
            points[0].setX(edgeX(v0));
            points[1].setX(edgeX(verts[chain[1]]));
            // A question sends its way back from its own corner, not from where the way in comes.
            corner = v0.node->shape == QLatin1String("diamond");
            if (corner)
                points[0] = QPointF(v0.x + v0.cross / 2, v0.y);
            bulge = QPointF(std::max(points[0].x(), points[1].x()) + BULGE,
                            (points[0].y() + points[1].y()) / 2);
            if (corner)
                bulge.setY((v0.y + points[1].y()) / 2);
            points.insert(1, bulge);
            bulged = true;
            maxX = std::max(maxX, bulge.x() + (side ? link.labelH : link.labelW) - 4);
        }
        {
            QPointF &tail = points.last();
            const QPointF before = points[points.size() - 2];
            double sign = signOf(tail.y() - before.y());
            if (!sign)
                sign = 1;
            if (link.edge.head != QLatin1String("none")) {
                if (route.into)
                    tail.rx() += route.into * ARROW.length;
                else
                    tail.ry() -= sign * ARROW.length;
            }
        }
        {
            QPointF &head0 = points[0];
            double startSign = signOf(points[1].y() - head0.y());
            if (!startSign)
                startSign = 1;
            if (link.edge.both && link.edge.head != QLatin1String("none") && !corner) {
                if (route.out)
                    head0.rx() += route.out * ARROW.length;
                else
                    head0.ry() += startSign * ARROW.length;
            }
        }
        Pts pts;
        const int lastPoint = int(points.size()) - 1;
        for (int i = 0; i < points.size(); ++i) {
            const QPointF xy = map(points[i].x(), points[i].y());
            if (!i) {
                pts << xy.x() << xy.y();
                continue;
            }
            const QPointF p = points[i - 1], q = points[i];
            const double mid = (q.y() - p.y()) / 2;
            // Out of the corner of a question, or out of the side of a block, the way leaves
            // sideways and turns to run beside the blocks; into a side it turns the other way.
            if ((corner || route.out) && i == 1) {
                const QPointF bend = map(q.x(), p.y());
                pts << bend.x() << bend.y() << bend.x() << bend.y() << xy.x() << xy.y();
                continue;
            }
            if (route.into && i == lastPoint) {
                const QPointF bend = map(p.x(), q.y());
                pts << bend.x() << bend.y() << bend.x() << bend.y() << xy.x() << xy.y();
                continue;
            }
            const QPointF a = map(p.x(), p.y() + mid), b = map(q.x(), q.y() - mid);
            pts << a.x() << a.y() << b.x() << b.y() << xy.x() << xy.y();
        }
        const int segment = int((chain.size() - 2) / 2);
        const QPointF a = points[spans[segment].second], b = points[spans[segment + 1].first];
        Routed routed;
        routed.edge = link.edge;
        routed.from = nodes[link.u]->id;
        routed.to = nodes[link.v]->id;
        routed.pts = pts;
        routed.labelAt =
            bulged ? map(bulge.x() + (side ? link.labelH : link.labelW) / 2 - 8, bulge.y())
            : link.seat >= 0 ? map(verts[link.seat].x, verts[link.seat].y)
                             : map((a.x() + b.x()) / 2, (a.y() + b.y()) / 2);
        routed.labelLines = link.labelLines;
        routed.labelW = link.labelW;
        routed.labelH = link.labelH;
        routed.bulged = bulged;
        layout.edges << routed;
    }
    for (const auto &v : verts) {
        if (v.link >= 0)
            continue;
        const QPointF xy = map(v.x, v.y);
        layout.places << Place{v.node, xy.x(), xy.y(), double(v.rank), double(v.order)};
        layout.hints.insert(v.node->id, v.x);
    }
    layout.width = side ? mainEnd : maxX - minX;
    layout.height = side ? maxX - minX : mainEnd;
    return layout;
}

// Blocks joined each to the next, and the last back to the first, are a cycle and are drawn as
// one: two rows, or two columns, with the arrows running round. `rows` lays it out along the
// line of reading; the way in then falls on the first block and the way round reads from it.
std::optional<Layout> layoutRing(Ctx &c, const QVector<const Node *> &nodes,
                                 const QVector<Edge> &edges, bool rows,
                                 const std::optional<QString> &first = std::nullopt)
{
    const int n = int(nodes.size());
    if (n < 3 || n > 10 || edges.size() != n)
        return std::nullopt;
    for (const Node *node : nodes) {
        if (node->cluster || node->card || node->shape == QLatin1String("start") ||
            node->shape == QLatin1String("end"))
            return std::nullopt;
    }
    QHash<QString, int> next, byId;
    for (int i = 0; i < n; ++i)
        byId.insert(nodes[i]->id, i);
    for (int i = 0; i < edges.size(); ++i) {
        const Edge &e = edges[i];
        if (e.lift || e.style == QLatin1String("hidden") || e.from == e.to ||
            next.contains(e.from) || !byId.contains(e.from) || !byId.contains(e.to))
            return std::nullopt;
        next.insert(e.from, i);
    }
    struct Step {
        const Node *node;
        const Edge *edge;
        QStringList lines;
        bool labelled = false;
        double lw = 0, lh = 0, a = 0, c = 0;
    };
    QVector<Step> loop;
    QSet<QString> seen;
    QString at = first && byId.contains(*first) ? *first : nodes[0]->id;
    for (int k = 0; k < n; ++k) {
        const int e = next.value(at, -1);
        if (e < 0 || seen.contains(at))
            return std::nullopt;
        seen.insert(at);
        loop << Step{nodes[byId.value(at)], &edges[e], {}, false, 0, 0, 0, 0};
        at = edges[e].to;
    }
    if (loop[n - 1].edge->to != loop[0].node->id)
        return std::nullopt;
    // Along the row a block takes its width, across it its height; turned to columns, the other
    // way round.
    const auto along = [&](const Node *node) { return rows ? node->w : node->h; };
    const auto acrossOf = [&](const Node *node) { return rows ? node->h : node->w; };
    const int k = (n + 1) / 2;
    const auto slotOf = [&](int i) { return i < k ? i : 2 * k - 1 - i; };
    const auto rowOf = [&](int i) { return i < k ? 0 : 1; };
    for (auto &step : loop) {
        if (step.edge->label.isEmpty())
            continue;
        step.labelled = true;
        step.lines = wrap(c, step.edge->label, 140, FONT::small);
        step.lw = textMax(c, step.lines, FONT::small) + 14;
        step.lh = step.lines.size() * 15 + 6;
    }
    // An arrow runs along a row between neighbours, and across between the rows at their ends.
    const auto runs = [&](int i) { return rowOf(i) == rowOf((i + 1) % n) && (i + 1) % n != 0; };
    const auto need = [&](bool alongRow, double base) {
        double out = base;
        for (int i = 0; i < n; ++i) {
            if (loop[i].labelled && runs(i) == alongRow)
                out = std::max(out, (alongRow == rows ? loop[i].lw : loop[i].lh) + 24);
        }
        return out;
    };
    const double gapA = need(true, rows ? FLOW_GAP.rankSide : FLOW_GAP.rank),
                 gapC = need(false, rows ? FLOW_GAP.rank : FLOW_GAP.rankSide);
    QVector<double> size(k, -INFINITY), at_(k, 0);
    for (int i = 0; i < n; ++i)
        size[slotOf(i)] = std::max(size[slotOf(i)], along(loop[i].node));
    double sum = 0;
    for (int s = 0; s < k; ++s) {
        at_[s] = sum + size[s] / 2;
        sum += size[s] + gapA;
    }
    double c0 = -INFINITY, c1 = -INFINITY;
    for (int i = 0; i < n; ++i)
        (i < k ? c0 : c1) = std::max(i < k ? c0 : c1, acrossOf(loop[i].node));
    const double cross[2] = {c0 / 2, c0 + gapC + c1 / 2};
    for (int i = 0; i < n; ++i) {
        loop[i].a = at_[slotOf(i)];
        loop[i].c = cross[rowOf(i)];
    }
    const auto xy = [&](double pa, double pc) { return rows ? QPointF(pa, pc) : QPointF(pc, pa); };
    const auto straightTo = [](const QPointF &p, const QPointF &q) {
        return straight(p.x(), p.y(), q.x(), q.y());
    };
    QVector<Routed> result;
    for (int i = 0; i < n; ++i) {
        const Step &step = loop[i], &to = loop[(i + 1) % n];
        const Edge &e = *step.edge;
        const double cut = e.head != QLatin1String("none") ? ARROW.length : 0,
                     lead = e.both && cut ? cut : 0;
        Pts pts;
        QPointF mid;
        if (runs(i)) {
            const double s = signOf(to.a - step.a);
            const QPointF p(step.a + s * along(step.node) / 2 + s * lead, step.c),
                q(to.a - s * along(to.node) / 2 - s * cut, to.c);
            const QPointF pp = xy(p.x(), p.y()), qq = xy(q.x(), q.y());
            pts << pp.x() << pp.y();
            pts += straightTo(pp, qq);
            mid = xy((p.x() + q.x()) / 2, p.y());
        } else if (slotOf(i) == slotOf((i + 1) % n)) {
            const double s = signOf(to.c - step.c);
            const QPointF p(step.a, step.c + s * acrossOf(step.node) / 2 + s * lead),
                q(to.a, to.c - s * acrossOf(to.node) / 2 - s * cut);
            const QPointF pp = xy(p.x(), p.y()), qq = xy(q.x(), q.y());
            pts << pp.x() << pp.y();
            pts += straightTo(pp, qq);
            mid = xy(p.x(), (p.y() + q.y()) / 2);
        } else {
            // An odd cycle closes round a corner: out of the side of the last block, and up
            // into the first.
            const QPointF p(step.a - along(step.node) / 2 - lead, step.c),
                q(to.a, to.c + acrossOf(to.node) / 2 + cut);
            const double r = std::min({14.0, (p.x() - q.x()) / 2, (p.y() - q.y()) / 2});
            const QPointF bend = xy(q.x(), p.y()), before = xy(q.x() + r, p.y()),
                          after = xy(q.x(), p.y() - r);
            const QPointF pp = xy(p.x(), p.y()), qq = xy(q.x(), q.y());
            pts << pp.x() << pp.y();
            pts += straightTo(pp, before);
            pts << bend.x() << bend.y() << bend.x() << bend.y() << after.x() << after.y();
            pts += straightTo(after, qq);
            mid = xy(q.x(), (p.y() - r + q.y()) / 2);
        }
        Routed routed;
        routed.edge = e;
        routed.from = step.node->id;
        routed.to = to.node->id;
        routed.pts = pts;
        routed.labelAt = mid;
        routed.labelLines = step.lines;
        routed.labelW = step.lw;
        routed.labelH = step.lh;
        result << routed;
    }
    const double alongEnd = at_[k - 1] + size[k - 1] / 2, acrossEnd = c0 + gapC + c1;
    // A caption on an arrow at the edge of the cycle may reach past the blocks: the drawing
    // makes room for it.
    double x0 = 0, y0 = 0, x1 = rows ? alongEnd : acrossEnd, y1 = rows ? acrossEnd : alongEnd;
    for (int i = 0; i < n; ++i) {
        if (!loop[i].labelled)
            continue;
        const Routed &e = result[i];
        x0 = std::min(x0, e.labelAt.x() - e.labelW / 2);
        x1 = std::max(x1, e.labelAt.x() + e.labelW / 2);
        y0 = std::min(y0, e.labelAt.y() - e.labelH / 2);
        y1 = std::max(y1, e.labelAt.y() + e.labelH / 2);
    }
    Layout layout;
    for (int i = 0; i < n; ++i) {
        const QPointF p = xy(loop[i].a, loop[i].c);
        layout.places << Place{loop[i].node, p.x() - x0, p.y() - y0, double(i), 0};
        layout.hints.insert(loop[i].node->id, loop[i].c);
    }
    for (auto e : result) {
        e.pts = shiftPts(e.pts, -x0, -y0);
        e.labelAt -= QPointF(x0, y0);
        layout.edges << e;
    }
    layout.width = x1 - x0;
    layout.height = y1 - y0;
    return layout;
}

// Blocks joined to nothing stand in rows under the rest, instead of one long rank.
std::optional<Layout> packLevel(Ctx &c, const QVector<const Node *> &nodes,
                                const QVector<Edge> &edges, const QString &dir, const Hints &hints,
                                double room, const Layout &full)
{
    QSet<QString> linked;
    for (const auto &e : edges) {
        if (e.from != e.to && (e.style != QLatin1String("hidden") || e.lift)) {
            linked.insert(e.from);
            linked.insert(e.to);
        }
    }
    QVector<const Node *> loose, core;
    for (const Node *node : nodes)
        (linked.contains(node->id) ? core : loose) << node;
    if (loose.size() < 2)
        return full;
    Layout main;
    if (!core.isEmpty()) {
        QVector<Edge> kept;
        for (const auto &e : edges) {
            if (linked.contains(e.from) && linked.contains(e.to))
                kept << e;
        }
        auto laid = layoutFlow(c, core, kept, dir, hints);
        if (!laid)
            return std::nullopt;
        main = *laid;
    }
    int greedy = 0;
    double greedyW = 0;
    for (const Node *node : loose) {
        if (greedy && greedyW + FLOW_GAP.node + node->w <= room) {
            greedyW += FLOW_GAP.node + node->w;
        } else {
            ++greedy;
            greedyW = node->w;
        }
    }
    const int per = int(std::ceil(double(loose.size()) / greedy));
    struct Row {
        QVector<const Node *> nodes;
        double w, h;
    };
    QVector<Row> rows;
    for (const Node *node : loose) {
        if (!rows.isEmpty() && rows.last().nodes.size() < per &&
            rows.last().w + FLOW_GAP.node + node->w <= room) {
            rows.last().nodes << node;
            rows.last().w += FLOW_GAP.node + node->w;
            rows.last().h = std::max(rows.last().h, node->h);
        } else {
            rows << Row{{node}, node->w, node->h};
        }
    }
    double width = main.width;
    for (const auto &row : rows)
        width = std::max(width, row.w);
    const double dx = (width - main.width) / 2;
    double depth = -1;
    for (const auto &p : main.places)
        depth = std::max(depth, p.rank);
    depth += 1;
    Layout layout;
    layout.hints = main.hints;
    for (auto p : main.places) {
        p.cx += dx;
        layout.places << p;
    }
    double y = main.height ? main.height + FLOW_GAP.rank : 0;
    for (int r = 0; r < rows.size(); ++r) {
        double x = (width - rows[r].w) / 2;
        for (int i = 0; i < rows[r].nodes.size(); ++i) {
            const Node *node = rows[r].nodes[i];
            layout.places << Place{node, x + node->w / 2, y + rows[r].h / 2, depth + r, double(i)};
            x += node->w + FLOW_GAP.node;
        }
        y += rows[r].h + (r < rows.size() - 1 ? FLOW_GAP.node : 0);
    }
    for (const auto &p : layout.places)
        layout.hints.insert(p.node->id, p.cx);
    for (auto e : main.edges) {
        e.pts = shiftPts(e.pts, dx, 0);
        e.labelAt.rx() += dx;
        layout.edges << e;
    }
    layout.width = width;
    layout.height = y;
    return layout;
}

Pts sCurve(const QPointF &a, const QPointF &b, bool side)
{
    if (side) {
        const double m = (a.x() + b.x()) / 2;
        return {m, a.y(), m, b.y(), b.x(), b.y()};
    }
    const double m = (a.y() + b.y()) / 2;
    return {a.x(), m, b.x(), m, b.x(), b.y()};
}

// A group of the scheme while it is laid out: its blocks, its groups and the links among them.
struct Level {
    Group group;
    bool root = false;
    QVector<const Node *> nodes;
    QVector<Level *> kids;
    QVector<Edge> edges;
    Layout layout;
    double w = 0, h = 0;
    Node node; // The group standing as a block in its parent's layout.
    QString used;
};

std::optional<Layout> layoutGraph(Ctx &c, const Graph &graph, const QString &dir,
                                  const Hints &hints, double room, bool pack = false,
                                  const QString &inner = {})
{
    // A scheme that is one cycle reads round in the direction it was written; rows too wide
    // for the room give way to columns.
    const auto wheel = [&](const QVector<const Node *> &nodes, const QVector<Edge> &edges,
                           const QString &way) -> std::optional<Layout> {
        auto rows = layoutRing(c, nodes, edges, lying(way));
        return rows && lying(way) && rows->width > room ? layoutRing(c, nodes, edges, false) : rows;
    };
    if (graph.groups.isEmpty()) {
        QVector<const Node *> all;
        for (const auto &node : graph.nodes)
            all << &node;
        if (auto round = wheel(all, graph.edges, dir))
            return round;
        std::optional<Layout> layout =
            all.isEmpty() ? Layout() : layoutFlow(c, all, graph.edges, dir, hints);
        if (!layout)
            return std::nullopt;
        return pack && layout->width > room
                   ? packLevel(c, all, graph.edges, dir, hints, room, *layout)
                   : layout;
    }
    std::deque<Level> levels;
    levels.emplace_back();
    Level *root = &levels.back();
    root->root = true;
    QHash<QString, Level *> groups;
    QVector<Level *> groupList;
    for (const auto &g : graph.groups) {
        levels.emplace_back();
        levels.back().group = g;
        groups.insert(g.id, &levels.back());
        groupList << &levels.back();
    }
    const auto levelOf = [&](const QString &id) { return groups.value(id, root); };
    for (Level *g : groupList)
        levelOf(g->group.parent)->kids << g;
    QHash<QString, const Node *> byId;
    for (const auto &node : graph.nodes) {
        byId.insert(node.id, &node);
        levelOf(node.grouped ? node.group : QString())->nodes << &node;
    }
    const auto up = [&](const QString &id) -> QString {
        if (groups.contains(id))
            return groups.value(id)->group.parent;
        const Node *node = byId.value(id);
        return node && node->grouped ? node->group : QString();
    };
    const auto path = [&](const QString &id) {
        QStringList out{id};
        QString cur = up(id);
        for (int k = 0; k < 64; ++k) {
            out << cur;
            if (cur.isEmpty())
                break;
            cur = up(cur);
        }
        return out;
    };
    struct Lifted {
        Edge edge;
        Level *level;
        QString ru, rv;
    };
    QVector<Lifted> lifted;
    for (const auto &e : graph.edges) {
        if (e.from == e.to)
            continue;
        const QStringList pu = path(e.from), pv = path(e.to);
        QString lca;
        for (int i = 1; i < pu.size(); ++i) {
            if (pv.indexOf(pu[i], 1) >= 1) {
                lca = pu[i];
                break;
            }
        }
        const int iu = int(pu.indexOf(lca, 1)), iv = int(pv.indexOf(lca, 1));
        if (iu < 1 || iv < 1)
            continue;
        const QString ru = pu[iu - 1], rv = pv[iv - 1];
        if (ru.isEmpty() || rv.isEmpty() || ru == rv)
            continue;
        Level *level = levelOf(lca);
        if (ru == e.from && rv == e.to) {
            level->edges << e;
            continue;
        }
        lifted << Lifted{e, level, ru, rv};
        const bool known =
            std::any_of(level->edges.cbegin(), level->edges.cend(),
                        [&](const Edge &x) { return x.lift && x.from == ru && x.to == rv; });
        if (!known) {
            Edge lift;
            lift.from = ru;
            lift.to = rv;
            lift.label = e.label;
            lift.style = QStringLiteral("hidden");
            lift.lift = true;
            level->edges << lift;
        }
    }
    for (auto &level : levels) {
        QVector<Edge> kept;
        for (const auto &x : level.edges) {
            const bool shadowed =
                x.lift && std::any_of(level.edges.cbegin(), level.edges.cend(), [&](const Edge &y) {
                    return !y.lift && y.from == x.from && y.to == x.to;
                });
            if (!shadowed)
                kept << x;
        }
        level.edges = kept;
    }
    // The blocks an arrow comes into from another group, in the order written.
    QStringList entered;
    for (const auto &l : lifted)
        entered << l.edge.to;

    std::function<std::optional<Layout>(Level *, const QString &, double, int)> lay =
        [&](Level *level, const QString &levelDir, double budget,
            int depth) -> std::optional<Layout> {
        if (depth > MaxDepth || c.cancelled())
            return std::nullopt;
        for (Level *kid : level->kids) {
            // `inner` is the way the groups run inside when the scheme itself is laid another way.
            const QString kidDir = !kid->group.dir.isEmpty()         ? kid->group.dir
                                   : level->root && !inner.isEmpty() ? inner
                                                                     : levelDir;
            auto laid =
                lay(kid, kidDir, std::max(CLUSTER.min, budget - CLUSTER.padX * 2), depth + 1);
            if (!laid)
                return std::nullopt;
            kid->layout = *laid;
            const double titleW = kid->group.title.isEmpty()
                                      ? 0
                                      : capsWidth(c, kid->group.title.toUpper()) + CLUSTER.padX * 2;
            kid->w = std::ceil(std::max(
                {CLUSTER.min, kid->layout.width + CLUSTER.padX * 2, std::min(titleW, budget)}));
            kid->h = std::ceil(CLUSTER.head + (kid->layout.height
                                                   ? kid->layout.height + CLUSTER.padBottom
                                                   : CLUSTER.empty));
            kid->node = Node();
            kid->node.id = kid->group.id;
            kid->node.label = kid->group.title;
            kid->node.shape = QStringLiteral("cluster");
            kid->node.w = kid->w;
            kid->node.h = kid->h;
            kid->node.seq = kid->group.seq;
            kid->node.cluster = kid;
        }
        QVector<const Node *> members = level->nodes;
        for (Level *kid : level->kids)
            members << &kid->node;
        jsSort(members, [](const Node *a, const Node *b) {
            return double(std::max(0, a->seq) - std::max(0, b->seq));
        });
        level->used = levelDir;
        if (members.isEmpty())
            return Layout();
        // A group that is one cycle stands across the way the scheme runs, so the arrow that
        // comes into it meets its first block and the arrow out leaves from the far side.
        std::optional<Layout> round;
        if (level->root) {
            round = wheel(members, level->edges, levelDir);
        } else {
            std::optional<QString> first;
            for (const QString &id : entered) {
                if (std::any_of(members.cbegin(), members.cend(),
                                [&](const Node *node) { return node->id == id; })) {
                    first = id;
                    break;
                }
            }
            round = layoutRing(c, members, level->edges, !lying(levelDir), first);
        }
        if (round)
            return round;
        auto layout = layoutFlow(c, members, level->edges, levelDir, hints);
        if (!layout)
            return std::nullopt;
        if ((level->root && !pack) || layout->width <= budget)
            return layout;
        layout = packLevel(c, members, level->edges, levelDir, hints, budget, *layout);
        if (!layout)
            return std::nullopt;
        if (!level->root && layout->width > budget && lying(levelDir)) {
            auto td = layoutFlow(c, members, level->edges, QStringLiteral("TD"), hints);
            if (!td)
                return std::nullopt;
            auto down =
                packLevel(c, members, level->edges, QStringLiteral("TD"), hints, budget, *td);
            if (!down)
                return std::nullopt;
            if (down->width < layout->width) {
                layout = down;
                level->used = QStringLiteral("TD");
            }
        }
        return layout;
    };
    auto top = lay(root, dir, room, 0);
    if (!top)
        return std::nullopt;
    // Groups set side by side stand on one line at the top, like the columns of a table, unless
    // an arrow drawn to a group as a whole would be left behind by the move.
    if (!inner.isEmpty() && lying(dir)) {
        const auto held = [&](const QString &id) {
            return std::any_of(top->edges.cbegin(), top->edges.cend(), [&](const Routed &e) {
                return !e.edge.lift && e.edge.style != QLatin1String("hidden") &&
                       (e.from == id || e.to == id);
            });
        };
        QVector<int> kids;
        for (int i = 0; i < top->places.size(); ++i) {
            if (top->places[i].node->cluster)
                kids << i;
        }
        if (kids.size() > 1 && std::none_of(kids.cbegin(), kids.cend(),
                                            [&](int i) { return held(top->places[i].node->id); })) {
            double head = INFINITY;
            for (const int i : kids)
                head = std::min(head, top->places[i].cy - top->places[i].node->h / 2);
            for (const int i : kids)
                top->places[i].cy = head + top->places[i].node->h / 2;
        }
    }

    Layout result;
    struct Box {
        double x, y, w, h;
    };
    // Boxes of blocks and groups by id, in the order first set (Map order).
    QVector<Box> boxes;
    QHash<QString, int> boxAt;
    const auto setBox = [&](const QString &id, const Box &box) {
        const int at = boxAt.value(id, -1);
        if (at >= 0) {
            boxes[at] = box;
        } else {
            boxAt.insert(id, int(boxes.size()));
            boxes << box;
        }
    };
    QHash<QString, Routed> routes;
    const bool across = lying(dir);
    std::function<void(Level *, double, double, int)> put = [&](Level *level, double ox, double oy,
                                                                int depth) {
        for (auto it = level->layout.hints.cbegin(); it != level->layout.hints.cend(); ++it)
            result.hints.insert(it.key(), it.value());
        for (const auto &p : level->layout.places) {
            const double cx = ox + p.cx, cy = oy + p.cy;
            const Node *n = p.node;
            setBox(n->id, Box{cx - n->w / 2, cy - n->h / 2, n->w, n->h});
            if (!n->cluster) {
                result.places << Place{n, cx, cy, (across ? cx : cy) / 110,
                                       (across ? cy : cx) / 400};
                continue;
            }
            Level *k = n->cluster;
            const double x = cx - k->w / 2, y = cy - k->h / 2;
            result.clusters << ClusterBox{k->group.id,
                                          caps(c, k->group.title, k->w - CLUSTER.padX * 2),
                                          x,
                                          y,
                                          k->w,
                                          k->h,
                                          (across ? x : y) / 110,
                                          depth};
            if (depth < MaxDepth)
                put(k, x + (k->w - k->layout.width) / 2, y + CLUSTER.head, depth + 1);
        }
        for (const auto &e : level->layout.edges) {
            Routed moved = e;
            moved.pts = shiftPts(e.pts, ox, oy);
            moved.labelAt += QPointF(ox, oy);
            const QString key = e.from + QLatin1Char('>') + e.to;
            if (!routes.contains(key) || e.edge.lift)
                routes.insert(key, moved);
            if (!e.edge.lift)
                result.edges << moved;
        }
    };
    root->layout = *top;
    put(root, 0, 0, 0);

    for (const auto &l : lifted) {
        const Edge &edge = l.edge;
        const int ia = boxAt.value(edge.from, -1), ib = boxAt.value(edge.to, -1),
                  iA = boxAt.value(l.ru, -1), iB = boxAt.value(l.rv, -1);
        if (ia < 0 || ib < 0 || iA < 0 || iB < 0)
            continue;
        const Box a = boxes[ia], b = boxes[ib], A = boxes[iA], B = boxes[iB];
        const bool side = lying(l.level->used);
        const auto lo = [&](const Box &box) {
            return side ? qMakePair(box.x, box.x + box.w) : qMakePair(box.y, box.y + box.h);
        };
        const auto mid = [&](const Box &box) {
            return side ? box.y + box.h / 2 : box.x + box.w / 2;
        };
        const double s = (lo(B).first + lo(B).second) >= (lo(A).first + lo(A).second) ? 1 : -1;
        const auto at = [&](const Box &box, double sign) {
            return sign > 0 ? lo(box).second : lo(box).first;
        };
        const auto point = [&](double cross, double main) {
            return side ? QPointF(main, cross) : QPointF(cross, main);
        };
        const auto setMain = [&](QPointF &p, double delta) {
            if (side)
                p.rx() += delta;
            else
                p.ry() += delta;
        };
        QVector<QPointF> inner;
        // The bends of the way between the two groups are kept, but not the bow a pair of
        // opposite arrows is given: these arrows start from their own blocks.
        const auto route = routes.constFind(l.ru + QLatin1Char('>') + l.rv);
        if (route != routes.constEnd() && !route->bulged) {
            for (int i = 6; i < route->pts.size() - 2; i += 6)
                inner << QPointF(route->pts[i], route->pts[i + 1]);
        }
        // Between a block and the edge of its group an arrow runs straight, unless other blocks
        // stand in the way: then it swings into the nearest free lane, or along the group's edge.
        const auto band = [&](const Box &box) {
            return side ? qMakePair(box.y, box.y + box.h) : qMakePair(box.x, box.x + box.w);
        };
        const auto within = [&](int box, int outer) {
            const Box &x = boxes[box], &o = boxes[outer];
            return box != outer && x.x >= o.x && x.y >= o.y && x.x + x.w <= o.x + o.w &&
                   x.y + x.h <= o.y + o.h;
        };
        struct LaneAt {
            double cross, turn;
        };
        const auto lane = [&](int own, int group, double dir) -> std::optional<LaneAt> {
            const Box &ownBox = boxes[own], &groupBox = boxes[group];
            const double from = at(ownBox, dir), to = at(groupBox, dir), want = mid(ownBox);
            QVector<int> ahead;
            for (int i = 0; i < boxes.size(); ++i) {
                if (i == own || !within(i, group) || within(own, i))
                    continue;
                const auto span = lo(boxes[i]);
                if (dir > 0 ? span.first >= from - 1 && span.first < to
                            : span.second <= from + 1 && span.second > to)
                    ahead << i;
            }
            const auto free = [&](double cross) {
                return std::none_of(ahead.cbegin(), ahead.cend(), [&](int i) {
                    const auto bnd = band(boxes[i]);
                    return cross > bnd.first - 6 && cross < bnd.second + 6;
                });
            };
            if (free(want))
                return std::nullopt;
            const auto [g0, g1] = band(groupBox);
            // A lane across the band where the group's name stands is taken only when there is
            // no other.
            const double head = side ? groupBox.y + CLUSTER.head + 10 : -INFINITY;
            QVector<double> lanes;
            QVector<double> candidates;
            for (const int i : ahead)
                candidates << band(boxes[i]).first - 10 << band(boxes[i]).second + 10;
            candidates << g0 + 7 << g1 - 7;
            for (const double cross : candidates) {
                if (cross >= g0 + 6 && cross <= g1 - 6 && free(cross))
                    lanes << cross;
            }
            QVector<double> options;
            for (const double cross : lanes) {
                if (cross > head)
                    options << cross;
            }
            if (options.isEmpty())
                options = lanes;
            if (options.isEmpty())
                return std::nullopt;
            double nearEdge = dir > 0 ? INFINITY : -INFINITY;
            for (const int i : ahead)
                nearEdge = dir > 0 ? std::min(nearEdge, lo(boxes[i]).first)
                                   : std::max(nearEdge, lo(boxes[i]).second);
            double best = options[0];
            for (int i = 1; i < options.size(); ++i) {
                if (std::abs(options[i] - want) < std::abs(best - want))
                    best = options[i];
            }
            return LaneAt{best, nearEdge - dir * 8};
        };
        const std::optional<LaneAt> out = ia != iA ? lane(ia, iA, s) : std::nullopt;
        const std::optional<LaneAt> into = ib != iB ? lane(ib, iB, -s) : std::nullopt;
        // An arrow that comes down into a group through the line of its name would cross the
        // name: it stops at the edge of the group instead, over the group's first block.
        const QString name = !side && s > 0 && ib != iB && !into && groups.contains(l.rv)
                                 ? groups.value(l.rv)->group.title
                                 : QString();
        bool stop = false;
        if (!name.isEmpty()) {
            const double w = capsWidth(c, caps(c, name, B.w - CLUSTER.padX * 2));
            stop = std::abs(mid(b) - (B.x + CLUSTER.padX + w / 2)) < w / 2 + 6;
        }
        // The way as points; `flat` marks one reached in a straight line rather than a bend.
        struct WayPoint {
            QPointF p;
            bool flat = false;
        };
        QVector<WayPoint> way{{point(mid(a), at(a, s)), false}};
        if (out)
            way << WayPoint{point(out->cross, out->turn), false}
                << WayPoint{point(out->cross, at(A, s)), true};
        else if (ia != iA)
            way << WayPoint{point(mid(a), at(A, s)), true};
        for (const auto &p : inner)
            way << WayPoint{p, false};
        if (into)
            way << WayPoint{point(into->cross, at(B, -s)), false}
                << WayPoint{point(into->cross, into->turn), true};
        else if (ib != iB && !stop)
            way << WayPoint{point(mid(b), at(B, -s)), false};
        QPointF end = stop ? point(mid(b), at(B, -s)) : point(mid(b), at(b, -s));
        if (edge.head != QLatin1String("none"))
            setMain(end, -s * ARROW.length);
        if (edge.both && edge.head != QLatin1String("none"))
            setMain(way[0].p, s * ARROW.length);
        way << WayPoint{end, ib != iB && !into && !stop};
        Pts pts{way[0].p.x(), way[0].p.y()};
        QPointF label = way[0].p;
        double longest = -1;
        for (int i = 1; i < way.size(); ++i) {
            const QPointF p = way[i - 1].p, q = way[i].p;
            const bool flat = way[i].flat;
            pts += flat ? straight(p.x(), p.y(), q.x(), q.y()) : sCurve(p, q, side);
            const double span = std::hypot(q.x() - p.x(), q.y() - p.y()) - (flat ? 1e4 : 0);
            if (span > longest) {
                longest = span;
                label = (p + q) / 2;
            }
        }
        Routed routed;
        routed.edge = edge;
        routed.from = edge.from;
        routed.to = edge.to;
        routed.pts = pts;
        routed.labelAt = label;
        if (!edge.label.isEmpty()) {
            routed.labelLines = wrap(c, edge.label, 140, FONT::small);
            routed.labelW = textMax(c, routed.labelLines, FONT::small) + 14;
            routed.labelH = routed.labelLines.size() * 15 + 6;
        }
        result.edges << routed;
    }
    result.width = top->width;
    result.height = top->height;
    return result;
}

/* Scene */

std::optional<Result> flowScene(Ctx &c, std::optional<Graph> parsed, const QString &dialect)
{
    if (!parsed)
        return std::nullopt;
    Graph &graph = *parsed;
    for (auto &node : graph.nodes)
        sizeNode(c, node);
    const Hints &hints = c.options.hints;
    const QString &sideways = c.options.sideways;
    const bool side = lying(graph.dir);
    const QString alt = side ? QStringLiteral("TD") : QStringLiteral("LR");
    bool turned = !sideways.isEmpty() && sideways == graph.dir;
    const double room = c.width - PAD * 2;
    const auto layOut = [&](const QString &d, bool pack = false, const QString &inner = {}) {
        return c.cancelled() ? std::nullopt : layoutGraph(c, graph, d, hints, room, pack, inner);
    };
    auto layout = layOut(turned ? alt : graph.dir);
    if (!layout)
        return std::nullopt;
    // A scheme stood on end lies down again the way it was written once the room takes it
    // whole; between that and the width that stood it up it stays, so it does not flip.
    if (turned) {
        auto flat = layOut(graph.dir);
        if (!flat)
            return std::nullopt;
        if (flat->width <= room) {
            layout = flat;
            turned = false;
        }
    }
    if (turned && layout->width > room) {
        auto packed = layOut(alt, true), back = layOut(graph.dir, true);
        if (!packed || !back)
            return std::nullopt;
        if (packed->width < layout->width)
            layout = packed;
        if (std::min(1.0, room / back->width) > std::min(1.0, room / layout->width) + TURN.gain) {
            layout = back;
            turned = false;
        }
    }
    if (side && !turned && layout->width * SIDE_FIT > room) {
        auto down = layOut(QStringLiteral("TD"));
        if (!down)
            return std::nullopt;
        if (std::min(1.0, room / down->width) > room / layout->width + 0.05) {
            layout = down;
            turned = true;
        }
    }
    // Groups in one tall narrow tower are set side by side, each still running down inside:
    // the drawing takes the width it has, and arrows between the groups keep clear of names.
    bool beside = false;
    int rootGroups = 0;
    for (const auto &group : graph.groups) {
        if (group.parent.isEmpty())
            ++rootGroups;
    }
    if (!side && !turned && rootGroups > 1 &&
        (sideways == QLatin1String("beside") ||
         (layout->height > BESIDE.tall && layout->height > layout->width * BESIDE.ratio))) {
        auto columns = layOut(QStringLiteral("LR"), false, QStringLiteral("TD"));
        if (!columns)
            return std::nullopt;
        if (columns->width * SIDE_FIT <= room && (sideways == QLatin1String("beside") ||
                                                  columns->height < layout->height * BESIDE.gain)) {
            layout = columns;
            beside = true;
        }
    }
    if (!beside && layout->width > room) {
        auto packed = layOut(turned ? alt : graph.dir, true);
        if (!packed)
            return std::nullopt;
        if (packed->width < layout->width)
            layout = packed;
    }
    if (!side && !turned && !beside && layout->width * TURN.fit > room) {
        auto across = layOut(alt, true);
        if (!across)
            return std::nullopt;
        if (std::min(1.0, room / across->width) > room / layout->width + TURN.gain) {
            layout = across;
            turned = true;
        }
    }
    const int accent = first(c.tones);
    Result result;
    QHash<QString, double> rank;
    // A scheme may carry a title, given in the block Mermaid opens a diagram with: it stands
    // over the drawing.
    double dy = 0, titleW = 0;
    if (!graph.title.isEmpty()) {
        const Title title = titleOf(c, graph.title, std::max(240.0, room - 80));
        result.items << title.item;
        titleW = title.width;
        dy = CHART.head + 6;
        Corner corner;
        corner.x = title.width + 16;
        corner.h = 28;
        result.corner = corner;
    }
    for (const auto &cl : layout->clusters) {
        rank.insert(cl.id, cl.rank);
        Spec spec =
            item(Type::Cluster, QStringLiteral("g:") + cl.id, cl.rank - 0.3 + cl.depth * 0.1);
        spec.props.x = cl.x;
        spec.props.y = cl.y + dy;
        spec.props.w = cl.w;
        spec.props.h = cl.h;
        spec.fixed.title = cl.title;
        spec.fixed.depth = cl.depth;
        result.items << spec;
    }
    QSet<QString> into, out;
    for (const auto &e : graph.edges) {
        if (e.from == e.to)
            continue;
        out.insert(e.from);
        into.insert(e.to);
    }
    const auto pseudoShape = [](const Node *node) {
        return node->shape == QLatin1String("start") || node->shape == QLatin1String("end");
    };
    // Where a process starts and where it ends are its key blocks, when there are few of them.
    QVector<const Place *> real;
    for (const auto &p : layout->places) {
        if (!pseudoShape(p.node))
            real << &p;
    }
    QSet<QString> keys;
    if (dialect == QLatin1String("flow") && real.size() > 3) {
        QVector<const Place *> sources, sinks;
        for (const Place *p : real) {
            if (!into.contains(p->node->id))
                sources << p;
            if (!out.contains(p->node->id) && into.contains(p->node->id))
                sinks << p;
        }
        if (sources.size() <= 2) {
            for (const Place *p : sources)
                keys.insert(p->node->id);
        }
        if (sinks.size() <= 2) {
            for (const Place *p : sinks)
                keys.insert(p->node->id);
        }
    }
    for (const auto &place : layout->places) {
        const Node *node = place.node;
        const bool pseudo = pseudoShape(node);
        rank.insert(node->id, place.rank);
        result.labels.insert(node->id, node->text.value_or(node->label));
        Spec spec =
            item(Type::Node, QStringLiteral("n:") + node->id, place.rank + place.order * 0.25);
        spec.props.x = place.cx;
        spec.props.y = place.cy + dy;
        spec.props.w = node->w;
        spec.props.h = node->h;
        spec.fixed.shape = node->shape;
        spec.fixed.lines = node->lines;
        spec.fixed.headLines = node->head;
        spec.fixed.tone = accent;
        spec.fixed.id = node->id;
        spec.fixed.key = pseudo || keys.contains(node->id);
        spec.fixed.card = node->card;
        spec.fixed.cardKey = node->card ? node->card->key : QString();
        result.items << spec;
    }
    QHash<QString, int> seen;
    for (const auto &edge : layout->edges) {
        if (edge.edge.style == QLatin1String("hidden"))
            continue;
        const QString base = edge.from + QLatin1Char('>') + edge.to;
        const int n = seen.value(base, 0);
        seen.insert(base, n + 1);
        const QString key = QStringLiteral("e:%1:%2").arg(base).arg(n);
        // Every edge leaves a block that is drawn; 0 stands for a rank never given.
        const double order = rank.value(edge.from, 0) + 0.55;
        Spec spec = item(Type::Edge, key, order);
        spec.props.pts = dy ? shiftPts(edge.pts, 0, dy) : edge.pts;
        spec.fixed.style = edge.edge.style;
        spec.fixed.head = edge.edge.head;
        spec.fixed.both = edge.edge.both;
        spec.fixed.hasEnds = edge.edge.hasEnds;
        spec.fixed.endStart = edge.edge.endStart;
        spec.fixed.endEnd = edge.edge.endEnd;
        spec.fixed.tone = accent;
        spec.fixed.a = edge.from;
        spec.fixed.b = edge.to;
        result.items << spec;
        if (!edge.edge.label.isEmpty()) {
            Spec label = item(Type::Label, QStringLiteral("l:") + key, order + 0.3);
            label.props.x = edge.labelAt.x();
            label.props.y = edge.labelAt.y() + dy;
            label.fixed.lines = edge.labelLines;
            label.fixed.pill = true;
            label.fixed.w = edge.labelW;
            label.fixed.h = edge.labelH;
            label.fixed.lineHeight = 15;
            label.fixed.cls = QStringLiteral("dg-edge-label");
            label.fixed.pop = true;
            result.items << label;
        }
    }
    result.kind = QStringLiteral("flow");
    result.dialect = dialect;
    result.width = std::max(layout->width, titleW);
    result.height = layout->height + dy;
    result.hints = layout->hints;
    result.sideways = beside ? QStringLiteral("beside") : turned ? graph.dir : QString();
    result.edges = true;
    return result;
}
} // namespace

std::optional<Result> flowKind(Ctx &c, const Lines &lines, const QString &dialect)
{
    std::optional<Graph> graph = dialect == QLatin1String("state")   ? parseState(c, lines.lines)
                                 : dialect == QLatin1String("er")    ? parseER(c, lines.lines)
                                 : dialect == QLatin1String("class") ? parseClass(c, lines.lines)
                                                                     : parseFlow(c, lines.lines);
    // titled: a scheme takes the title its lines came with, unless it names one itself.
    if (graph && graph->title.isEmpty())
        graph->title = lines.title;
    return flowScene(c, std::move(graph), dialect);
}

} // namespace diagram
