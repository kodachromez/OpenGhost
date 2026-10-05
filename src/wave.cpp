#include "wave.h"

#include "rich.h"

#include <QAbstractTextDocumentLayout>
#include <QCache>
#include <QPainter>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGOpacityNode>
#include <QSGTextNode>
#include <QSGTransformNode>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>
#include <cmath>

namespace
{
// A Gaussian blur (σ in pixels) of a premultiplied image: CSS filter: blur().
QImage blurred(const QImage &source, qreal sigma)
{
    const int radius = int(std::ceil(3 * sigma));
    if (radius < 1)
        return source;
    std::vector<float> kernel(2 * radius + 1);
    float sum = 0;
    for (int k = -radius; k <= radius; ++k)
        sum += kernel[k + radius] = std::exp(-0.5f * float(k * k) / float(sigma * sigma));
    for (float &w : kernel)
        w /= sum;
    QImage in = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = in.width(), h = in.height();
    QImage mid(w, h, QImage::Format_ARGB32_Premultiplied), out = mid;
    const auto pass = [&](const QImage &from, QImage &to, bool across) {
        for (int y = 0; y < h; ++y) {
            auto *row = reinterpret_cast<QRgb *>(to.scanLine(y));
            for (int x = 0; x < w; ++x) {
                float a = 0, r = 0, g = 0, b = 0;
                for (int k = -radius; k <= radius; ++k) {
                    const int sx = across ? x + k : x, sy = across ? y : y + k;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                        continue;
                    const QRgb p = reinterpret_cast<const QRgb *>(from.constScanLine(sy))[sx];
                    const float f = kernel[k + radius];
                    a += f * qAlpha(p);
                    r += f * qRed(p);
                    g += f * qGreen(p);
                    b += f * qBlue(p);
                }
                row[x] = qRgba(int(r + 0.5f), int(g + 0.5f), int(b + 0.5f), int(a + 0.5f));
            }
        }
    };
    pass(in, mid, true);
    pass(mid, out, false);
    out.setDevicePixelRatio(source.devicePixelRatio());
    return out;
}

// [from, to) cut into the document's fragments, each with its format.
struct Piece {
    int from, to;
    QTextCharFormat format;
};
QVector<Piece> pieces(QTextDocument *doc, int from, int to)
{
    QVector<Piece> out;
    for (QTextBlock block = doc->findBlock(from); block.isValid() && block.position() < to;
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const int a = std::max(from, fragment.position());
            const int z = std::min(to, fragment.position() + fragment.length());
            if (z > a)
                out.push_back({a, z, fragment.charFormat()});
        }
    }
    return out;
}

bool marked(QTextDocument *doc, int position)
{
    QTextCursor cursor(doc);
    cursor.setPosition(position + 1); // The format of the character before the cursor.
    return cursor.charFormat().hasProperty(TextWave::Hidden);
}
} // namespace

TextWave::TextWave(QQuickItem *parent) : QQuickItem(parent), m_clock([this](qreal) { tick(); })
{
    setFlag(ItemHasContents);
}

TextWave::~TextWave() { m_clock.stop(); }

double TextWave::ease(double t) { return cubicBezier(0.33, 1, 0.68, 1, t); }

// md-wave animates `top` from 0.35em to the span's `auto`, which do not
// interpolate: Chromium holds the grapheme low until the eased progress
// passes half, then drops it into place.
double TextWave::rise(qreal em, double eased) { return eased < 0.5 ? Rise * em : 0; }

void TextWave::setTarget(QQuickItem *target)
{
    if (target == m_target)
        return;
    settle();
    disconnect(m_selection);
    m_target = target;
    m_started = false;
    if (target) {
        const QMetaObject *meta = target->metaObject();
        if (const int changed = meta->indexOfSignal("selectedTextChanged()"); changed >= 0)
            m_selection = connect(target, meta->method(changed), this,
                                  metaObject()->method(metaObject()->indexOfSlot("settle()")));
    }
    emit targetChanged();
}

void TextWave::setReducedMotion(bool reduced)
{
    if (reduced == m_reducedMotion)
        return;
    m_reducedMotion = reduced;
    if (reduced)
        settle();
    emit reducedMotionChanged();
}

void TextWave::setSelected(bool selected)
{
    if (selected == m_selected)
        return;
    m_selected = selected;
    if (selected)
        settle();
    emit selectedChanged();
}

QTextDocument *TextWave::document() const
{
    if (!m_target)
        return nullptr;
    auto *quick = m_target->property("textDocument").value<QQuickTextDocument *>();
    return quick ? quick->textDocument() : nullptr;
}

bool TextWave::selecting() const
{
    return m_selected || (m_target && m_target->property("selectionStart").toInt() !=
                                          m_target->property("selectionEnd").toInt());
}

void TextWave::hide(QTextCursor &cursor, int from, int to)
{
    for (Piece piece : pieces(cursor.document(), from, to)) {
        if (piece.format.hasProperty(Hidden))
            continue;
        piece.format.setProperty(Hidden, piece.format.hasProperty(QTextFormat::ForegroundBrush)
                                             ? QVariant(piece.format.foreground())
                                             : QVariant(false));
        piece.format.setForeground(QBrush(Qt::transparent));
        cursor.setPosition(piece.from);
        cursor.setPosition(piece.to, QTextCursor::KeepAnchor);
        cursor.setCharFormat(piece.format);
    }
}

void TextWave::restore(QTextCursor &cursor, int from, int to)
{
    for (Piece piece : pieces(cursor.document(), from, to)) {
        if (!piece.format.hasProperty(Hidden))
            continue;
        const QVariant was = piece.format.property(Hidden);
        if (was.metaType() == QMetaType::fromType<QBrush>())
            piece.format.setForeground(was.value<QBrush>());
        else
            piece.format.clearForeground();
        piece.format.clearProperty(Hidden);
        cursor.setPosition(piece.from);
        cursor.setPosition(piece.to, QTextCursor::KeepAnchor);
        cursor.setCharFormat(piece.format);
    }
}

void TextWave::dress(Grapheme &g, QTextDocument *doc)
{
    const QTextBlock block = doc->findBlock(g.position);
    QTextCursor at(doc);
    at.setPosition(g.position + 1);
    const QTextCharFormat format = at.charFormat();
    QFont font = format.font().resolve(doc->defaultFont());
    // The colour: a highlighter's, else what the format had, else the item's.
    QColor color;
    if (const QTextLayout *lines = block.layout()) {
        const int in = g.position - block.position();
        for (const QTextLayout::FormatRange &range : lines->formats()) {
            if (in >= range.start && in < range.start + range.length &&
                range.format.hasProperty(QTextFormat::ForegroundBrush) &&
                range.format.foreground().color().alpha() > 0)
                color = range.format.foreground().color();
        }
    }
    if (!color.isValid()) {
        const QVariant was = format.property(Hidden);
        if (was.metaType() == QMetaType::fromType<QBrush>())
            color = was.value<QBrush>().color();
        else if (!format.hasProperty(Hidden) && format.hasProperty(QTextFormat::ForegroundBrush))
            color = format.foreground().color();
        else if (m_target)
            color = m_target->property("color").value<QColor>();
    }
    g.color = color;
    g.em = font.pointSizeF() > 0 ? font.pointSizeF() / Theme::pointsFor(1)
                                 : qreal(std::max(1, font.pixelSize()));
    const QString text = block.text().mid(g.position - block.position(), g.length);
    g.layout = std::make_unique<QTextLayout>(text, font);
    g.layout->setCacheEnabled(true);
    g.layout->beginLayout();
    QTextLine line = g.layout->createLine();
    if (line.isValid()) {
        line.setLineWidth(1e6);
        line.setPosition(QPointF(0, 0));
    }
    g.layout->endLayout();
    g.ascent = line.isValid() ? line.ascent() : 0;
    // The blurred copy, wide enough for the blur's tails (3 σ). Graphemes
    // repeat, so their copies are kept (at most BlurCache bytes).
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1;
    const qreal pad = std::ceil(3 * Blur) + 1;
    const QString key = text + QLatin1Char('\x1f') + font.key() + QLatin1Char('\x1f') +
                        QString::number(color.rgba(), 16) + QLatin1Char('\x1f') +
                        QString::number(dpr);
    static QCache<QString, QImage> copies(BlurCache);
    if (const QImage *kept = copies.object(key)) {
        g.blurred = *kept;
    } else {
        const QSizeF box((line.isValid() ? line.naturalTextWidth() : 0) + 2 * pad,
                         (line.isValid() ? line.height() : 0) + 2 * pad);
        QImage sharp(QSize(int(std::ceil(box.width() * dpr)), int(std::ceil(box.height() * dpr))),
                     QImage::Format_ARGB32_Premultiplied);
        sharp.setDevicePixelRatio(dpr);
        sharp.fill(Qt::transparent);
        {
            QPainter painter(&sharp);
            painter.setPen(color);
            g.layout->draw(&painter, QPointF(pad, pad));
        }
        g.blurred = blurred(sharp, Blur * dpr);
        copies.insert(key, new QImage(g.blurred), int(g.blurred.sizeInBytes()));
    }
    g.blurRect =
        QRectF(QPointF(-pad, -pad), QSizeF(g.blurred.width() / dpr, g.blurred.height() / dpr));
    if (g.node) {
        m_dead.push_back(g.node);
        g.node = nullptr;
    }
}

void TextWave::componentComplete()
{
    QQuickItem::componentComplete();
    // Text the producer set while this was being made is its first edit.
    QTextDocument *doc = document();
    if (doc && doc->characterCount() > 1)
        grew(0);
}

void TextWave::edited(QTextCursor &cursor, int from)
{
    QTextDocument *doc = cursor.document();
    // Until its bindings are set, componentComplete() takes the first edit.
    if (!doc || !isComponentComplete())
        return;
    const int length = doc->characterCount() - 1;
    const auto before = m_flying.size();
    // Replaced from `from` on: what flew there is gone with its text.
    for (auto it = m_flying.begin(); it != m_flying.end();) {
        if (it->position + it->length > from || it->position + it->length > length) {
            if (it->node)
                m_dead.push_back(it->node);
            it = m_flying.erase(it);
        } else {
            ++it;
        }
    }
    // Those the producer styled again lost their mark: hidden again, as they look now.
    for (Grapheme &g : m_flying) {
        if (!marked(doc, g.position)) {
            dress(g, doc);
            hide(cursor, g.position, g.position + g.length);
        }
    }
    land(cursor, 0);
    const bool first = !m_started;
    m_started = true;
    if (!m_active || m_reducedMotion || selecting() || (first && !m_fresh) || from >= length) {
        if (m_flying.size() != before)
            emit countChanged();
        polish();
        update();
        return;
    }
    // The new graphemes, spaces and objects (formulas) left out.
    struct Span {
        int position, length;
    };
    std::vector<Span> spans;
    for (QTextBlock block = doc->findBlock(from); block.isValid(); block = block.next()) {
        const QString text = block.text();
        const int base = block.position();
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
        int at = std::max(0, from - base);
        finder.setPosition(at);
        if (!finder.isAtBoundary())
            at = int(finder.toPreviousBoundary());
        while (at >= 0 && at < text.size()) {
            finder.setPosition(at);
            const int next = int(finder.toNextBoundary());
            if (next <= at)
                break;
            const QStringView piece = QStringView(text).mid(at, next - at);
            const bool blank = std::all_of(piece.begin(), piece.end(), [](QChar c) {
                return c.isSpace() || c == QChar::ObjectReplacementCharacter;
            });
            if (!blank && base + at >= from)
                spans.push_back({base + at, next - at});
            at = next;
        }
    }
    // Past the newest Most, they show at once (stagger()).
    const std::size_t skip = spans.size() > std::size_t(Most) ? spans.size() - Most : 0;
    const std::size_t count = spans.size() - skip;
    const double now = FrameClock::now();
    const double frame = FrameClock::interval() > 0 ? FrameClock::interval() : 1.0 / 60;
    const double spread = std::min(frame, Spread);
    for (std::size_t k = skip; k < spans.size(); ++k) {
        Grapheme g;
        g.position = spans[k].position;
        g.length = spans[k].length;
        // The earlier of a frame's graphemes started earlier within it.
        g.start = now - spread * (1 - double(k - skip + 1) / double(count));
        dress(g, doc);
        hide(cursor, g.position, g.position + g.length);
        m_flying.push_back(std::move(g));
    }
    if (!m_flying.empty() && m_clock.state() != QAbstractAnimation::Running)
        m_clock.start();
    if (m_flying.size() != before)
        emit countChanged();
    polish();
    update();
}

void TextWave::grew(int from)
{
    QTextDocument *doc = document();
    if (!doc)
        return;
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    edited(cursor, from);
    cursor.endEditBlock();
}

void TextWave::land(QTextCursor &cursor, double after)
{
    const double now = FrameClock::now();
    for (auto it = m_flying.begin(); it != m_flying.end();) {
        if (now - it->start >= Duration + after) {
            restore(cursor, it->position, it->position + it->length);
            if (it->node)
                m_dead.push_back(it->node);
            it = m_flying.erase(it);
        } else {
            ++it;
        }
    }
}

void TextWave::settle()
{
    if (m_flying.empty())
        return;
    if (QTextDocument *doc = document()) {
        QTextCursor cursor(doc);
        cursor.beginEditBlock();
        for (const Grapheme &g : m_flying)
            restore(cursor, g.position, g.position + g.length);
        cursor.endEditBlock();
    }
    dropAll();
    emit countChanged();
}

void TextWave::dropAll()
{
    for (Grapheme &g : m_flying) {
        if (g.node)
            m_dead.push_back(g.node);
    }
    m_flying.clear();
    m_clock.stop();
    update();
}

void TextWave::tick()
{
    if (m_flying.empty()) {
        m_clock.stop();
        return;
    }
    // A grapheme drawn here at rest looks as the text does: the text gets
    // its colour back with the next edit, or here once it has rested a while.
    const double now = FrameClock::now();
    const bool rested = std::any_of(m_flying.begin(), m_flying.end(), [now](const Grapheme &g) {
        return now - g.start >= Duration + 0.1;
    });
    if (rested) {
        if (QTextDocument *doc = document()) {
            const auto before = m_flying.size();
            QTextCursor cursor(doc);
            cursor.beginEditBlock();
            land(cursor, 0.1);
            cursor.endEditBlock();
            if (m_flying.size() != before)
                emit countChanged();
        }
    }
    polish();
    update();
}

void TextWave::place()
{
    QTextDocument *doc = document();
    if (!doc || m_flying.empty())
        return;
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    // The text item draws its document offset by its padding and alignment:
    // measured once, from where it puts the first grapheme.
    QPointF offset;
    bool calibrated = false;
    for (Grapheme &g : m_flying) {
        const QTextBlock block = doc->findBlock(g.position);
        const QTextLayout *lines = block.layout();
        if (!lines)
            continue;
        const int in = g.position - block.position();
        const QTextLine line = lines->lineForTextPosition(in);
        if (!line.isValid())
            continue;
        const QRectF box = layout->blockBoundingRect(block);
        const QPointF pen(box.x() + line.cursorToX(in), box.y() + line.y());
        if (!calibrated) {
            QRectF shown;
            QMetaObject::invokeMethod(m_target, "positionToRectangle", Q_RETURN_ARG(QRectF, shown),
                                      Q_ARG(int, g.position));
            offset = shown.topLeft() - pen;
            calibrated = true;
        }
        g.origin = pen + offset + QPointF(0, line.ascent() - g.ascent);
    }
}

QVariantMap TextWave::at(int position) const
{
    const double now = FrameClock::now();
    for (const Grapheme &g : m_flying) {
        if (position < g.position || position >= g.position + g.length)
            continue;
        const double t = now - g.start;
        const double e = ease(std::clamp(t / Duration, 0.0, 1.0));
        const double f = ease(std::clamp(t / Focus, 0.0, 1.0));
        return {{"rise", rise(g.em, e)}, {"opacity", e},      {"blur", Blur * (1 - f)},
                {"x", g.origin.x()},     {"y", g.origin.y()}, {"color", g.color}};
    }
    return {};
}

void TextWave::updatePolish() { place(); }

QSGNode *TextWave::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    QSGNode *root = old;
    if (!root) {
        // A new scene graph: every grapheme needs its nodes again.
        root = new QSGNode;
        m_dead.clear();
        for (Grapheme &g : m_flying)
            g.node = nullptr;
    }
    for (QSGTransformNode *node : m_dead) {
        root->removeChildNode(node);
        delete node;
    }
    m_dead.clear();
    QQuickWindow *view = window();
    if (!view)
        return root;
    const double now = FrameClock::now();
    const int renderType = m_target ? m_target->property("renderType").toInt() : 0;
    for (Grapheme &g : m_flying) {
        if (!g.node) {
            g.node = new QSGTransformNode;
            g.soft = new QSGOpacityNode;
            g.sharp = new QSGOpacityNode;
            if (!g.blurred.isNull()) {
                QSGImageNode *image = view->createImageNode();
                image->setTexture(view->createTextureFromImage(g.blurred));
                image->setOwnsTexture(true);
                image->setRect(g.blurRect);
                image->setFiltering(QSGTexture::Linear);
                g.soft->appendChildNode(image);
                g.blurred = QImage();
            }
            QSGTextNode *text = view->createTextNode();
            text->setRenderType(QSGTextNode::RenderType(renderType));
            text->setColor(g.color);
            text->addTextLayout(QPointF(0, 0), g.layout.get());
            g.sharp->appendChildNode(text);
            g.node->appendChildNode(g.soft);
            g.node->appendChildNode(g.sharp);
            root->appendChildNode(g.node);
        }
        const double t = now - g.start;
        const double e = ease(std::clamp(t / Duration, 0.0, 1.0));
        const double soft = 1 - ease(std::clamp(t / Focus, 0.0, 1.0));
        QMatrix4x4 matrix;
        matrix.translate(float(g.origin.x()), float(g.origin.y() + rise(g.em, e)));
        g.node->setMatrix(matrix);
        g.soft->setOpacity(e * soft);
        g.sharp->setOpacity(e * (1 - soft));
    }
    return root;
}

void TextWave::releaseResources()
{
    // The nodes go with the scene graph; new ones are made if it returns.
    m_dead.clear();
    for (Grapheme &g : m_flying)
        g.node = nullptr;
}
