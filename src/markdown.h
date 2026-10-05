#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

// Markdown as an inert display tree (OpenGhost's markdown.js): text with
// style runs and allowlisted block kinds, never HTML. Raw HTML in the source
// is ordinary text, links are http(s)/mailto only, and nothing here loads,
// fetches or executes anything. Pure and reentrant; parse off the GUI thread.
namespace markdown
{
// Inline styles; they combine. A link chip (link-chip.js: a bare URL shown
// as its host) is Link | Chip, apart from code inside a link's label.
enum Format : quint8 {
    Strong = 1,
    Emphasis = 2,
    Strike = 4,
    Code = 8,
    Link = 16,
    Math = 32,
    Chip = 64
};

// A styled range of display text. For code, `format` is a highlight::Token.
struct Run {
    int start = 0, length = 0;
    quint8 format = 0;
    int ref = -1; // Link: index into Inline::links (-1: a link still streaming); Math: into math.
    bool operator==(const Run &other) const
    {
        return start == other.start && length == other.length && format == other.format &&
               ref == other.ref;
    }
    bool operator!=(const Run &other) const { return !(*this == other); }
};

// Display text without markers. Each inline formula is one U+FFFC standing
// for math[ref] in a Math run.
struct Inline {
    QString text;
    QVector<Run> runs; // Sorted and disjoint.
    QStringList links; // Safe hrefs only.
    QStringList math;  // TeX sources.
    bool operator==(const Inline &other) const
    {
        return text == other.text && runs == other.runs && links == other.links &&
               math == other.math;
    }
};

constexpr QChar Object = QChar(0xFFFC);

enum class Kind : quint8 {
    Paragraph,
    Heading,
    Code,
    Diagram,
    Math,
    Table,
    Rule,
    Flow,
    List,
    Quote,
    Media
};

// A picture or a video a media block shows (markdown.js mediaItems): a
// picture is ![caption](src), or the same inside a link to the page it is
// from (href); a video is a link to it, with words (caption) or bare (src).
struct MediaItem {
    bool video = false;
    QString src;     // Picture: its address; video: the link.
    QString caption; // Picture: its caption; video: the link's words.
    QString href;    // Picture: the page it is from (safe hrefs only), or empty.
    bool operator==(const MediaItem &other) const
    {
        return video == other.video && src == other.src && caption == other.caption &&
               href == other.href;
    }
};

// What a paragraph shows when it is nothing but pictures and links to
// videos, or nothing when it is a paragraph like any other. While `live`,
// its last picture may be half written: what came before it is shown and
// `open` says one more is on its way.
struct MediaList {
    QVector<MediaItem> items;
    bool open = false;
};
bool mediaItems(const QString &text, bool live, MediaList &out);

struct Block;
using BlockPtr = std::shared_ptr<const Block>;
using Blocks = QVector<BlockPtr>;

struct Item {
    Blocks blocks;
    qint8 task = -1; // -1: not a task; 0: open; 1: done.
};

struct Block {
    Kind kind = Kind::Paragraph;
    int tone = -1;         // theme::tones index; -1: the neutral tone.
    Inline text;           // Paragraph, heading; quote: its citation's name.
    Inline role;           // Quote: its citation's role.
    int level = 0;         // Heading 1–6; ordered list start.
    bool flag = false;     // Heading: pseudo; code: art; list: ordered; quote: pull quote;
                           // table/diagram: wide.
    bool open = false;     // Code, diagram, math: the fence is still streaming; media: the
                           // pictures are still being written.
    bool clipped = false;  // Code, diagram, math: the fence is unclosed where the text was cut
                           // off (Options::clipped), so its source may be incomplete.
    QString source;        // Code, diagram and math source, exact.
    QString lang;          // Code label; diagram kind; callout kind.
    QVector<Run> tokens;   // Code highlight runs.
    int resume = 0;        // Code: where a longer source's highlighting continues.
    QVector<Inline> cells; // Flow parts; table header then rows, `columns` wide.
    QVector<quint8> align; // Table columns: 0 left, 1 center, 2 right.
    int columns = 0;       // Table.
    int headingIndex = -1; // Diagram and flow: the heading count before it.
    Blocks children;       // Quote.
    QVector<Item> items;   // List.
    int omitted = 0;       // List items or table rows past the bounds below.
    int pieces = 1;        // Displayed pieces, nested ones included (see MaxPieces).
    quint64 hash = 0;      // Content, children included.
    // Media: its pictures and videos.
    QVector<MediaItem> media;
};

// Quotes and lists nest at most this deep; deeper ones are one plain
// paragraph, so no text is dropped and nothing recurses without bound.
constexpr int MaxDepth = 16;
// Link labels nest at most this deep (markdown.js INLINE.depth).
constexpr int InlineDepth = 8;
// UTF-8 bytes of one message rendered; longer text is shown plain.
constexpr int RenderBytes = 256 * 1024;
// Each displayed piece is a native component: a block, a list item, a table
// cell, a flow step or a quote's citation. One message shows at most
// MaxPieces of them, every nesting level sharing that one budget, which is
// charged before a piece is built. From the first top-level block that does
// not fit (the first block included), the rest of the message is plain text;
// so is a table wider than MaxColumns. Lists show at most MaxItems items and
// tables MaxRows rows, the rest counted. Copy always has the whole message.
constexpr int MaxPieces = 1000;
constexpr int MaxColumns = 24;
constexpr int MaxItems = 300;
constexpr int MaxRows = 300;

struct Options {
    bool live = false;                    // Still streaming: hide partial syntax at the end.
    bool clipped = false;                 // The text is a display prefix: an unclosed fence
                                          // at its end is marked Block::clipped.
    std::array<int, 7> tones{};           // The message's tone order (theme::tones indexes).
    std::function<void(Inline &)> finish; // Called for every Inline built (math rendering).
    // Set by another thread: the parse stops at the next block, and its
    // incomplete result is to be discarded.
    const std::atomic_bool *cancel = nullptr;
};

// A parse that later text can resume: blocks before the last are final
// while the text only grows.
struct Document {
    QString text;        // What was parsed.
    Blocks blocks;       // Top level.
    QVector<int> starts; // Each top-level block's first source offset.
    int headings = 0;    // Headings among the blocks before the last.
    bool live = false;
    bool clipped = false;
    QString rest; // Text past MaxPieces, from a block's start; shown plain.
};

// Parses `text`; when it extends `previous.text` with the same liveness,
// every top-level block before the previous last one is reused as is.
Document parse(const QString &text, const Options &options, const Document &previous = {});

// True when `text` is within RenderBytes of UTF-8.
bool renderable(const QString &text);

// Inline text alone (table cells, citations): exposed for tests.
Inline inlineText(const QString &source, bool live = false);

// link-chip.js label(): the short host name a link chip shows.
QString linkLabel(const QString &url);

// Replaces formula `ref` (a U+FFFC) with its source shown as code.
void expandMath(Inline &text, int ref);

// Tone order for a message: Markdown.TONES shuffled by a seed.
std::array<int, 7> tonesFor(const QString &seed);
} // namespace markdown
