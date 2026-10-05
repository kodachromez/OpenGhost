#include "icon.h"

#include "filekinds.h"

#include "rich.h"
#include "theme.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace
{
struct Glyph {
    const char *path;
    qreal stroke; // Line width in view units; 0: filled.
    qreal view;   // Square view box size.
};

// From openghost/markdown.js, diagram.js and styles.css (icon masks).
const QHash<QString, Glyph> &glyphs()
{
    static const QHash<QString, Glyph> table = {
        {"copy",
         {"M5.25 7.5a2.25 2.25 0 0 1 2.25-2.25h4a2.25 2.25 0 0 1 2.25 2.25v4a2.25 2.25 0 0 1-2.25 "
          "2.25h-4a2.25 2.25 0 0 1-2.25-2.25zM10.75 3.25a2 2 0 0 0-2-2h-4.5a3 3 0 0 0-3 3v4.5a2 2 "
          "0 "
          "0 0 2 2",
          1.4, 16}},
        {"check", {"M3.5 8.5l3 3 6-7", 1.7, 16}},
        {"tick", {"M4 8.5l2.6 2.6L12 5.5", 2, 16}},
        {"edit",
         {"M10.2 3.3l2.5 2.5M3 13l.6-3 6.9-6.9a1.4 1.4 0 0 1 2 0l.4.4a1.4 1.4 0 0 1 0 2L6 12.4z",
          1.5, 16}},
        {"note", {"M14.2 8a6.2 6.2 0 1 1-12.4 0a6.2 6.2 0 1 1 12.4 0zM8 7.2v3.6M8 5h.01", 1.6, 16}},
        {"tip",
         {"M6 12.5h4M6.5 14.5h3M8 1.8a4.4 4.4 0 0 0-2.6 8c.4.3.6.8.6 1.3V12h4v-.9c0-.5.2-1 "
          ".6-1.3A4.4 "
          "4.4 0 0 0 8 1.8z",
          1.5, 16}},
        {"warning",
         {"M7 2.6a1.2 1.2 0 0 1 2 0l5.4 9.6a1.2 1.2 0 0 1-1 1.8H2.6a1.2 1.2 0 0 1-1-1.8zM8 6.4v3M8 "
          "11.6h.01",
          1.5, 16}},
        {"close", {"M3 3l6 6M9 3l-6 6", 1.6, 12}},
        // settings-general.js PLUS.
        {"plus-thin", {"M8 3.2v9.6M3.2 8h9.6", 1.6, 16}},
        // OpenGhost's 60-unit icons (viewBox 30 30 60 60, moved to 0 0),
        // stroked at --icon-stroke. The sidebar toggle is ToggleIcon.
        // settings-button.js: six teeth and a hub.
        {"gear",
         {"M25.90 14.02L26.56 8.27A22 22 0 0 1 33.44 8.27L34.10 14.02A16.5 16.5 0 0 1 41.79 18.46"
          "L47.10 16.15A22 22 0 0 1 50.54 22.12L45.89 25.56A16.5 16.5 0 0 1 45.89 34.44"
          "L50.54 37.88A22 22 0 0 1 47.10 43.85L41.79 41.54A16.5 16.5 0 0 1 34.10 45.98"
          "L33.44 51.73A22 22 0 0 1 26.56 51.73L25.90 45.98A16.5 16.5 0 0 1 18.21 41.54"
          "L12.90 43.85A22 22 0 0 1 9.46 37.88L14.11 34.44A16.5 16.5 0 0 1 14.11 25.56"
          "L9.46 22.12A22 22 0 0 1 12.90 16.15L18.21 18.46A16.5 16.5 0 0 1 25.90 14.02Z"
          "M37 30A7 7 0 1 1 23 30A7 7 0 1 1 37 30Z",
          5.5, 60}},
        // The composer's buttons: add-button.js, send-button.js and
        // model-button.js (its big outlined star and the small solid one at
        // rest). Stop is Native's own: OpenGhost stops only with Escape.
        {"add", {"M30 9V51M9 30H51", 6, 60}},
        {"send", {"M30 54V7M11 26L30 7L49 26", 6, 60}},
        {"sparkle",
         {"M25 14c1.5 10.45 8.55 17.5 19 19-10.45 1.5-17.5 8.55-19 19-1.5-10.45-8.55-17.5-19-19 "
          "10.45-1.5 17.5-8.55 19-19z",
          5.5, 60}},
        {"sparkle-small",
         {"M47 3c.63 4.4 3.6 7.37 8 8-4.4.63-7.37 3.6-8 8-.63-4.4-3.6-7.37-8-8 4.4-.63 7.37-3.6 "
          "8-8z",
          0, 60}},
        {"stop",
         {"M19 15h22a4 4 0 0 1 4 4v22a4 4 0 0 1-4 4H19a4 4 0 0 1-4-4V19a4 4 0 0 1 4-4z", 0, 60}},
        // script.js REMOVE: an attachment's ✕.
        {"remove", {"M2 2l6 6M8 2l-6 6", 1.6, 10}},
        // The sidebar's header (search-button.js, add-button.js) and list
        // (glyphs.js); the folder open and shut (.folder-front's two paths).
        {"search",
         {"M40 24.5A15.5 15.5 0 1 1 9 24.5A15.5 15.5 0 1 1 40 24.5ZM35.5 35.5L51 51", 5.5, 60}},
        {"new-chat", {"M30 9V51M9 30H51", 5.5, 60}},
        {"plus", {"M30 13v34M13 30h34", 5.5, 60}},
        {"folder",
         {"M8 40V16a4 4 0 0 1 4-4h10a4 4 0 0 1 3.2 1.6L28 17h20a4 4 0 0 1 4 4v2"
          "M12 27L56 27L50.8 43.2A4 4 0 0 1 47 46L12 46A4 4 0 0 1 8 42Z",
          5.5, 60}},
        {"folder-shut",
         {"M8 40V16a4 4 0 0 1 4-4h10a4 4 0 0 1 3.2 1.6L28 17h20a4 4 0 0 1 4 4v2"
          "M8 23L52 23L52 42A4 4 0 0 1 48 46L12 46A4 4 0 0 1 8 42Z",
          5.5, 60}},
        // OpenGhost 1.3's folderless Chats group (glyphs.js).
        {"bubble",
         {"M30 8c-13 0-23 8.6-23 19.5 0 5.4 2.5 10.3 6.6 13.8L12 51l10.8-5.2c2.3.5 4.7.8 7.2.8 "
          "13 0 23-8.6 23-19.5S43 8 30 8z",
          5.5, 60}},
        {"folder-add",
         {"M36 46H12a4 4 0 0 1-4-4V16a4 4 0 0 1 4-4h10a4 4 0 0 1 3.2 1.6L28 17h20a4 4 0 0 1 4 4v8"
          "M8 23h44M48 35v14M41 42h14",
          5.5, 60}},
        {"pin", {"M22 9h16M25.5 9v12.5L19 29h22l-6.5-7.5V9M30 29v20", 5.5, 60}},
        {"pin-fill", {"M22 9h16M25.5 9v12.5L19 29h22l-6.5-7.5V9M30 29v20", 0, 60}},
        {"trash",
         {"M10 15h40M23.5 15v-4.5a3.5 3.5 0 0 1 3.5-3.5h6a3.5 3.5 0 0 1 3.5 3.5V15"
          "M15 15l2.3 28.4a4 4 0 0 0 4 3.6h17.4a4 4 0 0 0 4-3.6L45 15M25.5 25v12M34.5 25v12",
          5.5, 60}},
        // The same trash in two parts, so its lid can lift (.trash-lid).
        {"trash-lid",
         {"M10 15h40M23.5 15v-4.5a3.5 3.5 0 0 1 3.5-3.5h6a3.5 3.5 0 0 1 3.5 3.5V15", 5.5, 60}},
        {"trash-body",
         {"M15 15l2.3 28.4a4 4 0 0 0 4 3.6h17.4a4 4 0 0 0 4-3.6L45 15M25.5 25v12M34.5 25v12", 5.5,
          60}},
        {"clear", {"M1.5 1.5l5 5M6.5 1.5l-5 5", 1.6, 8}},
        // glyphs.js pencil: the body, and the stroke it writes under its tip.
        {"pencil",
         {"M14 46l2.6-10.4 21.9-21.9a5.1 5.1 0 0 1 7.2 0l.6.6a5.1 5.1 0 0 1 0 7.2L24.4 43.4z"
          "M34.5 17.7l7.8 7.8",
          5.5, 60}},
        {"pencil-line", {"M30 50h18", 5.5, 60}},
        // OpenGhost's glyphs.js terminal and file icons.
        {"terminal",
         {"M14 10H46A8 8 0 0 1 54 18V42A8 8 0 0 1 46 50H14A8 8 0 0 1 6 42V18A8 8 0 0 1 14 10Z"
          "M17 23l7 7-7 7M31 37h11",
          5.5, 60}},
        {"file",
         {"M19 6h14l13 13v31a4 4 0 0 1-4 4H19a4 4 0 0 1-4-4V10a4 4 0 0 1 4-4zM32 6v14h14", 5.5,
          60}},
        // glyphs.js's bubble and check (the Settings menus), and its ghost:
        // filled with its eyes cut out (viewBox -1 -1 60 62, centred in 62),
        // then the eyes alone, which the menus fill with the app background.
        {"bubble",
         {"M30 8c-13 0-23 8.6-23 19.5 0 5.4 2.5 10.3 6.6 13.8L12 51l10.8-5.2c2.3.5 4.7.8 7.2.8 "
          "13 0 23-8.6 23-19.5S43 8 30 8z",
          5.5, 60}},
        {"check-mark", {"M15 31l10 10 20-22", 5.5, 60}},
        {"check-small", {"M2.6 6.3l2.3 2.3 4.6-4.9", 1.9, 12}},
        {"ghost",
         {"M2 30A29 29 0 0 1 60 30L60 58A2.5 2.5 0 0 1 55 58A6 6 0 0 0 43 58A3 3 0 0 1 37 58"
          "A6 6 0 0 0 25 58A3 3 0 0 1 19 58A6 6 0 0 0 7 58A2.5 2.5 0 0 1 2 58Z"
          "M22.8 30.5A3.8 4.1 0 1 1 15.2 30.5A3.8 4.1 0 1 1 22.8 30.5Z"
          "M46.8 30.5A3.8 4.1 0 1 1 39.2 30.5A3.8 4.1 0 1 1 46.8 30.5Z",
          0, 62}},
        {"ghost-eyes",
         {"M22.8 30.5A3.8 4.1 0 1 1 15.2 30.5A3.8 4.1 0 1 1 22.8 30.5Z"
          "M46.8 30.5A3.8 4.1 0 1 1 39.2 30.5A3.8 4.1 0 1 1 46.8 30.5Z",
          0, 62}},
        // settings-usage.js's chevron (the month arrows).
        {"chevron-back", {"M7.5 2.5 4 6l3.5 3.5", 1.6, 12}},
        // close-button.js's cross (viewBox 30 30 60 60, moved to 0 0).
        {"cross", {"M15 15 45 45M45 15 15 45", 5.5, 60}},
        // The agent modes (glyphs.js lock, shield and shieldAlert, view
        // 30 30 60 60 moved to 0 0; each was its own <path>, so the second
        // path's relative start is absolute here).
        {"lock",
         {"M19 25H41A6 6 0 0 1 47 31V44A6 6 0 0 1 41 50H19A6 6 0 0 1 13 44V31A6 6 0 0 1 19 25Z"
          "M19 25v-6.5a11 11 0 0 1 22 0V25M30 34.5v6",
          5.5, 60}},
        {"shield",
         {"M30 7.5L47 14v13c0 11-7 19.5-17 23-10-3.5-17-12-17-23V14zM22.5 29l5 5 10-10.5", 5.5,
          60}},
        {"shield-alert",
         {"M30 7.5L47 14v13c0 11-7 19.5-17 23-10-3.5-17-12-17-23V14zM30 20v11M30 39v.5", 5.5, 60}},
        // The approval card's reveal chevron (approval-card.js, view 12).
        {"reveal-chevron", {"M3 4.6l3 3 3-3", 1.6, 12}},
        {"globe",
         {"M52 30A22 22 0 1 1 8 30A22 22 0 1 1 52 30ZM8.5 30h43"
          "M30 8c-6.5 6-10 13.5-10 22s3.5 16 10 22c6.5-6 10-13.5 10-22s-3.5-16-10-22z",
          5.5, 60}},
        // The browser panel's controls (browser-panel.js ICONS, view 30 30
        // 60 60 moved to 0 0).
        {"browser-back", {"M35 12 17 30l18 18", 5.5, 60}},
        {"browser-forward", {"M25 12l18 18-18 18", 5.5, 60}},
        {"browser-reload", {"M47.5 21A19 19 0 1 0 49 30M49 8v13.5H35.5", 5.5, 60}},
        {"browser-stop", {"M16 16l28 28M44 16 16 44", 5.5, 60}},
        {"browser-plus", {"M30 14v32M14 30h32", 5.5, 60}},
        {"browser-close", {"M20 20l20 20M40 20 20 40", 5.5, 60}},
        {"browser-external",
         {"M35 10h15v15M50 10 28 32M43 37v8a5 5 0 0 1-5 5H15a5 5 0 0 1-5-5V22a5 5 0 0 1 5-5h8", 5.5,
          60}},
        // The jump to the latest message (scroll-button.js, view 30 30 60 60).
        {"arrow-down", {"M30 6v47M11 34L30 53L49 34", 6, 60}},
        // A link chip's globe (styles.css .link-chip-icon::before).
        {"link-globe",
         {"M14.2 8A6.2 6.2 0 1 1 1.8 8A6.2 6.2 0 1 1 14.2 8Z"
          "M10.7 8A2.7 6.2 0 1 1 5.3 8A2.7 6.2 0 1 1 10.7 8ZM2 8h12",
          1.3, 16}},
        // A reply's pictures and videos (media-embed.js PICTURE and PLAY,
        // media-slider.js CHEVRON).
        {"media-picture",
         {"M4.5 3h7a2.5 2.5 0 0 1 2.5 2.5v5a2.5 2.5 0 0 1-2.5 2.5h-7a2.5 2.5 0 0 1-2.5-2.5v-5"
          "a2.5 2.5 0 0 1 2.5-2.5zM7.1 6.8a1.1 1.1 0 1 1-2.2 0a1.1 1.1 0 1 1 2.2 0z"
          "M2.6 11.4l3.2-2.9 2.4 2 2.2-2.2 3 3",
          1.4, 16}},
        {"media-play",
         {"M9.2 6.6a1 1 0 0 1 1.5-.86l8.3 5.4a1 1 0 0 1 0 1.72l-8.3 5.4a1 1 0 0 1-1.5-.86z", 0,
          24}},
        {"media-chevron", {"M7.5 2.5 4 6l3.5 3.5", 1.7, 12}},
        // The selection menu's Ask (glyphs.js quote, view 24).
        {"quote",
         {"M10.2 6.3C6.6 7.6 4.4 10.4 4.4 14.1c0 2.4 1.5 4 3.5 4 1.8 0 3.2-1.3 3.2-3.1 0-1.7-1.2-2.9"
          "-2.8-2.9-.3 0-.6 0-.8.1.4-1.7 1.8-3.1 3.6-3.9zm9 0c-3.6 1.3-5.8 4.1-5.8 7.8 0 2.4 1.5 4 "
          "3.5 4 1.8 0 3.2-1.3 3.2-3.1 0-1.7-1.2-2.9-2.8-2.9-.3 0-.6 0-.8.1.4-1.7 1.8-3.1 3.6-3.9z",
          0, 24}},
        // Chromium's <summary> markers, shut and open: equilateral triangles.
        {"disclosure-closed", {"M0 0L8.66 5L0 10Z", 0, 10}},
        {"disclosure-open", {"M0 0H10L5 8.66Z", 0, 10}},
    };
    return table;
}

// Two path data with the same commands, their numbers `t` of the way from
// `a` to `b`, as CSS interpolates `d`; `b` when their commands differ.
QByteArray blend(const char *a, const char *b, qreal t)
{
    const char *const to = b;
    QByteArray out;
    auto skip = [](const char *&d) {
        while (*d == ' ' || *d == ',')
            ++d;
    };
    auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    for (skip(a), skip(b); *a || *b; skip(a), skip(b)) {
        if (letter(*a) || letter(*b)) {
            if (*a != *b)
                return to;
            out += *a++;
            ++b;
            continue;
        }
        char *endA = nullptr, *endB = nullptr;
        const double x = std::strtod(a, &endA), y = std::strtod(b, &endB);
        if (endA == a || endB == b)
            return to;
        a = endA;
        b = endB;
        out += QByteArray::number(x + (y - x) * t, 'g', 10) + ' ';
    }
    return out;
}

} // namespace

PathIcon::PathIcon(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setImplicitSize(16, 16);
}

void PathIcon::setName(const QString &name)
{
    if (name == m_name)
        return;
    m_name = name;
    emit nameChanged();
    update();
}

void PathIcon::setColor(const QColor &color)
{
    if (color == m_color)
        return;
    m_color = color;
    emit colorChanged();
    update();
}

void PathIcon::setMorphTo(const QString &name)
{
    if (name == m_morphTo)
        return;
    m_morphTo = name;
    emit morphToChanged();
    update();
}

void PathIcon::setMorph(qreal morph)
{
    if (qFuzzyCompare(morph, m_morph))
        return;
    m_morph = morph;
    emit morphChanged();
    update();
}

void PathIcon::setReveal(qreal reveal)
{
    if (qFuzzyCompare(reveal, m_reveal))
        return;
    m_reveal = reveal;
    emit revealChanged();
    update();
}

bool PathIcon::known(const QString &name) { return glyphs().contains(name); }

void PathIcon::setStroke(qreal stroke)
{
    if (qFuzzyCompare(stroke, m_stroke))
        return;
    m_stroke = stroke;
    update();
    emit strokeChanged();
}

void PathIcon::paint(QPainter *painter)
{
    const auto found = glyphs().constFind(m_name);
    if (found == glyphs().constEnd())
        return;
    // A morph to another glyph drawn the same way: `morph` of the way there.
    const auto other = m_morph > 0 ? glyphs().constFind(m_morphTo) : glyphs().constEnd();
    const bool morphs = other != glyphs().constEnd() && other->stroke == found->stroke &&
                        other->view == found->view;
    const QPainterPath path =
        morphs ? svgpath::parse(
                     blend(found->path, other->path, std::min<qreal>(1, m_morph)).constData())
               : svgpath::parse(found->path);
    const qreal scale = std::min(width(), height()) / found->view;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate((width() - found->view * scale) / 2, (height() - found->view * scale) / 2);
    painter->scale(scale, scale);
    if (found->stroke > 0) {
        const qreal stroke = m_stroke > 0 ? m_stroke : found->stroke;
        QPen pen(m_color, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        if (m_reveal <= 0)
            return;
        if (m_reveal < 1) { // One dash from the start (in line widths), as stroke-dashoffset shows.
            const qreal length = path.length() / stroke;
            pen.setDashPattern({m_reveal * length, length + 1});
        }
        painter->strokePath(path, pen);
    } else {
        painter->fillPath(path, m_color);
    }
}

ToggleIcon::ToggleIcon(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setImplicitSize(22, 22);
}

void ToggleIcon::setSplit(qreal split)
{
    if (qFuzzyCompare(split, m_split))
        return;
    m_split = split;
    emit splitChanged();
    update();
}

void ToggleIcon::setColor(const QColor &color)
{
    if (color == m_color)
        return;
    m_color = color;
    emit colorChanged();
    update();
}

void ToggleIcon::paint(QPainter *painter)
{
    // The "sidebar" glyph's frame: 54.5 × 42 from (2.75, 9), radius 11.
    static const QPainterPath frame = svgpath::parse(
        "M13.75 9H46.25A11 11 0 0 1 57.25 20V40A11 11 0 0 1 46.25 51H13.75A11 11 0 0 1 "
        "2.75 40V20A11 11 0 0 1 13.75 9Z");
    const qreal scale = std::min(width(), height()) / 60;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate((width() - 60 * scale) / 2, (height() - 60 * scale) / 2);
    painter->scale(scale, scale);
    QPainterPath pane;
    pane.addRect(QRectF(2.75, 9, std::max<qreal>(0, m_split - 2.75), 42));
    QColor shade = m_color;
    shade.setAlphaF(m_color.alphaF() * 0.3);
    painter->fillPath(frame.intersected(pane), shade);
    QPainterPath lines = frame;
    lines.moveTo(m_split, 9);
    lines.lineTo(m_split, 51);
    painter->strokePath(lines, QPen(m_color, 5.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
}

GlobeIcon::GlobeIcon(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setImplicitSize(22, 22);
}

void GlobeIcon::setSpin(qreal spin)
{
    if (qFuzzyCompare(spin, m_spin))
        return;
    m_spin = spin;
    emit spinChanged();
    update();
}

void GlobeIcon::setOpen(qreal open)
{
    if (qFuzzyCompare(open, m_open))
        return;
    m_open = open;
    emit openChanged();
    update();
}

void GlobeIcon::setColor(const QColor &color)
{
    if (color == m_color)
        return;
    m_color = color;
    emit colorChanged();
    update();
}

void GlobeIcon::paint(QPainter *painter)
{
    const qreal scale = std::min(width(), height()) / 60;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate((width() - 60 * scale) / 2, (height() - 60 * scale) / 2);
    painter->scale(scale, scale);
    const QPen pen(m_color, 5.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QPainterPath globe;
    globe.addEllipse(QPointF(30, 30), 22, 22);
    QColor fill = m_color;
    fill.setAlphaF(m_color.alphaF() * m_open * 0.3);
    painter->fillPath(globe, fill);
    painter->strokePath(globe, pen);
    // The meridian narrows (rx 10 → 3.5) and moves 2.5 units as it spins,
    // clipped to the globe.
    QPainterPath meridian;
    meridian.addEllipse(QPointF(30 + 2.5 * m_spin, 30), std::max<qreal>(0.5, 10 - 6.5 * m_spin),
                        22);
    QPainterPathStroker stroker(pen);
    painter->fillPath(stroker.createStroke(meridian).intersected(globe), m_color);
    QPainterPath equator;
    equator.moveTo(8.5, 30);
    equator.lineTo(51.5, 30);
    painter->strokePath(equator, pen);
}


FileIcon::FileIcon(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    connect(ThemeSignal::instance(), &ThemeSignal::changed, this, [this] { update(); });
    setAntialiasing(true);
    setImplicitSize(30, 38);
}

FileIcon::Described FileIcon::describe(const QString &name)
{
    const filekinds::Described kind = filekinds::describe(name);
    return {kind.kind, kind.label, kind.glyph, kind.tone};
}

void FileIcon::setFileName(const QString &name)
{
    if (name == m_fileName)
        return;
    m_fileName = name;
    const Described described = describe(name);
    m_glyph = described.glyph;
    m_label = described.label;
    m_tone = described.tone;
    m_kind = described.kind;
    emit fileNameChanged();
    emit labelRectChanged();
    update();
}

namespace
{
// The extension label's font, drawn at 100 px and scaled to its size.
QFont labelFont()
{
    QFont font = QGuiApplication::font();
    font.setPixelSize(100);
    font.setWeight(QFont::Weight(Theme::cssWeight(font.family(), 800)));
    font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    return font;
}

qreal labelSize(const QString &label)
{
    return label.size() <= 3 ? 7.6 : label.size() == 4 ? 6.6 : 5.6;
}
} // namespace

QRectF FileIcon::labelRect() const
{
    // The text's box at x 16, baseline 33.6 of the fitted 32 × 40 view box.
    const qreal scale = std::min(width() / 32, height() / 40);
    const QFontMetricsF metrics(labelFont());
    const qreal size = labelSize(m_label) / 100 * scale;
    const qreal w = metrics.horizontalAdvance(m_label) * size;
    const QPointF base((width() - 32 * scale) / 2 + 16 * scale,
                       (height() - 40 * scale) / 2 + 33.6 * scale);
    return QRectF(base.x() - w / 2, base.y() - metrics.ascent() * size, w,
                  (metrics.ascent() + metrics.descent()) * size);
}


void FileIcon::paint(QPainter *painter)
{
    filekinds::Described kind;
    kind.label = m_label;
    kind.glyph = m_glyph;
    kind.tone = m_tone;
    filekinds::paintIcon(*painter, QRectF(0, 0, width(), height()), kind, theme::current().light,
                         labelFont());
}

ImageTile::ImageTile(QQuickItem *parent) : QQuickPaintedItem(parent) { setAntialiasing(true); }

void ImageTile::setImage(const QImage &image)
{
    m_image = image;
    update();
    emit imageChanged();
}

void ImageTile::setRadius(qreal radius)
{
    if (qFuzzyCompare(radius, m_radius))
        return;
    m_radius = radius;
    update();
    emit radiusChanged();
}

void ImageTile::paint(QPainter *painter)
{
    if (m_image.isNull())
        return;
    painter->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, width(), height()), m_radius, m_radius);
    painter->setClipPath(clip);
    painter->drawImage(QRectF(0, 0, width(), height()), m_image);
}
