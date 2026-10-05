#include "tex.h"

#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

// A port of openghost/tex.js: the same recursive-descent reading of the
// source, but each construct is laid out as a box (width, ascent, descent and
// positioned items) following the math CSS in openghost/styles.css (.md-math,
// .md-imath, .mf, .ms, .mdel, .mgrid, …) instead of producing HTML. Lengths
// are in logical pixels; y grows downwards and a box's baseline is y = 0.
namespace
{
constexpr int MaxDepth = 48;     // Deeper atoms are read as literal text.
constexpr int MaxItems = 20000;  // Glyph runs, rules and shapes per formula.
constexpr int MaxRows = 256;     // Per environment.
constexpr int MaxColumns = 64;   // Per environment.
constexpr qreal MaxWidth = 8192; // Logical output bounds.
constexpr qreal MaxHeight = 4096;
constexpr qreal MaxPixels = 16e6; // At the device pixel ratio.
constexpr qreal MinFont = 5;      // Logical pixels.
constexpr qreal Axis = 0.25;      // CSS vertical-align: middle, above the baseline, in em.
constexpr qreal Ascent = 0.72, Descent = 0.24; // A text atom's box, in em.

// Scalable shapes: the CSS masks' SVG paths, in their own view boxes.
enum Shape : quint8 {
    NoShape,
    ParenL,
    ParenR,
    BracketL,
    BracketR,
    BraceL,
    BraceR,
    Bar,
    DoubleBar,
    AngleL,
    AngleR,
    Radical,
};

const char *const ShapePaths[] = {
    "",
    "M9.2 0.8C3.6 7 1.9 13.4 1.9 20s1.7 13 7.3 19.2l0.9-0.8C5.2 32.4 4.1 26.4 4.1 20S5.2 7.6 "
    "10.1 1.6z",
    "M0.8 0.8C6.4 7 8.1 13.4 8.1 20s-1.7 13-7.3 19.2l-0.9-0.8C4.8 32.4 5.9 26.4 5.9 20S4.8 "
    "7.6-0.1 1.6z",
    "M8.5 1H2.5v38h6v-1.6H4.3V2.6h4.2z",
    "M1.5 1h6v38h-6v-1.6h4.2V2.6H1.5z",
    "M8.6 1C5.6 1 4.6 2.6 4.6 5.6v10.2c0 2.3-1 3.6-3.4 4.2 2.4 0.6 3.4 1.9 3.4 4.2v10.2c0 3 1 "
    "4.6 4 4.6v-1.4c-1.8 0-2.3-1-2.3-3.4V24c0-2.3-0.9-3.4-2.3-4 1.4-0.6 2.3-1.7 2.3-4V5.8c0-2.4 "
    "0.5-3.4 2.3-3.4z",
    "M1.4 1c3 0 4 1.6 4 4.6v10.2c0 2.3 1 3.6 3.4 4.2-2.4 0.6-3.4 1.9-3.4 4.2v10.2c0 3-1 4.6-4 "
    "4.6v-1.4c1.8 0 2.3-1 2.3-3.4V24c0-2.3 0.9-3.4 2.3-4-1.4-0.6-2.3-1.7-2.3-4V5.8c0-2.4-0.5-3.4-"
    "2.3-3.4z",
    "M4.25 0.5h1.5v39h-1.5z",
    "M2.6 0.5h1.4v39H2.6zM6 0.5h1.4v39H6z",
    "M8.2 0.8L2.2 20l6 19.2 1.3-0.4L3.8 20 9.5 1.2z",
    "M1.8 0.8l6 19.2-6 19.2-1.3-0.4L6.2 20 0.5 1.2z",
    "M0.3 23.7L3.5 21.9l2.9 12.6L10.6 0.4H12v1.4h-0.2L7.7 39.6H6.2L2.8 25 1 26.1z",
};

// The trusted constants above use only M L H V C S Z (absolute or relative).
QPainterPath parsePath(const char *d)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    QPointF at, start, control;
    char command = 0;
    const auto number = [&d]() {
        while (*d == ' ' || *d == ',')
            ++d;
        char *end = nullptr;
        const double value = std::strtod(d, &end);
        d = end;
        return value;
    };
    for (;;) {
        while (*d == ' ' || *d == ',')
            ++d;
        if (!*d)
            break;
        if ((*d >= 'A' && *d <= 'Z') || (*d >= 'a' && *d <= 'z'))
            command = *d++;
        const bool relative = command >= 'a';
        const QPointF origin = relative ? at : QPointF();
        switch (command | 0x20) {
        case 'm':
            at = origin + QPointF(number(), 0);
            at.ry() += number();
            path.moveTo(at);
            start = control = at;
            command = relative ? 'l' : 'L'; // Further pairs are lines.
            break;
        case 'l':
            at = origin + QPointF(number(), 0);
            at.ry() += number();
            path.lineTo(at);
            control = at;
            break;
        case 'h':
            at.setX((relative ? at.x() : 0) + number());
            path.lineTo(at);
            control = at;
            break;
        case 'v':
            at.setY((relative ? at.y() : 0) + number());
            path.lineTo(at);
            control = at;
            break;
        case 'c': {
            QPointF c1 = origin + QPointF(number(), 0);
            c1.ry() += number();
            QPointF c2 = origin + QPointF(number(), 0);
            c2.ry() += number();
            at = origin + QPointF(number(), 0);
            at.ry() += number();
            path.cubicTo(c1, c2, at);
            control = c2;
            break;
        }
        case 's': {
            const QPointF c1 = at * 2 - control;
            QPointF c2 = origin + QPointF(number(), 0);
            c2.ry() += number();
            at = origin + QPointF(number(), 0);
            at.ry() += number();
            path.cubicTo(c1, c2, at);
            control = c2;
            break;
        }
        case 'z':
            path.closeSubpath();
            at = control = start;
            break;
        default:
            return path;
        }
    }
    return path;
}

const QPainterPath &shapePath(Shape shape)
{
    static const std::vector<QPainterPath> paths = [] {
        std::vector<QPainterPath> all;
        for (const char *d : ShapePaths)
            all.push_back(parsePath(d));
        return all;
    }();
    return paths.at(shape);
}

using Table = QHash<QString, QString>;

Table table(std::initializer_list<std::pair<const char *, const char *>> pairs)
{
    Table map;
    for (const auto &[key, value] : pairs)
        map.insert(QString::fromUtf8(key), QString::fromUtf8(value));
    return map;
}

QSet<QString> set(const char *words)
{
    QSet<QString> names;
    for (const auto &word : QString::fromUtf8(words).split(QLatin1Char(' '), Qt::SkipEmptyParts))
        names.insert(word);
    return names;
}

// tex.js's tables, verbatim except that HTML entities are plain characters.
struct Tables {
    Table symbols = table({
        {"alpha", "α"},     {"beta", "β"},       {"gamma", "γ"},     {"delta", "δ"},
        {"epsilon", "ϵ"},   {"varepsilon", "ε"}, {"zeta", "ζ"},      {"eta", "η"},
        {"theta", "θ"},     {"vartheta", "ϑ"},   {"iota", "ι"},      {"kappa", "κ"},
        {"lambda", "λ"},    {"mu", "μ"},         {"nu", "ν"},        {"xi", "ξ"},
        {"pi", "π"},        {"varpi", "ϖ"},      {"rho", "ρ"},       {"varrho", "ϱ"},
        {"sigma", "σ"},     {"varsigma", "ς"},   {"tau", "τ"},       {"upsilon", "υ"},
        {"phi", "ϕ"},       {"varphi", "φ"},     {"chi", "χ"},       {"psi", "ψ"},
        {"omega", "ω"},     {"Gamma", "Γ"},      {"Delta", "Δ"},     {"Theta", "Θ"},
        {"Lambda", "Λ"},    {"Xi", "Ξ"},         {"Pi", "Π"},        {"Sigma", "Σ"},
        {"Upsilon", "Υ"},   {"Phi", "Φ"},        {"Psi", "Ψ"},       {"Omega", "Ω"},
        {"infty", "∞"},     {"partial", "∂"},    {"nabla", "∇"},     {"forall", "∀"},
        {"exists", "∃"},    {"nexists", "∄"},    {"emptyset", "∅"},  {"varnothing", "∅"},
        {"hbar", "ℏ"},      {"ell", "ℓ"},        {"Re", "ℜ"},        {"Im", "ℑ"},
        {"aleph", "ℵ"},     {"prime", "′"},      {"degree", "°"},    {"angle", "∠"},
        {"triangle", "△"},  {"square", "□"},     {"checkmark", "✓"}, {"ldots", "…"},
        {"cdots", "⋯"},     {"vdots", "⋮"},      {"ddots", "⋱"},     {"dots", "…"},
        {"langle", "⟨"},    {"rangle", "⟩"},     {"lfloor", "⌊"},    {"rfloor", "⌋"},
        {"lceil", "⌈"},     {"rceil", "⌉"},      {"lvert", "|"},     {"rvert", "|"},
        {"vert", "|"},      {"Vert", "‖"},       {"lbrace", "{"},    {"rbrace", "}"},
        {"backslash", "∖"}, {"quad", " "},       {"qquad", "  "},    {",", " "},
        {":", " "},         {";", " "},          {">", " "},         {"!", ""},
        {" ", " "},         {"{", "{"},          {"}", "}"},         {"%", "%"},
        {"$", "$"},         {"#", "#"},          {"&", "&"},         {"_", "_"},
        {"|", "‖"},
    });
    QSet<QString> greek = set("alpha beta gamma delta epsilon varepsilon zeta eta theta vartheta "
                              "iota kappa lambda mu nu xi pi varpi rho varrho sigma varsigma tau "
                              "upsilon phi varphi chi psi omega");
    QSet<QString> openers = set("langle lfloor lceil lbrace {");
    Table relations = table({
        {"times", "×"},
        {"cdot", "·"},
        {"div", "÷"},
        {"pm", "±"},
        {"mp", "∓"},
        {"ast", "∗"},
        {"star", "⋆"},
        {"circ", "∘"},
        {"bullet", "•"},
        {"oplus", "⊕"},
        {"otimes", "⊗"},
        {"le", "≤"},
        {"leq", "≤"},
        {"ge", "≥"},
        {"geq", "≥"},
        {"ll", "≪"},
        {"gg", "≫"},
        {"ne", "≠"},
        {"neq", "≠"},
        {"approx", "≈"},
        {"equiv", "≡"},
        {"cong", "≅"},
        {"sim", "∼"},
        {"simeq", "≃"},
        {"propto", "∝"},
        {"to", "→"},
        {"rightarrow", "→"},
        {"leftarrow", "←"},
        {"gets", "←"},
        {"leftrightarrow", "↔"},
        {"Rightarrow", "⇒"},
        {"Leftarrow", "⇐"},
        {"Leftrightarrow", "⇔"},
        {"implies", "⇒"},
        {"iff", "⇔"},
        {"mapsto", "↦"},
        {"longrightarrow", "⟶"},
        {"longleftarrow", "⟵"},
        {"uparrow", "↑"},
        {"downarrow", "↓"},
        {"in", "∈"},
        {"notin", "∉"},
        {"ni", "∋"},
        {"subset", "⊂"},
        {"subseteq", "⊆"},
        {"supset", "⊃"},
        {"supseteq", "⊇"},
        {"cup", "∪"},
        {"cap", "∩"},
        {"setminus", "∖"},
        {"land", "∧"},
        {"wedge", "∧"},
        {"lor", "∨"},
        {"vee", "∨"},
        {"neg", "¬"},
        {"lnot", "¬"},
        {"perp", "⊥"},
        {"parallel", "∥"},
        {"mid", "∣"},
        {"nmid", "∤"},
        {"models", "⊨"},
        {"vdash", "⊢"},
    });
    QSet<QString> rel =
        set("= < > ≤ ≥ ≪ ≫ ≠ ≈ ≡ ≅ ∼ ≃ ∝ → ← ↔ ⇒ ⇐ ⇔ ↦ ⟶ ⟵ ↑ ↓ ∈ ∉ ∋ ⊂ ⊆ ⊃ ⊇ ∥ ∣ ∤ ⊨ "
            "⊢ :");
    QSet<QString> unary = set("+ − ± ∓");
    Table big = table({{"sum", "∑"},
                       {"prod", "∏"},
                       {"coprod", "∐"},
                       {"bigcup", "⋃"},
                       {"bigcap", "⋂"},
                       {"bigoplus", "⨁"},
                       {"bigotimes", "⨂"},
                       {"int", "∫"},
                       {"iint", "∬"},
                       {"iiint", "∭"},
                       {"oint", "∮"}});
    QSet<QString> integrals = set("int iint iiint oint");
    QSet<QString> functions =
        set("sin cos tan cot sec csc arcsin arccos arctan sinh cosh tanh coth log ln lg exp lim "
            "liminf limsup max min sup inf det deg gcd lcm mod bmod arg argmax argmin dim ker hom "
            "Pr sgn tr rank");
    QSet<QString> limits = set("lim liminf limsup max min sup inf argmax argmin");
    QSet<QString> text = set("text textrm textbf textit textsf texttt mathrm mathbf mathit mathsf "
                             "mathtt mathcal mathbb mathfrak operatorname mbox hbox boldsymbol bm");
    QSet<QString> silent = set("big Big bigg Bigg bigl bigr Bigl Bigr biggl biggr displaystyle "
                               "textstyle scriptstyle limits nolimits middle nonumber notag");
    Table accents = table({{"vec", "⃗"},
                           {"hat", "̂"},
                           {"widehat", "̂"},
                           {"bar", "̄"},
                           {"overline", "̅"},
                           {"dot", "̇"},
                           {"ddot", "̈"},
                           {"tilde", "̃"},
                           {"widetilde", "̃"}});
    Table spacingAccents = table({{"vec", "→"},
                                  {"hat", "ˆ"},
                                  {"widehat", "ˆ"},
                                  {"dot", "˙"},
                                  {"ddot", "¨"},
                                  {"tilde", "˜"},
                                  {"widetilde", "˜"}});
    Table operators = table({{"=", "="},
                             {"<", "<"},
                             {">", ">"},
                             {"+", "+"},
                             {"-", "−"},
                             {"*", "∗"},
                             {"±", "±"},
                             {"×", "×"},
                             {"÷", "÷"},
                             {"≤", "≤"},
                             {"≥", "≥"},
                             {"≠", "≠"},
                             {"≈", "≈"},
                             {"→", "→"}});
    Table doubleStruck =
        table({{"R", "ℝ"}, {"N", "ℕ"}, {"Z", "ℤ"}, {"Q", "ℚ"}, {"C", "ℂ"}, {"P", "ℙ"}, {"E", "𝔼"}});
    QHash<QString, Shape> delimiters = {
        {"(", ParenL},        {")", ParenR},         {"[", BracketL},        {"]", BracketR},
        {"\\{", BraceL},      {"\\}", BraceR},       {"\\lbrace", BraceL},   {"\\rbrace", BraceR},
        {"|", Bar},           {"\\|", DoubleBar},    {"\\vert", Bar},        {"\\lvert", Bar},
        {"\\rvert", Bar},     {"\\Vert", DoubleBar}, {"\\lVert", DoubleBar}, {"\\rVert", DoubleBar},
        {"\\langle", AngleL}, {"\\rangle", AngleR},  {".", NoShape},
    };
    QHash<QString, std::pair<Shape, Shape>> environments = {
        {"pmatrix", {ParenL, ParenR}},       {"bmatrix", {BracketL, BracketR}},
        {"Bmatrix", {BraceL, BraceR}},       {"vmatrix", {Bar, Bar}},
        {"Vmatrix", {DoubleBar, DoubleBar}}, {"cases", {BraceL, NoShape}},
    };
};

const Tables &tables()
{
    static const Tables all;
    return all;
}

// A shape's width in em (.mdl and its variants).
qreal shapeWidth(Shape shape)
{
    switch (shape) {
    case NoShape:
        return 0;
    case BraceL:
    case BraceR:
        return 0.46;
    case Bar:
        return 0.24;
    case DoubleBar:
        return 0.34;
    default:
        return 0.36;
    }
}

struct Item {
    enum Type : quint8 { Text, Rule, Scaled } type = Text;
    Shape shape = NoShape;
    int font = 0;
    QRectF rect; // Text: rect.topLeft() is the baseline origin.
    QString text;
};

enum class Kind : quint8 { Plain, Op, Integral, Limit, Break };

struct Box {
    qreal w = 0, a = 0, d = 0;
    std::vector<Item> items;
    Kind kind = Kind::Plain;
    bool none = false;   // tex.js returned '': nothing at all, not even a base.
    QString single;      // One character, for a combining accent.
    bool italic = false; // Of that character.
    bool bold = false;
    qreal height() const { return a + d; }
};

Box none()
{
    Box box;
    box.none = true;
    return box;
}

// dst gets src's items moved by (dx, dy).
void place(Box &dst, const Box &src, qreal dx, qreal dy)
{
    for (Item item : src.items) {
        item.rect.translate(dx, dy);
        dst.items.push_back(std::move(item));
    }
}

void append(Box &dst, const Box &src)
{
    place(dst, src, dst.w, 0);
    dst.w += src.w;
    dst.a = qMax(dst.a, src.a);
    dst.d = qMax(dst.d, src.d);
}

void pad(Box &box, qreal left, qreal right)
{
    if (left != 0)
        for (auto &item : box.items)
            item.rect.translate(left, 0);
    box.w = qMax<qreal>(0, box.w + left + right);
}

// Rows stacked top to bottom; each is at least minimum tall with its content
// centred vertically (CSS half-leading), and aligned by `align` (-1 left, 0
// centre, 1 right) within the widest. Returns the stack with y = 0 at its
// top (a = 0, d = height) and each row's baseline in `baselines`.
struct Row {
    const Box *box;
    qreal minimum;
    int align = 0;
    qreal gapBefore = 0;
};

Box stack(const std::vector<Row> &rows, qreal width, std::vector<qreal> *baselines = nullptr)
{
    Box out;
    for (const auto &row : rows)
        width = qMax(width, row.box->w);
    qreal y = 0;
    for (const auto &row : rows) {
        y += row.gapBefore;
        const qreal h = qMax(row.box->height(), row.minimum);
        const qreal base = y + (h - row.box->height()) / 2 + row.box->a;
        const qreal x = row.align < 0   ? 0
                        : row.align > 0 ? width - row.box->w
                                        : (width - row.box->w) / 2;
        place(out, *row.box, x, base);
        if (baselines)
            baselines->push_back(base);
        y += h;
    }
    out.w = width;
    out.d = y;
    return out;
}

// A top-anchored box (a = 0) centred on the math axis of text sized `size`.
void centerOnAxis(Box &box, qreal size)
{
    const qreal h = box.height();
    const qreal a = h / 2 + Axis * size;
    for (auto &item : box.items)
        item.rect.translate(0, -a);
    box.a = a;
    box.d = h - a;
}

bool isLetter(QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
           (c >= QLatin1Char('A') && c <= QLatin1Char('Z'));
}

struct Font {
    qreal size;
    bool italic, bold;
};

// Fonts, measurements and the item budget of one render call.
class Context
{
  public:
    Context(const tex::Style &style, bool block) : block(block), m_family(style.family)
    {
        m_device.fill(Qt::transparent);
    }
    const bool block; // Display math: the .md-math rules apply.
    QString error;

    bool failed() const { return !error.isEmpty(); }
    void fail(const QString &why)
    {
        if (error.isEmpty())
            error = why;
    }
    bool spend()
    {
        if (++m_items <= MaxItems)
            return true;
        fail(QStringLiteral("The formula is too complex to render."));
        return false;
    }

    int font(qreal size, bool italic, bool bold)
    {
        // Deep scripts and fractions stop shrinking at a legible floor.
        size = std::round(qMax(size, MinFont) * 100) / 100;
        for (int i = 0; i < int(m_fonts.size()); ++i) {
            const Font &f = m_fonts[i];
            if (f.size == size && f.italic == italic && f.bold == bold)
                return i;
        }
        QFont font;
        if (m_family.isEmpty())
            font.setFamilies({QStringLiteral("STIX Two Math"), QStringLiteral("Latin Modern Math"),
                              QStringLiteral("Cambria Math"), QStringLiteral("STIX Two Text"),
                              QStringLiteral("Times New Roman"), QStringLiteral("Noto Serif"),
                              QStringLiteral("DejaVu Serif")});
        else
            font.setFamilies({m_family});
        font.setStyleHint(QFont::Serif);
        font.setHintingPreference(QFont::PreferNoHinting);
        // Points at the measuring image's 96 dpi: exactly `size` pixels.
        font.setPointSizeF(size * 0.75);
        font.setItalic(italic);
        font.setBold(bold);
        m_fonts.push_back({size, italic, bold});
        m_qfonts.push_back(font);
        m_metrics.emplace_back(font, &m_device);
        m_advances.emplace_back();
        return int(m_fonts.size()) - 1;
    }
    const QFont &qfont(int font) const { return m_qfonts[font]; }
    qreal size(int font) const { return m_fonts[font].size; }
    qreal advance(int font, const QString &text)
    {
        auto &cache = m_advances[font];
        const auto found = cache.constFind(text);
        if (found != cache.cend())
            return *found;
        return cache.insert(text, m_metrics[font].horizontalAdvance(text)).value();
    }
    QRectF ink(int font, const QString &text) { return m_metrics[font].tightBoundingRect(text); }

  private:
    QString m_family;
    QImage m_device{1, 1, QImage::Format_ARGB32_Premultiplied};
    std::vector<Font> m_fonts;
    std::vector<QFont> m_qfonts;
    std::vector<QFontMetricsF> m_metrics;
    std::vector<QHash<QString, qreal>> m_advances;
    int m_items = 0;
};

enum class Variant : quint8 { Italic, Upright, Bold, BoldItalic };
enum class Halt : quint8 { None, Right, Cell };

class Parser
{
  public:
    Parser(Context &context, const QString &source, bool display, qreal size, int depth)
        : c(context), s(source), n(int(source.size())), display(display), depth(depth), size(size)
    {
    }

    // Parser state that nested content inherits and restores (CSS context).
    struct Scope {
        Parser &p;
        qreal size;
        bool script, frac;
        Variant variant;
        explicit Scope(Parser &p)
            : p(p), size(p.size), script(p.script), frac(p.frac), variant(p.variant)
        {
        }
        ~Scope()
        {
            p.size = size;
            p.script = script;
            p.frac = frac;
            p.variant = variant;
        }
    };
    struct Deeper {
        int &depth;
        explicit Deeper(int &depth) : depth(++depth) {}
        ~Deeper() { --depth; }
    };

    Context &c;
    const QString &s;
    const int n;
    int i = 0;
    const bool display; // tex.js p.display: big operators and limits.
    enum Prev { Start, Op, Open, Ord } prev = Start;
    int depth;
    qreal size;          // Current font size.
    bool script = false; // Inside sup, sub, .mss or .mlim slots.
    bool frac = false;   // Inside a fraction.
    Variant variant = Variant::Italic;

    QChar at(int k) const { return k < n ? s.at(k) : QChar(); }
    bool at(int k, QLatin1String word) const
    {
        return QStringView(s).mid(k).startsWith(word) && !isLetter(at(k + int(word.size())));
    }
    void spaces()
    {
        while (at(i) == QLatin1Char(' '))
            ++i;
    }
    // One code point (a surrogate pair stays whole).
    QString codePoint()
    {
        const int from = i++;
        if (s.at(from).isHighSurrogate() && i < n && s.at(i).isLowSurrogate())
            ++i;
        return s.mid(from, i - from);
    }

    Box text(const QString &text, bool italic, bool bold = false, qreal scale = 1)
    {
        Box box;
        if (text.isEmpty() || !c.spend())
            return box;
        const int font = c.font(size * scale, italic, bold);
        const qreal px = c.size(font);
        box.w = c.advance(font, text);
        box.a = Ascent * px;
        box.d = Descent * px;
        // Symbols from other fonts can be taller than a letter's box.
        for (QChar ch : text) {
            if (ch.unicode() >= 0x370) {
                const QRectF ink = c.ink(font, text);
                box.a = qMax(box.a, -ink.top());
                box.d = qMax(box.d, ink.bottom());
                break;
            }
        }
        Item item;
        item.font = font;
        item.text = text;
        box.items.push_back(std::move(item));
        box.italic = italic;
        box.bold = bold;
        return box;
    }

    Box space(qreal em)
    {
        Box box;
        box.w = em * size;
        return box;
    }

    Box rule(qreal x, qreal y, qreal w, qreal h)
    {
        Box box;
        if (!c.spend())
            return box;
        Item item;
        item.type = Item::Rule;
        item.rect = QRectF(x, y, w, h);
        box.items.push_back(item);
        return box;
    }

    Box scaled(Shape shape, qreal x, qreal y, qreal w, qreal h)
    {
        Box box;
        if (shape == NoShape || !c.spend())
            return box;
        Item item;
        item.type = Item::Scaled;
        item.shape = shape;
        item.rect = QRectF(x, y, w, h);
        box.items.push_back(item);
        return box;
    }

    // A letter in the current variant.
    Box letter(const QString &ch)
    {
        const bool italic = variant == Variant::Italic || variant == Variant::BoldItalic;
        const bool bold = variant == Variant::Bold || variant == Variant::BoldItalic;
        Box box = text(ch, italic, bold);
        box.single = ch;
        return box;
    }

    Box plain(const QString &ch)
    {
        Box box = text(ch, false);
        if (ch.size() == 1 || (ch.size() == 2 && ch.at(0).isHighSurrogate()))
            box.single = ch;
        return box;
    }

    // tex.js operator(): relation, unary or binary spacing (.mo).
    Box op(const QString &symbol)
    {
        const auto &t = tables();
        const bool unary =
            t.unary.contains(symbol) && (prev == Start || prev == Op || prev == Open);
        prev = Op;
        Box box = text(symbol, false);
        if (script)
            pad(box, 0.06 * size, 0.06 * size);
        else if (unary)
            pad(box, 0, 0.03 * size);
        else if (t.rel.contains(symbol))
            pad(box, 0.3 * size, 0.3 * size);
        else
            pad(box, 0.22 * size, 0.22 * size);
        return box;
    }

    QString readDelimiter()
    {
        spaces();
        if (at(i) != QLatin1Char('\\')) {
            if (i >= n)
                return QStringLiteral(".");
            return codePoint();
        }
        int j = i + 1;
        if (isLetter(at(j)))
            while (j < n && isLetter(s.at(j)))
                ++j;
        else
            ++j;
        j = qMin(j, n);
        const QString token = s.mid(i, j - i);
        i = j;
        return token;
    }

    QString raw()
    {
        spaces();
        if (at(i) != QLatin1Char('{'))
            return i < n ? codePoint() : QString();
        int depthNow = 0;
        const int start = ++i;
        for (; i < n; ++i) {
            if (s.at(i) == QLatin1Char('{'))
                ++depthNow;
            else if (s.at(i) == QLatin1Char('}') && !depthNow--)
                break;
        }
        const QString body = s.mid(start, i - start);
        if (i < n)
            ++i;
        return body;
    }

    // Nested source read by a fresh parser (tex.js render()), in this context.
    Box nested(const QString &source)
    {
        Parser inner(c, source, false, size, depth);
        inner.script = script;
        inner.frac = frac;
        inner.variant = variant;
        return inner.seq(QChar(), Halt::None);
    }

    Box arg()
    {
        spaces();
        if (i >= n)
            return none();
        if (s.at(i) == QLatin1Char('{')) {
            ++i;
            return seq(QLatin1Char('}'), Halt::None);
        }
        return atom();
    }

    bool halted(Halt halt) const
    {
        switch (halt) {
        case Halt::None:
            return false;
        case Halt::Right:
            return at(i, QLatin1String("\\right"));
        case Halt::Cell:
            return s.at(i) == QLatin1Char('&') ||
                   QStringView(s).mid(i).startsWith(QLatin1String("\\\\")) ||
                   at(i, QLatin1String("\\end"));
        }
        return false;
    }

    Box seq(QChar stop, Halt halt)
    {
        Deeper deeper(depth);
        std::vector<Box> lines;
        Box line, base;
        bool hasBase = false, joined = false;
        prev = Start;
        while (i < n && !c.failed()) {
            if (!stop.isNull() && s.at(i) == stop) {
                ++i;
                break;
            }
            if (halted(halt))
                break;
            if (s.at(i) == QLatin1Char('^') || s.at(i) == QLatin1Char('_')) {
                base = scripts(hasBase ? std::move(base) : Box());
                hasBase = true;
                continue;
            }
            Box next = atom();
            if (next.none)
                continue;
            if (next.kind == Kind::Break) {
                if (hasBase)
                    append(line, base);
                lines.push_back(std::move(line));
                line = Box();
                hasBase = joined = false;
                continue;
            }
            if (hasBase) {
                append(line, base);
                joined = true;
            }
            base = std::move(next);
            hasBase = true;
        }
        prev = Ord;
        if (lines.empty()) {
            if (!hasBase)
                return joined ? line : none();
            if (!joined)
                return base; // One atom keeps its kind (limits, accents).
        }
        if (hasBase)
            append(line, base);
        if (lines.empty())
            return line;
        lines.push_back(std::move(line));
        std::vector<Row> rows;
        for (const auto &l : lines)
            rows.push_back({&l, 1.5 * size, c.block ? 0 : -1});
        std::vector<qreal> baselines;
        Box out = stack(rows, 0, &baselines);
        // The first line's baseline is the box's.
        const qreal first = baselines.front();
        for (auto &item : out.items)
            item.rect.translate(0, -first);
        out.a = first;
        out.d -= first;
        return out;
    }

    Box atom()
    {
        Deeper deeper(depth);
        const QChar ch = s.at(i);
        if (depth > MaxDepth)
            return literal();
        if (ch == QLatin1Char('\\'))
            return command();
        if (ch == QLatin1Char('{')) {
            ++i;
            return seq(QLatin1Char('}'), Halt::None);
        }
        if (ch == QLatin1Char('}')) {
            ++i;
            return none();
        }
        if (ch == QLatin1Char('^') || ch == QLatin1Char('_'))
            return scripts(Box());
        if (ch == QLatin1Char('&')) {
            ++i;
            prev = Start;
            return space(1); // .mg
        }
        if (ch == QLatin1Char(' ') || ch == QLatin1Char('\n') || ch == QLatin1Char('\t') ||
            ch == QLatin1Char('\r')) {
            ++i;
            return none();
        }
        if (ch == QLatin1Char('~')) {
            ++i;
            return space(0.25);
        }
        const auto &t = tables();
        const auto symbol = t.operators.constFind(QString(ch));
        if (symbol != t.operators.cend()) {
            ++i;
            return op(*symbol);
        }
        if (isLetter(ch)) {
            ++i;
            prev = Ord;
            return letter(QString(ch));
        }
        if (ch == QLatin1Char('\'')) {
            ++i;
            prev = Ord;
            return plain(QStringLiteral("′"));
        }
        prev = ch == QLatin1Char('(') || ch == QLatin1Char('[') || ch == QLatin1Char(',') ? Open
               : ch == QLatin1Char(';')                                                   ? Op
                                                                                          : Ord;
        return plain(codePoint());
    }

    // Past the depth bound, atoms are plain text and nothing recurses.
    Box literal()
    {
        const QChar ch = s.at(i);
        if (ch == QLatin1Char('{'))
            return plain(raw());
        if (ch == QLatin1Char('}') || ch == QLatin1Char(' ') || ch == QLatin1Char('\n')) {
            ++i;
            return none();
        }
        prev = Ord;
        return plain(codePoint());
    }

    Box scripts(Box base)
    {
        const bool limits = display && (base.kind == Kind::Op || base.kind == Kind::Limit);
        Box sup = none(), sub = none();
        bool hasSup = false, hasSub = false;
        while (at(i) == QLatin1Char('^') || at(i) == QLatin1Char('_')) {
            const bool up = s.at(i) == QLatin1Char('^');
            if (up ? hasSup : hasSub)
                break;
            ++i;
            prev = Start;
            Box body;
            {
                Scope scope(*this);
                size *= limits ? 0.6 : 0.68;
                script = true;
                body = arg();
            }
            (up ? sup : sub) = std::move(body);
            (up ? hasSup : hasSub) = true;
            spaces();
        }
        prev = Ord;
        if (limits)
            return limitsBox(base, hasSup ? &sup : nullptr, hasSub ? &sub : nullptr);
        Box out = base;
        out.kind = Kind::Plain;
        out.single.clear();
        if (hasSup && hasSub) {
            Box pair = stacked(sup, sub, base.kind == Kind::Integral && c.block);
            append(out, pair);
            return out;
        }
        const qreal small = size * 0.68;
        Box &body = hasSup ? sup : sub;
        // CSS vertical-align super/sub (a third and a fifth of the text size),
        // raised further to clear a tall base.
        const qreal shift = hasSup ? -qMax(size / 3 + 0.06 * size, base.a - 0.62 * small)
                                   : qMax(size / 5 + 0.06 * size, base.d - 0.3 * small);
        const qreal left = hasSup ? 0.05 * small : 0;
        place(out, body, out.w + left, shift);
        out.w += left + body.w;
        out.a = qMax(out.a, body.a - shift);
        out.d = qMax(out.d, body.d + shift);
        return out;
    }

    // .mss: sup over sub beside the base, centred on the axis.
    Box stacked(const Box &sup, const Box &sub, bool integral)
    {
        const qreal small = size * 0.68;
        std::vector<Row> rows{{&sup, 1.12 * small, -1}, {&sub, 1.12 * small, -1}};
        if (integral)
            rows[1].gapBefore = 0.8 * small;
        Box box = stack(rows, 0);
        centerOnAxis(box, size);
        pad(box, (integral ? -0.08 : 0.04) * small, integral ? 0.18 * small : 0);
        return box;
    }

    // .mlim: limits above and below, the missing one's room kept.
    Box limitsBox(const Box &base, const Box *sup, const Box *sub)
    {
        const qreal small = size * 0.6;
        Box hiddenTop, hiddenBottom;
        const Box *top = sup, *bottom = sub;
        if (!top) {
            hiddenTop.a = sub->a, hiddenTop.d = sub->d, hiddenTop.w = sub->w;
            top = &hiddenTop;
        }
        if (!bottom) {
            hiddenBottom.a = sup->a, hiddenBottom.d = sup->d, hiddenBottom.w = sup->w;
            bottom = &hiddenBottom;
        }
        std::vector<Row> rows{{top, 1.3 * small},
                              {&base, base.kind == Kind::Op ? 0 : 1.15 * size},
                              {bottom, 1.3 * small}};
        Box box = stack(rows, 0);
        centerOnAxis(box, size);
        pad(box, 0.12 * size, 0.12 * size);
        return box;
    }

    Box fraction(const Box &top, const Box &bottom, bool bare, qreal parent)
    {
        const qreal f = size; // Already the fraction's own size.
        const qreal lh = c.block ? 1.35 : 1.3;
        const qreal padX = 0.22 * f, gap = 0.12 * f, thick = bare ? 0 : 0.055 * f;
        const qreal margin = (c.block ? 0.16 : 0.12) * f;
        const qreal width = qMax(top.w, bottom.w) + 2 * padX;
        std::vector<Row> rows{{&top, lh * f}, {&bottom, lh * f}};
        rows[1].gapBefore = 2 * gap + thick;
        Box box = stack(rows, width);
        if (!bare) {
            const qreal topH = qMax(top.height(), lh * f);
            place(box, rule(0, topH + gap, width, thick), 0, 0);
        }
        centerOnAxis(box, parent);
        pad(box, margin, margin);
        return box;
    }

    // .ms: the radical stretched to the body, which has a rule above it.
    Box radical(const Box &body)
    {
        const qreal rowH = qMax(body.height(), 1.2 * size);
        const qreal base = (rowH - body.height()) / 2 + body.a;
        const qreal thick = 0.06 * size, top = 0.22 * size, signW = 0.58 * size;
        const qreal x0 = signW - 0.02 * size, x = x0 + 0.04 * size;
        Box box;
        box.w = x + body.w + 0.1 * size;
        box.a = thick + top + base;
        box.d = rowH - base;
        place(box, scaled(Radical, 0, -box.a, signW, box.height()), 0, 0);
        place(box, rule(x0, -box.a, box.w - x0, thick), 0, 0);
        place(box, body, x, 0);
        return box;
    }

    // A delimiter token: a stretched shape, nothing ('.'), or a glyph (.mdg).
    struct Delimiter {
        Shape shape = NoShape;
        QString glyph;
    };
    Delimiter delimiter(const QString &token)
    {
        const auto &t = tables();
        const auto found = t.delimiters.constFind(token);
        if (found != t.delimiters.cend())
            return {*found, {}};
        if (token.startsWith(QLatin1Char('\\'))) {
            const QString name = token.mid(1);
            return {NoShape, t.symbols.value(name, name)};
        }
        return {NoShape, token};
    }

    // .mdel: delimiters as tall as the inner line box.
    Box fence(const Delimiter &open, const Box &inner, const Delimiter &close)
    {
        const qreal rowH = qMax(inner.height(), 1.3 * size);
        const qreal base = (rowH - inner.height()) / 2 + inner.a;
        Box box;
        box.a = base;
        box.d = rowH - base;
        box.w = 0.04 * size;
        const auto side = [&](const Delimiter &delimiter) {
            if (!delimiter.glyph.isEmpty()) {
                append(box, text(delimiter.glyph, false));
            } else if (delimiter.shape != NoShape) {
                const qreal w = shapeWidth(delimiter.shape) * size;
                place(box, scaled(delimiter.shape, 0, -box.a, w, rowH), box.w, 0);
                box.w += w;
            }
        };
        side(open);
        box.w += 0.06 * size;
        append(box, inner);
        box.w += 0.06 * size;
        side(close);
        box.w += 0.04 * size;
        return box;
    }

    Box environment(const QString &name)
    {
        if (name == QLatin1String("array") || name == QLatin1String("alignedat"))
            raw();
        std::vector<std::vector<Box>> rows(1);
        while (i < n && !c.failed()) {
            if (int(rows.back().size()) >= MaxColumns || int(rows.size()) > MaxRows) {
                c.fail(QStringLiteral("The matrix is too large to render."));
                return none();
            }
            rows.back().push_back(seq(QChar(), Halt::Cell));
            if (at(i) == QLatin1Char('&')) {
                ++i;
                continue;
            }
            if (QStringView(s).mid(i).startsWith(QLatin1String("\\\\"))) {
                i += 2;
                rows.emplace_back();
                continue;
            }
            if (at(i, QLatin1String("\\end"))) {
                i += 4;
                raw();
            }
            break;
        }
        const auto empty = [](const std::vector<Box> &row) {
            for (const auto &cell : row)
                if (!cell.none)
                    return false;
            return true;
        };
        while (rows.size() > 1 && empty(rows.back()))
            rows.pop_back();
        size_t columns = 0;
        for (const auto &row : rows)
            columns = qMax(columns, row.size());
        const bool align = name.startsWith(QLatin1String("align")) ||
                           name == QLatin1String("split") || name == QLatin1String("eqnarray");
        const bool cases = name == QLatin1String("cases");
        std::vector<qreal> widths(columns, 0);
        for (const auto &row : rows)
            for (size_t k = 0; k < row.size(); ++k)
                widths[k] = qMax(widths[k], row[k].w);
        const qreal gapX = (align ? 0 : cases ? 1.4 : 0.9) * size, gapY = 0.18 * size;
        const qreal margin = 0.1 * size;
        Box grid;
        qreal y = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            qreal a = 0, d = 0;
            for (const auto &cell : rows[r])
                a = qMax(a, cell.a), d = qMax(d, cell.d);
            const qreal h = qMax(a + d, 1.3 * size);
            const qreal base = y + (h - a - d) / 2 + a;
            qreal x = margin;
            for (size_t k = 0; k < columns; ++k) {
                if (k < rows[r].size()) {
                    const Box &cell = rows[r][k];
                    const qreal slack = widths[k] - cell.w;
                    const qreal dx = align ? (k % 2 ? 0 : slack) : cases ? 0 : slack / 2;
                    place(grid, cell, x + dx, base);
                }
                x += widths[k] + (k + 1 < columns ? gapX : 0);
            }
            grid.w = qMax(grid.w, x + margin);
            y += h + (r + 1 < rows.size() ? gapY : 0);
        }
        grid.d = y;
        centerOnAxis(grid, size);
        prev = Ord;
        const auto pair = tables().environments.constFind(name);
        if (pair == tables().environments.cend())
            return grid;
        return fence({pair->first, {}}, grid, {pair->second, {}});
    }

    Box accent(const QString &name, const Box &body)
    {
        const auto &t = tables();
        const bool wide = name == QLatin1String("vec") || name.startsWith(QLatin1String("wide")) ||
                          name == QLatin1String("overline");
        if (!body.single.isEmpty() && !wide)
            return text(body.single + t.accents.value(name), body.italic, body.bold);
        Box box = body;
        box.kind = Kind::Plain;
        box.single.clear();
        if (name == QLatin1String("bar") || name == QLatin1String("overline")) {
            const qreal thick = 0.06 * size, y = -(body.a + 0.04 * size) - thick;
            place(box, rule(0, y, body.w, thick), 0, 0);
            box.a = body.a + 0.1 * size;
            return box;
        }
        const bool vec = name == QLatin1String("vec");
        const QString mark = t.spacingAccents.value(name);
        Box glyph = text(mark, false, false, vec ? 0.7 : 0.85);
        if (glyph.items.empty())
            return box;
        const int font = glyph.items.front().font;
        const QRectF ink = c.ink(font, mark);
        const qreal x = 0.58 * body.w - (ink.left() + ink.width() / 2);
        const qreal y = -(body.a + 0.02 * size) - ink.bottom();
        place(box, glyph, x, y);
        box.a = qMax(box.a, -(y + ink.top()));
        return box;
    }

    Box bigOperator(const QString &name, const QString &symbol)
    {
        const bool integral = tables().integrals.contains(name);
        prev = Op;
        const qreal scale = display ? (integral ? 1.9 : 1.55) : 1;
        Box box = text(symbol, false, false, scale);
        const qreal px = size * scale;
        if (display && !box.items.empty()) {
            // Centred on the axis, whatever the fallback font's design.
            const QRectF ink = c.ink(box.items.front().font, symbol);
            const qreal shift = -Axis * size - (ink.top() + ink.height() / 2);
            for (auto &item : box.items)
                item.rect.translate(0, shift);
            box.a = qMax(0.0, -(ink.top() + shift));
            box.d = qMax(0.0, ink.bottom() + shift);
        }
        pad(box, 0.04 * px, 0.08 * px);
        box.kind = integral ? Kind::Integral : Kind::Op;
        box.single.clear();
        return box;
    }

    Box command()
    {
        const auto &t = tables();
        int j = ++i;
        if (isLetter(at(j))) {
            while (j < n && isLetter(s.at(j)))
                ++j;
        } else if (j < n) {
            ++j;
            if (s.at(j - 1).isHighSurrogate() && j < n && s.at(j).isLowSurrogate())
                ++j;
        }
        const QString name = s.mid(i, j - i);
        i = j;
        if (name == QLatin1String("frac") || name == QLatin1String("dfrac") ||
            name == QLatin1String("tfrac") || name == QLatin1String("cfrac") ||
            name == QLatin1String("binom")) {
            const qreal parent = size;
            Box top, bottom;
            {
                Scope scope(*this);
                size *= c.block && !frac ? 0.94 : 0.86;
                frac = true;
                top = arg();
                bottom = arg();
                prev = Ord;
                Box box = fraction(top, bottom, name == QLatin1String("binom"), parent);
                if (name != QLatin1String("binom"))
                    return box;
                size = parent;
                return fence({ParenL, {}}, box, {ParenR, {}});
            }
        }
        if (name == QLatin1String("sqrt")) {
            Box index;
            bool hasIndex = false;
            if (at(i) == QLatin1Char('[')) {
                const int end = s.indexOf(QLatin1Char(']'), i);
                const QString source = s.mid(i + 1, (end < 0 ? n : end) - i - 1);
                i = end < 0 ? n : end + 1;
                Scope scope(*this);
                size *= 0.55;
                script = true;
                index = nested(source);
                hasIndex = !index.none;
            }
            Box body = arg();
            prev = Ord;
            Box root = radical(body);
            if (!hasIndex)
                return root;
            // .mx: a small sup overlapping the radical's arm.
            const qreal small = size * 0.55;
            const qreal shift = -qMax(size / 3 + 0.06 * size, 0.45 * root.a);
            Box out;
            place(out, index, 0, shift);
            out.w = qMax<qreal>(0, index.w - 0.42 * small);
            out.a = qMax(root.a, index.a - shift);
            out.d = qMax(root.d, index.d + shift);
            append(out, root);
            return out;
        }
        if (name == QLatin1String("left")) {
            const QString open = readDelimiter();
            Box inner = seq(QChar(), Halt::Right);
            QString close = QStringLiteral(".");
            if (at(i, QLatin1String("\\right"))) {
                i += 6;
                close = readDelimiter();
            }
            prev = Ord;
            return fence(delimiter(open), inner, delimiter(close));
        }
        if (name == QLatin1String("right")) {
            readDelimiter();
            return none();
        }
        if (t.text.contains(name)) {
            const QString body = raw();
            prev = Ord;
            if (name == QLatin1String("mathbb")) {
                QString mapped;
                for (int k = 0; k < body.size(); ++k) {
                    const int length = body.at(k).isHighSurrogate() && k + 1 < body.size() ? 2 : 1;
                    const QString ch = body.mid(k, length);
                    mapped += t.doubleStruck.value(ch, ch);
                    k += length - 1;
                }
                return plain(mapped);
            }
            const bool bold = name.contains(QLatin1String("bf")) ||
                              name.contains(QLatin1String("bold")) || name == QLatin1String("bm");
            if (name.startsWith(QLatin1String("text")) || name == QLatin1String("mbox") ||
                name == QLatin1String("hbox"))
                return text(body, name == QLatin1String("textit"), bold);
            Scope scope(*this);
            variant = name == QLatin1String("boldsymbol") || name == QLatin1String("bm")
                          ? Variant::BoldItalic
                      : bold ? Variant::Bold
                      : name == QLatin1String("mathit") || name == QLatin1String("mathcal") ||
                              name == QLatin1String("mathfrak")
                          ? Variant::Italic
                          : Variant::Upright;
            Box box = nested(body);
            return box.none ? Box() : box;
        }
        if (name == QLatin1String("begin"))
            return environment(raw());
        if (name == QLatin1String("end")) {
            raw();
            return none();
        }
        if (name == QLatin1String("\\") || name == QLatin1String("newline") ||
            name == QLatin1String("cr")) {
            prev = Start;
            Box box;
            box.kind = Kind::Break;
            return box;
        }
        if (t.silent.contains(name)) {
            if (at(i) == QLatin1Char('.'))
                ++i;
            return none();
        }
        if (t.accents.contains(name)) {
            Box body = arg();
            prev = Ord;
            return accent(name, body);
        }
        const auto big = t.big.constFind(name);
        if (big != t.big.cend())
            return bigOperator(name, *big);
        const auto relation = t.relations.constFind(name);
        if (relation != t.relations.cend())
            return op(*relation);
        const auto symbol = t.symbols.constFind(name);
        if (symbol != t.symbols.cend()) {
            const QString &value = *symbol;
            if (value.isEmpty())
                return none();
            if (value.trimmed().isEmpty()) { // Spacing commands.
                qreal em = 0;
                for (QChar ch : value)
                    em += ch.unicode() == 0x2003 ? 1 : ch.unicode() == 0x2009 ? 0.2 : 0.25;
                return space(em);
            }
            prev = t.openers.contains(name) ? Open : Ord;
            return t.greek.contains(name) ? letter(value) : plain(value);
        }
        if (t.functions.contains(name)) {
            prev = Op;
            Box box = text(name, false);
            pad(box, 0.12 * size, 0.15 * size);
            box.kind = t.limits.contains(name) ? Kind::Limit : Kind::Plain;
            return box;
        }
        prev = Ord;
        return name.isEmpty() ? none() : text(name, false);
    }
};

void paint(QPainter &painter, const Context &context, const std::vector<Item> &items,
           QPointF origin)
{
    int font = -1;
    for (const auto &item : items) {
        const QRectF rect = item.rect.translated(origin);
        switch (item.type) {
        case Item::Text:
            if (item.font != font) {
                font = item.font;
                painter.setFont(context.qfont(font));
            }
            painter.drawText(rect.topLeft(), item.text);
            break;
        case Item::Rule:
            painter.fillRect(rect, painter.pen().color());
            break;
        case Item::Scaled: {
            const qreal viewWidth = item.shape == Radical ? 12 : 10;
            QTransform transform;
            transform.translate(rect.x(), rect.y());
            transform.scale(rect.width() / viewWidth, rect.height() / 40);
            painter.fillPath(transform.map(shapePath(item.shape)), painter.pen().color());
            break;
        }
        }
    }
}
// ---- Copy -------------------------------------------------------------------

// tex.js once more, building its HTML as a tree (not laid out) so that it can
// be copied as Chromium copies it: the children of a flex or grid box are
// boxes of their own, each set off by a Break; the rest is inline text.
struct El {
    enum Type : quint8 { Text, Span, Flex, Br, Delim };
    Type type = Span;
    QString text, cls; // Text: its characters; Span and Flex: the class.
    bool hidden = false;
    QVector<El> kids;
};
using Frag = QVector<El>;

El textEl(const QString &text)
{
    El e;
    e.type = El::Text;
    e.text = text;
    return e;
}

El spanEl(const QString &cls, Frag kids, El::Type type = El::Span)
{
    El e;
    e.type = type;
    e.cls = cls;
    e.kids = std::move(kids);
    return e;
}

El delimEl()
{
    El e;
    e.type = El::Delim;
    return e;
}

bool hasInt(const Frag &frag)
{
    for (const El &e : frag)
        if (e.cls.contains(QLatin1String("is-int")) || hasInt(e.kids))
            return true;
    return false;
}

class CopyReader
{
  public:
    CopyReader(const QString &source, bool display, int depth)
        : s(source), n(int(source.size())), display(display), depth(depth)
    {
    }

    Frag seq(QChar stop, bool (CopyReader::*halt)() const = nullptr)
    {
        Deeper deeper(depth);
        Frag out, base;
        prev = Start;
        while (i < n) {
            if (!stop.isNull() && s.at(i) == stop) {
                ++i;
                break;
            }
            if (halt && (this->*halt)())
                break;
            if (s.at(i) == QLatin1Char('^') || s.at(i) == QLatin1Char('_')) {
                base = scripts(base);
                continue;
            }
            Frag next = atom();
            if (next.isEmpty())
                continue;
            out += base;
            base = std::move(next);
        }
        prev = Ord;
        return out + base;
    }

  private:
    enum Prev { Start, Op, Open, Ord };
    struct Deeper {
        int &depth;
        explicit Deeper(int &depth) : depth(++depth) {}
        ~Deeper() { --depth; }
    };

    QChar at(int k) const { return k < n ? s.at(k) : QChar(); }
    bool word(QLatin1String w) const
    {
        return QStringView(s).mid(i).startsWith(w) && !isLetter(at(i + int(w.size())));
    }
    bool cell() const
    {
        return at(i) == QLatin1Char('&') || QStringView(s).mid(i).startsWith(u"\\\\") ||
               word(QLatin1String("\\end"));
    }
    bool right() const { return word(QLatin1String("\\right")); }
    void spaces()
    {
        while (at(i) == QLatin1Char(' '))
            ++i;
    }

    Frag op(const QString &symbol)
    {
        prev = Op;
        return {spanEl(QStringLiteral("mo"), {textEl(symbol)})};
    }

    static Frag delimiter(const QString &token)
    {
        const auto &t = tables();
        if (const auto found = t.delimiters.constFind(token); found != t.delimiters.cend())
            return *found == NoShape ? Frag{} : Frag{delimEl()};
        const bool named = token.startsWith(QLatin1Char('\\'));
        const QString glyph = named ? t.symbols.value(token.mid(1), token.mid(1)) : token;
        return {spanEl(QStringLiteral("mdg"), {textEl(glyph)})};
    }

    static Frag fence(bool open, const Frag &inner, bool close)
    {
        Frag kids;
        if (open)
            kids << delimEl();
        kids << spanEl(QStringLiteral("mdel-in"), inner);
        if (close)
            kids << delimEl();
        return {spanEl(QStringLiteral("mdel"), kids, El::Flex)};
    }

    QString readDelimiter()
    {
        spaces();
        if (at(i) != QLatin1Char('\\'))
            return i < n ? QString(s.at(i++)) : QStringLiteral(".");
        int j = i + 1;
        if (isLetter(at(j)))
            while (j < n && isLetter(s.at(j)))
                ++j;
        else
            ++j;
        const QString token = s.mid(i, j - i);
        i = std::min(j, n);
        return token;
    }

    QString raw()
    {
        spaces();
        if (at(i) != QLatin1Char('{'))
            return i < n ? QString(s.at(i++)) : QString();
        int nested = 0;
        const int start = ++i;
        for (; i < n; ++i) {
            if (s.at(i) == QLatin1Char('{'))
                ++nested;
            else if (s.at(i) == QLatin1Char('}') && !nested--)
                break;
        }
        const QString body = s.mid(start, i - start);
        ++i;
        return body;
    }

    Frag arg()
    {
        spaces();
        if (i >= n)
            return {};
        if (s.at(i) == QLatin1Char('{')) {
            ++i;
            return seq(QLatin1Char('}'));
        }
        return atom();
    }

    Frag render(const QString &source)
    {
        CopyReader inner(source, false, depth);
        return inner.seq(QChar());
    }

    Frag scripts(const Frag &base)
    {
        std::optional<Frag> sup, sub;
        while (at(i) == QLatin1Char('^') || at(i) == QLatin1Char('_')) {
            const bool up = s.at(i) == QLatin1Char('^');
            if ((up ? sup : sub).has_value())
                break;
            ++i;
            prev = Start;
            Frag body = arg();
            (up ? sup : sub) = std::move(body);
            spaces();
        }
        prev = Ord;
        const bool limits =
            !base.isEmpty() && (base.first().cls.startsWith(QLatin1String("mop")) ||
                                base.first().cls.startsWith(QLatin1String("mn is-lim")));
        if (display && limits && !hasInt(base)) {
            // An empty slot holds the other script, hidden, to keep the base centred.
            const auto slot = [](const QString &cls, const std::optional<Frag> &body,
                                 const std::optional<Frag> &twin) {
                El e = spanEl(cls, body ? *body : twin.value_or(Frag()));
                e.hidden = !body;
                return e;
            };
            return {spanEl(QStringLiteral("mlim"),
                           {slot(QStringLiteral("mlim-t"), sup, sub),
                            spanEl(QStringLiteral("mlim-o"), base),
                            slot(QStringLiteral("mlim-b"), sub, sup)},
                           El::Flex)};
        }
        if (sup && sub)
            return base + Frag{spanEl(QStringLiteral("mss"), {spanEl({}, *sup), spanEl({}, *sub)},
                                      El::Flex)};
        return base + Frag{sup ? spanEl(QStringLiteral("sup"), *sup)
                               : spanEl(QStringLiteral("sub"), *sub)};
    }

    Frag environment(const QString &name)
    {
        if (name == QLatin1String("array") || name == QLatin1String("alignedat"))
            raw();
        QVector<QVector<Frag>> rows(1);
        while (i < n) {
            rows.last() << seq(QChar(), &CopyReader::cell);
            if (at(i) == QLatin1Char('&')) {
                ++i;
                continue;
            }
            if (QStringView(s).mid(i).startsWith(u"\\\\")) {
                i += 2;
                rows << QVector<Frag>();
                continue;
            }
            if (word(QLatin1String("\\end"))) {
                i += 4;
                raw();
            }
            break;
        }
        while (rows.size() > 1 && std::all_of(rows.last().cbegin(), rows.last().cend(),
                                              [](const Frag &c) { return c.isEmpty(); }))
            rows.removeLast();
        qsizetype cols = 0;
        for (const auto &row : std::as_const(rows))
            cols = std::max(cols, row.size());
        Frag cells;
        for (const auto &row : std::as_const(rows))
            for (qsizetype k = 0; k < cols; ++k)
                cells << spanEl({}, k < row.size() ? row.at(k) : Frag());
        prev = Ord;
        const Frag grid{spanEl(QStringLiteral("mgrid"), cells, El::Flex)};
        const auto &envs = tables().environments;
        if (const auto pair = envs.constFind(name); pair != envs.cend())
            return fence(pair->first != NoShape, grid, pair->second != NoShape);
        return grid;
    }

    Frag command()
    {
        const auto &t = tables();
        int j = ++i;
        if (isLetter(at(j)))
            while (j < n && isLetter(s.at(j)))
                ++j;
        else
            ++j;
        const QString name = s.mid(i, j - i);
        i = std::min(j, n);
        if (name == QLatin1String("frac") || name == QLatin1String("dfrac") ||
            name == QLatin1String("tfrac") || name == QLatin1String("cfrac") ||
            name == QLatin1String("binom")) {
            const Frag top = arg(), bottom = arg();
            prev = Ord;
            const bool binom = name == QLatin1String("binom");
            const Frag frac{spanEl(binom ? QStringLiteral("mf is-bare") : QStringLiteral("mf"),
                                   {spanEl({}, top), spanEl({}, bottom)}, El::Flex)};
            return binom ? fence(true, frac, true) : frac;
        }
        if (name == QLatin1String("sqrt")) {
            Frag index;
            if (at(i) == QLatin1Char('[')) {
                const int end = int(s.indexOf(QLatin1Char(']'), i));
                index = render(s.mid(i + 1, (end < 0 ? n : end) - i - 1));
                i = end < 0 ? n : end + 1;
            }
            const Frag body = arg();
            prev = Ord;
            Frag out;
            if (!index.isEmpty())
                out << spanEl(QStringLiteral("mx"), index);
            out << spanEl(QStringLiteral("ms"), {spanEl(QStringLiteral("ms-in"), body)}, El::Flex);
            return out;
        }
        if (name == QLatin1String("left")) {
            const QString open = readDelimiter();
            const Frag inner = seq(QChar(), &CopyReader::right);
            QString close = QStringLiteral(".");
            if (right()) {
                i += 6;
                close = readDelimiter();
            }
            prev = Ord;
            return {spanEl(QStringLiteral("mdel"),
                           delimiter(open) + Frag{spanEl(QStringLiteral("mdel-in"), inner)} +
                               delimiter(close),
                           El::Flex)};
        }
        if (name == QLatin1String("right")) {
            readDelimiter();
            return {};
        }
        if (t.text.contains(name)) {
            const QString body = raw();
            prev = Ord;
            if (name == QLatin1String("mathbb")) {
                QString mapped;
                for (const QChar c : body)
                    mapped += t.doubleStruck.value(QString(c), QString(c));
                return mapped.isEmpty() ? Frag{} : Frag{textEl(mapped)};
            }
            const bool plain = name.startsWith(QLatin1String("text")) ||
                               name == QLatin1String("mbox") || name == QLatin1String("hbox");
            return {spanEl(QStringLiteral("mt"),
                           plain ? (body.isEmpty() ? Frag{} : Frag{textEl(body)}) : render(body))};
        }
        if (name == QLatin1String("begin"))
            return environment(raw());
        if (name == QLatin1String("end")) {
            raw();
            return {};
        }
        if (name == QLatin1String("\\") || name == QLatin1String("newline") ||
            name == QLatin1String("cr")) {
            prev = Start;
            El br;
            br.type = El::Br;
            return {br};
        }
        if (t.silent.contains(name)) {
            if (at(i) == QLatin1Char('.'))
                ++i;
            return {};
        }
        if (t.accents.contains(name)) {
            const Frag body = arg();
            prev = Ord;
            // One character, alone or in <i>, takes the combining accent.
            QString one;
            if (body.size() == 1) {
                const El &e = body.first();
                const El &inner =
                    e.type == El::Span && e.cls == QLatin1String("i") && e.kids.size() == 1
                        ? e.kids.first()
                        : e;
                if (inner.type == El::Text && inner.text.size() == 1 &&
                    !QStringLiteral("<&>\"").contains(inner.text))
                    one = inner.text;
            }
            if (!one.isEmpty() && name != QLatin1String("vec") &&
                !name.startsWith(QLatin1String("wide")) && name != QLatin1String("overline"))
                return {textEl(one + t.accents.value(name))};
            return {spanEl(QStringLiteral("ma"), body)};
        }
        if (const auto big = t.big.constFind(name); big != t.big.cend()) {
            prev = Op;
            return {spanEl(t.integrals.contains(name) ? QStringLiteral("mop is-int")
                                                      : QStringLiteral("mop"),
                           {textEl(*big)})};
        }
        if (const auto rel = t.relations.constFind(name); rel != t.relations.cend())
            return op(*rel);
        if (const auto symbol = t.symbols.constFind(name); symbol != t.symbols.cend()) {
            static const QRegularExpression blank(QStringLiteral("^[\\s\\x{2000}-\\x{200a}]*$"));
            if (blank.match(*symbol).hasMatch())
                return symbol->isEmpty() ? Frag{} : Frag{textEl(*symbol)};
            prev = t.openers.contains(name) ? Open : Ord;
            return t.greek.contains(name) ? Frag{spanEl(QStringLiteral("i"), {textEl(*symbol)})}
                                          : Frag{textEl(*symbol)};
        }
        if (t.functions.contains(name)) {
            prev = Op;
            return {
                spanEl(t.limits.contains(name) ? QStringLiteral("mn is-lim") : QStringLiteral("mn"),
                       {textEl(name)})};
        }
        prev = Ord;
        return name.isEmpty() ? Frag{} : Frag{textEl(name)};
    }

    Frag atom()
    {
        Deeper deeper(depth);
        const QChar c = s.at(i);
        if (depth > MaxDepth) { // Literal text and no recursion, as render() reads it.
            if (c == QLatin1Char('{')) {
                const QString body = raw();
                return body.isEmpty() ? Frag{} : Frag{textEl(body)};
            }
            ++i;
            if (c == QLatin1Char('}') || c == QLatin1Char(' ') || c == QLatin1Char('\n'))
                return {};
            prev = Ord;
            return {textEl(QString(c))};
        }
        if (c == QLatin1Char('\\'))
            return command();
        if (c == QLatin1Char('{')) {
            ++i;
            return seq(QLatin1Char('}'));
        }
        if (c == QLatin1Char('}')) {
            ++i;
            return {};
        }
        if (c == QLatin1Char('^') || c == QLatin1Char('_'))
            return scripts({});
        if (c == QLatin1Char('&')) {
            ++i;
            prev = Start;
            return {spanEl(QStringLiteral("mg"), {})};
        }
        if (c == QLatin1Char(' ') || c == QLatin1Char('\n') || c == QLatin1Char('\t') ||
            c == QLatin1Char('~')) {
            ++i;
            return c == QLatin1Char('~') ? Frag{textEl(QStringLiteral(" "))} : Frag{};
        }
        const auto &t = tables();
        if (const auto found = t.operators.constFind(QString(c)); found != t.operators.cend()) {
            ++i;
            return op(*found);
        }
        ++i;
        if (isLetter(c)) {
            prev = Ord;
            return {spanEl(QStringLiteral("i"), {textEl(QString(c))})};
        }
        if (c == QLatin1Char('\'')) {
            prev = Ord;
            return {textEl(QStringLiteral("′"))};
        }
        prev = c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char(',') ? Open
               : c == QLatin1Char(';')                                                 ? Op
                                                                                       : Ord;
        return {textEl(QString(c))};
    }

    const QString &s;
    int n = 0, i = 0;
    bool display = false;
    int depth = 0;
    Prev prev = Start;
};

// Chromium's plain text of the tree: a flex or grid box's children are
// boxes, each set off by Breaks; hidden text (an empty limit slot's twin)
// copies nothing; <br> is a line end; a run of spaces copies as one.
void flatten(const Frag &frag, bool boxes, bool hidden, QVector<tex::Piece> &out)
{
    for (const El &e : frag) {
        if (boxes)
            out << tex::Piece{tex::Piece::Break, {}};
        switch (e.type) {
        case El::Text:
            if (!hidden && !e.text.isEmpty()) {
                if (!out.isEmpty() && out.last().kind == tex::Piece::Text)
                    out.last().text += e.text;
                else
                    out << tex::Piece{tex::Piece::Text, e.text};
                static const QRegularExpression spaces(QStringLiteral(" {2,}"));
                out.last().text.replace(spaces, QStringLiteral(" "));
            }
            break;
        case El::Br:
            out << tex::Piece{tex::Piece::Text, QStringLiteral("\n")};
            break;
        case El::Delim:
            out << tex::Piece{tex::Piece::Mark, {}};
            break;
        case El::Span:
        case El::Flex:
            flatten(e.kids, e.type == El::Flex, hidden || e.hidden, out);
            break;
        }
        if (boxes)
            out << tex::Piece{tex::Piece::Break, {}};
    }
}
} // namespace

namespace tex
{
QVector<Piece> copy(const QString &source, bool display)
{
    if (source.size() > MaxSource)
        return {Piece{Piece::Text, source}};
    CopyReader reader(source, display, 0);
    QVector<Piece> out;
    flatten(reader.seq(QChar()), false, false, out);
    return out;
}

Rendered render(const QString &source, bool display, const Style &style, qreal dpr)
{
    Rendered result;
    if (source.size() > MaxSource) {
        result.error = QStringLiteral("The formula is too long to render.");
        return result;
    }
    if (source.trimmed().isEmpty()) {
        result.error = QStringLiteral("The formula is empty.");
        return result;
    }
    if (!(dpr >= 0.5 && dpr <= 8))
        dpr = 1;
    const qreal text = style.pixelSize >= 1 && style.pixelSize <= 512 ? style.pixelSize : 16;
    const qreal size = text * (display ? 1.4 : 1.1); // .md-math and .md-imath.
    Context context(style, display);
    Parser parser(context, source, display, size, 0);
    const Box box = parser.seq(QChar(), Halt::None);
    if (context.failed()) {
        result.error = context.error;
        return result;
    }
    // Room for italic overhang and ink past a letter's box.
    const qreal padX = 0.15 * size, padY = 0.12 * size;
    const qreal width = box.w + 2 * padX, height = box.height() + 2 * padY;
    const int pixelsW = int(std::ceil(width * dpr)), pixelsH = int(std::ceil(height * dpr));
    if (width > MaxWidth || height > MaxHeight || qreal(pixelsW) * pixelsH > MaxPixels) {
        result.error = QStringLiteral("The formula is too large to render.");
        return result;
    }
    QImage image(pixelsW, pixelsH, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        result.error = QStringLiteral("The formula is too large to render.");
        return result;
    }
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        painter.setPen(style.color);
        paint(painter, context, box.items, QPointF(padX, padY + box.a));
    }
    result.image = std::move(image);
    result.size = QSizeF(pixelsW / dpr, pixelsH / dpr);
    result.baseline = padY + box.a;
    result.ok = true;
    return result;
}
} // namespace tex
