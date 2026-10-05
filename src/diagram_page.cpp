#include "diagram_engine.h"

#include <QDate>
#include <QDateTime>
#include <QTime>
#include <QTimeZone>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <vector>

// Page sketches and folders: wireframe and files (diagram.js WF_SECTIONS …
// filesScene).
namespace diagram
{
namespace
{
// Math.round: halves go up.
double jsRound(double v) { return std::floor(v + 0.5); }

const QString &ellipsis()
{
    static const QString s(QChar(0x2026));
    return s;
}

// parseInt(text, 10): NaN without leading digits; a huge number as a double.
double parseIntJs(const QString &text)
{
    static const Re lead(QStringLiteral("^\\s*([+-]?)(\\d+)"));
    const auto m = lead.match(text);
    if (!m.hasMatch())
        return NaN;
    const double v = std::strtod(m.captured(2).toLatin1().constData(), nullptr);
    return m.captured(1) == QLatin1String("-") ? -v : v;
}

// +text for a text of ASCII digits only.
double digitsValue(const QString &digits)
{
    return std::strtod(digits.toLatin1().constData(), nullptr);
}

const Re &digitsOnly()
{
    static const Re re(QStringLiteral("^\\d+\\z"));
    return re;
}

/* Wireframes: a website or app screen drawn from a list of sections */

const QHash<QString, QString> &wfSections()
{
    static const QHash<QString, QString> map = [] {
        QHash<QString, QString> m;
        const auto put = [&](const char *type, std::initializer_list<const char *> words) {
            for (const char *word : words)
                m.insert(QLatin1String(word), QLatin1String(type));
        };
        put("nav", {"nav", "navbar", "header", "menu", "navigation", "topbar"});
        put("hero", {"hero", "banner", "intro", "jumbotron", "welcome"});
        put("logos", {"logos", "clients", "partners", "brands", "trust"});
        put("features",
            {"features", "benefits", "advantages", "services", "pains", "problems", "grid", "why"});
        put("cards", {"cards", "cases", "products", "portfolio", "projects", "team", "blog",
                      "articles", "catalog"});
        put("steps", {"steps", "process", "how", "workflow", "timeline"});
        put("stats", {"stats", "numbers", "metrics", "facts", "kpi"});
        put("reviews", {"reviews", "testimonials", "quotes", "feedback"});
        put("pricing", {"pricing", "plans", "prices", "tariffs"});
        put("faq", {"faq", "questions", "accordion"});
        put("cta", {"cta", "offer", "action", "callout"});
        put("form",
            {"form", "contact", "contacts", "signup", "subscribe", "lead", "login", "newsletter"});
        put("gallery", {"gallery", "images", "photos", "showcase"});
        put("section", {"section", "about", "content", "split", "block", "story", "feature"});
        put("footer", {"footer"});
        return m;
    }();
    return map;
}

bool wfList(const QString &type)
{
    return type == QLatin1String("nav") || type == QLatin1String("logos") ||
           type == QLatin1String("steps") || type == QLatin1String("form") ||
           type == QLatin1String("footer") || type == QLatin1String("gallery");
}

QString wfMedia(const QString &word)
{
    static const QHash<QString, QString> map{
        {QStringLiteral("image"), QStringLiteral("image")},
        {QStringLiteral("img"), QStringLiteral("image")},
        {QStringLiteral("photo"), QStringLiteral("image")},
        {QStringLiteral("picture"), QStringLiteral("image")},
        {QStringLiteral("screenshot"), QStringLiteral("image")},
        {QStringLiteral("illustration"), QStringLiteral("image")},
        {QStringLiteral("video"), QStringLiteral("video")},
        {QStringLiteral("map"), QStringLiteral("map")}};
    return map.value(word);
}

// I18n wf.<type> (English).
QString wfLabel(const QString &type)
{
    static const QHash<QString, QString> map{
        {QStringLiteral("nav"), QStringLiteral("Navigation")},
        {QStringLiteral("hero"), QStringLiteral("First screen")},
        {QStringLiteral("logos"), QStringLiteral("Logos")},
        {QStringLiteral("features"), QStringLiteral("Features")},
        {QStringLiteral("cards"), QStringLiteral("Cards")},
        {QStringLiteral("steps"), QStringLiteral("Steps")},
        {QStringLiteral("stats"), QStringLiteral("Numbers")},
        {QStringLiteral("reviews"), QStringLiteral("Reviews")},
        {QStringLiteral("pricing"), QStringLiteral("Pricing")},
        {QStringLiteral("faq"), QStringLiteral("Questions")},
        {QStringLiteral("cta"), QStringLiteral("Call to action")},
        {QStringLiteral("form"), QStringLiteral("Form")},
        {QStringLiteral("gallery"), QStringLiteral("Gallery")},
        {QStringLiteral("section"), QStringLiteral("Text block")},
        {QStringLiteral("footer"), QStringLiteral("Footer")}};
    return map.value(type, QStringLiteral("wf.") + type);
}

struct WfSet {
    double max, phone, bar, status, home, radius, phoneRadius, btn, gap;
};
constexpr WfSet WF{1040, 390, 46, 46, 30, 14, 44, 40, 16};

// How text is set in a sketch: [size, weight, line height].
struct WStyle {
    double size;
    int weight;
    double line;
};
namespace WT
{
constexpr WStyle brand{15, 650, 20}, link{12.5, 500, 16}, h1{34, 650, 40}, h1s{25, 650, 30},
    lead{15, 450, 22}, leads{14, 450, 20}, h2{22, 620, 28}, h2s{18, 620, 24}, sub{13.5, 450, 20},
    title{14, 600, 20}, body{12.5, 450, 18}, small{12, 500, 16}, tag{11, 600, 14},
    value{28, 650, 32}, price{24, 650, 28}, btn{13, 600, 18}, logo{15, 650, 18},
    quote{13.5, 450, 20}, q{13.5, 600, 20};
} // namespace WT

// The painter's GLYPHS draw these by name.
const char *const WF_ICONS[] = {"spark", "bolt",  "shield", "chart",
                                "heart", "clock", "star",   "check"};
constexpr int WF_ICON_COUNT = int(sizeof(WF_ICONS) / sizeof(WF_ICONS[0]));

const Re &wfArea()
{
    static const Re re(QString::fromUtf8("(comment|message|question|details|коммент|сообщ|вопрос|"
                                         "описан|задач|пожелан|текст)"),
                       I);
    return re;
}

const Re &colonSpace()
{
    static const Re re(QStringLiteral(":\\s"));
    return re;
}

struct WfItem {
    QString title, desc;
};

struct WfSection {
    QString type, key;
    int n = 0;
    QString heading;
    QStringList text;
    QVector<WfItem> items;
    QStringList buttons;
    QString media, badge;
};

struct WfPage {
    QString title;
    bool mobile = false;
    QVector<WfSection> sections;
};

WfItem wfItem(const QString &text)
{
    const QString s = cleanLabel(text);
    const auto m = colonSpace().match(s);
    const qsizetype at = m.hasMatch() ? m.capturedStart() : -1;
    return at > 0 ? WfItem{s.left(at).trimmed(), s.mid(at + 1).trimmed()} : WfItem{s, {}};
}

// splitList, each cleaned, empty ones left out.
QStringList wfListOf(const QString &text)
{
    QStringList out;
    for (const QString &part : splitList(text)) {
        const QString s = cleanLabel(part);
        if (!s.isEmpty())
            out << s;
    }
    return out;
}

void wfLine(WfSection &sec, const QString &word, const QString &rest, const QString &line)
{
    const QString media = wfMedia(word);
    if (!media.isEmpty()) {
        sec.media = media;
        return;
    }
    const auto is = [&](std::initializer_list<const char *> words) {
        return std::any_of(words.begin(), words.end(),
                           [&](const char *w) { return word == QLatin1String(w); });
    };
    if (is({"title", "heading", "h1", "h2"})) {
        sec.heading = cleanLabel(rest);
        return;
    }
    if (is({"text", "subtitle", "p", "description", "lead"})) {
        if (!rest.isEmpty())
            sec.text << cleanLabel(rest);
        return;
    }
    if (is({"button", "btn"})) {
        if (!rest.isEmpty())
            sec.buttons << cleanLabel(rest);
        return;
    }
    if (is({"buttons"})) {
        sec.buttons << wfListOf(rest);
        return;
    }
    if (is({"badge", "tag", "eyebrow"})) {
        sec.badge = cleanLabel(rest);
        return;
    }
    if (is({"links", "fields", "items", "list"})) {
        for (const QString &s : wfListOf(rest))
            sec.items << wfItem(s);
        return;
    }
    if (is({"link", "field", "input", "item"})) {
        if (!rest.isEmpty())
            sec.items << wfItem(rest);
        return;
    }
    if (wfList(sec.type) && !colonSpace().match(line).hasMatch() &&
        line.contains(QLatin1Char(','))) {
        for (const QString &s : wfListOf(line))
            sec.items << wfItem(s);
    } else {
        sec.items << wfItem(line);
    }
}

std::optional<WfPage> parseWireframe(const QStringList &lines)
{
    static const Re device(QStringLiteral("\\b(mobile|phone|iphone|android|app)\\b"), I),
        phoneWord(QStringLiteral("mobile|phone"), I),
        wordLine(QStringLiteral("^([a-z][a-z0-9-]*)(?:\\s+(.*))?\\z")),
        bar(QStringLiteral("\\s+\\|\\s+"));
    WfPage page;
    page.mobile = !lines.isEmpty() && device.match(lines.first()).hasMatch();
    QHash<QString, int> counts;
    int base = -1;
    WfSection *current = nullptr;
    const auto open = [&](const QString &type, const QString &rest) {
        const QStringList parts = rest.split(bar);
        const int n = counts.value(type, -1) + 1;
        counts.insert(type, n);
        WfSection s;
        s.type = type;
        s.key = type + QString::number(n);
        s.n = n;
        s.heading = cleanLabel(parts.value(0));
        if (!parts.value(1).isEmpty()) {
            for (const QString &part : splitList(parts.at(1)))
                s.items << wfItem(part);
        }
        if (!parts.value(2).isEmpty())
            s.buttons << wfListOf(parts.at(2));
        page.sections << s;
        current = &page.sections.last();
    };
    for (int i = 1; i < lines.size(); ++i) {
        const QString &raw = lines.at(i);
        const int indent = indentOf(raw);
        const QString line = bare(raw);
        if (line.isEmpty())
            continue;
        const auto m = wordLine.match(line);
        const QString word = m.hasMatch() ? m.captured(1) : QString(),
                      rest = m.hasMatch() ? m.captured(2).trimmed() : QString();
        if (!current && word == QLatin1String("title")) {
            page.title = cleanLabel(rest);
            continue;
        }
        if (!current && (word == QLatin1String("device") || word == QLatin1String("mobile") ||
                         word == QLatin1String("desktop"))) {
            page.mobile = phoneWord.match(word + QLatin1Char(' ') + rest).hasMatch();
            continue;
        }
        const QString type = wfSections().value(word);
        if (!type.isEmpty() && (!current || indent <= base)) {
            base = indent;
            open(type, rest);
            continue;
        }
        if (!current) {
            base = indent - 1;
            open(QStringLiteral("section"), QString());
        }
        wfLine(*current, word, rest, line);
    }
    if (page.sections.isEmpty())
        return std::nullopt;
    return page;
}

enum class CardKind { Feature, Image, Review, Plan, Cell };

class WireScene
{
  public:
    WireScene(Ctx &c, const WfPage &page) : c(c), page(page) {}
    std::optional<Result> build();

  private:
    Ctx &c;
    const WfPage &page;
    bool phone = false, wide = false;
    double W = 0, cw = 0, x0 = 0, cx = 0, pad = 0;
    int tone = 1;
    QVector<Spec> items;
    QHash<QString, Tip> tips;
    QString scope = QStringLiteral("wf");
    double base = 0;

    Spec &add(const QString &name, Type type, double d = 0, int layer = DefaultLayer)
    {
        Spec s = item(type, scope + QLatin1Char(':') + name, base + d, layer);
        s.fixed.tone = tone;
        items << s;
        return items.last();
    }
    Spec &label(const QString &name, double x, double y, const QStringList &ls, const QString &cls,
                const QString &anchor, double lineHeight, double size, int weight, double d)
    {
        Spec &s = add(name, Type::Label, d);
        s.props.x = x;
        s.props.y = y;
        s.fixed.lines = ls;
        s.fixed.cls = cls;
        s.fixed.anchor = anchor;
        s.fixed.lineHeight = lineHeight;
        s.fixed.size = size;
        s.fixed.weight = weight;
        return s;
    }
    QStringList lines(const QString &str, WStyle st, double maxW, int cap = 0)
    {
        QStringList out = wrap(c, str, std::max(40.0, maxW), st.size, st.weight);
        if (cap && out.size() > cap) {
            out = out.mid(0, cap);
            out[cap - 1] = truncate(c, out.at(cap - 1) + ellipsis(), maxW, st.size, st.weight);
        }
        return out;
    }
    double tall(const QString &str, WStyle st, double maxW, int cap = 0)
    {
        return str.isEmpty() ? 0 : lines(str, st, maxW, cap).size() * st.line;
    }
    double text(const QString &name, const QString &str, WStyle st, const QString &cls, double x,
                double top, double maxW, const QString &anchor = QStringLiteral("start"),
                int cap = 0, double d = 0)
    {
        if (str.isEmpty())
            return 0;
        const QStringList ls = lines(str, st, maxW, cap);
        const double h = ls.size() * st.line;
        label(name, x, top + h / 2, ls, cls, anchor, st.line, st.size, st.weight, d);
        return h;
    }
    int box(const QString &name, double x, double y, double w, double h, const QString &cls,
            double rx = 12, double d = 0, int layer = Edges)
    {
        Spec &s = add(name, Type::WBox, d, layer);
        s.props.x = x;
        s.props.y = y;
        s.props.w = w;
        s.props.h = h;
        s.fixed.cls = cls;
        s.fixed.rx = rx;
        return int(items.size()) - 1;
    }
    void glyph(const QString &name, const QString &icon, double x, double y, double size,
               const QString &cls = {}, double d = 0)
    {
        Spec &s = add(name, Type::WGlyph, d, Labels);
        s.props.x = x;
        s.props.y = y;
        s.props.s = size;
        s.fixed.icon = icon;
        s.fixed.cls = cls;
    }
    void skel(const QString &name, double x, double y, double w, double h = 9, double d = 0,
              const QString &cls = {})
    {
        box(name, x, y, w, h, QStringLiteral("dg-wf-skel") + cls, h / 2, d, Nodes);
    }
    void rule(const QString &name, double x1, double y, double x2, double d = 0)
    {
        Spec &s = add(name, Type::Line, d, Edges);
        s.props.x1 = x1;
        s.props.y1 = y;
        s.props.x2 = x2;
        s.props.y2 = y;
        s.fixed.cls = QStringLiteral("dg-wf-rule");
    }
    // Moves what was drawn since `from` down (a line by both ends).
    void shift(int from, double dy, int to = -1)
    {
        if (to < 0)
            to = int(items.size());
        for (int i = from; i < to && i < items.size(); ++i) {
            Props &p = items[i].props;
            if (items.at(i).type == Type::Line) {
                p.y1 += dy;
                p.y2 += dy;
            } else {
                p.y += dy;
            }
        }
    }
    double btnW(const QString &text)
    {
        return text.isEmpty() ? 112
                              : std::ceil(c.textWidth(text, WT::btn.size, WT::btn.weight)) + 40;
    }
    void button(const QString &name, const QString &text, double x, double y, double w,
                bool primary, double h = WF.btn, double d = 0.2)
    {
        box(name, x, y, w, h,
            primary ? QStringLiteral("dg-wf-btn is-primary") : QStringLiteral("dg-wf-btn"), h / 2,
            d, Nodes);
        if (!text.isEmpty())
            label(name + QLatin1Char('t'), x + w / 2, y + h / 2,
                  {truncate(c, text, w - 24, WT::btn.size, WT::btn.weight)},
                  primary ? QStringLiteral("dg-wf-on") : QStringLiteral("dg-wf-strong"),
                  QStringLiteral("middle"), WT::btn.line, WT::btn.size, WT::btn.weight, d + 0.02);
        else
            skel(name + QLatin1Char('s'), x + w / 2 - 28, y + h / 2 - 4, 56, 8, d + 0.02,
                 primary ? QStringLiteral(" is-on") : QString());
    }
    double buttons(const QString &name, const QStringList &labels, double x, double top,
                   double maxW, const QString &align, double d = 0.2)
    {
        if (labels.isEmpty())
            return 0;
        QVector<double> ws;
        double total = 10 * (labels.size() - 1);
        for (const QString &l : labels) {
            ws << btnW(l);
            total += ws.last();
        }
        const bool middle = align == QLatin1String("middle");
        if (total <= maxW) {
            double bx = middle ? x - total / 2 : x;
            for (int i = 0; i < labels.size(); ++i) {
                button(name + QString::number(i), labels.at(i), bx, top, ws.at(i), i == 0, WF.btn,
                       d + i * 0.03);
                bx += ws.at(i) + 10;
            }
            return WF.btn;
        }
        const double w = std::min(maxW, 360.0), left = middle ? x - w / 2 : x;
        for (int i = 0; i < labels.size(); ++i)
            button(name + QString::number(i), labels.at(i), left, top + i * (WF.btn + 10), w,
                   i == 0, WF.btn, d + i * 0.03);
        return labels.size() * (WF.btn + 10) - 10;
    }
    double pill(const QString &name, const QString &text, double x, double y, const QString &align)
    {
        const double w = std::ceil(c.textWidth(text, WT::tag.size, WT::tag.weight)) + 22,
                     left = align == QLatin1String("middle") ? x - w / 2 : x;
        box(name, left, y, w, 24, QStringLiteral("dg-wf-pill"), 12, 0, Nodes);
        label(name + QLatin1Char('t'), left + w / 2, y + 12, {text},
              QStringLiteral("dg-wf-pill-text"), QStringLiteral("middle"), 14, WT::tag.size,
              WT::tag.weight, 0.02);
        return 24;
    }
    void media(const QString &name, double x, double y, double w, double h,
               const QString &kind = QStringLiteral("image"), double d = 0.15)
    {
        box(name, x, y, w, h, QStringLiteral("dg-wf-media"), std::min(14.0, h / 4), d);
        glyph(name + QLatin1Char('g'),
              kind == QLatin1String("video") ? QStringLiteral("play")
              : kind == QLatin1String("map") ? QStringLiteral("pin")
                                             : QStringLiteral("image"),
              x + w / 2, y + h / 2, std::min(34.0, h * 0.35), QStringLiteral("is-media"), d + 0.05);
    }
    double header(const WfSection &s, double y)
    {
        return header(s, y, QStringLiteral("middle"), std::min(cw, 640.0), cx);
    }
    double header(const WfSection &s, double y, const QString &align, double maxW, double x)
    {
        const double start = y;
        if (!s.badge.isEmpty())
            y += pill(QStringLiteral("badge"), s.badge, x, y, align) + 16;
        const double h = text(QStringLiteral("h"), s.heading, wide ? WT::h2 : WT::h2s,
                              QStringLiteral("dg-wf-strong"), x, y, maxW, align);
        y += h;
        const QString sub = s.text.join(QLatin1Char(' '));
        if (!sub.isEmpty()) {
            if (h)
                y += 10;
            y += text(QStringLiteral("t"), sub, WT::sub, QStringLiteral("dg-wf-text"), x, y, maxW,
                      align, 0, 0.05);
        }
        return y > start ? y + (wide ? 32 : 24) : y;
    }
    int colsFor(double min)
    {
        return int(std::max(1.0, std::min(4.0, std::floor((cw + WF.gap) / (min + WF.gap)))));
    }
    double grid(const QString &name, const QVector<WfItem> &list, double y, int maxCols,
                CardKind card, const WfSection &s)
    {
        const int n = int(list.size());
        if (!n)
            return y;
        const int across = std::max(1, std::min(maxCols, n));
        const int rows = (n + across - 1) / across, per = (n + rows - 1) / rows;
        const double w = (cw - WF.gap * (per - 1)) / per;
        for (int r = 0; r < rows; ++r) {
            const QVector<WfItem> row = list.mid(r * per, per);
            double h = -INFINITY;
            for (const WfItem &it : row)
                h = std::max(h, measure(card, it, w));
            double x = cx - (row.size() * w + WF.gap * (row.size() - 1)) / 2;
            for (int k = 0; k < row.size(); ++k) {
                draw(card, name + QString::number(r * per + k), row.at(k), x, y, w, h, r * per + k,
                     s);
                x += w + WF.gap;
            }
            y += h + (r < rows - 1 ? WF.gap : 0);
        }
        return y;
    }
    static QVector<WfItem> blank(int n) { return QVector<WfItem>(n); }
    double titleOrSkel(const QString &key, const QString &str, WStyle st, const QString &cls,
                       double x, double y, double w,
                       const QString &anchor = QStringLiteral("start"), int cap = 0, double d = 0)
    {
        if (!str.isEmpty())
            return text(key, str, st, cls, x, y, w, anchor, cap, d);
        skel(key, anchor == QLatin1String("middle") ? x - w * 0.3 : x, y + 5, w * 0.6, 10, d);
        return 20;
    }

    // A review: the quote, and who said it (Name, role).
    struct Review {
        QString quote, name, role;
    };
    static Review reviewOf(const WfItem &item)
    {
        const QString quote = !item.desc.isEmpty() ? item.desc : item.title,
                      who = !item.desc.isEmpty() ? item.title : QString();
        const qsizetype at = who.indexOf(QLatin1Char(','));
        return {quote, at > 0 ? who.left(at).trimmed() : who,
                at > 0 ? who.mid(at + 1).trimmed() : QString()};
    }
    // A plan: Name* (the featured one): price · feature · feature.
    struct Plan {
        bool featured = false;
        QString name, price;
        QStringList features;
    };
    static Plan planOf(const WfItem &item)
    {
        static const Re featured(QString::fromUtf8("(\\*|★)\\s*\\z|\\((popular|популяр|хит|best|"
                                                   "рекоменд)[^)]*\\)\\s*\\z"),
                                 I),
            mark(QString::fromUtf8("\\s*(\\*+|★)\\s*\\z|\\s*\\((popular|популяр|хит|best|рекоменд)"
                                   "[^)]*\\)\\s*\\z"),
                 I),
            parts(QString::fromUtf8("\\s*[·|;]\\s*|,\\s+"));
        Plan p;
        p.featured = featured.match(item.title).hasMatch();
        QString name = item.title;
        const auto m = mark.match(name);
        if (m.hasMatch())
            name.remove(m.capturedStart(), m.capturedLength());
        p.name = name.trimmed();
        QStringList list;
        for (const QString &part : item.desc.split(parts)) {
            if (!part.trimmed().isEmpty())
                list << part.trimmed();
        }
        p.price = list.value(0);
        p.features = list.mid(1);
        return p;
    }

    double measure(CardKind card, const WfItem &item, double w)
    {
        switch (card) {
        case CardKind::Feature:
            return 20 + 36 + 16 +
                   (!item.title.isEmpty() ? tall(item.title, WT::title, w - 40) : 20) +
                   (!item.desc.isEmpty()    ? 6 + tall(item.desc, WT::body, w - 40)
                    : !item.title.isEmpty() ? 0
                                            : 22) +
                   24;
        case CardKind::Image:
            return 8 + jsRound((w - 16) * 0.6) + 16 +
                   (!item.title.isEmpty() ? tall(item.title, WT::title, w - 32) : 20) +
                   (!item.desc.isEmpty() ? 6 + tall(item.desc, WT::body, w - 32) : 0) + 20;
        case CardKind::Review: {
            const Review r = reviewOf(item);
            return 20 + 30 + (!r.quote.isEmpty() ? tall(r.quote, WT::quote, w - 40) : 40) + 20 +
                   34 + 20;
        }
        case CardKind::Plan: {
            const Plan p = planOf(item);
            double features = 0;
            for (const QString &t : p.features)
                features += tall(t, WT::body, w - 72) + 8;
            return 24 + 20 + 8 + WT::price.line + 20 + features + 16 + WF.btn + 24;
        }
        case CardKind::Cell:
            return jsRound(w * 0.72) +
                   (!item.title.isEmpty() ? 10 + tall(item.title, WT::small, w) : 0);
        }
        return 0;
    }
    void draw(CardKind card, const QString &key, const WfItem &item, double x, double y, double w,
              double h, int i, const WfSection &s)
    {
        const auto k = [&](const char *suffix) { return key + QLatin1String(suffix); };
        switch (card) {
        case CardKind::Feature: {
            const double d = 0.08 + i * 0.04;
            box(k("c"), x, y, w, h, QStringLiteral("dg-wf-card"), 16, d);
            box(k("i"), x + 20, y + 20, 36, 36, QStringLiteral("dg-wf-icon"), 10, d + 0.02, Nodes);
            glyph(k("g"), QLatin1String(WF_ICONS[i % WF_ICON_COUNT]), x + 38, y + 38, 18,
                  QStringLiteral("is-icon"), d + 0.03);
            double ty = y + 72;
            ty += titleOrSkel(k("t"), item.title, WT::title, QStringLiteral("dg-wf-strong"), x + 20,
                              ty, w - 40, QStringLiteral("start"), 0, d + 0.03);
            if (!item.desc.isEmpty())
                text(k("d"), item.desc, WT::body, QStringLiteral("dg-wf-text"), x + 20, ty + 6,
                     w - 40, QStringLiteral("start"), 0, d + 0.05);
            else if (item.title.isEmpty())
                skel(k("d"), x + 20, ty + 8, (w - 40) * 0.85, 8, d + 0.05);
            return;
        }
        case CardKind::Image: {
            const double d = 0.08 + i * 0.04, mh = jsRound((w - 16) * 0.6);
            box(k("c"), x, y, w, h, QStringLiteral("dg-wf-card"), 16, d);
            media(k("m"), x + 8, y + 8, w - 16, mh, QStringLiteral("image"), d + 0.02);
            double ty = y + 8 + mh + 16;
            ty += titleOrSkel(k("t"), item.title, WT::title, QStringLiteral("dg-wf-strong"), x + 16,
                              ty, w - 32, QStringLiteral("start"), 0, d + 0.03);
            if (!item.desc.isEmpty())
                text(k("d"), item.desc, WT::body, QStringLiteral("dg-wf-text"), x + 16, ty + 6,
                     w - 32, QStringLiteral("start"), 0, d + 0.05);
            return;
        }
        case CardKind::Review: {
            const double d = 0.08 + i * 0.05;
            const Review r = reviewOf(item);
            box(k("c"), x, y, w, h, QStringLiteral("dg-wf-card"), 16, d);
            label(k("q"), x + 18, y + 40, {QString(QChar(0x201c))}, QStringLiteral("dg-wf-quote"),
                  QStringLiteral("start"), 40, 44, 600, d + 0.02);
            if (!r.quote.isEmpty()) {
                text(k("t"), r.quote, WT::quote, QStringLiteral("dg-wf-strong is-soft"), x + 20,
                     y + 50, w - 40, QStringLiteral("start"), 0, d + 0.03);
            } else {
                skel(k("t"), x + 20, y + 56, w - 60, 8, d + 0.03);
                skel(k("u"), x + 20, y + 72, (w - 60) * 0.7, 8, d + 0.04);
            }
            const double ay = y + h - 20 - 17;
            Spec &avatar = add(k("a"), Type::Dot, d + 0.05, Nodes);
            avatar.props.x = x + 37;
            avatar.props.y = ay;
            avatar.props.r = 17;
            avatar.fixed.cls = QStringLiteral("dg-wf-avatar");
            if (!r.name.isEmpty())
                text(k("n"), r.name, WStyle{13, 600, 17}, QStringLiteral("dg-wf-strong"), x + 62,
                     ay - (!r.role.isEmpty() ? 17 : 8.5), w - 82, QStringLiteral("start"), 1,
                     d + 0.06);
            else
                skel(k("n"), x + 62, ay - 4, 90, 8, d + 0.06);
            if (!r.role.isEmpty())
                text(k("r"), r.role, WT::small, QStringLiteral("dg-wf-muted"), x + 62, ay + 1,
                     w - 82, QStringLiteral("start"), 1, d + 0.07);
            return;
        }
        case CardKind::Plan: {
            const double d = 0.08 + i * 0.05;
            const Plan p = planOf(item);
            box(k("c"), x, y, w, h,
                p.featured ? QStringLiteral("dg-wf-card is-featured")
                           : QStringLiteral("dg-wf-card"),
                18, d);
            double ty = y + 24;
            ty += titleOrSkel(k("n"), p.name, WT::title, QStringLiteral("dg-wf-text"), x + 24, ty,
                              w - 48, QStringLiteral("start"), 1, d + 0.02) +
                  8;
            if (!p.price.isEmpty()) {
                ty += text(k("p"), p.price, WT::price, QStringLiteral("dg-wf-strong"), x + 24, ty,
                           w - 48, QStringLiteral("start"), 1, d + 0.03);
            } else {
                skel(k("p"), x + 24, ty + 6, 90, 18, d + 0.03);
                ty += WT::price.line;
            }
            ty += 20;
            for (int f = 0; f < p.features.size(); ++f) {
                glyph(key + QLatin1Char('k') + QString::number(f), QStringLiteral("check"), x + 32,
                      ty + WT::body.line / 2, 15, QStringLiteral("is-check"), d + 0.04 + f * 0.02);
                ty += text(key + QLatin1Char('f') + QString::number(f), p.features.at(f), WT::body,
                           QStringLiteral("dg-wf-text"), x + 48, ty, w - 72,
                           QStringLiteral("start"), 0, d + 0.04 + f * 0.02) +
                      8;
            }
            // The featured plan takes the first button, the others the last.
            const QString label =
                s.buttons.value(p.featured ? 0 : int(s.buttons.size()) - 1, QString());
            button(k("b"), label, x + 24, y + h - 24 - WF.btn, w - 48, p.featured, WF.btn,
                   d + 0.08);
            return;
        }
        case CardKind::Cell: {
            const double mh = jsRound(w * 0.72);
            media(k("m"), x, y, w, mh, QStringLiteral("image"), 0.08 + i * 0.04);
            if (!item.title.isEmpty())
                text(k("t"), item.title, WT::small, QStringLiteral("dg-wf-text"), x + w / 2,
                     y + mh + 10, w, QStringLiteral("middle"), 0, 0.12 + i * 0.04);
            return;
        }
        }
    }

    double nav(const WfSection &s, double y);
    double hero(const WfSection &s, double y);
    double logos(const WfSection &s, double y);
    double gallery(const WfSection &s, double y);
    double steps(const WfSection &s, double y);
    double stats(const WfSection &s, double y);
    double faq(const WfSection &s, double y);
    double cta(const WfSection &s, double y);
    double form(const WfSection &s, double y);
    double section(const WfSection &s, double y);
    double footer(const WfSection &s, double y);
    double lay(const WfSection &s, double y);
};

double WireScene::nav(const WfSection &s, double y)
{
    const double h = wide ? 64 : 56, mid = y + h / 2;
    const QString &brand = s.heading;
    box(QStringLiteral("mark"), x0, mid - 11, 22, 22, QStringLiteral("dg-wf-mark"), 7, 0, Nodes);
    double left = x0 + 32, right = x0 + cw;
    if (!brand.isEmpty()) {
        const double bw =
            std::min(std::ceil(c.textWidth(brand, WT::brand.size, WT::brand.weight)), cw * 0.42);
        text(QStringLiteral("b"), brand, WT::brand, QStringLiteral("dg-wf-strong"), left,
             mid - WT::brand.line / 2, bw + 1, QStringLiteral("start"), 1);
        left += bw;
    } else {
        skel(QStringLiteral("b"), left, mid - 5, 70, 10);
        left += 70;
    }
    left += 28;
    QStringList links;
    for (const WfItem &item : s.items) {
        if (!item.title.isEmpty())
            links << item.title;
    }
    if (!wide && !links.isEmpty()) {
        glyph(QStringLiteral("menu"), QStringLiteral("menu"), right - 10, mid, 22, QString(), 0.1);
        right -= 36;
    }
    const QStringList navButtons = s.buttons.mid(0, wide ? 2 : 1);
    QVector<double> ws;
    double need = 0;
    for (const QString &l : navButtons) {
        ws << std::ceil(c.textWidth(l, 12.5, 600)) + 30;
        need += ws.last() + 8;
    }
    if (need && right - need >= left) {
        for (int i = int(navButtons.size()) - 1; i >= 0; --i) {
            right -= ws.at(i);
            const bool primary = i == navButtons.size() - 1;
            const QString n = QLatin1Char('n') + QString::number(i);
            box(n, right, mid - 16, ws.at(i), 32,
                primary ? QStringLiteral("dg-wf-btn is-primary") : QStringLiteral("dg-wf-btn"), 16,
                0.12, Nodes);
            label(n + QLatin1Char('t'), right + ws.at(i) / 2, mid, {navButtons.at(i)},
                  primary ? QStringLiteral("dg-wf-on") : QStringLiteral("dg-wf-strong"),
                  QStringLiteral("middle"), 16, 12.5, 600, 0.14);
            right -= 8;
        }
        right -= 20;
    }
    if (wide && !links.isEmpty()) {
        QVector<double> lw;
        for (const QString &t : links)
            lw << std::ceil(c.textWidth(t, WT::link.size, WT::link.weight));
        const double gap = 26;
        const auto span = [&](int k) {
            double sum = 0;
            for (int i = 0; i < k; ++i)
                sum += lw.at(i);
            return sum + gap * (k - 1);
        };
        int n = int(links.size());
        while (n > 0 && span(n) > right - left)
            --n;
        double lx = clamp(cx - span(n) / 2, left, right - span(n));
        for (int i = 0; i < n; ++i) {
            text(QLatin1Char('l') + QString::number(i), links.at(i), WT::link,
                 QStringLiteral("dg-wf-text"), lx, mid - WT::link.line / 2, lw.at(i) + 2,
                 QStringLiteral("start"), 1, 0.05 + i * 0.02);
            lx += lw.at(i) + gap;
        }
    }
    rule(QStringLiteral("rule"), 0, y + h, W, 0.1);
    return y + h;
}

double WireScene::hero(const WfSection &s, double y)
{
    const double top = y + (wide ? 64 : 40);
    const bool split = !s.media.isEmpty() && cw >= 700;
    const int mark = int(items.size());
    const double colW = split ? std::floor(cw * 0.5) : std::min(cw, 720.0);
    const QString align = split ? QStringLiteral("start") : QStringLiteral("middle");
    const double ax = split ? x0 : cx;
    double ty = top;
    if (!s.badge.isEmpty())
        ty += pill(QStringLiteral("badge"), s.badge, ax, ty, align) + 18;
    if (!s.heading.isEmpty()) {
        ty += text(QStringLiteral("h"), s.heading, wide ? WT::h1 : WT::h1s,
                   QStringLiteral("dg-wf-strong"), ax, ty, colW, align);
    } else {
        skel(QStringLiteral("h"), split ? ax : ax - colW * 0.4, ty + 4, colW * 0.8, 22);
        skel(QStringLiteral("h2"), split ? ax : ax - colW * 0.28, ty + 36, colW * 0.56, 22, 0.02);
        ty += 60;
    }
    const QString lead = s.text.join(QLatin1Char(' '));
    if (!lead.isEmpty())
        ty += 16 + text(QStringLiteral("t"), lead, wide ? WT::lead : WT::leads,
                        QStringLiteral("dg-wf-text"), ax, ty + 16,
                        split ? colW - 24 : std::min(cw, 560.0), align, 0, 0.08);
    if (!s.buttons.isEmpty())
        ty += 28 + buttons(QStringLiteral("b"), s.buttons, ax, ty + 28, colW, align, 0.16);
    QStringList trustParts;
    for (const WfItem &item : s.items) {
        const QString t =
            !item.desc.isEmpty() ? item.title + QLatin1Char(' ') + item.desc : item.title;
        if (!t.isEmpty())
            trustParts << t;
    }
    const QString trust = trustParts.join(QString::fromUtf8("   ·   "));
    if (!trust.isEmpty())
        ty += 18 + text(QStringLiteral("n"), trust, WT::small, QStringLiteral("dg-wf-muted"), ax,
                        ty + 18, colW, align, 0, 0.22);
    double bottom = ty;
    if (split) {
        // Text and picture side by side, the shorter centred on the other.
        const double mw = std::floor(cw * 0.45), mh = jsRound(mw * 0.76), block = ty - top;
        if (mh > block)
            shift(mark, (mh - block) / 2);
        media(QStringLiteral("m"), x0 + cw - mw, top + std::max(0.0, (block - mh) / 2), mw, mh,
              s.media);
        bottom = top + std::max(mh, block);
    } else if (!s.media.isEmpty()) {
        const double mh = jsRound(std::min(cw * 0.5, 420.0));
        media(QStringLiteral("m"), x0, ty + 40, cw, mh, s.media);
        bottom = ty + 40 + mh;
    }
    return bottom + (wide ? 64 : 40);
}

double WireScene::logos(const WfSection &s, double y)
{
    y += wide ? 40 : 28;
    if (!s.heading.isEmpty())
        y += text(QStringLiteral("h"), s.heading, WT::small, QStringLiteral("dg-wf-muted"), cx, y,
                  cw, QStringLiteral("middle")) +
             22;
    QStringList names;
    for (const WfItem &item : s.items) {
        if (!item.title.isEmpty())
            names << item.title;
    }
    // `logos 6` (or a lone number) asks for that many blank logos.
    double count = 0;
    if (names.size() == 1 && digitsOnly().match(names.first()).hasMatch()) {
        count = digitsValue(names.first());
    } else if (names.isEmpty()) {
        const double n = parseIntJs(s.heading);
        count = finite(n) && n ? n : 5;
        if (std::isinf(n))
            count = n;
    }
    const QStringList entries =
        count ? QStringList(int(clamp(count, 1, 12)), QString()) : names.mid(0, 16);
    QVector<double> size;
    for (const QString &name : entries)
        size << (!name.isEmpty()
                     ? std::min(160.0, std::ceil(c.textWidth(name, WT::logo.size, WT::logo.weight)))
                     : 86);
    const double gap = wide ? 44 : 26;
    struct Row {
        QVector<int> list;
        double w = 0;
    };
    QVector<Row> rows;
    for (int i = 0; i < entries.size(); ++i) {
        if (!rows.isEmpty() && rows.last().w + gap + size.at(i) <= cw) {
            rows.last().list << i;
            rows.last().w += gap + size.at(i);
        } else {
            rows << Row{{i}, size.at(i)};
        }
    }
    for (int r = 0; r < rows.size(); ++r) {
        double x = cx - rows.at(r).w / 2;
        for (const int i : rows.at(r).list) {
            const QString key = QLatin1Char('l') + QString::number(i);
            if (!entries.at(i).isEmpty())
                text(key, entries.at(i), WT::logo, QStringLiteral("dg-wf-logo"), x, y + 5,
                     size.at(i) + 2, QStringLiteral("start"), 1, 0.05 + i * 0.02);
            else
                skel(key, x, y + 6, size.at(i), 16, 0.05 + i * 0.02);
            x += size.at(i) + gap;
        }
        y += 28 + (r < rows.size() - 1 ? 14 : 0);
    }
    return y + (wide ? 40 : 28);
}

double WireScene::gallery(const WfSection &s, double y)
{
    QVector<WfItem> named;
    for (const WfItem &item : s.items) {
        if (!item.title.isEmpty() && !digitsOnly().match(item.title).hasMatch())
            named << item;
    }
    // `gallery` with a lone number: that many pictures; six otherwise.
    const int count = s.items.size() == 1 && digitsOnly().match(s.items.first().title).hasMatch()
                          ? int(clamp(digitsValue(s.items.first().title), 1, 12))
                          : 6;
    return grid(QStringLiteral("g"), !named.isEmpty() ? named : blank(count), header(s, y + pad),
                cw >= 700 ? 3 : 2, CardKind::Cell, s) +
           pad;
}

double WireScene::steps(const WfSection &s, double y)
{
    static const Re numbered(QStringLiteral("^(\\d{1,2})[.)]?\\s+(.+)\\z"));
    y = header(s, y + pad);
    struct Step {
        QString n, title, desc;
    };
    QVector<Step> list;
    const QVector<WfItem> source = !s.items.isEmpty() ? s.items : blank(3);
    for (int i = 0; i < source.size(); ++i) {
        const WfItem &item = source.at(i);
        const auto m = numbered.match(item.title);
        list << Step{m.hasMatch() ? QString::number(m.captured(1).toInt()) : QString::number(i + 1),
                     m.hasMatch() ? m.captured(2) : item.title, item.desc};
    }
    const int n = int(list.size());
    const double col = cw / n;
    const auto circle = [&](int i, double x, double cy, double d) {
        Spec &dot = add(QLatin1Char('c') + QString::number(i), Type::Dot, d, Nodes);
        dot.props.x = x;
        dot.props.y = cy;
        dot.props.r = 19;
        dot.fixed.cls = QStringLiteral("dg-wf-step");
        label(QLatin1Char('n') + QString::number(i), x, cy, {list.at(i).n},
              QStringLiteral("dg-wf-strong"), QStringLiteral("middle"), 16, 13, 600, d + 0.02);
    };
    const auto link = [&](int i, double x1, double y1, double x2, double y2, double d) {
        Spec &l = add(QLatin1Char('k') + QString::number(i), Type::Line, d, Edges);
        l.props.x1 = x1;
        l.props.y1 = y1;
        l.props.x2 = x2;
        l.props.y2 = y2;
        l.fixed.cls = QStringLiteral("dg-wf-rule is-step");
        l.fixed.draw = 1;
    };
    double bottom = y;
    if (col >= 140) {
        // In a row, joined left to right.
        const double cy = y + 19;
        for (int i = 0; i < n; ++i) {
            const Step &step = list.at(i);
            const double mx = x0 + col * (i + 0.5), d = 0.08 + i * 0.07;
            if (i < n - 1)
                link(i, mx + 29, cy, mx + col - 29, cy, d + 0.05);
            circle(i, mx, cy, d);
            double ty = cy + 35;
            ty += titleOrSkel(QLatin1Char('t') + QString::number(i), step.title, WT::title,
                              QStringLiteral("dg-wf-strong"), mx, ty, col - 24,
                              QStringLiteral("middle"), 0, d + 0.03);
            if (!step.desc.isEmpty())
                ty += 6 + text(QLatin1Char('d') + QString::number(i), step.desc, WT::body,
                               QStringLiteral("dg-wf-text"), mx, ty + 6, col - 24,
                               QStringLiteral("middle"), 0, d + 0.05);
            bottom = std::max(bottom, ty);
        }
        return bottom + pad;
    }
    // Too narrow for a row: down a line.
    double ty = y;
    for (int i = 0; i < n; ++i) {
        const Step &step = list.at(i);
        const double d = 0.08 + i * 0.07, cy = ty + 19;
        circle(i, x0 + 19, cy, d);
        double h = titleOrSkel(QLatin1Char('t') + QString::number(i), step.title, WT::title,
                               QStringLiteral("dg-wf-strong"), x0 + 56, ty + 9, cw - 56,
                               QStringLiteral("start"), 0, d + 0.03);
        if (!step.desc.isEmpty())
            h += 4 + text(QLatin1Char('d') + QString::number(i), step.desc, WT::body,
                          QStringLiteral("dg-wf-text"), x0 + 56, ty + 13 + h, cw - 56,
                          QStringLiteral("start"), 0, d + 0.05);
        const double block = std::max(38.0, 9 + h);
        if (i < n - 1)
            link(i, x0 + 19, cy + 25, x0 + 19, ty + block + 22 - 6, d + 0.05);
        ty += block + 22;
    }
    return ty - 22 + pad;
}

double WireScene::stats(const WfSection &s, double y)
{
    static const Re valued(QStringLiteral("^(\\S*\\d\\S*)\\s+(.+)\\z"));
    y = header(s, y + pad);
    struct Stat {
        QString value, label;
    };
    QVector<Stat> list;
    for (const WfItem &item : !s.items.isEmpty() ? s.items : blank(3)) {
        if (!item.desc.isEmpty()) {
            list << Stat{item.title, item.desc};
            continue;
        }
        const auto m = valued.match(item.title);
        list << (m.hasMatch() ? Stat{m.captured(1), m.captured(2)} : Stat{item.title, QString()});
    }
    const int count = int(list.size());
    const int across = std::min(count, wide ? 4 : 2);
    const int rows = (count + across - 1) / across, per = (count + rows - 1) / rows;
    const double col = cw / per;
    for (int r = 0; r < rows; ++r) {
        const QVector<Stat> row = list.mid(r * per, per);
        double h = 0;
        for (int k = 0; k < row.size(); ++k) {
            const Stat &stat = row.at(k);
            const int i = r * per + k;
            const double mx = cx + (k - (row.size() - 1) / 2.0) * col, d = 0.08 + i * 0.05;
            double ty = y;
            if (!stat.value.isEmpty()) {
                ty += text(QLatin1Char('v') + QString::number(i), stat.value, WT::value,
                           QStringLiteral("dg-wf-strong"), mx, ty, col - 16,
                           QStringLiteral("middle"), 1, d);
            } else {
                skel(QLatin1Char('v') + QString::number(i), mx - 36, ty + 6, 72, 22, d);
                ty += WT::value.line;
            }
            if (!stat.label.isEmpty())
                ty += 6 + text(QLatin1Char('l') + QString::number(i), stat.label, WT::small,
                               QStringLiteral("dg-wf-text"), mx, ty + 6, col - 24,
                               QStringLiteral("middle"), 0, d + 0.03);
            h = std::max(h, ty - y);
        }
        y += h + (r < rows - 1 ? 28 : 0);
    }
    return y + pad;
}

double WireScene::faq(const WfSection &s, double y)
{
    y = header(s, y + pad);
    const double w = std::min(cw, 720.0), x = cx - w / 2;
    rule(QStringLiteral("r0"), x, y, x + w, 0.05);
    const QVector<WfItem> list = !s.items.isEmpty() ? s.items : blank(4);
    for (int i = 0; i < list.size(); ++i) {
        QString q = list.at(i).title, a = list.at(i).desc;
        // "Question? Answer" on one line parts at the question mark.
        const qsizetype k = q.indexOf(QLatin1Char('?'));
        if (a.isEmpty() && k > 0 && k < q.size() - 1) {
            a = q.mid(k + 1).trimmed();
            q = q.left(k + 1);
        }
        const bool open = i == 0 && !a.isEmpty();
        const double d = 0.08 + i * 0.05;
        const QString n = QString::number(i);
        double ty = y + 18;
        double qh;
        if (!q.isEmpty()) {
            qh = text(QLatin1Char('q') + n, q, WT::q, QStringLiteral("dg-wf-strong"), x + 4, ty,
                      w - 56, QStringLiteral("start"), 0, d);
        } else {
            skel(QLatin1Char('q') + n, x + 4, ty + 6, w * 0.5, 9, d);
            qh = WT::q.line;
        }
        glyph(QLatin1Char('g') + n, open ? QStringLiteral("minus") : QStringLiteral("plus"),
              x + w - 14, ty + WT::q.line / 2, 17, QString(), d + 0.02);
        ty += qh;
        if (open)
            ty += 8 + text(QLatin1Char('a') + n, a, WT::body, QStringLiteral("dg-wf-text"), x + 4,
                           ty + 8, w - 56, QStringLiteral("start"), 0, d + 0.03);
        y = ty + 18;
        rule(QLatin1Char('r') + QString::number(i + 1), x, y, x + w, d + 0.04);
    }
    return y + pad;
}

double WireScene::cta(const WfSection &s, double y)
{
    const double top = y + (wide ? 40 : 24), inner = wide ? 52 : 32,
                 maxW = std::min(cw - inner * 2, 620.0);
    // The banner's height is known once its content is laid out.
    const int bg = box(QStringLiteral("bg"), x0, top, cw, 0, QStringLiteral("dg-wf-banner"), 24, 0);
    double ty = top + inner;
    if (!s.heading.isEmpty()) {
        ty += text(QStringLiteral("h"), s.heading, wide ? WT::h2 : WT::h2s,
                   QStringLiteral("dg-wf-strong"), cx, ty, maxW, QStringLiteral("middle"), 0, 0.04);
    } else {
        skel(QStringLiteral("h"), cx - maxW * 0.3, ty + 4, maxW * 0.6, 18, 0.04);
        ty += 26;
    }
    const QString sub = s.text.join(QLatin1Char(' '));
    if (!sub.isEmpty())
        ty += 10 + text(QStringLiteral("t"), sub, WT::sub, QStringLiteral("dg-wf-text"), cx,
                        ty + 10, maxW, QStringLiteral("middle"), 0, 0.08);
    if (!s.buttons.isEmpty())
        ty += 26 + buttons(QStringLiteral("b"), s.buttons, cx, ty + 26, cw - inner * 2,
                           QStringLiteral("middle"), 0.14);
    items[bg].props.h = ty + inner - top;
    return ty + inner + (wide ? 40 : 24);
}

double WireScene::form(const WfSection &s, double y)
{
    y += pad;
    // Wide enough: the heading on the left, the form on the right.
    const bool side = cw >= 760 && (!s.heading.isEmpty() || !s.text.isEmpty());
    const int mark = int(items.size());
    const double fw = side ? std::min(420.0, std::floor(cw * 0.46)) : std::min(cw, 440.0),
                 fx = side ? x0 + cw - fw : cx - fw / 2;
    const double head =
        side ? header(s, y, QStringLiteral("start"), std::floor(cw * 0.44), x0) - 32 : 0;
    const int headEnd = int(items.size());
    double ty = side ? y : header(s, y);
    const QVector<WfItem> fields = !s.items.isEmpty() ? s.items : blank(2);
    for (int i = 0; i < fields.size(); ++i) {
        const WfItem &field = fields.at(i);
        const bool area = wfArea().match(field.title).hasMatch();
        const double h = area ? 92 : 46, d = 0.1 + i * 0.04;
        box(QLatin1Char('f') + QString::number(i), fx, ty, fw, h, QStringLiteral("dg-wf-input"), 12,
            d, Nodes);
        if (!field.title.isEmpty())
            text(QLatin1Char('p') + QString::number(i), field.title, WT::body,
                 QStringLiteral("dg-wf-muted"), fx + 16, ty + (area ? 14 : (h - WT::body.line) / 2),
                 fw - 32, QStringLiteral("start"), 1, d + 0.02);
        else
            skel(QLatin1Char('p') + QString::number(i), fx + 16, ty + h / 2 - 4, fw * 0.3, 8,
                 d + 0.02);
        ty += h + 10;
    }
    button(QStringLiteral("b"), s.buttons.value(0), fx, ty + 4, fw, true, 46, 0.3);
    ty += 50;
    if (side) {
        const double headH = head - y, formH = ty - y;
        if (headH < formH)
            shift(mark, (formH - headH) / 2, headEnd);
        ty = std::max(ty, head);
    }
    return ty + pad;
}

double WireScene::section(const WfSection &s, double y)
{
    y += pad;
    const bool split = !s.media.isEmpty() && cw >= 700;
    const int mark = int(items.size());
    const auto bullets = [&](double x, double ty, double w, double d) {
        for (int k = 0; k < s.items.size(); ++k) {
            const WfItem &item = s.items.at(k);
            const QString str =
                !item.desc.isEmpty() ? item.title + QStringLiteral(": ") + item.desc : item.title;
            glyph(QLatin1Char('k') + QString::number(k), QStringLiteral("check"), x + 7,
                  ty + WT::body.line / 2, 15, QStringLiteral("is-check"), d + k * 0.03);
            ty += text(QLatin1Char('i') + QString::number(k), str, WT::body,
                       QStringLiteral("dg-wf-text"), x + 24, ty, w - 24, QStringLiteral("start"), 0,
                       d + k * 0.03) +
                  8;
        }
        return ty;
    };
    if (split) {
        // Text beside the picture, every other section the other way round.
        const bool flip = s.n % 2 == 1;
        const double tw = std::floor(cw * 0.48), mw = std::floor(cw * 0.45);
        const double tx = flip ? x0 + cw - tw : x0, mx = flip ? x0 : x0 + cw - mw;
        double ty = y;
        if (!s.badge.isEmpty())
            ty += pill(QStringLiteral("badge"), s.badge, tx, ty, QStringLiteral("start")) + 16;
        ty += titleOrSkel(QStringLiteral("h"), s.heading, WT::h2, QStringLiteral("dg-wf-strong"),
                          tx, ty, tw, QStringLiteral("start"), 0, 0.02);
        for (int k = 0; k < s.text.size(); ++k)
            ty += 12 + text(QLatin1Char('p') + QString::number(k), s.text.at(k), WT::sub,
                            QStringLiteral("dg-wf-text"), tx, ty + 12, tw, QStringLiteral("start"),
                            0, 0.05 + k * 0.03);
        if (!s.items.isEmpty())
            ty = bullets(tx, ty + 18, tw, 0.1) - 8;
        if (!s.buttons.isEmpty())
            ty += 26 + buttons(QStringLiteral("b"), s.buttons, tx, ty + 26, tw,
                               QStringLiteral("start"), 0.18);
        const double block = ty - y, mh = jsRound(mw * 0.74);
        if (mh > block)
            shift(mark, (mh - block) / 2);
        media(QStringLiteral("m"), mx, y + std::max(0.0, (block - mh) / 2), mw, mh, s.media);
        return y + std::max(block, mh) + pad;
    }
    const double w = std::min(cw, 600.0), list = std::min(w, 460.0);
    double ty = y;
    const auto gap = [&](double size) {
        if (ty > y)
            ty += size;
    };
    if (!s.badge.isEmpty())
        ty += pill(QStringLiteral("badge"), s.badge, cx, ty, QStringLiteral("middle")) + 16;
    if (!s.heading.isEmpty())
        ty += text(QStringLiteral("h"), s.heading, wide ? WT::h2 : WT::h2s,
                   QStringLiteral("dg-wf-strong"), cx, ty, w, QStringLiteral("middle"));
    for (int k = 0; k < s.text.size(); ++k) {
        gap(12);
        ty += text(QLatin1Char('p') + QString::number(k), s.text.at(k), WT::sub,
                   QStringLiteral("dg-wf-text"), cx, ty, w, QStringLiteral("middle"), 0,
                   0.05 + k * 0.03);
    }
    if (!s.items.isEmpty()) {
        gap(24);
        ty = bullets(cx - list / 2, ty, list, 0.1) - 8;
    }
    if (!s.buttons.isEmpty()) {
        gap(26);
        ty += buttons(QStringLiteral("b"), s.buttons, cx, ty, cw, QStringLiteral("middle"), 0.18);
    }
    if (!s.media.isEmpty()) {
        gap(32);
        const double mh = jsRound(std::min(cw * 0.46, 380.0));
        media(QStringLiteral("m"), x0, ty, cw, mh, s.media);
        ty += mh;
    }
    if (ty == y) {
        // Nothing written: a placeholder heading and line.
        skel(QStringLiteral("h"), cx - 120, ty + 4, 240, 16);
        skel(QStringLiteral("p"), cx - 170, ty + 34, 340, 9, 0.03);
        ty += 48;
    }
    return ty + pad;
}

double WireScene::footer(const WfSection &s, double y)
{
    rule(QStringLiteral("r"), 0, y, W);
    y += wide ? 36 : 28;
    QStringList links;
    for (const WfItem &item : s.items) {
        if (!item.title.isEmpty())
            links << item.title;
    }
    double left = y;
    left += !s.heading.isEmpty()
                ? text(QStringLiteral("b"), s.heading, WT::title, QStringLiteral("dg-wf-strong"),
                       x0, left, wide ? cw * 0.35 : cw, QStringLiteral("start"), 1)
                : 0;
    if (!s.text.isEmpty())
        left += 6 + text(QStringLiteral("t"), s.text.join(QLatin1Char(' ')), WT::small,
                         QStringLiteral("dg-wf-muted"), x0, left + 6, wide ? cw * 0.4 : cw,
                         QStringLiteral("start"), 0, 0.05);
    double right = wide ? y : left + (left > y ? 20 : 0);
    if (!links.isEmpty()) {
        QVector<double> lw;
        for (const QString &t : links)
            lw << std::ceil(c.textWidth(t, WT::small.size, WT::small.weight));
        const double gap = 24, maxW = wide ? cw * 0.55 : cw;
        struct Row {
            QVector<int> list;
            double w = 0;
        };
        QVector<Row> rows;
        for (int i = 0; i < lw.size(); ++i) {
            if (!rows.isEmpty() && rows.last().w + gap + lw.at(i) <= maxW) {
                rows.last().list << i;
                rows.last().w += gap + lw.at(i);
            } else {
                rows << Row{{i}, lw.at(i)};
            }
        }
        for (const Row &row : rows) {
            double lx = wide ? x0 + cw - row.w : x0;
            for (const int i : row.list) {
                text(QLatin1Char('l') + QString::number(i), links.at(i), WT::small,
                     QStringLiteral("dg-wf-text"), lx, right, lw.at(i) + 2, QStringLiteral("start"),
                     1, 0.05 + i * 0.02);
                lx += lw.at(i) + gap;
            }
            right += WT::small.line + 10;
        }
        right -= 10;
    }
    if (s.heading.isEmpty() && links.isEmpty() && s.text.isEmpty()) {
        skel(QStringLiteral("b"), x0, y + 4, 90, 10);
        left = y + 18;
    }
    return std::max(left, right) + (wide ? 36 : 28);
}

double WireScene::lay(const WfSection &s, double y)
{
    const QString &t = s.type;
    if (t == QLatin1String("nav"))
        return nav(s, y);
    if (t == QLatin1String("hero"))
        return hero(s, y);
    if (t == QLatin1String("logos"))
        return logos(s, y);
    if (t == QLatin1String("features"))
        return grid(QStringLiteral("c"), !s.items.isEmpty() ? s.items : blank(3),
                    header(s, y + pad), colsFor(200), CardKind::Feature, s) +
               pad;
    if (t == QLatin1String("cards"))
        return grid(QStringLiteral("c"), !s.items.isEmpty() ? s.items : blank(3),
                    header(s, y + pad), colsFor(220), CardKind::Image, s) +
               pad;
    if (t == QLatin1String("reviews"))
        return grid(QStringLiteral("c"), !s.items.isEmpty() ? s.items : blank(3),
                    header(s, y + pad), colsFor(250), CardKind::Review, s) +
               pad;
    if (t == QLatin1String("pricing"))
        return grid(QStringLiteral("c"), !s.items.isEmpty() ? s.items : blank(3),
                    header(s, y + pad), colsFor(220), CardKind::Plan, s) +
               pad;
    if (t == QLatin1String("gallery"))
        return gallery(s, y);
    if (t == QLatin1String("steps"))
        return steps(s, y);
    if (t == QLatin1String("stats"))
        return stats(s, y);
    if (t == QLatin1String("faq"))
        return faq(s, y);
    if (t == QLatin1String("cta"))
        return cta(s, y);
    if (t == QLatin1String("form"))
        return form(s, y);
    if (t == QLatin1String("footer"))
        return footer(s, y);
    return section(s, y);
}

std::optional<Result> WireScene::build()
{
    phone = page.mobile;
    const double room = std::max(240.0, c.width - PAD * 2);
    W = jsRound(phone ? std::min(WF.phone, room) : std::min(WF.max, room));
    const double gutter = W >= 720 ? 48 : W >= 520 ? 32 : 20;
    cw = std::min(W - gutter * 2, 960.0);
    x0 = (W - cw) / 2;
    cx = W / 2;
    wide = cw >= 620;
    pad = wide ? 56 : 36;
    tone = first(c.tones);
    std::optional<Corner> corner;

    double y = phone ? WF.status : WF.bar;
    for (int i = 0; i < page.sections.size(); ++i) {
        const WfSection &s = page.sections.at(i);
        if (!c.budget(1 + int(s.items.size() + s.buttons.size() + s.text.size())) || c.cancelled())
            return std::nullopt;
        scope = s.key;
        base = 0.3 + i * 0.5;
        const double top = y;
        const int mark = int(items.size());
        y = lay(s, y);
        // Each section on a band that lights up with its name as the pointer passes.
        Spec &band = add(QStringLiteral("band"), Type::WBox, -0.02, Back);
        band.props.x = 4;
        band.props.y = top + 2;
        band.props.w = W - 8;
        band.props.h = std::max(8.0, y - top - 4);
        band.fixed.cls = QStringLiteral("dg-wf-band");
        band.fixed.rx = 10;
        const QString bandKey = band.key;
        const QString name = wfLabel(s.type);
        const double lw = std::ceil(c.textWidth(name, 11, 600)) + 18;
        Spec &tag = add(QStringLiteral("tag"), Type::Label, 0, Front);
        tag.props.x = cx;
        tag.props.y = top + 14;
        tag.fixed.lines = {name};
        tag.fixed.pill = true;
        tag.fixed.w = lw;
        tag.fixed.h = 20;
        tag.fixed.cls = QStringLiteral("dg-wf-sectag");
        tag.fixed.anchor = QStringLiteral("middle");
        tag.fixed.lineHeight = 14;
        Tip tip;
        tip.silent = true;
        tip.hot = QStringList{bandKey, tag.key};
        for (int k = mark; k < items.size(); ++k)
            tips.insert(items.at(k).key, tip);
    }

    scope = QStringLiteral("wf");
    base = 0;
    const double H = jsRound(y + (phone ? WF.home : 0));
    box(QStringLiteral("window"), 0, 0, W, H,
        phone ? QStringLiteral("dg-wf-window is-phone") : QStringLiteral("dg-wf-window"),
        phone ? WF.phoneRadius : WF.radius, -0.2, Back);
    items.prepend(items.takeLast());
    if (phone) {
        // A phone: the status bar, the island and the home bar.
        label(QStringLiteral("time"), 34, WF.status / 2, {QStringLiteral("9:41")},
              QStringLiteral("dg-wf-strong"), QStringLiteral("start"), 16, 13.5, 600, -0.1);
        box(QStringLiteral("island"), cx - 52, 10, 104, 28, QStringLiteral("dg-wf-island"), 14,
            -0.1, Nodes);
        box(QStringLiteral("battery"), W - 52, WF.status / 2 - 6, 24, 12,
            QStringLiteral("dg-wf-battery"), 4, -0.1, Nodes);
        box(QStringLiteral("charge"), W - 50, WF.status / 2 - 4, 16, 8,
            QStringLiteral("dg-wf-charge"), 2, -0.1, Nodes);
        box(QStringLiteral("home"), cx - 62, H - 14, 124, 5, QStringLiteral("dg-wf-home"), 2.5,
            -0.1, Nodes);
    } else {
        // A browser window: its lights and the address with the site's name.
        for (int i = 0; i < 3; ++i) {
            Spec &light = add(QStringLiteral("light") + QString::number(i), Type::Dot,
                              -0.12 + i * 0.02, Nodes);
            light.props.x = 20 + i * 17;
            light.props.y = WF.bar / 2;
            light.props.r = 5.5;
            light.fixed.cls = QStringLiteral("dg-wf-light");
        }
        const double aw = jsRound(std::min(340.0, std::max(150.0, W * 0.36)));
        QString url = page.title;
        if (url.isEmpty()) {
            for (const WfSection &s : page.sections) {
                if (s.type == QLatin1String("nav")) {
                    url = s.heading;
                    break;
                }
            }
        }
        box(QStringLiteral("address"), cx - aw / 2, WF.bar / 2 - 13, aw, 26,
            QStringLiteral("dg-wf-address"), 9, -0.1, Nodes);
        if (!url.isEmpty()) {
            const QString shown = truncate(c, url, aw - 48, 12, 500);
            const double tw = c.textWidth(shown, 12, 500);
            glyph(QStringLiteral("lock"), QStringLiteral("lock"), cx - tw / 2 - 5, WF.bar / 2, 13,
                  QStringLiteral("is-lock"), -0.06);
            label(QStringLiteral("url"), cx + 5, WF.bar / 2, {shown}, QStringLiteral("dg-wf-muted"),
                  QStringLiteral("middle"), 14, 12, 500, -0.06);
        } else {
            skel(QStringLiteral("url"), cx - 50, WF.bar / 2 - 4, 100, 8, -0.06);
        }
        rule(QStringLiteral("bar"), 0, WF.bar, W, -0.1);
        corner = Corner{cx + aw / 2 + 12, WF.bar, 7};
    }
    Result r;
    r.kind = QStringLiteral("wireframe");
    r.width = W;
    r.height = H;
    r.items = std::move(items);
    r.tips = std::move(tips);
    r.corner = corner;
    r.mobile = phone;
    return r;
}

/* Files: what a folder holds, shown the way a file manager would */

// A handful of entries become tiles with big icons, a longer list becomes
// rows, nested entries a tree; a bar above shows what takes the space.
struct FvSet {
    double max, pad, head, meter, row, indent;
    int cap;
};
constexpr FvSet FV{760, 16, 42, 4, 34, 18, 80};

struct FvKindRow {
    const char *kind;
    const char *glyphs[5]; // The file-kinds.js glyphs it gathers (null-padded).
    const char *tone;
};
const FvKindRow FV_KINDS[] = {
    {"image", {"image"}, "mint"},
    {"video", {"video"}, "purple"},
    {"audio", {"audio"}, "pink"},
    {"archive", {"archive"}, "brown"},
    {"app", {"binary"}, "steel"},
    {"doc", {"pdf", "text", "book", "sheet", "slides"}, "blue"},
    {"code", {"code", "braces", "terminal"}, "indigo"},
    {"font", {"font"}, "gray"},
};
const char *const FV_FOLDER = "gray";

// I18n files.kind.<kind> (English).
QString fvKindName(const QString &kind)
{
    static const QHash<QString, QString> map{
        {QStringLiteral("image"), QStringLiteral("Images")},
        {QStringLiteral("video"), QStringLiteral("Video")},
        {QStringLiteral("audio"), QStringLiteral("Audio")},
        {QStringLiteral("archive"), QStringLiteral("Archives")},
        {QStringLiteral("app"), QStringLiteral("Apps")},
        {QStringLiteral("doc"), QStringLiteral("Documents")},
        {QStringLiteral("code"), QStringLiteral("Code")},
        {QStringLiteral("font"), QStringLiteral("Fonts")},
        {QStringLiteral("folder"), QStringLiteral("Folders")},
        {QStringLiteral("other"), QStringLiteral("Other")}};
    return map.value(kind, QStringLiteral("files.kind.") + kind);
}

double fvUnit(const QString &unit)
{
    static const QHash<QString, double> map{{QStringLiteral("b"), 1},
                                            {QStringLiteral("byte"), 1},
                                            {QStringLiteral("bytes"), 1},
                                            {QStringLiteral("k"), 1e3},
                                            {QStringLiteral("kb"), 1e3},
                                            {QStringLiteral("kib"), 1024},
                                            {QStringLiteral("m"), 1e6},
                                            {QStringLiteral("mb"), 1e6},
                                            {QStringLiteral("mib"), 1048576},
                                            {QStringLiteral("g"), 1e9},
                                            {QStringLiteral("gb"), 1e9},
                                            {QStringLiteral("gib"), 1073741824},
                                            {QStringLiteral("t"), 1e12},
                                            {QStringLiteral("tb"), 1e12},
                                            {QStringLiteral("tib"), 1099511627776},
                                            {QString::fromUtf8("б"), 1},
                                            {QString::fromUtf8("байт"), 1},
                                            {QString::fromUtf8("кб"), 1e3},
                                            {QString::fromUtf8("мб"), 1e6},
                                            {QString::fromUtf8("гб"), 1e9},
                                            {QString::fromUtf8("тб"), 1e12}};
    return map.value(unit, NaN);
}

const Re &fvCount()
{
    static const Re re(QString::fromUtf8("^(\\d[\\d \\x{a0},]*)\\s*(files?|items?|folders?|entries|"
                                         "objects?|файл\\S*|элемент\\S*|объект\\S*|папк\\S*)\\z"),
                       I);
    return re;
}

// A size as a listing writes it (2.4 MB, 1 234 567 bytes, 12,5 Мб), in bytes.
std::optional<double> fvSize(const QString &text)
{
    static const Re size(
        QString::fromUtf8("^(\\d{1,3}(?:[ \\x{a0},']\\d{3})+|\\d+(?:[.,]\\d+)?)\\s*"
                          "([a-zа-яё]{0,5})\\.?\\z"),
        I),
        grouped(QString::fromUtf8("^\\d{1,3}([ \\x{a0},']\\d{3})+\\z")),
        separators(QString::fromUtf8("[ \\x{a0},']"));
    const auto m = size.match(text.trimmed());
    const QString unit =
        m.hasMatch() && !m.captured(2).isEmpty() ? m.captured(2).toLower() : QStringLiteral("b");
    const double scale = fvUnit(unit);
    if (!m.hasMatch() || !finite(scale))
        return std::nullopt;
    QString digits = m.captured(1);
    if (grouped.match(digits).hasMatch()) {
        digits.remove(separators);
    } else {
        const qsizetype comma = digits.indexOf(QLatin1Char(','));
        if (comma >= 0)
            digits[comma] = QLatin1Char('.');
    }
    const double value = parseFloatJs(digits);
    if (!finite(value))
        return std::nullopt;
    // Not upstream: a size past what a double holds would poison the bar.
    if (!finite(value * scale))
        return std::nullopt;
    return value * scale;
}

// A local wall-clock time (ms as if UTC) to UTC ms, as Date reads a local time.
double fromLocal(double wall)
{
    if (!finite(wall) || std::abs(wall) > 8.64e15 + 2 * DAY)
        return NaN;
    const QDateTime asUtc = QDateTime::fromMSecsSinceEpoch(qint64(wall), QTimeZone::UTC);
    const QDateTime local(asUtc.date(), asUtc.time(), QTimeZone(QTimeZone::LocalTime));
    const double t = double(local.toMSecsSinceEpoch());
    return local.isValid() && std::abs(t) <= 8.64e15 ? t : NaN;
}

// Date.parse of a text with words, as V8's legacy parser (Electron's) reads
// it: garbage words before the first number, a month by its first three
// letters, numbers into the day and the time, am/pm, a zone. NaN otherwise.
double legacyParse(const QString &s)
{
    constexpr int None = std::numeric_limits<int>::max();
    enum Kind { End, Num, Sym, Word, Space, Unknown };
    enum WordType { Invalid, MonthName, AmPm, ZoneName, TimeSep };
    struct Tok {
        Kind kind = End;
        int value = 0, length = 0;
        ushort sym = 0;
        WordType type = Invalid;
    };
    struct Keyword {
        char a, b, c;
        WordType type;
        int value;
    };
    static const Keyword keywords[] = {
        {'j', 'a', 'n', MonthName, 1},  {'f', 'e', 'b', MonthName, 2},
        {'m', 'a', 'r', MonthName, 3},  {'a', 'p', 'r', MonthName, 4},
        {'m', 'a', 'y', MonthName, 5},  {'j', 'u', 'n', MonthName, 6},
        {'j', 'u', 'l', MonthName, 7},  {'a', 'u', 'g', MonthName, 8},
        {'s', 'e', 'p', MonthName, 9},  {'o', 'c', 't', MonthName, 10},
        {'n', 'o', 'v', MonthName, 11}, {'d', 'e', 'c', MonthName, 12},
        {'a', 'm', 0, AmPm, 0},         {'p', 'm', 0, AmPm, 12},
        {'u', 't', 0, ZoneName, 0},     {'u', 't', 'c', ZoneName, 0},
        {'z', 0, 0, ZoneName, 0},       {'g', 'm', 't', ZoneName, 0},
        {'c', 'd', 't', ZoneName, -5},  {'c', 's', 't', ZoneName, -6},
        {'e', 'd', 't', ZoneName, -4},  {'e', 's', 't', ZoneName, -5},
        {'m', 'd', 't', ZoneName, -6},  {'m', 's', 't', ZoneName, -7},
        {'p', 'd', 't', ZoneName, -7},  {'p', 's', 't', ZoneName, -8},
        {'t', 0, 0, TimeSep, 0}};
    const auto digit = [](QChar ch) { return ch.unicode() >= '0' && ch.unicode() <= '9'; };
    QVector<Tok> toks;
    const int n = int(s.size());
    for (int i = 0; i < n;) {
        const int start = i;
        const QChar ch = s.at(i);
        Tok t;
        if (digit(ch)) {
            while (i < n && s.at(i) == QLatin1Char('0'))
                ++i;
            int v = 0, k = 0;
            for (; i < n && digit(s.at(i)); ++i, ++k) {
                if (k < 9)
                    v = v * 10 + (s.at(i).unicode() - '0');
            }
            t.kind = Num;
            t.value = v;
            t.length = i - start;
        } else if (ch == QLatin1Char(':') || ch == QLatin1Char('-') || ch == QLatin1Char('+') ||
                   ch == QLatin1Char('.') || ch == QLatin1Char(')')) {
            t.kind = Sym;
            t.sym = ch.unicode();
            ++i;
        } else if (ch.unicode() >= 'A' && !ch.isSpace()) {
            ushort pre[3] = {0, 0, 0};
            int len = 0;
            for (; i < n && s.at(i).unicode() >= 'A' && !s.at(i).isSpace(); ++i, ++len) {
                if (len < 3)
                    pre[len] = s.at(i).unicode() | 0x20;
            }
            t.kind = Word;
            t.length = len;
            for (const Keyword &w : keywords) {
                if (pre[0] == ushort(w.a) && pre[1] == ushort(w.b) && pre[2] == ushort(w.c) &&
                    (len <= 3 || w.type == MonthName)) {
                    t.type = w.type;
                    t.value = w.value;
                    break;
                }
            }
        } else if (ch.isSpace()) {
            while (i < n && s.at(i).isSpace())
                ++i;
            t.kind = Space;
        } else if (ch == QLatin1Char('(')) {
            // Parenthesized text is ignored.
            int balance = 0;
            do {
                if (s.at(i) == QLatin1Char(')'))
                    --balance;
                else if (s.at(i) == QLatin1Char('('))
                    ++balance;
                ++i;
            } while (balance > 0 && i < n);
            t.kind = Unknown;
        } else {
            ++i;
            t.kind = Unknown;
        }
        toks << t;
    }
    int at = 0;
    const auto peek = [&]() { return at < toks.size() ? toks.at(at) : Tok{}; };
    const auto next = [&]() { return at < toks.size() ? toks.at(at++) : Tok{}; };
    const auto isSym = [](const Tok &t, char c) { return t.kind == Sym && t.sym == ushort(c); };
    const auto skip = [&](char c) {
        if (!isSym(peek(), c))
            return false;
        ++at;
        return true;
    };
    const auto isSign = [&](const Tok &t) { return isSym(t, '+') || isSym(t, '-'); };
    const auto between = [](int v, int lo, int hi) { return v >= lo && v <= hi; };

    int day[3] = {0, 0, 0}, dayN = 0, namedMonth = None;
    int time[4] = {0, 0, 0, 0}, timeN = 0, hourOffset = None;
    int tzSign = None, tzHour = None, tzMinute = None;
    const auto timeAdd = [&](int v) {
        if (timeN >= 4)
            return false;
        time[timeN++] = v;
        return true;
    };
    const auto timeAddFinal = [&](int v) {
        if (!timeAdd(v))
            return false;
        while (timeN < 4)
            time[timeN++] = 0;
        return true;
    };
    const auto timeExpecting = [&](int v) {
        return (timeN == 1 && between(v, 0, 59)) || (timeN == 2 && between(v, 0, 59)) ||
               (timeN == 3 && between(v, 0, 999));
    };
    const auto tzExpecting = [&](int v) {
        return tzHour != None && tzMinute == None && between(v, 0, 59);
    };
    bool hasReadNumber = false;
    for (Tok t = next(); t.kind != End; t = next()) {
        if (t.kind == Num) {
            hasReadNumber = true;
            const int v = t.value;
            if (skip(':')) {
                if (skip(':')) {
                    if (timeN)
                        return NaN;
                    timeAdd(v);
                    timeAdd(0);
                } else {
                    if (!timeAdd(v))
                        return NaN;
                    if (isSym(peek(), '.'))
                        next();
                }
            } else if (skip('.') && timeExpecting(v)) {
                timeAdd(v);
                if (peek().kind != Num)
                    return NaN;
                const Tok msTok = next();
                int ms = msTok.value, length = std::min(msTok.length, 9);
                if (length == 1)
                    ms *= 100;
                else if (length == 2)
                    ms *= 10;
                for (; length > 3; --length)
                    ms /= 10;
                timeAddFinal(ms);
            } else if (tzExpecting(v)) {
                tzMinute = v;
            } else if (timeExpecting(v)) {
                timeAddFinal(v);
                const Tok p = peek();
                const bool z = p.kind == Word && p.type == ZoneName && p.length == 1;
                if (p.kind != End && p.kind != Space && !z && !isSign(p))
                    return NaN;
            } else {
                if (dayN >= 3)
                    return NaN;
                day[dayN++] = v;
                skip('-');
            }
        } else if (t.kind == Word) {
            if (t.type == AmPm && timeN) {
                hourOffset = t.value;
            } else if (t.type == MonthName) {
                namedMonth = t.value;
                skip('-');
            } else if (t.type == ZoneName && hasReadNumber) {
                tzSign = t.value < 0 ? -1 : 1;
                tzHour = std::abs(t.value);
                tzMinute = 0;
            } else {
                // Garbage words only before the first number, and apart from it.
                if (hasReadNumber || peek().kind == Num)
                    return NaN;
            }
        } else if (isSign(t) && ((tzHour == 0 && tzMinute == 0) || timeN)) {
            tzSign = isSym(t, '-') ? -1 : 1;
            int v = 0, length = 0;
            if (peek().kind == Num) {
                const Tok nt = next();
                length = nt.length;
                v = nt.value;
            }
            hasReadNumber = true;
            if (isSym(peek(), ':')) {
                tzHour = v;
                tzMinute = None;
            } else if (length == 1 || length == 2) {
                tzHour = v;
                tzMinute = 0;
            } else if (length == 3 || length == 4) {
                tzHour = v / 100;
                tzMinute = v % 100;
            } else {
                return NaN;
            }
        } else if ((isSign(t) || isSym(t, ')')) && hasReadNumber) {
            return NaN;
        }
    }
    // DayComposer::Write: missing parts are 1; a lone year comes first.
    if (dayN < 1)
        return NaN;
    while (dayN < 3)
        day[dayN++] = 1;
    double year = 0;
    int month, d;
    if (namedMonth == None) {
        if (!between(day[0], 1, 31)) {
            year = day[0];
            month = day[1];
            d = day[2];
        } else {
            month = day[0];
            d = day[1];
            year = day[2];
        }
    } else {
        month = namedMonth;
        if (!between(day[0], 1, 31)) {
            year = day[0];
            d = day[1];
        } else {
            d = day[0];
            year = day[1];
        }
    }
    if (between(int(year), 0, 49))
        year += 2000;
    else if (between(int(year), 50, 99))
        year += 1900;
    if (!between(month, 1, 12) || !between(d, 1, 31) || year > 275760)
        return NaN;
    while (timeN < 4)
        time[timeN++] = 0;
    int hour = time[0];
    if (hourOffset != None) {
        if (!between(hour, 0, 12))
            return NaN;
        hour = hour % 12 + hourOffset;
    }
    if (!between(hour, 0, 23) || !between(time[1], 0, 59) || !between(time[2], 0, 59) ||
        !between(time[3], 0, 999)) {
        // A 24th hour only on the hour.
        if (hour != 24 || time[1] || time[2] || time[3])
            return NaN;
    }
    const double wall = utc(year, month - 1, d, hour, time[1]) + time[2] * 1000.0 + time[3];
    if (tzSign == None)
        return fromLocal(wall);
    const double offset =
        tzSign * ((tzHour == None ? 0 : tzHour) * 3600.0 + (tzMinute == None ? 0 : tzMinute) * 60);
    const double t = wall - offset * 1000;
    return finite(t) && std::abs(t) <= 8.64e15 ? t : NaN;
}

struct FvDate {
    double t = NaN;
    bool timed = false;
};

// Dates as a listing writes them: 2026-09-27 14:05, 27.09.2026 14:05,
// 9/27/2026 2:05 PM, or words a date parser knows.
std::optional<FvDate> fvDate(const QString &text)
{
    static const Re iso(
        QStringLiteral("^(\\d{4})[-./](\\d{1,2})[-./](\\d{1,2})(?:[ T,]+(\\d{1,2}):(\\d{2}))?")),
        dotted(
            QStringLiteral("^(\\d{1,2})\\.(\\d{1,2})\\.(\\d{2,4})(?:[ ,]+(\\d{1,2}):(\\d{2}))?")),
        slashed(QStringLiteral("^(\\d{1,2})/(\\d{1,2})/(\\d{2,4})(?:[ ,]+(\\d{1,2}):(\\d{2})"
                               "(?::\\d{2})?\\s*([ap]\\.?m\\.?)?)?"),
                I),
        letter(QStringLiteral("\\p{L}")), digit(QStringLiteral("\\d")),
        clock(QStringLiteral("\\d:\\d\\d")), pm(QStringLiteral("p"), I);
    const QString s = text.trimmed();
    QRegularExpressionMatch m;
    int y = 0, mo = 0, d = 0;
    int yi = 0, moi = 0, di = 0;
    if ((m = iso.match(s)).hasMatch()) {
        yi = 1, moi = 2, di = 3;
    } else if ((m = dotted.match(s)).hasMatch()) {
        di = 1, moi = 2, yi = 3;
    } else if ((m = slashed.match(s)).hasMatch()) {
        moi = 1, di = 2, yi = 3;
    } else if (letter.match(s).hasMatch() && digit.match(s).hasMatch()) {
        const double t = legacyParse(s);
        if (!finite(t))
            return std::nullopt;
        return FvDate{t, clock.match(s).hasMatch()};
    } else {
        return std::nullopt;
    }
    y = m.captured(yi).toInt();
    mo = m.captured(moi).toInt();
    d = m.captured(di).toInt();
    const bool timed = m.capturedStart(4) >= 0;
    int h = timed ? m.captured(4).toInt() : 0;
    const int mi = m.capturedStart(5) >= 0 ? m.captured(5).toInt() : 0;
    if (timed && m.lastCapturedIndex() >= 6 && !m.captured(6).isEmpty())
        h = h % 12 + (pm.match(m.captured(6)).hasMatch() ? 12 : 0);
    if (mo < 1 || mo > 12 || d < 1 || d > 31)
        return std::nullopt;
    const double t = fromLocal(utc(y < 100 ? 2000 + y : y, mo - 1, d, h, mi));
    // Not upstream: a date Date cannot hold would make Intl throw.
    if (!finite(t))
        return std::nullopt;
    return FvDate{t, timed};
}

// Today and yesterday say so, with the time; other days are a short date,
// with the year once it isn't this one (en-US, as Intl writes them).
QString fvWhen(const FvDate &when)
{
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const QTimeZone local(QTimeZone::LocalTime);
    const QDateTime date = QDateTime::fromMSecsSinceEpoch(qint64(when.t), local),
                    now = QDateTime::currentDateTime();
    const auto midnight = [&](const QDateTime &v) {
        return double(QDateTime(v.date(), QTime(0, 0), local).toMSecsSinceEpoch());
    };
    const double ago = jsRound((midnight(now) - midnight(date)) / DAY);
    if (ago == 0 || ago == 1) {
        const QString day = ago == 0 ? QStringLiteral("Today") : QStringLiteral("Yesterday");
        if (!when.timed)
            return day;
        const int h = date.time().hour();
        return QStringLiteral("%1, %2:%3 %4")
            .arg(day)
            .arg(h % 12 == 0 ? 12 : h % 12, 2, 10, QLatin1Char('0'))
            .arg(date.time().minute(), 2, 10, QLatin1Char('0'))
            .arg(h < 12 ? QStringLiteral("AM") : QStringLiteral("PM"));
    }
    const QDate d = date.date();
    QString out =
        QLatin1String(months[d.month() - 1]) + QLatin1Char(' ') + QString::number(d.day());
    if (d.year() != now.date().year())
        out += QStringLiteral(", ") + QString::number(d.year());
    return out;
}

struct FvEntry {
    QString name;
    bool folder = false;
    std::optional<double> size, count;
    std::optional<FvDate> when;
    QString note;
    QVector<int> children;
};

struct FvPage {
    QString title, path, view;
    double more = 0;
    QVector<FvEntry> all; // Every entry; a child after its folder.
    QVector<int> entries; // The top ones.
};

// The digits of a text as parseInt reads them once the rest is gone.
double digitsOf(const QString &text)
{
    QString digits;
    for (const QChar ch : text) {
        if (ch.unicode() >= '0' && ch.unicode() <= '9')
            digits += ch;
    }
    return digits.isEmpty() ? NaN : digitsValue(digits);
}

// One entry: the name first (a folder ends with / or \), then in any order
// its size, a count of what it holds, when it changed and any note,
// separated by |, by · or by tabs.
FvEntry fvEntry(const QString &line)
{
    static const Re split(QString::fromUtf8("\\s*\\|\\s*|\\s+·\\s+|\\t+")),
        slash(QStringLiteral("[\\\\/]\\z")), slashes(QStringLiteral("[\\\\/]+\\z")),
        comma(QStringLiteral("\\s*,\\s*"));
    QStringList parts;
    for (const QString &part : line.split(split)) {
        if (!part.trimmed().isEmpty())
            parts << part.trimmed();
    }
    const QString raw = cleanLabel(parts.isEmpty() ? QString() : parts.takeFirst());
    FvEntry entry;
    entry.folder = slash.match(raw).hasMatch();
    QString name = raw;
    name.remove(slashes);
    entry.name = !name.isEmpty() ? name : raw;
    const auto field = [&](const QString &text) {
        const auto count = fvCount().match(text);
        if (count.hasMatch()) {
            entry.count = digitsOf(count.captured(1));
            return true;
        }
        if (!entry.when) {
            if (const auto when = fvDate(text)) {
                entry.when = when;
                return true;
            }
        }
        if (!entry.size) {
            if (const auto size = fvSize(text)) {
                entry.size = size;
                return true;
            }
        }
        return false;
    };
    for (const QString &part : parts) {
        if (field(part))
            continue;
        const QStringList pieces = part.split(comma);
        const bool all = pieces.size() > 1 &&
                         std::all_of(pieces.begin(), pieces.end(), [](const QString &piece) {
                             return fvCount().match(piece).hasMatch() || fvSize(piece).has_value();
                         });
        if (all) {
            for (const QString &piece : pieces)
                field(piece);
        } else {
            entry.note =
                !entry.note.isEmpty() ? entry.note + QString::fromUtf8(" · ") + part : part;
        }
    }
    return entry;
}

// Lines under the header: title, path, view (grid, list or tree), more (how
// many were left out), and the entries, nested by indentation under folders.
std::optional<FvPage> parseFiles(Ctx &c, const QStringList &lines)
{
    static const Re marks(QString::fromUtf8("[|·\\t]")),
        directive(QStringLiteral("^(title|path|view|more)(?:\\s*:\\s*|\\s+)(.+)\\z"), I);
    FvPage page;
    struct Open {
        int indent, entry;
    };
    QVector<Open> open;
    for (int i = 1; i < lines.size(); ++i) {
        if (!c.budget())
            return std::nullopt;
        const QString &raw = lines.at(i);
        int indent = 0;
        while (indent < raw.size() && raw.at(indent).isSpace())
            ++indent;
        const QString line = bare(raw);
        const auto m =
            marks.match(line).hasMatch() ? QRegularExpressionMatch() : directive.match(line);
        if (m.hasMatch()) {
            const QString key = m.captured(1).toLower(), value = cleanLabel(m.captured(2));
            if (key == QLatin1String("more")) {
                const double n = digitsOf(value);
                page.more = n == n && n ? n : 0;
            } else if (key == QLatin1String("view")) {
                page.view = value.toLower();
            } else if (key == QLatin1String("title")) {
                page.title = value;
            } else {
                page.path = value;
            }
            continue;
        }
        FvEntry entry = fvEntry(line);
        if (entry.name.isEmpty())
            continue;
        while (!open.isEmpty() && open.last().indent >= indent)
            open.removeLast();
        const int index = int(page.all.size());
        if (!open.isEmpty()) {
            FvEntry &parent = page.all[open.last().entry];
            parent.folder = true;
            parent.children << index;
        } else {
            page.entries << index;
        }
        page.all << entry;
        open << Open{indent, index};
    }
    if (page.entries.isEmpty() && page.title.isEmpty() && page.path.isEmpty())
        return std::nullopt;
    return page;
}

struct FvRow {
    int entry, depth;
};

// The entries in reading order, each with how deep it sits (no recursion:
// the nesting is the model's to make as deep as it likes).
QVector<FvRow> fvFlatten(const FvPage &page)
{
    QVector<FvRow> out;
    std::vector<FvRow> stack;
    for (auto it = page.entries.crbegin(); it != page.entries.crend(); ++it)
        stack.push_back({*it, 0});
    while (!stack.empty()) {
        const FvRow row = stack.back();
        stack.pop_back();
        out << row;
        const auto &children = page.all.at(row.entry).children;
        for (auto it = children.crbegin(); it != children.crend(); ++it)
            stack.push_back({*it, row.depth + 1});
    }
    return out;
}

// fvTotal of every entry (NaN for null): its size, or what its children add
// up to. Children come after their folder, so from the end it is one pass.
QVector<double> fvTotals(const FvPage &page)
{
    QVector<double> total(page.all.size(), NaN);
    for (int i = int(page.all.size()) - 1; i >= 0; --i) {
        const FvEntry &e = page.all.at(i);
        if (e.size) {
            total[i] = *e.size;
        } else if (!e.children.isEmpty()) {
            double sum = 0;
            for (const int child : e.children)
                sum += total.at(child) == total.at(child) ? total.at(child) : 0;
            total[i] = sum ? sum : NaN;
        }
    }
    return total;
}

QString fvKind(const FvEntry &entry)
{
    if (entry.folder)
        return QStringLiteral("folder");
    const QString glyph = describeFile(entry.name).glyph;
    for (const FvKindRow &row : FV_KINDS) {
        for (const char *g : row.glyphs) {
            if (g && glyph == QLatin1String(g))
                return QLatin1String(row.kind);
        }
    }
    return QStringLiteral("other");
}

QColor fvTone(const QString &kind)
{
    if (kind == QLatin1String("folder"))
        return fileTone(QLatin1String(FV_FOLDER));
    for (const FvKindRow &row : FV_KINDS) {
        if (kind == QLatin1String(row.kind))
            return fileTone(QLatin1String(row.tone));
    }
    return fileTone(QStringLiteral("gray"));
}

// How much an entry is (a folder tells what it holds, a file its size) and
// when it changed.
struct FvMeta {
    QString amount, when;
};
FvMeta fvMeta(const FvEntry &entry)
{
    const QString size = entry.size ? formatSize(jsRound(*entry.size)) : QString();
    const QString count = !entry.count ? QString()
                          : *entry.count == 1
                              ? QStringLiteral("1 item")
                              : QStringLiteral("%1 items").arg(format(*entry.count));
    return {entry.folder ? (!count.isEmpty() ? count : size) : size,
            entry.when ? fvWhen(*entry.when) : QString()};
}

bool fvFits(Ctx &c, const QString &text, double max, double size, int weight)
{
    return c.textWidth(text, size, weight) <= max;
}

// Whether a cut at unit i would split a surrogate pair.
bool midPair(const QString &text, int i)
{
    return i > 0 && i < text.size() && text.at(i).isLowSurrogate() &&
           text.at(i - 1).isHighSurrogate();
}

// The end of a text that fits after an ellipsis, like the tail of a long path.
QString fvTail(Ctx &c, const QString &text, double max, double size, int weight)
{
    if (fvFits(c, text, max, size, weight))
        return text;
    int from = 0;
    while (from < text.size() - 1 &&
           (midPair(text, from) || !fvFits(c, ellipsis() + text.mid(from), max, size, weight)))
        ++from;
    if (midPair(text, from))
        --from;
    return ellipsis() + text.mid(from);
}

// A file name on one line keeps its extension: the middle gives way first.
QString fvShort(Ctx &c, const QString &text, double max, double size, int weight)
{
    if (fvFits(c, text, max, size, weight))
        return text;
    const int length = int(text.size()), dot = int(text.lastIndexOf(QLatin1Char('.')));
    int from = dot > 0 && length - dot <= 8 ? std::max(0, dot - 3) : std::max(0, length - 5);
    if (midPair(text, from))
        --from;
    const QString tail = text.mid(from);
    int head = length - int(tail.size());
    while (head > 1 &&
           !fvFits(c, trimEnd(cutUnits(text, head)) + ellipsis() + tail, max, size, weight))
        --head;
    return trimEnd(cutUnits(text, head)) + ellipsis() + tail;
}

// String.prototype.lastIndexOf(ch, from): the last at or before `from`.
int lastIndexAt(const QString &text, QChar ch, int from)
{
    for (int i = std::min(from, int(text.size()) - 1); i >= 0; --i) {
        if (text.at(i) == ch)
            return i;
        if (i == 0)
            break;
    }
    return -1;
}

// A file name on two lines: it breaks at a space or after . _ - where it can,
// and a name too long for both keeps its end, where the extension is.
// (Upstream has it unused; kept for the grid of tiles it was written for.)
[[maybe_unused]] QStringList fvLines(Ctx &c, const QString &text, double max, double size,
                                     int weight)
{
    if (fvFits(c, text, max, size, weight))
        return {text};
    int cut = int(text.size());
    while (cut > 1 && !fvFits(c, trimEnd(cutUnits(text, cut)), max, size, weight))
        --cut;
    int soft = lastIndexAt(text, QLatin1Char(' '), std::max(0, cut));
    for (const char ch : {'.', '_', '-'})
        soft = std::max(soft, lastIndexAt(text, QLatin1Char(ch), std::max(0, cut - 1)) + 1);
    if (soft > cut * 0.3)
        cut = soft;
    if (midPair(text, cut))
        --cut;
    QString rest = text.mid(cut);
    while (!rest.isEmpty() && rest.at(0).isSpace())
        rest.remove(0, 1);
    return {trimEnd(text.left(cut)), fvTail(c, rest, max, size, weight)};
}

std::optional<Result> filesScene(Ctx &c, const FvPage &page)
{
    // The card fills the column like a code block: the scene keeps the usual
    // margin, the card reaches into it.
    const double W = clamp(c.width - PAD * 2, 280, FV.max), left = -PAD + FV.pad,
                 right = W + PAD - FV.pad;
    QVector<Spec> items;
    QHash<QString, Tip> tips;
    const QVector<FvRow> flat = fvFlatten(page);
    const QVector<FvRow> shown = flat.mid(0, FV.cap);
    const double hidden = double(flat.size() - shown.size()) + page.more;
    const bool tree =
        page.view == QLatin1String("tree") ||
        (page.view != QLatin1String("list") &&
         std::any_of(flat.begin(), flat.end(), [](const FvRow &row) { return row.depth; }));
    const int files = int(std::count_if(flat.begin(), flat.end(), [&](const FvRow &row) {
        return !page.all.at(row.entry).folder;
    }));
    const int folders = int(flat.size()) - files;
    const QVector<double> totals = fvTotals(page);
    double total = 0;
    for (const int e : page.entries)
        total += totals.at(e) == totals.at(e) ? totals.at(e) : 0;

    // One line on top: the folder, its name and where it is, and on the right
    // what it holds.
    QString title = page.title;
    if (title.isEmpty()) {
        static const Re slash(QStringLiteral("[\\\\/]"));
        const QStringList parts = page.path.split(slash, Qt::SkipEmptyParts);
        title = !parts.isEmpty() ? parts.last() : QStringLiteral("Files");
    }
    QStringList countParts;
    if (files)
        countParts << (files == 1 ? QStringLiteral("1 file")
                                  : QStringLiteral("%1 files").arg(format(files)));
    if (folders)
        countParts << (folders == 1 ? QStringLiteral("1 folder")
                                    : QStringLiteral("%1 folders").arg(format(folders)));
    const QString counts = countParts.join(QString::fromUtf8(" · "));
    const QString size = total ? formatSize(jsRound(total)) : QString();
    const double sumW =
        (!size.isEmpty() ? c.textWidth(size, 12.5, 650) + (!counts.isEmpty() ? 16 : 0) : 0) +
        (!counts.isEmpty() ? c.textWidth(counts, 12.5, 500) : 0);
    const double nameRoom = right - left - 28 - (sumW ? sumW + 20 : 0);
    const QString name = truncate(c, title, std::max(60.0, nameRoom * 0.6), 14, 650);
    const double pathRoom = nameRoom - c.textWidth(name, 14, 650) - 10;
    {
        Spec head = item(Type::FHead, QStringLiteral("fv:head"), 0);
        head.fixed.name = name;
        head.fixed.path = !page.path.isEmpty() && pathRoom > 60
                              ? fvTail(c, page.path, pathRoom, 12, 500)
                              : QString();
        head.fixed.sizeText = size;
        head.fixed.counts = counts;
        head.fixed.left = left;
        head.fixed.right = right;
        head.fixed.nameW = c.textWidth(name, 14, 650);
        items << head;
    }
    double y = FV.head;

    // What takes the space: a thin line of the kinds inside, in the colours of
    // their icons, parting the head from the list.
    QVector<QPair<QString, double>> kinds; // In the order they come (a Map).
    for (const FvRow &row : flat) {
        const FvEntry &entry = page.all.at(row.entry);
        const std::optional<double> bytes =
            entry.folder ? (row.depth == 0 && entry.children.isEmpty() ? entry.size : std::nullopt)
                         : entry.size;
        if (!bytes || !*bytes)
            continue;
        const QString kind = fvKind(entry);
        auto found = std::find_if(kinds.begin(), kinds.end(),
                                  [&](const QPair<QString, double> &k) { return k.first == kind; });
        if (found == kinds.end())
            kinds << qMakePair(kind, *bytes);
        else
            found->second += *bytes;
    }
    double sized = 0;
    for (const auto &k : kinds)
        sized += k.second;
    QVector<QPair<QString, double>> list = kinds;
    std::stable_sort(list.begin(), list.end(),
                     [](const QPair<QString, double> &a, const QPair<QString, double> &b) {
                         return b.second - a.second < 0;
                     });
    const auto sizedRows = std::count_if(flat.begin(), flat.end(), [&](const FvRow &row) {
        const auto &s = page.all.at(row.entry).size;
        return s && *s;
    });
    if (sized > 0 && sizedRows >= 2) {
        const double room = right - left - 2 * (list.size() - 1);
        QVector<double> widths;
        double sum = 0;
        for (const auto &k : list) {
            // Not upstream: sizes past a double's range share nothing (NaN).
            const double share = k.second / sized * room;
            widths << (finite(share) ? std::max(3.0, share) : 3.0);
            sum += widths.last();
        }
        const double fit = room / sum;
        double x = left;
        for (int i = 0; i < list.size(); ++i) {
            const QString &kind = list.at(i).first;
            const double bytes = list.at(i).second;
            const QString key = QStringLiteral("fv:seg:") + kind;
            const double w = widths.at(i) * fit;
            Spec seg = item(Type::FSeg, key, 0.3 + i * 0.08);
            seg.props.x = x;
            seg.props.w = w;
            seg.fixed.y = y;
            seg.fixed.h = FV.meter;
            seg.fixed.color = fvTone(kind);
            items << seg;
            Tip tip;
            tip.title = fvKindName(kind);
            TipRow sizeRow, shareRow;
            sizeRow.name = QStringLiteral("Size");
            sizeRow.value = formatSize(jsRound(bytes));
            shareRow.name = QStringLiteral("Share");
            shareRow.value = format(jsRound(bytes / sized * 1000) / 10) + QLatin1Char('%');
            tip.rows = {sizeRow, shareRow};
            tips.insert(key, tip);
            x += w + 2;
        }
    } else {
        items << lineSpec(QStringLiteral("fv:rule"), 0.3, left, y + FV.meter / 2, right,
                          y + FV.meter / 2, QStringLiteral("dg-fv-rule"));
    }
    y += FV.meter + 6;

    // The entries: name, then the date and the size in columns on the right;
    // a tree steps folders in.
    QVector<FvMeta> metas;
    double sizeW = 0, dateW = 0;
    for (const FvRow &row : shown) {
        metas << fvMeta(page.all.at(row.entry));
        if (!metas.last().amount.isEmpty())
            sizeW = std::max(sizeW, c.textWidth(metas.last().amount, 12.5, 550));
        if (!tree && !metas.last().when.isEmpty())
            dateW = std::max(dateW, c.textWidth(metas.last().when, 12, 500));
    }
    const double sizeX = right, dateX = right - (sizeW ? sizeW + 22 : 0);
    for (int i = 0; i < shown.size(); ++i) {
        if (!c.budget(8))
            return std::nullopt;
        const FvEntry &entry = page.all.at(shown.at(i).entry);
        const int depth = shown.at(i).depth;
        const QString key = QStringLiteral("fv:row:%1:").arg(i) + entry.name;
        const double indent = tree ? depth * FV.indent : 0, x = left + indent + 26;
        const double end = (dateW ? dateX - dateW : sizeW ? sizeX - sizeW : right) - 18;
        const QString label = fvShort(c, entry.name, std::max(40.0, end - x), 13.5, 500);
        const double labelW = c.textWidth(label, 13.5, 500);
        const QString note = !entry.note.isEmpty() && labelW + 24 < end - x
                                 ? fvShort(c, entry.note, end - x - labelW - 12, 12, 500)
                                 : QString();
        Spec row = item(Type::FRow, key, 0.6 + i * 0.1);
        row.props.x = 0;
        row.props.y = y + i * FV.row;
        row.fixed.name = entry.name;
        row.fixed.folder = entry.folder;
        row.fixed.label = label;
        row.fixed.note = note;
        row.fixed.labelW = labelW;
        row.fixed.iconX = left + indent;
        row.fixed.textX = x;
        row.fixed.amount = metas.at(i).amount;
        row.fixed.when = dateW ? metas.at(i).when : QString();
        row.fixed.sizeX = sizeX;
        row.fixed.dateX = dateX;
        row.fixed.h = FV.row;
        row.fixed.plateX = -PAD + 6;
        row.fixed.plateW = W + PAD * 2 - 12;
        row.fixed.rule = i < shown.size() - 1 || hidden > 0 ? x : 0;
        row.fixed.right = right;
        items << row;
        // The row already says what a tip would, so it only lights up.
        Tip tip;
        tip.silent = true;
        tips.insert(key, tip);
    }
    // In a tree a quiet line runs down from each open folder along what it holds.
    if (tree) {
        for (int i = 0; i < shown.size(); ++i) {
            const int depth = shown.at(i).depth;
            int end = i;
            while (end + 1 < shown.size() && shown.at(end + 1).depth > depth)
                ++end;
            if (end == i)
                continue;
            const double x = left + depth * FV.indent + 8;
            items << lineSpec(QStringLiteral("fv:guide:%1:").arg(i) +
                                  page.all.at(shown.at(i).entry).name,
                              0.8 + i * 0.1, x, y + i * FV.row + FV.row - 7, x,
                              y + end * FV.row + FV.row / 2, QStringLiteral("dg-fv-guide"), true);
        }
    }
    y += shown.size() * FV.row;
    if (hidden > 0) {
        items << labelSpec(QStringLiteral("fv:more"), 0.6 + shown.size() * 0.1, left + 26,
                           y + FV.row / 2, {QStringLiteral("+%1 more").arg(format(hidden))},
                           QStringLiteral("dg-fv-more"), QStringLiteral("start"));
        y += FV.row;
    }
    y += 6;
    // The card goes under everything and grows with the list as the reply
    // comes in.
    Spec card = item(Type::FCard, QStringLiteral("fv:card"), 0);
    card.props.x = -PAD;
    card.props.y = -6;
    card.props.w = W + PAD * 2;
    card.props.h = y + 6;
    items << card;

    Result r;
    r.kind = QStringLiteral("files");
    r.width = W;
    r.height = y;
    r.items = std::move(items);
    r.tips = std::move(tips);
    return r;
}
} // namespace

std::optional<Result> wireframeKind(Ctx &c, const Lines &lines)
{
    const auto page = parseWireframe(lines.lines);
    if (!page)
        return std::nullopt;
    return WireScene(c, *page).build();
}

std::optional<Result> filesKind(Ctx &c, const Lines &lines)
{
    const auto page = parseFiles(c, lines.lines);
    if (!page)
        return std::nullopt;
    return filesScene(c, *page);
}

} // namespace diagram
