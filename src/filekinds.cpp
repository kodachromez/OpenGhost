#include "filekinds.h"

#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace svgpath
{
QPainterPath parse(const char *d)
{
    QPainterPath path;
    QPointF at, start, control;
    char command = 0;
    auto skip = [&d] {
        while (*d == ' ' || *d == ',')
            ++d;
    };
    auto number = [&]() {
        skip();
        char *end = nullptr;
        const double value = std::strtod(d, &end);
        d = end;
        return value;
    };
    auto point = [&](bool relative) {
        const double x = number(), y = number();
        return relative ? at + QPointF(x, y) : QPointF(x, y);
    };
    for (skip(); *d; skip()) {
        if ((*d >= 'A' && *d <= 'Z') || (*d >= 'a' && *d <= 'z'))
            command = *d++;
        const bool relative = command >= 'a';
        switch (command | 0x20) {
        case 'm':
            at = start = control = point(relative);
            path.moveTo(at);
            command = relative ? 'l' : 'L';
            break;
        case 'l':
            at = control = point(relative);
            path.lineTo(at);
            break;
        case 'h':
            at.setX((relative ? at.x() : 0) + number());
            control = at;
            path.lineTo(at);
            break;
        case 'v':
            at.setY((relative ? at.y() : 0) + number());
            control = at;
            path.lineTo(at);
            break;
        case 'c': {
            const QPointF c1 = point(relative), c2 = point(relative), end = point(relative);
            path.cubicTo(c1, c2, end);
            control = c2;
            at = end;
            break;
        }
        case 's': {
            const QPointF c1 = at * 2 - control, c2 = point(relative), end = point(relative);
            path.cubicTo(c1, c2, end);
            control = c2;
            at = end;
            break;
        }
        case 'a': {
            // Endpoint to centre parameterization (SVG 1.1, F.6.5).
            double rx = std::abs(number()), ry = std::abs(number());
            const double phi = number() * M_PI / 180;
            const bool large = number() != 0, sweep = number() != 0;
            const QPointF end = point(relative);
            if (rx == 0 || ry == 0) {
                path.lineTo(end);
            } else {
                const double cosPhi = std::cos(phi), sinPhi = std::sin(phi);
                const double dx = (at.x() - end.x()) / 2, dy = (at.y() - end.y()) / 2;
                const double x1 = cosPhi * dx + sinPhi * dy, y1 = -sinPhi * dx + cosPhi * dy;
                const double scale = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
                if (scale > 1)
                    rx *= std::sqrt(scale), ry *= std::sqrt(scale);
                const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
                double k = std::sqrt(std::max(0.0, num / (rx * rx * y1 * y1 + ry * ry * x1 * x1)));
                if (large == sweep)
                    k = -k;
                const double cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx;
                const double cx = cosPhi * cx1 - sinPhi * cy1 + (at.x() + end.x()) / 2;
                const double cy = sinPhi * cx1 + cosPhi * cy1 + (at.y() + end.y()) / 2;
                auto angle = [](double ux, double uy, double vx, double vy) {
                    return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
                };
                const double theta = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
                double delta =
                    angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
                if (!sweep && delta > 0)
                    delta -= 2 * M_PI;
                else if (sweep && delta < 0)
                    delta += 2 * M_PI;
                const int segments = int(std::ceil(std::abs(delta) / (M_PI / 2)));
                const double step = delta / segments, t = 4.0 / 3 * std::tan(step / 4);
                auto on = [&](double a) {
                    return QPointF(cx + rx * std::cos(a) * cosPhi - ry * std::sin(a) * sinPhi,
                                   cy + rx * std::cos(a) * sinPhi + ry * std::sin(a) * cosPhi);
                };
                auto tangent = [&](double a) {
                    return QPointF(-rx * std::sin(a) * cosPhi - ry * std::cos(a) * sinPhi,
                                   -rx * std::sin(a) * sinPhi + ry * std::cos(a) * cosPhi);
                };
                for (int i = 0; i < segments; ++i) {
                    const double a = theta + i * step, b = a + step;
                    path.cubicTo(on(a) + t * tangent(a), on(b) - t * tangent(b),
                                 i + 1 == segments ? end : on(b));
                }
            }
            at = control = end;
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
} // namespace svgpath

namespace filekinds
{
namespace
{
// The constants' paths, parsed once per thread.
const QPainterPath &parse(const char *d)
{
    thread_local QHash<const char *, QPainterPath> parsed;
    auto found = parsed.constFind(d);
    if (found == parsed.constEnd())
        found = parsed.insert(d, svgpath::parse(d));
    return *found;
}

// openghost/file-kinds.js: a file's kind by extension (or whole name), its
// tone, glyph and name.
struct Kind {
    const char *tone;
    const char *glyph;
    const char *name;
};

const QHash<QString, QString> &tones()
{
    static const QHash<QString, QString> table = {
        {"blue", "100,160,255"},   {"steel", "138,170,222"},  {"indigo", "134,142,255"},
        {"violet", "168,140,255"}, {"purple", "196,138,250"}, {"pink", "255,132,184"},
        {"red", "255,112,104"},    {"orange", "255,158,92"},  {"amber", "255,196,92"},
        {"yellow", "236,212,98"},  {"green", "112,204,132"},  {"mint", "92,212,184"},
        {"teal", "84,196,222"},    {"cyan", "104,202,255"},   {"brown", "204,162,122"},
        {"gray", "168,170,180"},
    };
    return table;
}

const QHash<QString, Kind> &types()
{
    static const QHash<QString, Kind> table = [] {
        QHash<QString, Kind> out;
        const auto define = [&out](const char *list, const char *tone, const char *glyph,
                                   const char *name) {
            for (const QString &ext : QString::fromLatin1(list).split(u' '))
                out.insert(ext, {tone, glyph, name});
        };
        define("py pyw pyi", "blue", "code", "Python");
        define("ipynb", "orange", "code", "Jupyter Notebook");
        define("js mjs cjs", "yellow", "code", "JavaScript");
        define("jsx", "yellow", "code", "JavaScript React");
        define("ts mts cts", "indigo", "code", "TypeScript");
        define("tsx", "indigo", "code", "TypeScript React");
        define("c h", "steel", "code", "C");
        define("cpp cc cxx c++ hpp hh hxx h++ ino", "violet", "code", "C++");
        define("cs csx", "purple", "code", "C#");
        define("m mm", "steel", "code", "Objective-C");
        define("java", "orange", "code", "Java");
        define("kt kts", "purple", "code", "Kotlin");
        define("scala sc", "red", "code", "Scala");
        define("swift", "orange", "code", "Swift");
        define("go", "teal", "code", "Go");
        define("rs", "brown", "code", "Rust");
        define("rb erb rake gemspec", "red", "code", "Ruby");
        define("php", "indigo", "code", "PHP");
        define("dart", "cyan", "code", "Dart");
        define("lua", "indigo", "code", "Lua");
        define("r rmd", "blue", "code", "R");
        define("jl", "purple", "code", "Julia");
        define("pl pm", "steel", "code", "Perl");
        define("ex exs", "purple", "code", "Elixir");
        define("erl hrl", "red", "code", "Erlang");
        define("hs", "purple", "code", "Haskell");
        define("clj cljs edn", "green", "code", "Clojure");
        define("elm", "teal", "code", "Elm");
        define("zig", "amber", "code", "Zig");
        define("nim", "yellow", "code", "Nim");
        define("fs fsx", "cyan", "code", "F#");
        define("vb bas", "steel", "code", "Visual Basic");
        define("sol", "gray", "code", "Solidity");
        define("asm s", "gray", "code", "Assembly");
        define("wat", "purple", "code", "WebAssembly");
        define("vue", "green", "code", "Vue");
        define("svelte", "orange", "code", "Svelte");
        define("astro", "orange", "code", "Astro");
        define("glsl hlsl frag vert wgsl shader", "mint", "code", "Shader");
        define("cmake gradle", "green", "code", "Build script");
        define("dockerfile", "cyan", "code", "Dockerfile");
        define("makefile mk", "gray", "terminal", "Makefile");
        define("sh bash zsh fish", "green", "terminal", "Shell script");
        define("ps1 psm1 psd1", "blue", "terminal", "PowerShell");
        define("bat cmd", "gray", "terminal", "Batch file");
        define("html htm xhtml", "orange", "code", "HTML");
        define("xml xsl xslt plist xaml", "orange", "code", "XML");
        define("svg", "amber", "code", "SVG");
        define("css", "cyan", "braces", "CSS");
        define("scss sass less styl", "pink", "braces", "Stylesheet");
        define("json jsonc json5", "amber", "braces", "JSON");
        define("jsonl ndjson", "amber", "braces", "JSON Lines");
        define("yaml yml", "pink", "braces", "YAML");
        define("toml", "brown", "braces", "TOML");
        define("ini cfg conf env properties editorconfig", "gray", "braces", "Config");
        define("gitignore gitattributes gitmodules dockerignore npmrc", "gray", "braces", "Config");
        define("lock", "gray", "braces", "Lockfile");
        define("sql psql", "teal", "braces", "SQL");
        define("graphql gql", "pink", "braces", "GraphQL");
        define("proto", "blue", "braces", "Protocol Buffers");
        define("csv tsv", "green", "sheet", "Table");
        define("txt text log", "gray", "text", "Text");
        define("md mdx markdown rst adoc", "steel", "text", "Markdown");
        define("tex bib", "teal", "text", "LaTeX");
        define("rtf", "blue", "text", "Rich Text");
        define("srt vtt", "gray", "text", "Subtitles");
        define("doc docx odt pages", "blue", "text", "Document");
        define("xls xlsx xlsm ods numbers", "green", "sheet", "Spreadsheet");
        define("ppt pptx odp key", "orange", "slides", "Presentation");
        define("pdf", "red", "pdf", "PDF");
        define("epub mobi fb2", "purple", "book", "Book");
        define("png jpg jpeg jfif gif webp heic heif avif bmp tif tiff ico", "mint", "image",
               "Image");
        define("psd ai sketch fig xd", "violet", "image", "Design");
        define("mp3 wav flac aac m4a ogg opus aiff wma", "pink", "audio", "Audio");
        define("mp4 mov m4v avi mkv webm wmv flv", "purple", "video", "Video");
        define("zip rar 7z tar gz tgz bz2 xz zst", "brown", "archive", "Archive");
        define("dmg iso img", "gray", "archive", "Disk image");
        define("ttf otf woff woff2", "gray", "font", "Font");
        define("exe msi dll so dylib bin app apk ipa deb rpm wasm jar class o", "gray", "binary",
               "App");
        return out;
    }();
    return table;
}

// Each glyph's paths in the 32 × 40 view: stroked at 1.5, or filled.
struct Mark {
    const char *path;
    bool fill;
};

const QHash<QString, QVector<Mark>> &marks()
{
    static const QHash<QString, QVector<Mark>> table = {
        {"code", {{"M13.2 16.2 10.5 19l2.7 2.8M18.8 16.2l2.7 2.8-2.7 2.8M16.9 15l-1.8 8", false}}},
        {"braces",
         {{"M13.6 14.6c-1.3 0-1.9.6-1.9 1.8v.9c0 .9-.4 1.5-1.2 1.7.8.2 1.2.8 1.2 1.7v.9c0 1.2.6 "
           "1.8 1.9 1.8M18.4 14.6c1.3 0 1.9.6 1.9 1.8v.9c0 .9.4 1.5 1.2 1.7-.8.2-1.2.8-1.2 "
           "1.7v.9c0 1.2-.6 1.8-1.9 1.8",
           false}}},
        {"terminal", {{"M11.2 16l3 3-3 3M16.2 22.4h4.6", false}}},
        {"text", {{"M11 15.4h10M11 18.6h10M11 21.8h6.4", false}}},
        {"sheet", {{"M11 15h10v8H11zM11 19h10M15.4 15v8", false}}},
        {"slides", {{"M10.6 14.8h10.8v6.8H10.6zM16 21.6v2.2M13.6 23.8h4.8", false}}},
        {"pdf", {{"M11 15.4h10M11 18.6h10M11 21.8h10", false}}},
        {"image",
         {{"M10.6 23.2l3.4-4 2.4 2.5 1.9-1.9 3.1 3.4z", true},
          {"M17.5 15.9a1.5 1.5 0 1 0 3 0a1.5 1.5 0 1 0-3 0z", true}}},
        {"audio",
         {{"M14.6 22.2v-6.8l6-1.4v6.6", false},
          {"M11.5 22.2a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0-3.2 0zM17.5 20.6a1.6 1.6 0 1 0 3.2 "
           "0a1.6 1.6 0 1 0-3.2 0z",
           true}}},
        {"video",
         {{"M14 15.3c0-.6.7-1 1.2-.7l5.6 3.7c.5.3.5 1 0 1.4l-5.6 3.7c-.5.3-1.2 0-1.2-.7z", true}}},
        {"archive", {{"M16 12.6v1.4M16 15.4v1.4M16 18.2v1.4M14.7 21h2.6v2.8h-2.6z", false}}},
        {"font", {{"M11.6 23.4 15.1 14.6h1.8l3.5 8.8M12.9 20.4h6.2", false}}},
        {"binary",
         {{"M16 14l4.6 2.5v5L16 24l-4.6-2.5v-5zM11.4 16.5 16 19l4.6-2.5M16 19v5", false}}},
        {"book",
         {{"M10.8 15.4c1.9-.7 3.6-.5 5.2.7 1.6-1.2 3.3-1.4 5.2-.7v7.6c-1.9-.7-3.6-.5-5.2.7-1.6-1.2"
           "-3.3-1.4-5.2-.7zM16 16.1v7.6",
           false}}},
    };
    return table;
}

const char *const page = "M7 1.5h13.2L29.5 10.8V34a4.5 4.5 0 0 1-4.5 4.5H7A4.5 4.5 0 0 1 2.5 34V6"
                         "A4.5 4.5 0 0 1 7 1.5z";
const char *const fold = "M20.2 1.5v5.8a3.5 3.5 0 0 0 3.5 3.5h5.8z";

// color-mix(in oklab, c 70%, black): OKLab's lightness and chroma × .7.
QColor deeper(const QColor &c)
{
    const auto linear = [](qreal v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    const auto encode = [](qreal v) {
        v = std::clamp(v, 0.0, 1.0);
        return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
    };
    const qreal r = linear(c.redF()), g = linear(c.greenF()), b = linear(c.blueF());
    // Linear sRGB to LMS, cube roots; scaling Lab by .7 scales these roots by .7.
    const qreal l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b) * 0.7;
    const qreal m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b) * 0.7;
    const qreal s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b) * 0.7;
    const qreal L = l * l * l, M = m * m * m, S = s * s * s;
    return QColor::fromRgbF(encode(4.0767416621 * L - 3.3077115913 * M + 0.2309699292 * S),
                            encode(-1.2684380046 * L + 2.6097574011 * M - 0.3413193965 * S),
                            encode(-0.0041960863 * L - 0.7034186147 * M + 1.7076147010 * S));
}
} // namespace

Described describe(const QString &name)
{
    // describe(): special whole names, else the last extension.
    static const QHash<QString, QString> names = {
        {"dockerfile", "dockerfile"}, {"makefile", "makefile"}, {"gnumakefile", "makefile"},
        {"license", "txt"},           {"readme", "txt"},        {"changelog", "txt"},
        {"procfile", "conf"},         {"gemfile", "rb"},        {"rakefile", "rb"}};
    static const QHash<QString, QString> labels = {
        {"dockerfile", "DOCK"},  {"makefile", "MAKE"},
        {"gitignore", "GIT"},    {"gitattributes", "GIT"},
        {"gitmodules", "GIT"},   {"dockerignore", "DOCK"},
        {"editorconfig", "CFG"}, {"npmrc", "NPM"},
        {"c++", "C++"},          {"h++", "H++"}};
    QString base = name.toLower();
    base = base.section(u'/', -1).section(u'\\', -1);
    QString ext, label;
    if (names.contains(base)) {
        ext = names.value(base);
        label = labels.value(base, ext);
    } else if (const int dot = int(base.lastIndexOf(u'.')); dot >= 0) {
        ext = base.mid(dot + 1);
        label = labels.value(ext, ext);
    }
    const auto found = types().constFind(ext);
    const Kind kind = found != types().constEnd() ? *found : Kind{"gray", "text", ""};
    Described out;
    out.glyph = QString::fromLatin1(kind.glyph);
    label = label.toUpper().left(5);
    out.label = !label.isEmpty()        ? label
                : out.glyph == u"image" ? QStringLiteral("IMG")
                                        : QStringLiteral("FILE");
    out.tone = tone(QString::fromLatin1(kind.tone));
    static const QHash<QString, QString> kindNames = {
        {"code", "Code"},   {"braces", "Data"},       {"terminal", "Script"},
        {"text", "Text"},   {"sheet", "Spreadsheet"}, {"slides", "Presentation"},
        {"pdf", "PDF"},     {"image", "Image"},       {"audio", "Audio"},
        {"video", "Video"}, {"archive", "Archive"},   {"font", "Font"},
        {"binary", "App"},  {"book", "Book"}};
    out.kind = *kind.name       ? QString::fromLatin1(kind.name)
               : !ext.isEmpty() ? ext.toUpper() + QStringLiteral(" file")
                                : kindNames.value(out.glyph, QStringLiteral("File"));
    return out;
}

QColor tone(const QString &name)
{
    const QStringList rgb = tones().value(name, tones().value(QStringLiteral("gray"))).split(u',');
    return QColor(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt());
}

QString formatSize(double bytes)
{
    if (!std::isfinite(bytes))
        return QStringLiteral("0 B");
    if (bytes < 1000)
        return QString::number(bytes, 'g', 15) + QStringLiteral(" B");
    static const char *const units[] = {"KB", "MB", "GB"};
    double value = bytes / 1000;
    int unit = 0;
    while (value >= 1000 && unit < 2) {
        value /= 1000;
        ++unit;
    }
    QString shown;
    if (value < 10) {
        shown = QString::number(value, 'f', 1);
        if (shown.endsWith(QLatin1String(".0")))
            shown.chop(2);
    } else {
        shown = QString::number(std::round(value), 'f', 0);
    }
    return shown + QLatin1Char(' ') + QLatin1String(units[unit]);
}

namespace
{
qreal labelSize(const QString &label)
{
    return label.size() <= 3 ? 7.6 : label.size() == 4 ? 6.6 : 5.6;
}
} // namespace

void paintIcon(QPainter &painter, const QRectF &box, const Described &kind, bool light,
               const QFont &labelFont)
{
    // viewBox 0 0 32 40, fitted and centred as SVG's default does.
    const qreal scale = std::min(box.width() / 32, box.height() / 40);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(box.x() + (box.width() - 32 * scale) / 2,
                      box.y() + (box.height() - 40 * scale) / 2);
    painter.scale(scale, scale);
    const auto alpha = [](QColor c, qreal a) {
        c.setAlphaF(float(a * c.alphaF()));
        return c;
    };
    // On white the page's lines, fold, glyph and label take a deeper ink:
    // the tone 70 % toward black in OKLab (1.2's light .file-icon).
    const QColor ink = light ? deeper(kind.tone) : kind.tone;
    const QPainterPath sheet = parse(page);
    painter.fillPath(sheet, alpha(kind.tone, 0.15));
    painter.strokePath(sheet, QPen(alpha(ink, light ? 0.45 : 0.42), 1));
    painter.fillPath(parse(fold), alpha(ink, light ? 0.36 : 0.34));
    const QPen line(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    for (const Mark &mark : marks().value(kind.glyph, marks().value(QStringLiteral("text")))) {
        if (mark.fill)
            painter.fillPath(parse(mark.path), ink);
        else
            painter.strokePath(parse(mark.path), line);
    }
    // The extension at 800, centred on x 16 at baseline 33.6 (drawn large
    // and scaled: sizes are fractional).
    QFont font = labelFont;
    font.setPixelSize(100);
    // The label's outline, made once per thread, font and label.
    thread_local QHash<QString, QPainterPath> labels;
    const QString key = font.key() + QLatin1Char('\x1f') + kind.label;
    auto found = labels.constFind(key);
    if (found == labels.constEnd()) {
        if (labels.size() > 256)
            labels.clear();
        QPainterPath outline;
        outline.addText(-QFontMetricsF(font).horizontalAdvance(kind.label) / 2, 0, font,
                        kind.label);
        found = labels.insert(key, outline);
    }
    const QPainterPath &text = *found;
    painter.translate(16, 33.6);
    const qreal size = labelSize(kind.label);
    painter.scale(size / 100, size / 100);
    painter.fillPath(text, ink);
    painter.restore();
}
} // namespace filekinds
