#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QVector>

// TeX math without a browser: OpenGhost's tex.js subset laid out as boxes and
// painted with QPainter into an image. Reentrant and bounded; it only reads
// its source string and never executes, loads or fetches anything. Call it
// off the GUI thread.
namespace tex
{
struct Style {
    QString family;       // Font family; empty: OpenGhost's math serif stack (STIX Two
                          // Math, Latin Modern Math, Cambria Math, … Noto/DejaVu Serif).
    qreal pixelSize = 16; // Surrounding text size, logical pixels; math is 1.1× (inline)
                          // or 1.4× (display) of it, as .md-imath/.md-math.
    QColor color = QColor::fromRgbF(1, 1, 1, 0.85);
};

struct Rendered {
    QImage image;       // Premultiplied ARGB at the requested ratio; null unless ok.
    QSizeF size;        // Logical size.
    qreal baseline = 0; // Logical distance from the top to the text baseline.
    bool ok = false;
    QString error; // A refusal (too long, too deep, too large): show the source instead.
};

constexpr int MaxSource = 8192; // UTF-16 units.

Rendered render(const QString &source, bool display, const Style &style, qreal dpr);

// What OpenGhost 1.2's Ctrl+C takes from a formula: tex.js's HTML as
// Chromium copies it. A Break stands where a box laid out by a flex or grid
// (.mf, .mss, .ms, .mdel, .mlim, .mgrid) starts or ends: between texts it
// copies as one line end, however many meet. A Mark is an empty delimiter
// box (.mdl): it copies nothing, but a selection that starts or ends with it
// keeps the line end beside it. Bounded like render(): a source past
// MaxSource is one Text, itself.
struct Piece {
    enum Kind : quint8 { Text, Break, Mark };
    Kind kind = Text;
    QString text;
    bool operator==(const Piece &other) const { return kind == other.kind && text == other.text; }
};
QVector<Piece> copy(const QString &source, bool display);
} // namespace tex
