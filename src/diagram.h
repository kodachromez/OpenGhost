#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

// Drawings without a browser: OpenGhost 1.3's diagram.js (its 43 kinds)
// parsed, laid out into a scene of keyed items (diagram_engine.h) and
// painted with QPainter. Reentrant and bounded; it only reads its source
// string and never executes, loads or fetches anything. compile() and
// render() run off the GUI thread.
namespace diagram
{
struct Result;
using Hints = QHash<QString, double>;

struct Options {
    qreal width = 640; // The stage's logical width: the room a drawing has.
    qreal column = 0;  // The text column inside it (0: the whole stage).
    qreal zoom = 1;    // Shown this much larger than laid out (TYPE).
    qreal dpr = 1;     // Device pixel ratio of a rendered image.
    // The section's colour the drawing leads with (SECTION: blue 0, orange
    // 1, turquoise 2, lilac 3, yellow 4, pink 5, green 6).
    int section = 0;
    QString family, mono; // Fonts; empty: the application's default.
    // A scheme's places and way from its last layout, kept while it grows.
    Hints hints;
    QString sideways;
    // Set by another thread: work stops between its steps, unfinished.
    const std::atomic_bool *cancel = nullptr;
};

constexpr int MaxSource = 64 * 1024; // UTF-16 units.

// SECTION's index of a theme tone (theme::tones order); -1 (neutral): 0.
int sectionOf(int themeTone);

// The drawing's scene, or why there is none.
struct Compiled {
    std::shared_ptr<const Result> result;
    QString error;
};
// `kind` is the fence's language hint (markdown's DIAGRAM_KINDS value, or
// empty for "mermaid" and bare headers).
Compiled compile(const QString &source, const QString &kind, const Options &options);

// Where a drawing stands in its stage (DiagramView.viewSpec), in logical px:
// its origin (ox, top) and scale s, its drawn width cw, the stage's size
// (w, h) and where the tools go (tx, ty).
struct View {
    double ox = 0, s = 1, cw = 0, top = 0, h = 0, w = 0, tx = 0, ty = 0;
};
View viewOf(const Result &result, double room, double columnLeft, double columnWidth);
// The margin painted around the drawing in a rendered image.
double margin(const Result &result);

struct Rendered {
    QImage image;    // Premultiplied ARGB at options.dpr; null unless ok.
    QSizeF size;     // The stage's logical size (view w × h).
    double left = 0; // Where the image's left edge stands in the stage.
    QString kind;    // The kind drawn, e.g. "flow".
    View view;
    std::shared_ptr<const Result> result;
    // Each line of text's box, in logical stage coordinates: what a
    // selection paints.
    QVector<QRectF> texts;
    bool ok = false;
    QString error; // Why nothing was drawn.
};
// compile() and the settled drawing painted.
Rendered render(const QString &source, const QString &kind, const Options &options);

// The texts a selection copies from a drawn diagram, in document order
// (layer by layer, then as laid out). `ok` false: it does not draw.
struct Texts {
    QStringList texts;
    bool ok = false;
};
Texts texts(const QString &source, const QString &kind, const Options &options);
Texts texts(const Result &result);
} // namespace diagram
