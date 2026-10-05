#pragma once

#include <QColor>
#include <QFont>
#include <QPainterPath>
#include <QRectF>
#include <QString>

class QPainter;

// SVG path data for trusted constants (OpenGhost's glyphs): M L H V C S A Z,
// absolute and relative.
namespace svgpath
{
QPainterPath parse(const char *d);
} // namespace svgpath

// openghost/file-kinds.js: a file's kind by extension (or whole name), its
// tone, glyph and name, and the page icon attachments and folders wear.
namespace filekinds
{
struct Described {
    QString kind, label, glyph;
    QColor tone;
};
Described describe(const QString &name);
// FileKinds.tones[name], or gray.
QColor tone(const QString &name);
// FileKinds.formatSize: 812 B, 2.4 MB.
QString formatSize(double bytes);
// The icon (viewBox 0 0 32 40) fitted and centred in `box`: the page in the
// kind's tone, its glyph and its extension label, in `labelFont` (its pixel
// size ignored) at 800. `light` takes the light theme's deeper ink.
void paintIcon(QPainter &painter, const QRectF &box, const Described &kind, bool light,
               const QFont &labelFont);
} // namespace filekinds
