#include "diagram.h"

#include "diagram_engine.h"
#include "diagram_paint.h"

#include <QPainter>

// The engine's public face: compile, where a drawing stands (viewSpec),
// the settled drawing painted, and its texts.
namespace diagram
{
int sectionOf(int themeTone)
{
    // theme::tones order (lilac, turquoise, blue, pink, yellow, orange, green)
    // to SECTION's (blue, orange, turquoise, lilac, yellow, pink, green).
    static const int sections[] = {3, 2, 0, 5, 4, 1, 6};
    return themeTone >= 0 && themeTone < 7 ? sections[themeTone] : 0;
}

namespace
{
CompileOptions compileOptions(const QString &kind, const Options &options)
{
    CompileOptions out;
    out.width = options.width;
    out.column = options.column;
    out.zoom = options.zoom;
    out.kind = kind;
    out.hints = options.hints;
    out.sideways = options.sideways;
    out.family = options.family;
    out.mono = options.mono;
    out.cancel = options.cancel;
    return out;
}

// tonesOf: the series from the section's own colour on, round the ring.
Tones tonesFor(int section)
{
    const Tones all = series();
    const int k = std::clamp(section, 0, int(all.size()) - 1);
    return all.mid(k) + all.mid(0, k);
}
} // namespace

Compiled compile(const QString &source, const QString &kind, const Options &options)
{
    Compiled out;
    if (source.size() > MaxSource) {
        out.error =
            QStringLiteral("The diagram source is longer than %1 characters.").arg(MaxSource);
        return out;
    }
    Ctx c(compileOptions(kind, options));
    c.tones = tonesFor(options.section);
    if (c.cancelled()) {
        out.error = QStringLiteral("Drawing was cancelled.");
        return out;
    }
    auto result = compileResult(c, source);
    if (!result) {
        out.error = c.error.isEmpty() ? QStringLiteral("This diagram could not be read.") : c.error;
        return out;
    }
    out.result = std::make_shared<const Result>(std::move(*result));
    return out;
}

View viewOf(const Result &result, double room, double columnLeft, double columnWidth)
{
    room = std::max(260.0, finite(room) ? room : 640.0);
    const double zoom = result.zoom > 0 ? result.zoom : 1;
    const bool flush = result.flush;
    const double padX = flush ? 0 : PAD, padY = flush ? PAD_FLUSH : PAD;
    const double natural = result.width * zoom;
    const double s =
        std::max(MIN_SCALE, std::min(1.0, natural > 0 ? (room - padX * 2) / natural : 1.0)) * zoom;
    const double cw = result.width * s, ch = result.height * s;
    const double w = std::max(room, cw + padX * 2);
    const double ox = flush && cw <= columnWidth + 1 ? columnLeft : std::max(padX, (w - cw) / 2);
    double band = 0, tx = ox + cw + TOOLS.gap, ty = padY - 3;
    if (tx + TOOLS.width > room) {
        // Too wide for its stage: the tools keep to the corner that stays in
        // view, into the free corner of its first line or onto a band above.
        const auto &corner = result.corner;
        const double edge = std::min(ox + cw, room);
        tx = edge - TOOLS.width - (corner ? corner->inset : 0) * s;
        if (corner && edge - ox - TOOLS.width >= corner->x * s &&
            corner->h * s + padY >= TOOLS.height + 2)
            ty = std::max(2.0, padY + (corner->h * s - TOOLS.height) / 2);
        else {
            band = TOOLS.height + 12 - padY;
            ty = 4;
        }
    }
    View v;
    v.ox = ox;
    v.s = s;
    v.cw = cw;
    v.top = padY + band;
    v.h = ch + padY * 2 + band;
    v.w = w;
    v.tx = tx;
    v.ty = ty;
    return v;
}

double margin(const Result &result) { return result.flush ? PAD_FLUSH : PAD; }

Rendered render(const QString &source, const QString &kind, const Options &options)
{
    Rendered out;
    const Compiled compiled = compile(source, kind, options);
    if (!compiled.result) {
        out.error = compiled.error;
        return out;
    }
    const Result &result = *compiled.result;
    const double room = std::max(260.0, finite(options.width) ? options.width : 640.0);
    const double columnWidth =
        options.column > 0 ? std::min(room, std::max(260.0, options.column)) : room;
    const View v = viewOf(result, room, (room - columnWidth) / 2, columnWidth);
    const double m = margin(result);
    const double x0 = std::max(0.0, v.ox - m), x1 = std::min(v.w, v.ox + v.cw + m);
    const double w = std::ceil(std::max(1.0, x1 - x0)), h = std::ceil(std::max(1.0, v.h));
    if (w > MaxWidth || h > MaxHeight) {
        out.error = QStringLiteral("This diagram is too large to draw.");
        return out;
    }
    double dpr = finite(options.dpr) && options.dpr > 0 ? std::min(options.dpr, 8.0) : 1.0;
    if (w * h * dpr * dpr > MaxPixels)
        dpr = std::sqrt(MaxPixels / (w * h));
    QImage image(int(std::ceil(w * dpr)), int(std::ceil(h * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        out.error = QStringLiteral("This diagram is too large to draw.");
        return out;
    }
    image.setDotsPerMeterX(2835);
    image.setDotsPerMeterY(2835);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QVector<Live> live;
    live.reserve(result.items.size());
    for (const Spec &spec : result.items) {
        if (spec.type == Type::View)
            continue;
        Live item;
        item.spec = spec;
        item.cur = spec.props;
        live << item;
    }
    QVector<const Live *> items;
    for (const Live &item : live)
        items << &item;
    {
        CompileOptions fonts;
        fonts.family = options.family;
        fonts.mono = options.mono;
        Ctx c(fonts);
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                               QPainter::SmoothPixmapTransform);
        // Text boxes in stage coordinates: painted from the image's left.
        painter.translate(-x0, 0);
        painter.translate(v.ox, v.top);
        painter.scale(v.s, v.s);
        QVector<QRectF> texts;
        paintItems(painter, c, items, Look(), result.kind, &texts);
        for (const QRectF &r : texts)
            out.texts << r;
    }
    if (options.cancel && options.cancel->load(std::memory_order_relaxed)) {
        out.error = QStringLiteral("Drawing was cancelled.");
        return out;
    }
    out.image = image;
    out.size = QSizeF(v.w, v.h);
    out.left = x0;
    out.kind = result.kind;
    out.view = v;
    out.result = compiled.result;
    out.ok = true;
    return out;
}

Texts texts(const Result &result)
{
    Texts out;
    const auto add = [&out](const QStringList &lines) {
        if (!lines.isEmpty())
            out.texts << lines.join(QString());
    };
    const auto one = [&out](const QString &text) {
        if (!text.isEmpty())
            out.texts << text;
    };
    // Each item's <text>s as diagram.js's TYPES create them, layer by layer
    // as the scene's <g> layers hold them.
    for (int layer = 0; layer < Layers; ++layer) {
        for (const Spec &spec : result.items) {
            // A group's name is made in the labels layer.
            const int in = spec.type == Type::Cluster ? int(Labels) : layerOf(spec);
            if (in != layer)
                continue;
            const Fixed &fx = spec.fixed;
            switch (spec.type) {
            case Type::Node:
                if (fx.card) {
                    if (!fx.card->sub.isEmpty())
                        one(fx.card->sub);
                    one(fx.card->title);
                    for (const auto &row : fx.card->rows) {
                        one(row.lead);
                        one(row.text);
                        one(row.meta);
                    }
                } else {
                    add(fx.lines);
                }
                break;
            case Type::Label:
            case Type::Note:
                add(fx.lines);
                break;
            case Type::Legend:
                one(fx.label);
                one(fx.value);
                one(fx.share);
                break;
            case Type::Badge:
                one(fx.n);
                break;
            case Type::Frame:
                add({fx.kind, fx.label});
                break;
            case Type::Cluster:
                one(fx.title);
                break;
            case Type::Chip:
                one(fx.text);
                break;
            case Type::Figure:
                add({fx.figure, fx.unit});
                break;
            case Type::FHead:
                one(fx.name);
                one(fx.path);
                one(fx.counts);
                one(fx.sizeText);
                break;
            case Type::FRow:
                // A file's icon holds its extension, an SVG text of its own.
                if (!fx.folder)
                    one(describeFile(fx.name).label);
                one(fx.label);
                one(fx.note);
                one(fx.when);
                one(fx.amount);
                break;
            default:
                break;
            }
        }
    }
    out.ok = true;
    return out;
}

Texts texts(const QString &source, const QString &kind, const Options &options)
{
    const Compiled compiled = compile(source, kind, options);
    return compiled.result ? texts(*compiled.result) : Texts();
}
} // namespace diagram
