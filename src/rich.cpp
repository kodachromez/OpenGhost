#include "rich.h"
#include "platform/platform.h"

#include "cssfont.h"
#include "ghost.h"
#include "highlight.h"
#include "icon.h"
#include "media.h"
#include "theme.h"
#include "presentation.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QCache>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImageReader>
#include <QMutex>
#include <QPainter>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QScreen>
#include <QSet>
#include <QStyleHints>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QThreadPool>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <deque>

namespace
{
// Rich-content work (parses, drawings, preview decodes) runs on two worker
// threads, never on the GUI thread, and its outstanding amount is bounded
// (audit P5-03). An owner asks for work; until a worker is free the queue
// holds only the owner and how to make its job, at most once per owner (a
// preview decode excepted), and the job takes the owner's inputs when it
// starts: replaced inputs are never queued, and a destroyed owner's request
// is dropped. At most Workers jobs run or wait to answer, so their inputs and
// results are all the payload held. A job gets a flag its owner sets when
// the result can no longer be used; expensive steps check it and stop early.
// Its answer runs on the GUI thread unless the owner was destroyed. Exit
// drops what is queued, stops what runs and waits only for that.
class Work final : public QObject
{
  public:
    using Cancel = std::shared_ptr<std::atomic_bool>;
    // Runs on a worker; what it returns runs back here.
    using Job = std::function<std::function<void()>()>;
    // Runs here when a worker is free; no job when nothing is wanted now.
    using Make = std::function<Job(const Cancel &)>;
    static constexpr int Workers = 2;

    Work() : QObject(QCoreApplication::instance())
    {
        m_pool.setMaxThreadCount(Workers);
        m_pool.setObjectName(QStringLiteral("rich"));
    }
    ~Work() override { stop(); }

    // `coalesce`: replaces the owner's request still waiting.
    void want(QObject *owner, Make make, bool coalesce = true)
    {
        if (coalesce) {
            for (Wanted &wanted : m_queue) {
                if (wanted.coalesce && wanted.owner == owner) {
                    wanted.make = std::move(make);
                    return;
                }
            }
        }
        m_queue.push_back({owner, std::move(make), coalesce});
        dispatch();
    }
    // The owner's running jobs are told to stop; its answers still run.
    void cancel(QObject *owner)
    {
        for (const Flight &flight : m_flights)
            if (flight.owner == owner)
                *flight.cancel = true;
    }
    // A destroyed owner: nothing of it waits, and what runs stops.
    void forget(QObject *owner)
    {
        cancel(owner);
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                     [owner](const Wanted &w) { return w.owner == owner; }),
                      m_queue.end());
    }
    void stop()
    {
        m_queue.clear();
        for (const Flight &flight : m_flights)
            *flight.cancel = true;
        m_pool.waitForDone();
    }
    RichWork stats() const { return {int(m_queue.size()), int(m_flights.size()), m_started}; }
    std::function<void()> beforeJob; // Tests: runs on the worker before each job.

  private:
    struct Wanted {
        QPointer<QObject> owner;
        Make make;
        bool coalesce = true;
    };
    struct Flight {
        const QObject *owner;
        Cancel cancel;
    };

    void dispatch()
    {
        while (m_flights.size() < Workers && !m_queue.empty()) {
            Wanted next = std::move(m_queue.front());
            m_queue.pop_front();
            if (!next.owner)
                continue;
            // The worker is taken first: making the job may ask for more.
            auto cancel = std::make_shared<std::atomic_bool>(false);
            m_flights.push_back({next.owner.data(), cancel});
            Job job = next.make(cancel);
            if (!job) {
                land(cancel);
                continue;
            }
            ++m_started;
            m_pool.start([this, guard = next.owner, cancel, job = std::move(job),
                          before = beforeJob]() mutable {
                if (before)
                    before();
                auto answer = job();
                job = {};
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, cancel, answer = std::move(answer)] {
                        land(cancel);
                        if (guard)
                            answer();
                        dispatch();
                    },
                    Qt::QueuedConnection);
            });
        }
    }
    void land(const Cancel &cancel)
    {
        m_flights.erase(std::find_if(m_flights.begin(), m_flights.end(),
                                     [&cancel](const Flight &f) { return f.cancel == cancel; }));
    }

    QThreadPool m_pool;
    std::deque<Wanted> m_queue;
    std::vector<Flight> m_flights;
    quint64 m_started = 0;
};

// Owned by the application; made again if asked for after it is gone.
Work &work()
{
    static QPointer<Work> instance;
    if (!instance)
        instance = new Work;
    return *instance;
}

const char *const kindNames[] = {"paragraph", "heading", "code", "diagram", "math", "table",
                                 "rule",      "flow",    "list", "quote",   "media"};

quint64 mix(quint64 h, quint64 v) { return (h ^ v) * 1099511628211ull + 0x9e3779b97f4a7c15ull; }

// A formula's TeX, kept on its image so a copied selection gives the source.
constexpr int TexProperty = QTextFormat::UserProperty + 1;
} // namespace

// ---- BlockModel --------------------------------------------------------

BlockModel::BlockModel(RichDocument *document, Container container, int depth, QObject *parent)
    : QAbstractListModel(parent), m_document(document), m_container(container), m_depth(depth)
{
}

int BlockModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QString BlockModel::kindOf(const Node &node)
{
    return node.item >= 0 ? QStringLiteral("item")
                          : QString::fromLatin1(kindNames[int(node.block->kind)]);
}

quint64 BlockModel::hashOf(const Node &node)
{
    if (node.item < 0)
        return node.block->hash;
    const markdown::Item &item = node.block->items.at(node.item);
    quint64 h = mix(quint64(item.task + 3), quint64(node.block->level + node.item));
    h = mix(h, node.block->flag);
    for (const auto &child : item.blocks)
        h = mix(h, child->hash);
    return h;
}

QVector<BlockModel::Node> BlockModel::childNodes(const Node &node) const
{
    QVector<Node> out;
    if (node.item >= 0) {
        for (const auto &child : node.block->items.at(node.item).blocks)
            out << Node{child, -1};
    } else if (node.block->kind == markdown::Kind::Quote) {
        for (const auto &child : node.block->children)
            out << Node{child, -1};
    } else if (node.block->kind == markdown::Kind::List) {
        for (int k = 0; k < node.block->items.size(); ++k)
            out << Node{node.block, k};
    }
    return out;
}

void BlockModel::apply(const QVector<Node> &nodes, const QString &path)
{
    m_path = path;
    // Rows keep their place while their kind does; from the first row whose
    // kind changed, the tail is replaced.
    int same = 0;
    while (same < m_rows.size() && same < nodes.size() &&
           kindOf(m_rows.at(same).node) == kindOf(nodes.at(same)))
        ++same;
    for (int i = 0; i < same; ++i) {
        Row &row = m_rows[i];
        const Node &next = nodes.at(i);
        const bool changed = row.node.block != next.block || row.node.item != next.item;
        row.node = next;
        const quint64 hash = hashOf(next);
        if (!changed && hash == row.hash)
            continue;
        const bool content = hash != row.hash;
        row.hash = hash;
        if (row.children)
            row.children->apply(childNodes(next), path + QLatin1Char('/') + QString::number(i));
        if (content)
            emit dataChanged(index(i), index(i));
    }
    if (same < m_rows.size()) {
        beginRemoveRows({}, same, int(m_rows.size()) - 1);
        for (int i = same; i < m_rows.size(); ++i)
            delete m_rows.at(i).children;
        m_rows.resize(same);
        endRemoveRows();
    }
    if (same < nodes.size()) {
        beginInsertRows({}, same, int(nodes.size()) - 1);
        for (int i = same; i < nodes.size(); ++i) {
            Row row;
            row.node = nodes.at(i);
            row.hash = hashOf(row.node);
            row.born = QDateTime::currentMSecsSinceEpoch();
            const QString kind = kindOf(row.node);
            if (kind == QLatin1String("quote") || kind == QLatin1String("list") ||
                kind == QLatin1String("item")) {
                const int depth = kind == QLatin1String("item") ? m_depth + 1 : m_depth;
                const Container holds = kind == QLatin1String("quote")  ? Container::Quote
                                        : kind == QLatin1String("list") ? Container::List
                                                                        : Container::Item;
                row.children = new BlockModel(m_document, holds, depth, this);
                row.children->apply(childNodes(row.node),
                                    path + QLatin1Char('/') + QString::number(i));
            }
            m_rows << row;
        }
        endInsertRows();
    }
}

// markdown.js's CSS margins, collapsed: the space above a row.
int BlockModel::gap(int row) const
{
    if (row <= 0) {
        // The later .markdown > .md-diagram.md-wide rule outranks
        // .markdown > :first-child: even the first wide diagram has 6 px.
        if (row == 0 && m_container == Container::Top && !m_rows.isEmpty()) {
            const auto &block = *m_rows.first().node.block;
            if (block.kind == markdown::Kind::Diagram && block.flag)
                return 6;
        }
        return 0;
    }
    switch (m_container) {
    case Container::Plain:
        return 0;
    case Container::Quote:
        return 8;
    case Container::List:
    case Container::Item:
        return 6;
    case Container::Top:
        break;
    }
    auto margins = [](const markdown::Block &b) -> std::pair<int, int> {
        switch (b.kind) {
        case markdown::Kind::Heading:
            return {b.level >= 4 ? 22 : 28, 12};
        case markdown::Kind::Math:
            return {16, 18};
        case markdown::Kind::Diagram:
            return b.flag ? std::pair<int, int>{6, 22} : std::pair<int, int>{0, 14};
        case markdown::Kind::Rule:
            return {26, 0};
        case markdown::Kind::Media: // .md-media; one with nothing yet is not shown.
            return b.media.isEmpty() ? std::pair<int, int>{0, 0} : std::pair<int, int>{4, 18};
        default:
            return {0, 14};
        }
    };
    return std::max(margins(*m_rows.at(row - 1).node.block).second,
                    margins(*m_rows.at(row).node.block).first);
}

RichInline BlockModel::rich(const markdown::Inline &text) const
{
    RichInline value;
    value.text = text;
    for (const QString &source : text.math) {
        const auto found = m_document->math().constFind(source);
        const bool ok = found != m_document->math().constEnd() && found->ok;
        value.math << (ok ? found->image : QImage());
        value.baselines << (ok ? found->baseline : 0);
    }
    return value;
}

void BlockModel::retone()
{
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(int(m_rows.size()) - 1), {ToneRole, PaletteRole});
    for (const Row &row : std::as_const(m_rows))
        if (row.children)
            row.children->retone();
}

void BlockModel::units(QVector<SelectionUnit> &out, const QString &prefix,
                       const QVector<bool> *breaks) const
{
    const auto add = [&out](QString path, const markdown::Inline &text, char after) {
        SelectionUnit unit;
        unit.path = std::move(path);
        unit.text = text;
        unit.after = after;
        out << std::move(unit);
    };
    const auto plain = [](const QString &text) {
        markdown::Inline inline_;
        inline_.text = text;
        return inline_;
    };
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        const markdown::Block &b = *row.node.block;
        const QString path = prefix + m_path + QLatin1Char('/') + QString::number(i);
        if (row.node.item >= 0) { // A list item: its blocks.
            if (row.children)
                row.children->units(out, prefix);
            continue;
        }
        switch (b.kind) {
        case markdown::Kind::Paragraph:
            add(path + QStringLiteral(":t"), b.text,
                !breaks ? 'p' : breaks->value(i) ? 'l' : char(0));
            break;
        case markdown::Kind::Heading:
            add(path + QStringLiteral(":t"), b.text, 'l');
            break;
        case markdown::Kind::Code:
            add(path + QStringLiteral(":l"), plain(b.lang), 'p');
            add(path + QStringLiteral(":s"), plain(b.source), 'l');
            break;
        case markdown::Kind::Math: {
            SelectionUnit unit;
            unit.path = path + QStringLiteral(":m");
            unit.text.text = QString(markdown::Object);
            unit.text.runs = {markdown::Run{0, 1, markdown::Math, 0}};
            unit.text.math = {b.source};
            unit.display = true;
            out << std::move(unit);
            break;
        }
        case markdown::Kind::Table:
            for (int k = 0; k < b.cells.size(); ++k) {
                const int column = b.columns > 0 ? k % b.columns : 0;
                add(path + QStringLiteral(":c") + QString::number(b.columns > 0 ? k / b.columns : 0) +
                        QLatin1Char('.') + QString::number(column),
                    b.cells.at(k), column + 1 < b.columns ? 't' : 'l');
            }
            break;
        case markdown::Kind::Flow:
            for (int k = 0; k < b.cells.size(); ++k)
                add(path + QStringLiteral(":f") + QString::number(k), b.cells.at(k), 'l');
            break;
        case markdown::Kind::List:
            if (row.children)
                row.children->units(out, prefix);
            break;
        case markdown::Kind::Quote: {
            // A callout's title shows its kind in capitals; its text (copied) is
            // OpenGhost's "Note", "Tip", ….
            if (!b.lang.isEmpty()) {
                QString title = b.lang.toLower();
                if (!title.isEmpty())
                    title[0] = title.at(0).toUpper();
                SelectionUnit unit;
                unit.path = path + QStringLiteral(":h");
                unit.text.text = title.toUpper();
                unit.copy = title;
                out << std::move(unit);
            }
            if (row.children)
                row.children->units(out, prefix);
            if (b.flag && !b.text.text.isEmpty()) { // A pull quote's citation.
                add(path + QStringLiteral(":n"), b.text, 'l');
                if (!b.role.text.isEmpty())
                    add(path + QStringLiteral(":r"), b.role, 'l');
            }
            break;
        }
        case markdown::Kind::Diagram: {
            // Its texts, as drawn (selection.cpp copies them).
            SelectionUnit unit;
            unit.path = path + QStringLiteral(":g");
            unit.text.text = QString(markdown::Object);
            unit.diagram = b.source;
            unit.lang = b.lang;
            unit.after = 'd';
            out << std::move(unit);
            break;
        }
        case markdown::Kind::Rule:
        case markdown::Kind::Media: // Pictures and cards, not text (data-static).
            break;
        }
    }
}

QVariant BlockModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    const markdown::Block &b = *row.node.block;
    const bool item = row.node.item >= 0;
    switch (role) {
    case KindRole:
        return kindOf(row.node);
    case TextRole:
        return item ? QVariant() : QVariant::fromValue(rich(b.text));
    case RoleRole:
        return item ? QVariant() : QVariant::fromValue(rich(b.role));
    case SourceRole:
        return b.source;
    case LangRole:
        return b.lang;
    case LevelRole:
        return b.level;
    case FlagRole:
        return b.flag;
    case OpenRole:
        return b.open;
    case ToneRole:
        return m_document->tone(b.tone);
    case SectionRole:
        return b.tone >= 0 && b.tone < theme::ToneCount ? b.tone : -1;
    case TokensRole:
        return QVariant::fromValue(b.tokens);
    case CellsRole: {
        QVariantList cells;
        cells.reserve(b.cells.size());
        for (const auto &cell : b.cells)
            cells << QVariant::fromValue(rich(cell));
        return cells;
    }
    case ColumnsRole:
        return b.columns;
    case AlignRole: {
        QVariantList align;
        for (quint8 a : b.align)
            align << int(a);
        return align;
    }
    case PaletteRole:
        return m_document->palette(b.headingIndex);
    case ChildrenRole:
        return QVariant::fromValue<QObject *>(row.children);
    case TaskRole:
        return item ? b.items.at(row.node.item).task : -1;
    case NumberRole:
        return item ? b.level + row.node.item : 0;
    case DepthRole:
        return m_depth;
    case PathRole:
        return m_path + QLatin1Char('/') + QString::number(index.row());
    case GapRole:
        return gap(index.row());
    case OmittedRole:
        return item ? 0 : b.omitted;
    case ClippedRole:
        return !item && b.clipped;
    case BornRole:
        return double(row.born);
    case MediaRole: {
        // Each picture or video with what its presentation needs: the
        // link chip's label, a video's id and the words of its link.
        QVariantList items;
        if (item)
            return items;
        for (const markdown::MediaItem &m : b.media) {
            const QString link = m.video ? m.src : m.href.isEmpty() ? m.src : m.href;
            QVariantMap value{{QStringLiteral("video"), m.video},
                              {QStringLiteral("src"), m.src},
                              {QStringLiteral("caption"), m.caption},
                              {QStringLiteral("href"), m.href},
                              {QStringLiteral("link"), link},
                              {QStringLiteral("label"), markdown::linkLabel(link)},
                              {QStringLiteral("host"), media::hostOf(m.src)}};
            if (m.video) {
                const media::VideoWords words = media::videoWords(m.caption, m.src);
                value.insert(QStringLiteral("id"), media::videoId(m.src));
                value.insert(QStringLiteral("title"), words.title);
                value.insert(QStringLiteral("by"), words.by);
                value.insert(QStringLiteral("time"), words.time);
            }
            items << value;
        }
        return items;
    }
    default:
        return {};
    }
}

namespace
{
// Estimates of a block's height before its components exist. They only need
// to be close (a built block keeps its measured height): average character
// widths and line boxes of the QML components' fonts, their paddings and gaps.
struct Metrics {
    qreal prose = 8;   // Average character width at 16 px.
    qreal code = 21.6; // Line height of 13.5 px code (1.6).
};

const Metrics &metrics()
{
    static const Metrics measured = [] {
        QFont prose = QGuiApplication::font();
        prose.setPointSizeF(Theme::pointsFor(16));
        prose.setWeight(QFont::Weight(Theme::cssWeight(prose.family(), QFont::DemiBold)));
        return Metrics{QFontMetricsF(prose).averageCharWidth(), 13.5 * 1.6};
    }();
    return measured;
}

// Wrapped lines of `text` at `width` for characters `advance` wide.
int wrappedLines(const QString &text, qreal width, qreal advance)
{
    const qsizetype perLine =
        std::max<qsizetype>(1, qsizetype(width / std::max<qreal>(1, advance)));
    int lines = 0;
    for (qsizetype start = 0;;) {
        const qsizetype end = text.indexOf(QLatin1Char('\n'), start);
        const qsizetype length = (end < 0 ? text.size() : end) - start;
        lines += int(std::max<qsizetype>(1, (length + perLine - 1) / perLine));
        if (end < 0)
            return lines;
        start = end + 1;
    }
}

qreal estimateBlock(const markdown::Block &b, qreal width);

qreal estimateBlocks(const markdown::Blocks &blocks, qreal width, qreal gap)
{
    qreal height = 0;
    for (qsizetype k = 0; k < blocks.size(); ++k)
        height += (k ? gap : 0) + estimateBlock(*blocks.at(k), width);
    return height;
}

qreal estimateBlock(const markdown::Block &b, qreal width)
{
    using markdown::Kind;
    const Metrics &m = metrics();
    switch (b.kind) {
    case Kind::Paragraph:
        return wrappedLines(b.text.text, width, m.prose) * 16 * 1.65;
    case Kind::Heading: {
        static const qreal sizes[] = {24, 20.8, 18.24, 16.32, 16.32, 16.32};
        const qreal size = sizes[std::clamp(b.level, 1, 6) - 1];
        return wrappedLines(b.text.text, width - size * 0.84, m.prose * size / 16) * size * 1.32 +
               size * 0.18;
    }
    case Kind::Code:
        return 38 + (b.source.count(QLatin1Char('\n')) + 1) * m.code + (b.flag ? 18 : 14);
    case Kind::Diagram:
        return 320;
    case Kind::Math:
        return 64;
    case Kind::Table: {
        if (b.columns <= 0)
            return 0;
        const qreal column = std::max<qreal>(40, width / b.columns - 32);
        qreal height = 0;
        for (qsizetype r = 0; r < b.cells.size() / b.columns; ++r) {
            int lines = 1;
            for (int c = 0; c < b.columns; ++c)
                lines = std::max(lines, wrappedLines(b.cells.at(r * b.columns + c).text, column,
                                                     m.prose * 14.5 / 16));
            height += r ? 24 + lines * 14.5 * 1.5 : 12 + lines * 12.5 * 1.5;
        }
        return height;
    }
    case Kind::Rule:
        return 0;
    case Kind::Flow:
        return 30;
    case Kind::Media: {
        // A plate for the pictures, a card per video (two to a row when many).
        qreal height = 0;
        int videos = 0;
        bool pictures = false;
        for (const markdown::MediaItem &item : b.media) {
            videos += item.video;
            pictures = pictures || !item.video;
        }
        if (pictures)
            height += std::min<qreal>(media::FrameWidth, width) * 3 / 4 + 30;
        if (videos) {
            const bool many = videos > 1;
            const qreal card =
                many ? std::max<qreal>(170, width / 2 - 7) : std::min<qreal>(380, width);
            const int rows = many ? (videos + 1) / 2 : 1;
            height += (pictures ? 18 : 0) + rows * (card * 9 / 16 + 56) + (rows - 1) * 20;
        }
        return height;
    }
    case Kind::List: {
        const qreal indent = b.flag ? 32 : 22;
        qreal height = 0;
        for (qsizetype k = 0; k < b.items.size(); ++k)
            height += (k ? 6 : 0) +
                      std::max(16 * 1.65, estimateBlocks(b.items.at(k).blocks, width - indent, 6));
        return height;
    }
    case Kind::Quote: {
        const bool pull = b.flag;
        const qreal inner = width - (pull ? 58 : 38);
        return (pull ? 23 : 20) + (b.lang.isEmpty() ? 0 : 28) +
               estimateBlocks(b.children, inner, 8) + (b.text.text.isEmpty() ? 0 : 24);
    }
    }
    return 0;
}
} // namespace

qreal BlockModel::estimate(int row, qreal width) const
{
    if (row < 0 || row >= m_rows.size())
        return 0;
    const Node &node = m_rows.at(row).node;
    return node.item >= 0 ? estimateBlocks(node.block->items.at(node.item).blocks, width, 6)
                          : estimateBlock(*node.block, width);
}

QHash<int, QByteArray> BlockModel::roleNames() const
{
    return {{KindRole, "kind"},       {TextRole, "text"},       {RoleRole, "citeRole"},
            {SourceRole, "source"},   {LangRole, "lang"},       {LevelRole, "level"},
            {FlagRole, "flag"},       {OpenRole, "open"},       {ToneRole, "tone"},
            {SectionRole, "section"}, {TokensRole, "tokens"},   {CellsRole, "cells"},
            {ColumnsRole, "columns"}, {AlignRole, "align"},     {PaletteRole, "tones"},
            {ChildrenRole, "nested"}, {TaskRole, "task"},       {NumberRole, "number"},
            {DepthRole, "depth"},     {PathRole, "path"},       {GapRole, "gap"},
            {OmittedRole, "omitted"}, {ClippedRole, "clipped"}, {BornRole, "born"},
            {MediaRole, "media"}};
}

// ---- RichDocument --------------------------------------------------------

namespace
{
// stream-view.js PACE: live text reveals at most `max` units per second;
// settled text reveals within `settle` seconds.
constexpr double PaceLag = 0.28, PaceDrain = 0.14, PaceMin = 40, PaceMax = 2400, PaceSmooth = 0.1,
                 PaceSettle = 0.5;
} // namespace

struct RichDocument::Result {
    markdown::Document doc;
    QHash<QString, tex::Rendered> math;
    QString text;
    bool live = false, clipped = false;
    bool partial = false; // Formulas were left undrawn (shown as source).
    quint64 generation = 0;
};

RichDocument::RichDocument(QObject *parent)
    : QObject(parent), m_root(this, BlockModel::Container::Top, 0, this),
      m_plain(this, BlockModel::Container::Plain, 0, this),
      m_clock([this](qreal elapsed) { tick(elapsed); })
{
    m_coalesce.setSingleShot(true);
    connect(&m_coalesce, &QTimer::timeout, this, &RichDocument::schedule);
    // Formulas are drawn in the palette's ink: draw them again, rows and all.
    connect(ThemeSignal::instance(), &ThemeSignal::changed, this, [this] {
        m_root.retone();
        if (m_math.isEmpty())
            return;
        m_math.clear();
        reset();
        want();
    });
}

RichDocument::~RichDocument()
{
    m_clock.stop();
    work().forget(this);
}

void RichDocument::abandonParse()
{
    ++m_generation;
    m_parsing = false;
    work().cancel(this);
}

void RichDocument::setSource(const QString &given)
{
    QString source = given, notice;
    if (source == exhaustedNotice()) {
        notice = source;
        source.clear();
    } else if (source.endsWith(clippedNotice())) {
        source.chop(clippedNotice().size());
        notice = clippedNotice().trimmed();
    }
    m_notice = notice;
    // Cut off by the display bound: an unclosed fence at the end is marked
    // clipped, so its Copy never passes for the whole source (P5-09).
    const bool clipped = !notice.isEmpty() && notice != exhaustedNotice();
    if (source == m_source && clipped == m_clipped && m_started)
        return;
    m_clipped = clipped;
    const bool grew = source.startsWith(m_source);
    m_source = source;
    // Replaced: nothing parsed for the old text will be shown.
    if (!grew)
        abandonParse();
    const bool renderable = markdown::renderable(source);
    if (renderable != m_renderable) {
        m_renderable = renderable;
        // Shown plain: no formatted rows, formulas or parse stay behind it.
        if (!renderable) {
            reset();
            m_math.clear();
        }
        emit renderableChanged();
    }
    if (!renderable) {
        setLazy(true);
        applyPlain(source);
        emit shownChanged();
    }
    if (!m_started) {
        // Shown whole: history, or a row scrolled back into view.
        m_started = true;
        m_shown = source.size();
    } else if (!grew) {
        m_shown = std::min(m_shown, double(source.size()));
    }
    emit sourceChanged();
    want();
}

void RichDocument::setLive(bool live)
{
    if (live == m_live)
        return;
    m_live = live;
    m_done = !live;
    m_draining = m_done && m_shown < m_source.size();
    if (m_draining) {
        m_from = m_shown;
        m_settled = 0;
    }
    emit liveChanged();
    want();
}

void RichDocument::setSeed(const QString &seed)
{
    if (seed == m_seed)
        return;
    m_seed = seed;
    const auto tones = markdown::tonesFor(seed);
    if (tones != m_tones) {
        m_tones = tones;
        // Colours are part of every block: parse everything again.
        m_doc = {};
        abandonParse();
        want();
    }
    emit seedChanged();
}

void RichDocument::setReducedMotion(bool reduced)
{
    if (reduced == m_reducedMotion)
        return;
    m_reducedMotion = reduced;
    if (reduced)
        m_shown = m_source.size();
    emit reducedMotionChanged();
    want();
}

void RichDocument::setDevicePixelRatio(qreal dpr)
{
    if (!(dpr > 0) || qFuzzyCompare(dpr, m_dpr))
        return;
    m_dpr = dpr;
    // Formulas are images at this ratio: draw them again, rows and all.
    m_math.clear();
    reset();
    emit devicePixelRatioChanged();
    want();
}

void RichDocument::revealFromStart()
{
    if (m_reducedMotion)
        return;
    m_shown = 0;
    m_rate = 0;
    reset();
    emit shownChanged();
    want();
}

void RichDocument::reset()
{
    m_doc = {};
    abandonParse();
    m_rowsPending = false;
    m_partial = false;
    m_root.apply({}, {});
    applyPlain({});
}

void RichDocument::setLazy(bool lazy)
{
    if (lazy == m_lazy)
        return;
    m_lazy = lazy;
    emit lazyChanged();
}

void RichDocument::applyPlain(const QString &text)
{
    markdown::Blocks blocks;
    QVector<bool> breaks;
    QVector<BlockModel::Node> nodes;
    for (qsizetype start = 0, k = 0; start < text.size(); ++k) {
        // PlainLines lines within PlainChunk units; else up to the last line
        // end within them; else (one long line) all of them.
        const qsizetype limit = std::min(text.size(), start + PlainChunk);
        qsizetype end = limit, next = limit, line = start - 1;
        int lines = 0;
        while (lines < PlainLines) {
            const qsizetype at = text.indexOf(QLatin1Char('\n'), line + 1);
            if (at < 0 || at >= limit)
                break;
            line = at;
            ++lines;
        }
        if (line > start && (lines == PlainLines || limit < text.size())) {
            end = line;
            next = line + 1;
        } else if (limit < text.size() && text.at(limit - 1).isHighSurrogate()) {
            end = next = limit - 1;
        }
        const QStringView piece = QStringView(text).mid(start, end - start);
        markdown::BlockPtr block = m_plainBlocks.value(k);
        if (!block || block->text.text != piece) {
            markdown::Block b;
            b.text.text = piece.toString();
            b.hash = qHash(b.text.text);
            block = std::make_shared<const markdown::Block>(std::move(b));
        }
        blocks << block;
        breaks << (next > end);
        nodes << BlockModel::Node{block, -1};
        start = next;
    }
    m_plainBlocks = blocks;
    m_plainBreaks = breaks;
    m_plain.apply(nodes, {});
    ++m_shownVersion;
}

QString RichDocument::plainNote() const
{
    if (m_renderable && m_doc.rest.isEmpty())
        return {};
    return empty() ? QStringLiteral("This message is too large to format; it is shown as plain text.")
                   : QStringLiteral("The rest of this message is too large to format; it is shown "
                                    "as plain text.");
}

QVector<SelectionUnit> RichDocument::units() const
{
    QVector<SelectionUnit> out;
    m_root.units(out, QStringLiteral("r"));
    if (const QString note = plainNote(); !note.isEmpty()) {
        SelectionUnit unit;
        unit.path = QStringLiteral("n:t");
        unit.text.text = note;
        out << std::move(unit);
    }
    m_plain.units(out, QStringLiteral("p"), &m_plainBreaks);
    return out;
}

QVector<SelectionUnit> RichDocument::unitsFor(const QString &given)
{
    // As setSource() and a parse would show it, whole and settled.
    RichDocument doc;
    QString source = given;
    bool clipped = false;
    if (source == exhaustedNotice()) {
        source.clear();
    } else if (source.endsWith(clippedNotice())) {
        source.chop(clippedNotice().size());
        clipped = true;
    }
    if (!markdown::renderable(source)) {
        doc.m_renderable = false;
        doc.applyPlain(source);
        return doc.units();
    }
    markdown::Options options;
    options.clipped = clipped;
    options.tones = doc.m_tones;
    const markdown::Document parsed = markdown::parse(source, options, {});
    QVector<BlockModel::Node> nodes;
    nodes.reserve(parsed.blocks.size());
    for (const auto &block : parsed.blocks)
        nodes << BlockModel::Node{block, -1};
    doc.m_root.apply(nodes, {});
    doc.m_doc.rest = parsed.rest;
    doc.applyPlain(parsed.rest);
    return doc.units();
}

int RichDocument::cut() const
{
    const int end = int(m_source.size());
    int n = std::min(end, int(std::floor(m_shown)));
    if (n > 0 && n < end && m_source.at(n - 1).isHighSurrogate())
        n++;
    return n;
}

void RichDocument::tick(qreal elapsed)
{
    if (m_rowsPending) {
        applyRows();
        emit shownChanged();
        emit grew();
    }
    const double end = m_source.size();
    const double dt = std::min(double(elapsed), 0.05);
    if (m_reducedMotion) {
        m_shown = end;
    } else {
        const double backlog = end - m_shown;
        const double goal =
            std::min(PaceMax, std::max(PaceMin, backlog / (m_done ? PaceDrain : PaceLag)));
        m_rate += (goal - m_rate) * (1 - std::exp(-dt / PaceSmooth));
        if (backlog > 0)
            m_shown = std::min(end, m_shown + std::max(PaceMin, m_rate) * dt);
        if (m_done) {
            // The settled drain is linear in real time from the text shown at
            // settlement; the ordinary pace wins while it is faster.
            m_settled += elapsed;
            const double floor = m_from + (end - m_from) * std::min(1.0, m_settled / PaceSettle);
            if (floor > m_shown)
                m_shown = std::min(end, floor);
        }
    }
    want();
}

void RichDocument::componentComplete()
{
    m_complete = true;
    want();
}

void RichDocument::setPaced(bool paced)
{
    if (m_paced == paced)
        return;
    m_paced = paced;
    emit pacedChanged();
}

void RichDocument::setHold(bool hold)
{
    if (m_hold == hold)
        return;
    m_hold = hold;
    emit holdChanged();
    want();
}

void RichDocument::want()
{
    if (!m_complete || m_hold)
        return;
    // Only a live message, and one draining after it settled, is paced.
    if (m_reducedMotion || (m_done && !m_draining))
        m_shown = m_source.size();
    const int count = cut();
    if (m_draining && count >= m_source.size())
        m_draining = false;
    const bool whole = count >= m_source.size();
    m_wantText = m_source.left(count);
    m_wantLive = !(m_done && whole);
    const bool running = m_clock.state() == QAbstractAnimation::Running;
    if (!whole && !m_reducedMotion && !running) {
        m_clock.start();
        emit revealingChanged();
    } else if ((whole || m_reducedMotion) && running && !m_rowsPending) {
        m_clock.stop();
        emit revealingChanged();
    }
    schedule();
}

void RichDocument::schedule()
{
    if (m_parsing || !m_renderable)
        return;
    if (m_parses && m_wantText == m_doc.text && m_wantLive == m_doc.live &&
        m_clipped == m_doc.clipped && !m_partial)
        return;
    // A long streaming fence is parsed again whole with each delta: while
    // it grows, such parses start at most every CoalesceMs, so the pool is
    // not kept busy re-reading it every frame. Settlement is not delayed.
    if (m_wantLive && m_doc.live && !m_doc.starts.isEmpty() && m_parsed.isValid() && !m_partial &&
        m_wantText.size() - m_doc.starts.last() > CoalesceUnits &&
        m_wantText.startsWith(m_doc.text)) {
        const qint64 wait = CoalesceMs - m_parsed.elapsed();
        if (wait > 0) {
            if (!m_coalesce.isActive())
                m_coalesce.start(int(wait));
            return;
        }
    }
    m_coalesce.stop();
    const auto wanted = [this] {
        auto result = std::make_shared<Result>();
        result->text = m_wantText;
        result->live = m_wantLive;
        result->clipped = m_clipped;
        result->generation = m_generation;
        return result;
    };
    // A message first shown whole and small is parsed here and now, so its
    // row has its height at once (no empty row while scrolling history).
    // That parse's work is linear in its at most SyncParse units, except
    // drawing formulas: past SyncMath they are shown as source until a
    // pool parse draws them. Rows made below the view together (fast
    // scrolling) share one slice's budget (Pacer, audit P6-07); the rest,
    // and everything else, parse on the pool.
    if (!m_parses && m_wantText.size() <= SyncParse &&
        (!m_paced || Pacer::takeParse(int(m_wantText.size())))) {
        QElapsedTimer timer;
        timer.start();
        const auto result = wanted();
        parse(*result, m_doc, m_math, m_tones, m_dpr, SyncMath);
        Pacer::spent(timer.nsecsElapsed());
        m_parsing = true;
        finished(result);
        return;
    }
    m_parsing = true;
    // The parse takes the text wanted when a worker is free, not now.
    work().want(this, [this, wanted](const std::shared_ptr<std::atomic_bool> &cancel) {
        std::function<std::function<void()>()> job;
        if (!m_parsing)
            return job;
        // A partial parse's blocks are not reused: they are parsed again whole.
        job = [this, result = wanted(), cancel, previous = m_partial ? markdown::Document() : m_doc,
               previousMath = m_math, tones = m_tones, dpr = m_dpr]() -> std::function<void()> {
            parse(*result, previous, previousMath, tones, dpr, -1, cancel.get());
            return [this, result] { finished(result); };
        };
        return job;
    });
}

namespace
{
// The formulas an inline shows as images (one expanded to its source is not).
void drawnFormulas(const markdown::Inline &text, QSet<QString> &out)
{
    for (const markdown::Run &run : text.runs)
        if ((run.format & markdown::Math) && run.ref >= 0 && run.ref < text.math.size())
            out.insert(text.math.at(run.ref));
}

void drawnFormulas(const markdown::Block &block, QSet<QString> &out)
{
    drawnFormulas(block.text, out);
    drawnFormulas(block.role, out);
    for (const auto &cell : block.cells)
        drawnFormulas(cell, out);
    for (const auto &child : block.children)
        drawnFormulas(*child, out);
    for (const auto &item : block.items)
        for (const auto &child : item.blocks)
            drawnFormulas(*child, out);
}
} // namespace

void RichDocument::parse(Result &result, const markdown::Document &previous,
                         const QHash<QString, tex::Rendered> &previousMath,
                         const std::array<int, 7> &tones, qreal dpr, int mathBudget,
                         const std::atomic_bool *cancel)
{
    // Every formula is drawn once; one drawn for the previous parse is kept.
    QHash<QString, tex::Rendered> drawn = previousMath;
    tex::Style style;
    style.pixelSize = 16;
    style.color = theme::fg(0.85);
    const auto bytes = [&drawn](const QSet<QString> &formulas) {
        qint64 total = 0;
        for (const QString &source : formulas)
            total += drawn.value(source).image.sizeInBytes();
        return total;
    };
    // Parses with the formulas in `charged` already counted: each other formula
    // is drawn while its image fits in MathBytes, and shows its source after.
    const auto attempt = [&](QSet<QString> charged) {
        qint64 spent = bytes(charged);
        markdown::Options options;
        options.live = result.live;
        options.clipped = result.clipped;
        options.tones = tones;
        options.cancel = cancel;
        options.finish = [&](markdown::Inline &text) {
            for (int k = 0; k < text.math.size(); ++k) {
                if (cancel && cancel->load(std::memory_order_relaxed))
                    return;
                const QString &source = text.math.at(k);
                auto found = drawn.find(source);
                if (found == drawn.end()) {
                    if (mathBudget >= 0) {
                        const int cost = SyncMathEach + int(source.size());
                        if (result.partial || cost > mathBudget) {
                            // Left for the pool: every later formula too.
                            result.partial = true;
                            markdown::expandMath(text, k);
                            continue;
                        }
                        mathBudget -= cost;
                    }
                    found = drawn.insert(source, tex::render(source, false, style, dpr));
                }
                bool shown = found->ok;
                if (shown && !charged.contains(source)) {
                    const qint64 size = found->image.sizeInBytes();
                    shown = spent + size <= MathBytes;
                    if (shown) {
                        spent += size;
                        charged.insert(source);
                    }
                }
                if (!shown)
                    markdown::expandMath(text, k);
            }
        };
        result.doc = markdown::parse(result.text, options, previous);
    };
    attempt({});
    QSet<QString> shown;
    for (const auto &block : std::as_const(result.doc.blocks))
        drawnFormulas(*block, shown);
    if (bytes(shown) > MathBytes) {
        // Blocks reused from the previous parse keep their formulas (within
        // the budget there); the rest are parsed again around them.
        QSet<QString> kept;
        for (qsizetype k = 0; k < result.doc.blocks.size() && k < previous.blocks.size() &&
                              result.doc.blocks.at(k) == previous.blocks.at(k);
             ++k)
            drawnFormulas(*result.doc.blocks.at(k), kept);
        attempt(kept);
        shown.clear();
        for (const auto &block : std::as_const(result.doc.blocks))
            drawnFormulas(*block, shown);
    }
    // Only what the shown blocks draw is kept (audit P5-04).
    result.math.clear();
    for (const QString &source : std::as_const(shown))
        result.math.insert(source, drawn.value(source));
}

void RichDocument::finished(const std::shared_ptr<Result> &result)
{
    if (result->generation != m_generation)
        return; // Superseded by a restart; its successor is scheduled below.
    m_parsing = false;
    // Replaced or settled while parsing: never shown, its successor is
    // parsed now. A streaming parse of text still wanted is progress.
    if (!m_wantText.startsWith(result->text) || (result->live && !m_wantLive) ||
        result->clipped != m_clipped) {
        schedule();
        return;
    }
    m_doc = result->doc;
    m_doc.text = result->text;
    m_math = result->math;
    m_partial = result->partial;
    m_parsed.start();
    ++m_parses;
    int pieces = 0;
    for (const auto &block : std::as_const(m_doc.blocks))
        pieces += block->pieces;
    setLazy(pieces > EagerPieces || !m_doc.rest.isEmpty());
    applyRows();
    applyPlain(m_doc.rest);
    emit shownChanged();
    emit grew();
    schedule();
}

void RichDocument::applyRows()
{
    const int total = int(m_doc.blocks.size());
    const bool paced = !m_reducedMotion &&
                       (m_live || m_draining || m_clock.state() == QAbstractAnimation::Running);
    const int limit = paced ? std::min(total, m_root.rowCount() + RowsPerFrame) : total;
    QVector<BlockModel::Node> nodes;
    nodes.reserve(limit);
    for (int k = 0; k < limit; ++k)
        nodes << BlockModel::Node{m_doc.blocks.at(k), -1};
    m_root.apply(nodes, {});
    ++m_shownVersion;
    m_rowsPending = limit < total;
    if (m_rowsPending && m_clock.state() != QAbstractAnimation::Running) {
        m_clock.start();
        emit revealingChanged();
    }
}

QColor RichDocument::tone(int index) const
{
    const theme::Palette &palette = theme::current();
    return index >= 0 && index < theme::ToneCount ? palette.tones[index] : palette.neutral;
}

QVariantList RichDocument::palette(int headingIndex) const
{
    QVariantList colors;
    const int base = std::max(0, headingIndex);
    for (int k = 0; k < theme::ToneCount; ++k)
        colors << theme::current().tones[m_tones[(base + k) % theme::ToneCount]];
    return colors;
}

// ---- InlineFormat --------------------------------------------------------

namespace
{
// styles.css: `.markdown code` is .86em with .12em/.38em padding in its own
// em and a 6 px radius; `.link-chip` is .875em with a 1 px margin, 6 px
// padding, a 14 px globe and 6 px before the label, 9 px after it, and its
// line-height 1.65; a link's underline sits 3 px under the baseline.
constexpr qreal CodeScale = 0.86, CodePadX = 0.38, CodePadY = 0.12;
constexpr qreal ChipScale = 0.875, ChipLine = 1.65, ChipBefore = 1 + 6 + 14 + 6, ChipAfter = 9 + 1;
constexpr qreal UnderlineOffset = 3;

QTextCharFormat styled(const markdown::Run &run, const RichInline &content, qreal pixelSize)
{
    using namespace markdown;
    QTextCharFormat format;
    const quint8 f = run.format;
    if (f & Math) {
        const int ref = run.ref;
        QTextImageFormat image;
        const QImage &drawn = content.math.value(ref);
        image.setName(QStringLiteral("openghost-math:%1").arg(ref));
        image.setWidth(drawn.width() / drawn.devicePixelRatio());
        image.setHeight(drawn.height() / drawn.devicePixelRatio());
        image.setVerticalAlignment(QTextCharFormat::AlignMiddle);
        image.setProperty(TexProperty, content.text.math.value(ref));
        return image;
    }
    // Boxes, the chip's globe and the link's underline are drawn under the
    // text from decorations(): QTextCharFormat has no padding, radius or
    // underline offset.
    if (f & Link) {
        format.setAnchor(true);
        if (run.ref >= 0)
            format.setAnchorHref(content.text.links.value(run.ref));
        if (f & Chip) { // A link chip: a quiet pill naming the host, in the text's weight.
            format.setForeground(theme::fg(0.9));
            format.setFontPointSize(Theme::pointsFor(pixelSize * ChipScale));
            return format;
        }
        format.setForeground(theme::current().link);
    }
    if (f & Code) {
        format.setFontFamilies({Theme::monoFamily()});
        format.setFontFixedPitch(true);
        format.setFontPointSize(Theme::pointsFor(pixelSize * CodeScale));
        format.setFontWeight(Theme::cssWeight(Theme::monoFamily(), QFont::Medium));
        format.setForeground(theme::fg(0.92));
    }
    if (f & Strong) {
        format.setFontWeight(Theme::cssWeight(QGuiApplication::font().family(), QFont::ExtraBold));
        if (!(f & Link))
            format.setForeground(theme::fg(1));
    }
    if (f & Emphasis) {
        format.setFontItalic(true);
        if (!(f & (Link | Strong | Code)))
            format.setForeground(theme::fg(0.95));
    }
    if (f & Strike) {
        format.setFontStrikeOut(true);
        if (!(f & Link))
            format.setForeground(theme::fg(theme::current().secondary));
    }
    return format;
}
} // namespace

InlineFormat::InlineFormat(QObject *parent) : QObject(parent)
{
    // Every run's colours come from the palette: style them all again.
    connect(ThemeSignal::instance(), &ThemeSignal::changed, this, [this] {
        m_shown.text.runs.clear();
        sync();
    });
}

void InlineFormat::setTarget(QQuickItem *target)
{
    if (target == m_target)
        return;
    if (m_target)
        disconnect(m_target, nullptr, this, nullptr);
    m_target = target;
    if (m_target) {
        m_settle.setSingleShot(true);
        m_settle.setInterval(120);
        connect(&m_settle, &QTimer::timeout, this, &InlineFormat::selectionSettled,
                Qt::UniqueConnection);
        const QMetaObject *meta = m_target->metaObject();
        const auto slot = [this](const char *name) {
            return metaObject()->method(metaObject()->indexOfSlot(name));
        };
        if (const int font = meta->indexOfSignal("fontChanged(QFont)"); font >= 0)
            connect(m_target.data(), meta->method(font), this, slot("measure()"));
        if (const int selected = meta->indexOfSignal("selectedTextChanged()"); selected >= 0)
            connect(m_target.data(), meta->method(selected), &m_settle,
                    m_settle.metaObject()->method(m_settle.metaObject()->indexOfSlot("start()")));
    }
    m_shown = {};
    m_attached = false;
    emit targetChanged();
    measure();
    sync();
}

void InlineFormat::setContent(const QVariant &content)
{
    RichInline next = content.value<RichInline>();
    if (next.text == m_content.text && next.math.size() == m_content.math.size()) {
        bool same = true;
        for (int k = 0; k < next.math.size() && same; ++k)
            same = next.math.at(k).cacheKey() == m_content.math.at(k).cacheKey();
        if (same)
            return;
    }
    m_content = std::move(next);
    emit contentChanged();
    sync();
}

void InlineFormat::setPixelSize(qreal size)
{
    if (qFuzzyCompare(size, m_pixelSize))
        return;
    m_pixelSize = size;
    m_shown.text.runs.clear(); // Restyle all.
    emit pixelSizeChanged();
    measure();
    sync();
}

QTextDocument *InlineFormat::document() const
{
    if (!m_target)
        return nullptr;
    auto *quick = m_target->property("textDocument").value<QQuickTextDocument *>();
    return quick ? quick->textDocument() : nullptr;
}

void InlineFormat::sync()
{
    QTextDocument *doc = document();
    if (!doc)
        return;
    if (!m_attached) {
        doc->setUndoRedoEnabled(false);
        m_attached = true;
    }
    // Boxes follow the text's layout, which a new width also changes.
    if (QObject *layout = doc->documentLayout(); layout != m_layout) {
        if (m_layout)
            disconnect(m_layout, nullptr, this, nullptr);
        m_layout = layout;
        connect(doc->documentLayout(), &QAbstractTextDocumentLayout::update, this,
                &InlineFormat::scheduleDecorate);
    }
    // The document may not hold what was shown (a TextEdit completing
    // after its first text): then everything is set again.
    if (doc->characterCount() - 1 != m_shown.text.text.size())
        m_shown = {};
    const QString &next = m_content.text.text;
    const QString &shown = m_shown.text.text;
    int from = int(shown.size());
    // New text from here on waves (stream-view.js text()): after what is
    // kept of the shown text.
    int fresh = int(next.size());
    if (next != shown) {
        fresh = 0;
        while (fresh < shown.size() && fresh < next.size() && shown.at(fresh) == next.at(fresh))
            ++fresh;
        if (fresh > 0 && next.at(fresh - 1).isHighSurrogate())
            --fresh;
    }
    if (next != shown) {
        if (next.startsWith(shown) && !shown.isEmpty()) {
            // Only the new suffix; a selection edge at the end is not extended.
            const int at = int(shown.size());
            const int start = m_target->property("selectionStart").toInt();
            const int end = m_target->property("selectionEnd").toInt();
            const int cursor = m_target->property("cursorPosition").toInt();
            QMetaObject::invokeMethod(m_target, "insert", Q_ARG(int, at),
                                      Q_ARG(QString, next.mid(at)));
            if (end == at) {
                if (start == end)
                    m_target->setProperty("cursorPosition", cursor);
                else
                    QMetaObject::invokeMethod(m_target, "select",
                                              Q_ARG(int, cursor == start ? end : start),
                                              Q_ARG(int, cursor));
            }
            from = at;
        } else {
            m_target->setProperty("text", next);
            m_shown = {};
            from = 0;
            if (doc->characterCount() - 1 != next.size()) {
                // The TextEdit applies its text once it is complete, in the
                // same pass that creates it: style it then.
                const QMetaObject *meta = m_target->metaObject();
                const int changed = meta->indexOfSignal("textChanged()");
                if (changed >= 0)
                    connect(m_target.data(), meta->method(changed), this,
                            metaObject()->method(metaObject()->indexOfSlot("sync()")),
                            Qt::ConnectionType(Qt::DirectConnection | Qt::SingleShotConnection));
                return;
            }
        }
    }
    // Restyle from the first run that differs (or the new text).
    const auto &oldRuns = m_shown.text.runs;
    const auto &newRuns = m_content.text.runs;
    int k = 0;
    while (k < oldRuns.size() && k < newRuns.size() && oldRuns.at(k) == newRuns.at(k) &&
           !((newRuns.at(k).format & markdown::Math) &&
             m_content.math.value(newRuns.at(k).ref).cacheKey() !=
                 m_shown.math.value(oldRuns.at(k).ref).cacheKey()))
        ++k;
    if (k < oldRuns.size())
        from = std::min(from, oldRuns.at(k).start);
    if (k < newRuns.size())
        from = std::min(from, newRuns.at(k).start);
    // The character before a restyled run holds that run's leading room
    // (pad()): it is restyled too, with the run it belongs to.
    if (from > 0 && from < int(next.size())) {
        from--;
        while (k > 0 && newRuns.at(k - 1).start + newRuns.at(k - 1).length > from)
            k--;
    }
    const int length = int(next.size());
    // Line height for the blocks the change reached (TextEdit has none):
    // one ranged edit, so the document lays out once.
    const qreal height = m_pixelSize * m_lineHeight;
    const int reached = std::max(0, std::min(int(shown.size()), from) - 1);
    const int lineType =
        m_leading < 0 ? QTextBlockFormat::FixedHeight : QTextBlockFormat::MinimumHeight;
    const QTextBlockFormat last = doc->lastBlock().blockFormat();
    const bool lines =
        m_lineHeight > 0 && (next.size() > shown.size() || from == 0) &&
        (!qFuzzyCompare(last.lineHeight(), height) || last.lineHeightType() != lineType);
    if (from >= length && !lines) {
        m_shown = m_content;
        scheduleDecorate();
        return;
    }
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    if (from < length) {
        cursor.setPosition(from);
        cursor.setPosition(length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(QTextCharFormat());
        for (int i = k; i < newRuns.size(); ++i) {
            const markdown::Run &run = newRuns.at(i);
            const int start = std::max(run.start, from),
                      stop = std::min(run.start + run.length, length);
            if (stop <= start)
                continue;
            QTextCharFormat format;
            if (run.format & markdown::Math) {
                const QImage image = aligned(run.ref);
                if (image.isNull())
                    continue;
                doc->addResource(QTextDocument::ImageResource,
                                 QUrl(QStringLiteral("openghost-math:%1").arg(run.ref)), image);
                QTextImageFormat formula = styled(run, m_content, m_pixelSize).toImageFormat();
                formula.setWidth(image.width() / image.devicePixelRatio());
                formula.setHeight(image.height() / image.devicePixelRatio());
                format = formula;
            } else {
                format = styled(run, m_content, m_pixelSize);
            }
            cursor.setPosition(start);
            cursor.setPosition(stop, QTextCursor::KeepAnchor);
            cursor.setCharFormat(format);
        }
        pad(cursor, from);
    }
    if (lines) {
        // A CSS line box is exactly its line-height, even below the font's
        // own height (a heading's 1.32 in Noto Sans, 1.36): then the line is
        // fixed, its baseline 4/5 down, where CSS's half-leading puts it
        // within .01em. Otherwise the room goes above the text (InlineText).
        QTextBlockFormat format;
        format.setLineHeight(height, lineType);
        cursor.setPosition(doc->findBlock(reached).position());
        cursor.setPosition(length, QTextCursor::KeepAnchor);
        cursor.mergeBlockFormat(format);
    }
    if (m_wave)
        m_wave->edited(cursor, fresh);
    cursor.endEditBlock();
    m_shown = m_content;
    if (!m_hoveredLink.isEmpty())
        lightChips();
    scheduleDecorate();
}

// Room for the boxes' horizontal padding (and a chip's margin and globe):
// letter spacing after the character before a run and after its last one,
// so the text is laid out as CSS pads it without changing a character. A run
// that starts its block is indented instead. Only the characters from
// `from` are set; those were reset with their runs.
void InlineFormat::pad(QTextCursor &cursor, int from)
{
    QTextDocument *doc = cursor.document();
    const int length = doc->characterCount() - 1;
    const qreal code = m_pixelSize * CodeScale * CodePadX;
    QHash<int, qreal> spacing;
    QHash<int, qreal> indents; // Block position → indent.
    for (const markdown::Run &run : m_content.text.runs) {
        const bool chip = (run.format & markdown::Chip) != 0;
        if (run.format & markdown::Math || !(chip || run.format & markdown::Code))
            continue;
        const int start = run.start, last = run.start + run.length - 1;
        if (last < from - 1 || last >= length)
            continue;
        const qreal before = chip ? ChipBefore : code, after = chip ? ChipAfter : code;
        const QTextBlock block = doc->findBlock(start);
        if (start == block.position())
            indents[start] += before;
        else
            spacing[start - 1] += before;
        spacing[last] += after;
    }
    for (auto it = spacing.cbegin(); it != spacing.cend(); ++it) {
        if (it.key() < from || it.key() >= length)
            continue;
        QTextCharFormat format;
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing(it.value());
        cursor.setPosition(it.key());
        cursor.setPosition(it.key() + 1, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    for (QTextBlock block = doc->findBlock(from); block.isValid(); block = block.next()) {
        const qreal indent = indents.value(block.position());
        if (qFuzzyCompare(block.blockFormat().textIndent() + 1, indent + 1))
            continue;
        QTextBlockFormat format;
        format.setTextIndent(indent);
        cursor.setPosition(block.position());
        cursor.mergeBlockFormat(format);
    }
}

void InlineFormat::setHoveredLink(const QString &link)
{
    if (link == m_hoveredLink)
        return;
    m_hoveredLink = link;
    emit hoveredLinkChanged();
    lightChips();
    scheduleDecorate();
}

// A hovered chip's label is full white (.link-chip:hover).
void InlineFormat::lightChips()
{
    QTextDocument *doc = document();
    if (!doc)
        return;
    const int length = doc->characterCount() - 1;
    QTextCursor cursor(doc);
    bool editing = false;
    for (const markdown::Run &run : m_shown.text.runs) {
        if (!(run.format & markdown::Chip) || run.start + run.length > length)
            continue;
        const QColor color = theme::fg(
            m_content.text.links.value(run.ref) == m_hoveredLink && !m_hoveredLink.isEmpty() ? 1
                                                                                             : 0.9);
        cursor.setPosition(run.start + 1);
        if (cursor.charFormat().foreground().color() == color)
            continue;
        if (!editing)
            cursor.beginEditBlock();
        editing = true;
        QTextCharFormat format;
        format.setForeground(color);
        cursor.setPosition(run.start);
        cursor.setPosition(run.start + run.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    if (editing)
        cursor.endEditBlock();
}

void InlineFormat::setNatural(bool natural)
{
    if (natural == m_natural)
        return;
    m_natural = natural;
    emit naturalChanged();
    scheduleDecorate();
}

// Coalesced: the layout reports each block it lays out.
void InlineFormat::scheduleDecorate()
{
    if (m_decorating)
        return;
    m_decorating = true;
    QMetaObject::invokeMethod(this, &InlineFormat::decorate, Qt::QueuedConnection);
}

void InlineFormat::decorate()
{
    m_decorating = false;
    QTextDocument *doc = document();
    QVariantList out;
    qreal natural = 0;
    if (doc && m_target) {
        const int length = doc->characterCount() - 1;
        QAbstractTextDocumentLayout *layout = doc->documentLayout();
        const QFont base = doc->defaultFont();
        QTextCursor cursor(doc);
        for (const markdown::Run &run : m_shown.text.runs) {
            const bool chip = (run.format & markdown::Chip) != 0;
            const bool code = !chip && run.format & markdown::Code;
            const bool link = !chip && run.format & markdown::Link;
            if (run.format & markdown::Math || !(chip || code || link))
                continue;
            const int start = run.start, end = run.start + run.length;
            if (end > length)
                continue;
            cursor.setPosition(start + 1);
            // Chromium rounds a font's ascent and descent to whole pixels.
            const QFontMetricsF metrics(cursor.charFormat().font().resolve(base));
            const qreal ascent = std::round(metrics.ascent()),
                        descent = std::round(metrics.descent());
            const QString href = link || chip ? m_content.text.links.value(run.ref) : QString();
            for (QTextBlock block = doc->findBlock(start);
                 block.isValid() && block.position() < end; block = block.next()) {
                const QRectF box = layout->blockBoundingRect(block);
                const QTextLayout *lines = block.layout();
                const int from = std::max(start, block.position()) - block.position();
                const int to =
                    std::min(end, block.position() + block.length() - 1) - block.position();
                for (int i = 0; lines && i < lines->lineCount(); ++i) {
                    const QTextLine line = lines->lineAt(i);
                    const int a = std::max(from, line.textStart());
                    const int b = std::min(to, line.textStart() + line.textLength());
                    if (b <= a)
                        continue;
                    const bool first = block.position() + a == start;
                    const bool last = block.position() + b == end;
                    const qreal x1 = box.x() + line.cursorToX(a), x2 = box.x() + line.cursorToX(b);
                    const qreal baseline = box.y() + line.y() + line.ascent();
                    const auto add = [&](const char *kind, qreal x, qreal y, qreal w, qreal h) {
                        out << QVariantMap{{"kind", kind}, {"x", x},      {"y", y},
                                           {"w", w},       {"h", h},      {"first", first},
                                           {"last", last}, {"href", href}};
                    };
                    if (code) {
                        const qreal padX = m_pixelSize * CodeScale * CodePadX;
                        const qreal padY = m_pixelSize * CodeScale * CodePadY;
                        const qreal left = first ? x1 - padX : x1;
                        add("code", left, baseline - ascent - padY, x2 - left,
                            ascent + descent + 2 * padY);
                    }
                    if (chip) {
                        const qreal height = m_pixelSize * ChipScale * ChipLine;
                        const qreal left = first ? x1 - ChipBefore + 1 : x1;
                        const qreal right = last ? x2 - 1 : x2;
                        add("chip", left, baseline - ascent - (height - ascent - descent) / 2,
                            right - left, height);
                        if (first)
                            add("globe", left + 6, baseline + 2 - 14, 14, 14);
                    } else if (link) {
                        add("underline", x1, baseline + UnderlineOffset, x2 - x1, 1);
                    }
                }
            }
        }
        if (m_natural) {
            // CSS max-content: each block laid out without a width.
            for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
                QTextLayout unwrapped(block.text(), base);
                unwrapped.setFormats(block.textFormats());
                QTextOption option = doc->defaultTextOption();
                option.setWrapMode(QTextOption::ManualWrap);
                unwrapped.setTextOption(option);
                unwrapped.beginLayout();
                for (QTextLine line = unwrapped.createLine(); line.isValid();
                     line = unwrapped.createLine())
                    line.setLineWidth(1e6);
                unwrapped.endLayout();
                for (int i = 0; i < unwrapped.lineCount(); ++i)
                    natural = std::max(natural, unwrapped.lineAt(i).naturalTextWidth() +
                                                    (i ? 0 : block.blockFormat().textIndent()));
            }
        }
    }
    if (out != m_decorations) {
        m_decorations = out;
        emit decorationsChanged();
    }
    if (!qFuzzyCompare(natural + 1, m_naturalWidth + 1)) {
        m_naturalWidth = natural;
        emit naturalWidthChanged();
    }
}

void InlineFormat::setLineHeight(qreal height)
{
    if (qFuzzyCompare(height, m_lineHeight))
        return;
    m_lineHeight = height;
    m_shown = {}; // Set every block again.
    emit lineHeightChanged();
    measure();
    sync();
}

void InlineFormat::measure()
{
    qreal leading = 0;
    if (m_target && m_lineHeight > 0)
        leading = m_pixelSize * m_lineHeight -
                  QFontMetricsF(m_target->property("font").value<QFont>()).height();
    if (qFuzzyCompare(leading + 1, m_leading + 1))
        return;
    const bool fixed = (leading < 0) != (m_leading < 0); // The line type changes.
    m_leading = leading;
    emit leadingChanged();
    if (fixed) {
        m_shown = {};
        sync();
    }
}

// A formula padded so Qt's middle alignment (ascent (h + x/2) / 2, descent
// (h - x/2) / 2) puts its baseline on the text's baseline.
QImage InlineFormat::aligned(int ref) const
{
    const QImage image = m_content.math.value(ref);
    if (image.isNull() || !m_target)
        return image;
    const qreal dpr = image.devicePixelRatio();
    const qreal height = image.height() / dpr, baseline = m_content.baselines.value(ref);
    const qreal halfX = QFontMetricsF(m_target->property("font").value<QFont>()).xHeight() / 2;
    const qreal descent = height - baseline;
    qreal top = 0, bottom = 0;
    if (baseline - descent > halfX)
        bottom = baseline - halfX - descent;
    else
        top = descent + halfX - baseline;
    if (top < 0.5 / dpr && bottom < 0.5 / dpr)
        return image;
    QImage padded(image.width(), int(std::ceil((height + top + bottom) * dpr)), image.format());
    padded.setDevicePixelRatio(dpr);
    padded.fill(Qt::transparent);
    QPainter painter(&padded);
    painter.drawImage(QPointF(0, top), image);
    return padded;
}

QString InlineFormat::selected(QTextDocument *document, int start, int end)
{
    if (!document || start == end)
        return {};
    const int from = std::min(start, end), to = std::max(start, end);
    QString text;
    for (QTextBlock block = document->findBlock(from); block.isValid() && block.position() < to;
         block = block.next()) {
        if (block.position() > from)
            text += QLatin1Char('\n');
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const int a = std::max(from, fragment.position());
            const int b = std::min(to, fragment.position() + fragment.length());
            if (b <= a)
                continue;
            const QString piece = fragment.text().mid(a - fragment.position(), b - a);
            const QTextCharFormat format = fragment.charFormat();
            if (format.isImageFormat() && format.hasProperty(TexProperty)) {
                const QString tex = format.property(TexProperty).toString();
                for (const QChar c : piece)
                    text += c == markdown::Object ? QLatin1Char('$') + tex + QLatin1Char('$')
                                                  : QString(c);
            } else {
                text += piece;
            }
        }
    }
    text.replace(QChar::LineSeparator, QLatin1Char('\n'));
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return text;
}

// ---- CodeFormat ------------------------------------------------------------

CodeFormat::CodeFormat(QObject *parent) : QSyntaxHighlighter(parent)
{
    connect(ThemeSignal::instance(), &ThemeSignal::changed, this, [this] {
        if (document())
            rehighlight();
    });
}

void CodeFormat::setDocument(QQuickTextDocument *document)
{
    if (document == m_document)
        return;
    m_document = document;
    QTextDocument *doc = document ? document->textDocument() : nullptr;
    if (doc)
        doc->setUndoRedoEnabled(false);
    QSyntaxHighlighter::setDocument(doc);
    emit documentChanged();
}

void CodeFormat::setTokens(const QVariant &tokens)
{
    const auto next = tokens.value<QVector<markdown::Run>>();
    if (next == m_tokens)
        return;
    // Only blocks from the first changed run are highlighted again, so a
    // growing block costs its new lines, not the whole block each time.
    int k = 0;
    while (k < m_tokens.size() && k < next.size() && m_tokens.at(k) == next.at(k))
        ++k;
    int from = INT_MAX;
    if (k < m_tokens.size())
        from = m_tokens.at(k).start;
    if (k < next.size())
        from = std::min(from, next.at(k).start);
    m_tokens = next;
    emit tokensChanged();
    QTextDocument *doc = QSyntaxHighlighter::document();
    if (!doc || from == INT_MAX)
        return;
    for (QTextBlock block = doc->findBlock(from); block.isValid(); block = block.next())
        rehighlightBlock(block);
}

void CodeFormat::setTone(const QColor &tone)
{
    if (tone == m_tone)
        return;
    m_tone = tone;
    emit toneChanged();
    rehighlight();
}

void CodeFormat::highlightBlock(const QString &text)
{
    const int position = currentBlock().position(), end = position + int(text.size());
    auto it = std::lower_bound(
        m_tokens.cbegin(), m_tokens.cend(), position,
        [](const markdown::Run &run, int at) { return run.start + run.length <= at; });
    for (; it != m_tokens.cend() && it->start < end; ++it) {
        const int start = std::max(it->start, position),
                  stop = std::min(it->start + it->length, end);
        if (stop <= start)
            continue;
        QTextCharFormat format;
        const theme::Palette &palette = theme::current();
        switch (it->format) {
        case highlight::Keyword:
            format.setForeground(palette.hlKeyword);
            break;
        case highlight::String:
            format.setForeground(palette.hlString);
            break;
        case highlight::Number:
            format.setForeground(palette.hlNumber);
            break;
        case highlight::Comment:
            format.setForeground(theme::fg(palette.hlComment));
            format.setFontItalic(true);
            break;
        case highlight::Function:
            format.setForeground(palette.hlFunction);
            break;
        case highlight::Type:
            format.setForeground(palette.hlType);
            break;
        case highlight::Property:
            format.setForeground(palette.hlProperty);
            break;
        case highlight::Inserted:
            format.setForeground(palette.hlInserted);
            break;
        case highlight::Deleted:
            format.setForeground(palette.hlDeleted);
            break;
        case highlight::ArtLine: {
            QColor line = m_tone.isValid() ? m_tone : palette.neutral;
            line.setAlphaF(0.6f);
            format.setForeground(line);
            break;
        }
        case highlight::ArtArrow:
            format.setForeground(m_tone.isValid() ? m_tone : palette.neutral);
            break;
        default:
            continue;
        }
        setFormat(start - position, stop - start, format);
    }
    // Characters a TextWave hides stay transparent over their highlight.
    for (auto it = currentBlock().begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid() || !fragment.charFormat().hasProperty(TextWave::Hidden))
            continue;
        for (int i = fragment.position() - position, stop = i + fragment.length(); i < stop; ++i) {
            QTextCharFormat hidden = format(i);
            hidden.setForeground(QBrush(Qt::transparent));
            setFormat(i, 1, hidden);
        }
    }
}

// ---- RichImage -------------------------------------------------------------

RichImage::RichImage(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &RichImage::render);
    connect(ThemeSignal::instance(), &ThemeSignal::changed, this, [this] { changed(0); });
}

RichImage::~RichImage() { work().forget(this); }

void RichImage::setKind(const QString &kind)
{
    if (kind == m_kind)
        return;
    m_kind = kind;
    emit kindChanged();
    changed(0);
}

void RichImage::setSource(const QString &source)
{
    if (source == m_source)
        return;
    m_source = source;
    emit sourceChanged();
    // Once anything was drawn, or is being drawn, streaming changes wait
    // until they pause. Not whether pixels still wait for upload: they are
    // released once uploaded (audit P5-11).
    changed(m_live && (m_drawn || m_rendering || !m_error.isEmpty()) ? LiveDelayMs : 0);
}

void RichImage::setLive(bool live)
{
    if (live == m_live)
        return;
    m_live = live;
    emit liveChanged();
    if (!live)
        changed(0);
}

void RichImage::setImage(const QImage &image)
{
    if (image.cacheKey() == m_image.cacheKey())
        return;
    m_image = image;
    m_drawn = !image.isNull();
    m_error.clear();
    m_dirty = true;
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1;
    setImplicitSize(image.width() / dpr, image.height() / dpr);
    update();
    emit resultChanged();
}

qreal RichImage::ratio() const
{
    return window() ? window()->effectiveDevicePixelRatio() : qGuiApp->devicePixelRatio();
}

namespace
{
// A formula as drawn, or why it could not be.
struct Drawing {
    QImage image;
    QSizeF size;
    bool ok = false;
    QString error;
};

// Drawings by a digest of everything that drew them, so a block created
// again (a row scrolled back into view, a reopened conversation) has its
// drawing and its height at once, instead of drawing again and growing after
// it appears. Bounded by what each entry holds (pixels, error text, key),
// each costing at least 1/CacheEntries of the budget, so the entries are
// bounded too (audit P5-05); the source is not kept. GUI thread only.
QCache<QByteArray, Drawing> &drawings()
{
    static QCache<QByteArray, Drawing> cache(RichImage::CacheBytes);
    return cache;
}

qint64 held(const QByteArray &key, const Drawing &drawing)
{
    return drawing.image.sizeInBytes() + drawing.error.size() * qint64(sizeof(QChar)) + key.size() +
           qint64(sizeof(Drawing));
}

qint64 costOf(const QByteArray &key, const Drawing &drawing)
{
    return std::max(held(key, drawing), RichImage::CacheBytes / RichImage::CacheEntries);
}
} // namespace

RichImage::Cached RichImage::cached()
{
    Cached out;
    auto &cache = drawings();
    out.count = int(cache.count());
    out.cost = cache.totalCost();
    for (const QByteArray &key : cache.keys())
        out.bytes += held(key, *cache.object(key));
    return out;
}

// Everything but the source that decides a drawing.
QString RichImage::inputsKey() const
{
    // Drawn in the palette's ink.
    QString key = m_kind + QLatin1Char('\x1f') + QString::number(ratio(), 'f', 3) +
                  QLatin1Char('\x1f') + QLatin1Char(theme::current().light ? 'l' : 'd') +
                  QLatin1Char('\x1f');
    return key;
}

QByteArray RichImage::drawingKey() const
{
    const QString inputs = inputsKey();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(inputs.size()) + ':');
    for (const QString *part : {&inputs, &m_source})
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(part->constData()),
                                    part->size() * qsizetype(sizeof(QChar))));
    return hash.result();
}

bool RichImage::shown() const
{
    const QQuickItem *parent = parentItem();
    return parent ? parent->isVisible() : isVisible();
}

bool RichImage::progress(const QString &inputs, const QString &source) const
{
    return m_live && inputs == inputsKey() && m_source.startsWith(source);
}

bool RichImage::fromCache()
{
    if (m_kind == QLatin1String("image") || m_source.trimmed().isEmpty())
        return false;
    const Drawing *found = drawings().object(drawingKey());
    if (!found)
        return false;
    m_timer.stop();
    show(found->ok, found->image, found->size, found->error, m_source);
    return true;
}

void RichImage::show(bool ok, const QImage &image, QSizeF size, const QString &error,
                     const QString &source)
{
    if (ok) {
        m_image = image;
        m_drawn = true;
        m_shownSource = source;
        m_error.clear();
        m_dirty = true;
        setImplicitSize(size.width(), size.height());
        update();
    } else {
        m_error = error.isEmpty() ? QStringLiteral("This could not be drawn.") : error;
        // A formula never stands in for a different source.
        if (m_drawn) {
            m_image = QImage();
            m_drawn = false;
            m_shownSource.clear();
            setImplicitSize(0, 0);
            update();
        }
    }
    emit resultChanged();
    emit pendingChanged();
}

void RichImage::changed(int delay)
{
    if ((m_source.isEmpty() && m_kind.isEmpty()) || m_kind == QLatin1String("image"))
        return;
    ++m_request; // Work in flight is for what was asked before.
    // A drawing that cannot be shown even as progress stops early.
    if (m_flying && !progress(m_flightInputs, m_flightSource))
        work().cancel(this);
    // Drawn before: shown now, in the same pass that created this block.
    if (!m_rendering && fromCache())
        return;
    const bool wasPending = pending();
    m_timer.start(delay);
    if (!wasPending)
        emit pendingChanged();
}

void RichImage::itemChange(ItemChange change, const ItemChangeData &value)
{
    if (change == ItemSceneChange && value.window) {
        m_dirty = true;
        if (m_image.isNull() ||
            !qFuzzyCompare(m_image.devicePixelRatio(), value.window->effectiveDevicePixelRatio()))
            changed(0);
    }
    if (change == ItemParentHasChanged) {
        // The item's own `visible` shows its result; whether it is on view
        // at all is its parent's.
        disconnect(m_parentShown);
        if (value.item)
            m_parentShown = connect(value.item, &QQuickItem::visibleChanged, this, [this] {
                if (m_hidden && shown()) {
                    m_hidden = false;
                    changed(0);
                }
            });
    }
    QQuickItem::itemChange(change, value);
}

void RichImage::render()
{
    if (!window()) {
        emit pendingChanged();
        return;
    }
    if (!shown()) {
        // Hidden (audit P5-03): drawn once it is shown again.
        m_hidden = true;
        emit pendingChanged();
        return;
    }
    if (m_source.trimmed().isEmpty()) {
        // Nothing to draw yet while streaming; settled, that is the result.
        if (m_live || m_kind.isEmpty())
            emit pendingChanged();
        else
            show(false, {}, {}, QStringLiteral("There is nothing to draw."), m_source);
        return;
    }
    if (m_rendering) {
        m_again = true;
        return;
    }
    if (fromCache())
        return;
    m_rendering = true;
    // The job draws what is asked for when a worker is free, not now.
    work().want(this, [this](const std::shared_ptr<std::atomic_bool> &cancel) {
        std::function<std::function<void()>()> job;
        m_rendering = false;
        m_again = false;
        if (!window() || !shown() || m_source.trimmed().isEmpty()) {
            render(); // Says why nothing is drawn now.
            return job;
        }
        if (fromCache())
            return job;
        m_rendering = m_flying = true;
        m_flightInputs = inputsKey();
        m_flightSource = m_source;
        struct Out {
            QImage image;
            QSizeF size;
            bool ok = false, cancelled = false;
            QString error, source;
        };
        job = [this, cancel, kind = m_kind, source = m_source, dpr = ratio(),
               inputs = m_flightInputs, key = drawingKey(), request = m_request,
               before = m_beforeDraw]() -> std::function<void()> {
            if (before)
                before();
            Out out;
            out.source = source;
            if (cancel->load()) {
                // Stopped before it began.
            } else if (kind == QLatin1String("math")) {
                tex::Style style;
                style.pixelSize = 16;
                style.color = theme::fg(0.95);
                const auto drawn = tex::render(source, true, style, dpr);
                out.image = drawn.image;
                out.size = drawn.size;
                out.ok = drawn.ok;
                out.error = drawn.error;
            }
            out.cancelled = cancel->load();
            return [this, out = std::move(out), inputs, key, request] {
                m_rendering = m_flying = false;
                if (!out.cancelled) {
                    ++m_renders;
                    // A long source's failure is not kept: drawn again if needed.
                    if (out.ok || out.source.size() <= CachedFailure) {
                        auto *drawing = new Drawing{out.image, out.size, out.ok, out.error};
                        drawings().insert(key, drawing, costOf(key, *drawing));
                    }
                }
                // Checked before anything is shown: a result asked for before
                // a replacement, retry, settlement or resize is dropped.
                // Streaming text that still starts with its source is progress.
                const bool current = request == m_request && !out.cancelled;
                if (current || (!out.cancelled && out.ok && progress(inputs, out.source)))
                    show(out.ok, out.image, out.size, out.error, out.source);
                if (m_again || !current) {
                    m_again = false;
                    // A change still settling draws when its delay ends.
                    if (!m_timer.isActive())
                        render();
                }
                emit pendingChanged();
            };
        };
        return job;
    });
    emit pendingChanged();
}

QSGNode *RichImage::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGImageNode *>(old);
    if (!m_drawn || width() <= 0 || height() <= 0) {
        delete node;
        return nullptr;
    }
    if (!node && m_image.isNull()) {
        // The scene graph was rebuilt after the pixels were released.
        QMetaObject::invokeMethod(this, [this] { changed(0); }, Qt::QueuedConnection);
        return nullptr;
    }
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
        m_dirty = true;
    }
    if (m_dirty && !m_image.isNull()) {
        node->setTexture(window()->createTextureFromImage(m_image));
        // The texture holds the pixels; a drawing can be drawn again, so its
        // CPU copy goes (a preview's stays: it cannot).
        if (m_kind != QLatin1String("image"))
            m_image = QImage();
    }
    m_dirty = false;
    node->setRect(QRectF(0, 0, width(), height()));
    return node;
}

// ---- Previews --------------------------------------------------------------

Preview decodePreview(const QByteArray &bytes, int side)
{
    Preview out;
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    reader.setAllocationLimit(128);
    const QByteArray format = reader.format();
    if (format != "png" && format != "jpeg" && format != "webp") {
        out.error = QStringLiteral("This file is not a PNG, JPEG or WebP image.");
        return out;
    }
    const QSize size = reader.size();
    if (!size.isValid()) {
        out.error = QStringLiteral("This image could not be decoded.");
        return out;
    }
    if (size.width() > 16384 || size.height() > 16384 ||
        qint64(size.width()) * size.height() > PreviewSource) {
        out.error = QStringLiteral("This image is too large to preview.");
        return out;
    }
    // Orientation may swap the sides; the bound is on the longer one.
    const int longer = std::max(size.width(), size.height());
    if (longer > side)
        reader.setScaledSize(size.scaled(side, side, Qt::KeepAspectRatio));
    out.image = reader.read();
    if (out.image.isNull()) {
        out.error = QStringLiteral("This image could not be decoded.");
        return out;
    }
    if (std::max(out.image.width(), out.image.height()) > side)
        out.image = out.image.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    out.image = std::move(out.image).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return out;
}

void decodePreviewAsync(QObject *receiver, const QByteArray &bytes,
                        std::function<void(Preview)> done,
                        std::shared_ptr<const std::atomic_bool> dropped)
{
    // Never coalesced: each request is answered. The window keeps one
    // preview at a time between OpenGhost's reply and this answer.
    work().want(
        receiver,
        [bytes, done = std::move(done), dropped](const std::shared_ptr<std::atomic_bool> &cancel) {
            return std::function<std::function<void()>()>(
                [bytes, done, dropped, cancel]() -> std::function<void()> {
                    const bool skip = (dropped && *dropped) || *cancel;
                    Preview preview = skip ? Preview{} : decodePreview(bytes);
                    return [done, preview = std::move(preview)] { done(preview); };
                });
        },
        false);
}

RichWork richWork() { return work().stats(); }

void stopRichWork() { work().stop(); }

void setBeforeRichJob(std::function<void()> before) { work().beforeJob = std::move(before); }

void wantRichWork(QObject *owner, RichMake make) { work().want(owner, std::move(make)); }
void cancelRichWork(QObject *owner) { work().cancel(owner); }
void forgetRichWork(QObject *owner) { work().forget(owner); }

// ---- Theme -------------------------------------------------------------------

ThemeSignal *ThemeSignal::instance()
{
    static ThemeSignal *signal = new ThemeSignal;
    return signal;
}

namespace
{
QString themeChoice = QStringLiteral("dark");
std::optional<bool> systemDarkOverride;

bool systemDark()
{
    if (systemDarkOverride)
        return *systemDarkOverride;
    const auto *hints = qGuiApp ? QGuiApplication::styleHints() : nullptr;
    return !hints || hints->colorScheme() != Qt::ColorScheme::Light;
}

// Shows the palette the choice resolves to, announcing a change.
void showTheme(QPointF origin)
{
    const bool light = themeChoice == QLatin1String("light") ||
                       (themeChoice == QLatin1String("system") && !systemDark());
    if (light == theme::lightShown.load())
        return;
    emit ThemeSignal::instance() -> changing(origin);
    theme::lightShown.store(light);
    emit ThemeSignal::instance() -> changed();
}

// "system" follows the desktop: a switch there fades in.
void watchSystem()
{
    static bool watching = false;
    if (watching || !qGuiApp)
        return;
    watching = true;
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                     ThemeSignal::instance(), [] { showTheme({}); });
}
} // namespace

const QStringList &Theme::choices()
{
    static const QStringList all{QStringLiteral("light"), QStringLiteral("dark"),
                                 QStringLiteral("system")};
    return all;
}

QString Theme::chosen() { return themeChoice; }

void Theme::choose(const QString &choice, QPointF origin)
{
    watchSystem();
    if (!choices().contains(choice) || choice == themeChoice)
        return;
    themeChoice = choice;
    emit ThemeSignal::instance() -> choiceChanged();
    showTheme(origin);
}

void Theme::setSystemDark(std::optional<bool> dark)
{
    systemDarkOverride = dark;
    showTheme({});
}

Theme::Theme(QObject *parent)
    : QObject(parent), mono(monoFamily()), motion({0.32, 0.72, 0, 1, 1, 1}),
      ease({0.25, 0.1, 0.25, 1, 1, 1})
{
    watchSystem();
    applyPalette();
    connect(ThemeSignal::instance(), &ThemeSignal::changed, this, [this] {
        applyPalette();
        emit paletteChanged();
    });
    connect(ThemeSignal::instance(), &ThemeSignal::choiceChanged, this, &Theme::choiceChanged);
    m_reducedMotion = platform::reducedMotion();
}

namespace
{
QVariantList frameOf(const theme::Palette &p)
{
    QVariantList frame;
    for (const theme::Palette::FrameLight &light : p.frame) {
        frame << QVector4D(light.color.redF(), light.color.greenF(), light.color.blueF(), 1)
              << QVector4D(light.alpha[0], light.alpha[1], light.alpha[2], 0);
    }
    return frame;
}
} // namespace

QVariantMap Theme::paletteOf(bool light) const
{
    const theme::Palette &p = theme::palette(light);
    return {{"fg", p.fgBase},
            {"appBg", p.appBg},
            {"chatBg", p.chatBg},
            {"contourOuter", p.contourOuter},
            {"contourInner", p.contourInner},
            {"composerBg", p.composerBg},
            {"composerBorder", p.composerBorder},
            {"accent", p.accent},
            {"onAccent", p.onAccent},
            {"shadow", p.shadow},
            {"frame", frameOf(p)}};
}

void Theme::applyPalette()
{
    const theme::Palette &p = theme::current();
    m_light = p.light;
    fg = p.fgBase;
    appBg = p.appBg;
    chatBg = p.chatBg;
    contourOuter = p.contourOuter;
    contourInner = p.contourInner;
    composerBg = p.composerBg;
    composerBorder = p.composerBorder;
    accent = p.accent;
    onAccent = p.onAccent;
    muted = p.muted;
    text = theme::fg(p.primary);
    strong = theme::fg(1);
    secondary = theme::fg(p.secondary);
    tertiary = theme::fg(p.tertiary);
    quaternary = theme::fg(p.quaternary);
    link = p.link;
    success = p.success;
    danger = p.danger;
    warn = p.warn;
    hover = p.hover;
    rowActive = p.rowActive;
    rowHover = p.rowHover;
    selection = p.selection;
    backdrop = p.backdrop;
    tipBg = p.tipBg;
    wellBg = p.wellBg;
    diffAdded = p.diffAdded;
    diffRemoved = p.diffRemoved;
    noteBg = p.noteBg;
    noteHoverBg = p.noteHoverBg;
    glow = p.glow;
    paintShade = p.paintShade;
    shadow = p.shadow;
    tones.clear();
    for (const QColor &tone : p.tones)
        tones << tone;
    frame = frameOf(p);
    const theme::Palette::Splash &o = p.splash;
    splash = {{"night", o.night},     {"mistDeep", o.mistDeep},   {"mistLit", o.mistLit},
              {"glow", o.glow},       {"core", o.core},           {"aura", o.aura},
              {"glowRgb", o.glowRgb}, {"mistAlpha", o.mistAlpha}, {"glowAlpha", o.glowAlpha},
              {"trail", o.trail},     {"motes", o.motes},         {"shade", o.shade}};
}

void Theme::setReducedMotion(bool reduced)
{
    if (reduced == m_reducedMotion)
        return;
    m_reducedMotion = reduced;
    emit reducedMotionChanged();
}

qreal Theme::pointsFor(qreal pixels)
{
    const QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpi = screen ? screen->logicalDotsPerInch() : 96;
    return pixels * 72 / (dpi > 0 ? dpi : 96);
}

QString Theme::monoFamily()
{
    static const QString family = [] {
        for (const auto &name : {QStringLiteral("Cascadia Code"), QStringLiteral("Consolas")}) {
            if (QFontDatabase::hasFamily(name))
                return name;
        }
        return QStringLiteral("monospace");
    }();
    return family;
}

int Theme::cssWeight(const QString &family, int weight) { return cssfont::weight(family, weight); }

int Theme::weight(int css) const { return cssWeight(QGuiApplication::font().family(), css); }

void Theme::lines(QQuickTextDocument *document, int from, qreal height) const
{
    QTextDocument *doc = document ? document->textDocument() : nullptr;
    if (!doc)
        return;
    const auto mode = height > 0 ? QTextBlockFormat::MinimumHeight : QTextBlockFormat::SingleHeight;
    QTextCursor cursor(doc);
    bool editing = false;
    for (QTextBlock block = doc->findBlock(std::max(0, from)); block.isValid();
         block = block.next()) {
        const QTextBlockFormat shown = block.blockFormat();
        if (shown.lineHeightType() == mode &&
            (height <= 0 || qFuzzyCompare(shown.lineHeight(), height)))
            continue;
        if (!editing) {
            cursor.beginEditBlock(); // One layout for the whole change.
            editing = true;
        }
        QTextBlockFormat format;
        format.setLineHeight(height > 0 ? height : 100, mode);
        cursor.setPosition(block.position());
        cursor.mergeBlockFormat(format);
    }
    if (editing)
        cursor.endEditBlock();
}

void Theme::fieldLines(QQuickTextDocument *document, qreal height) const
{
    QTextDocument *doc = document ? document->textDocument() : nullptr;
    if (!doc)
        return;
    bool missing = false;
    for (QTextBlock block = doc->firstBlock(); block.isValid() && !missing; block = block.next()) {
        const QTextBlockFormat shown = block.blockFormat();
        missing = shown.lineHeightType() != QTextBlockFormat::MinimumHeight ||
                  !qFuzzyCompare(shown.lineHeight(), height);
    }
    if (!missing)
        return;
    const bool undo = doc->isUndoRedoEnabled();
    doc->setUndoRedoEnabled(false);
    lines(document, 0, height);
    doc->setUndoRedoEnabled(undo);
}

QColor Theme::alpha(const QColor &color, qreal opacity) const
{
    QColor out = color;
    out.setAlphaF(float(color.alphaF() * opacity));
    return out;
}

void Theme::settle(QQuickItem *item) const
{
    if (!item)
        return;
    const auto children = item->childItems();
    for (QQuickItem *child : children)
        settle(child);
    if (item->inherits("QQuickBasePositioner"))
        QMetaObject::invokeMethod(item, "forceLayout");
}

namespace
{
qreal aheadOfWindow(const QQuickItem *item)
{
    if (!item || !item->window())
        return 0;
    const QRectF area = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    return std::max(qreal(0), area.top() - item->window()->height());
}
} // namespace

qreal Theme::ahead(QQuickItem *item) const { return aheadOfWindow(item); }

// ---- Pacer ----------------------------------------------------------------

namespace
{
class Slices final : public QObject
{
  public:
    Slices() : QObject(QCoreApplication::instance())
    {
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(Pacer::SliceMs);
        connect(&m_timer, &QTimer::timeout, this, &Slices::drain);
    }

    void request(QObject *item, qreal priority)
    {
        m_waiting.removeIf([](const Wait &wait) { return wait.item.isNull(); });
        for (Wait &wait : m_waiting) {
            if (wait.item == item) {
                wait.priority = priority;
                return;
            }
        }
        roll();
        // In or above the view: never deferred.
        if (!m_pacing || priority <= 0 || (m_waiting.isEmpty() && fits())) {
            admit(item, priority > 0);
            return;
        }
        m_waiting.append({item, priority, m_order++});
        m_stats.mostWaiting = std::max(m_stats.mostWaiting, int(m_waiting.size()));
        if (!m_timer.isActive())
            m_timer.start();
    }

    void withdraw(QObject *item)
    {
        m_waiting.removeIf([item](const Wait &wait) { return wait.item == item; });
    }

    bool takeParse(int units)
    {
        roll();
        if (m_pacing && m_units > 0 &&
            (m_units + qint64(units) > Pacer::SyncUnits || !timeLeft())) {
            ++m_stats.deferred;
            return false;
        }
        m_units += units;
        use();
        m_stats.mostUnits = std::max(m_stats.mostUnits, int(m_units));
        return true;
    }

    void spent(qint64 nanoseconds)
    {
        m_spent += nanoseconds;
        burst(0, nanoseconds);
    }

    Pacer::Stats stats() const
    {
        Pacer::Stats out = m_stats;
        out.waiting = int(std::count_if(m_waiting.cbegin(), m_waiting.cend(),
                                        [](const Wait &wait) { return !wait.item.isNull(); }));
        return out;
    }

    void reset() { m_stats = {}; }
    void setPaced(bool paced) { m_pacing = paced; }

  private:
    struct Wait {
        QPointer<QObject> item;
        qreal priority;
        quint64 order;
    };

    // Starts a new slice once the current one is SliceMs old.
    void roll()
    {
        if (m_slice.isValid() && m_slice.elapsed() < Pacer::SliceMs)
            return;
        m_stats.over += m_spent > qint64(Pacer::SliceMs) * 1000000;
        m_slice.start();
        m_paced = 0;
        m_units = 0;
        m_spent = 0;
        m_used = false;
    }
    bool timeLeft() const { return m_spent < qint64(Pacer::SpendMs) * 1000000; }
    bool fits() const
    {
        return (m_paced == 0 && m_spent == 0) || (m_paced < Pacer::PerSlice && timeLeft());
    }
    void use()
    {
        if (!m_used) {
            m_used = true;
            ++m_stats.slices;
        }
    }

    // Building one block can create nested blocks that ask too: they count
    // as admitted, and their time once, in the outermost admission.
    void admit(QObject *item, bool paced)
    {
        use();
        m_paced += paced;
        ++m_stats.admitted;
        m_stats.mostPaced = std::max(m_stats.mostPaced, m_paced);
        QElapsedTimer timer;
        timer.start();
        ++m_depth;
        burst(1, 0);
        QMetaObject::invokeMethod(item, "admit");
        if (--m_depth == 0)
            spent(timer.nsecsElapsed());
    }

    // Counts work until the event loop runs again (tests).
    void burst(int admitted, qint64 nanoseconds)
    {
        if (!m_bursting) {
            m_bursting = true;
            m_burst = 0;
            m_burstNs = 0;
            QTimer::singleShot(0, this, [this] { m_bursting = false; });
        }
        m_burst += admitted;
        m_burstNs += nanoseconds;
        m_stats.large += admitted && m_burst == Pacer::PerSlice + 1;
        m_stats.mostBurst = std::max(m_stats.mostBurst, m_burst);
        m_stats.longestBurstMs = std::max(m_stats.longestBurstMs, m_burstNs / 1e6);
    }

    void drain()
    {
        roll();
        m_waiting.removeIf([](const Wait &wait) { return wait.item.isNull(); });
        // Where each waiting item is now: the view may have moved to it.
        QList<QPointer<QObject>> reached;
        for (Wait &wait : m_waiting) {
            if (auto *item = qobject_cast<QQuickItem *>(wait.item.data()))
                wait.priority = aheadOfWindow(item);
            if (wait.priority <= 0)
                reached << wait.item;
        }
        for (const QPointer<QObject> &item : std::as_const(reached)) {
            withdraw(item);
            if (item)
                admit(item, false);
        }
        while (fits()) {
            m_waiting.removeIf([](const Wait &wait) { return wait.item.isNull(); });
            if (m_waiting.isEmpty())
                break;
            const auto next = std::min_element(
                m_waiting.cbegin(), m_waiting.cend(), [](const Wait &a, const Wait &b) {
                    return a.priority < b.priority ||
                           (a.priority == b.priority && a.order < b.order);
                });
            const QPointer<QObject> item = next->item;
            m_waiting.erase(next);
            admit(item, true);
        }
        if (m_waiting.isEmpty())
            m_timer.stop();
    }

    QList<Wait> m_waiting;
    quint64 m_order = 0;
    QTimer m_timer;
    QElapsedTimer m_slice;
    int m_paced = 0, m_depth = 0;
    qint64 m_units = 0, m_spent = 0;
    bool m_used = false; // This slice admitted or parsed something.
    bool m_pacing = true;
    bool m_bursting = false;
    int m_burst = 0;
    qint64 m_burstNs = 0;
    Pacer::Stats m_stats;
};

Slices &slices()
{
    static QPointer<Slices> instance;
    if (!instance)
        instance = new Slices;
    return *instance;
}
} // namespace

void Pacer::request(QObject *item, qreal priority)
{
    if (item)
        slices().request(item, priority);
}

void Pacer::withdraw(QObject *item) { slices().withdraw(item); }

bool Pacer::takeParse(int units) { return slices().takeParse(units); }

void Pacer::spent(qint64 nanoseconds) { slices().spent(nanoseconds); }

Pacer::Stats Pacer::stats() { return slices().stats(); }

void Pacer::reset() { slices().reset(); }

void Pacer::setPaced(bool paced) { slices().setPaced(paced); }

// Generated by qt_add_qml_module for OpenGhost.Cpp (QML_ELEMENT types above).
void qml_register_types_OpenGhost_Cpp();

void registerNativeTypes()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    qRegisterMetaType<RichInline>();
    qRegisterMetaType<QVector<markdown::Run>>();
    qml_register_types_OpenGhost_Cpp();
}
