#pragma once

#include "markdown.h"
#include "motion.h"
#include "tex.h"
#include "wave.h"

#include <QAbstractAnimation>
#include <QAbstractListModel>
#include <QColor>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QQmlParserStatus>
#include <QQuickItem>
#include <QSyntaxHighlighter>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include <QQuickTextDocument>
class QTextDocument;
class RichDocument;

// Rich message presentation for QML. Message text is parsed off the GUI
// thread into markdown's inert tree and shown through allowlisted QML
// components: nested list models of blocks, TextEdit documents whose text is
// always plain and whose styles are char formats set here, and images drawn
// off the GUI thread. Nothing here parses HTML, resolves a URL or loads a
// resource: the only images a document holds are the formulas added below.

// An inline's text, styles and rendered formulas, as QML passes it along.
struct RichInline {
    Q_GADGET
    QML_ANONYMOUS
    Q_PROPERTY(QString plain READ plain CONSTANT)
  public:
    QString plain() const { return text.text; }
    markdown::Inline text;
    QVector<QImage> math; // Per formula; null when it could not be drawn.
    QVector<qreal> baselines;
};
Q_DECLARE_METATYPE(RichInline)
Q_DECLARE_METATYPE(QVector<markdown::Run>)

// A selectable text of a message (selection.h), in reading order: what its
// text component shows, which selection offsets index, and what separates
// it from the next text in a copy, as Chromium's selection serializes
// OpenGhost's DOM: after a paragraph (or a code block's label) an empty
// line, after any other block a line end, between a table row's cells a tab,
// after a diagram two empty lines.
struct SelectionUnit {
    QString path;          // Unique: "r" (blocks), "p" (plain rows) or "u" (a user
                           // message's parts), the block row's path and the text's
                           // part, as in "r/3/0:t".
    markdown::Inline text; // As shown; a formula or a diagram is one U+FFFC.
    QString copy;          // When set, copied instead of `text` (same length).
    bool display = false;  // A display formula: its one U+FFFC is the formula.
    // 'p' a line end, then an empty line once anything is copied; 'l' a line
    // end; 't' a tab; 'd' a line end, then two empty lines once anything is
    // copied; 0 nothing (selection.cpp, Writer).
    char after = 'l';
    QString diagram, lang; // A diagram: its source and fence language.
};

// One level of blocks: a document's top level, a quote's children, or a
// list's items (whose rows each hold that item's blocks). Rows are matched by
// position and replaced only when their kind changes, so delegates, text
// selections and nested rows survive a growing message.
class BlockModel final : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Blocks come from a RichDocument")
  public:
    enum Role {
        KindRole = Qt::UserRole + 1,
        TextRole,
        RoleRole,
        SourceRole,
        LangRole,
        LevelRole,
        FlagRole,
        OpenRole,
        ToneRole,
        SectionRole, // The tone's index (theme::tones; -1 neutral): a drawing's lead colour.
        TokensRole,
        CellsRole,
        ColumnsRole,
        AlignRole,
        PaletteRole,
        ChildrenRole,
        TaskRole,
        NumberRole,
        DepthRole,
        PathRole,
        GapRole,
        OmittedRole,
        ClippedRole,
        BornRole,
    };

    // What holds these blocks: it sets the space between them. Plain:
    // consecutive pieces of one plain text.
    enum class Container { Top, Quote, List, Item, Plain };
    BlockModel(RichDocument *document, Container container, int depth, QObject *parent);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    // A block row's content height at `width`, roughly (without the gap above
    // it), from the sizes its components use: the height of a block not built.
    Q_INVOKABLE qreal estimate(int row, qreal width) const;

    struct Node {
        markdown::BlockPtr block; // The block, or for an item row its list.
        int item = -1;            // An item row: its index in block->items.
    };
    void apply(const QVector<Node> &nodes, const QString &path);
    // The palette changed: every row's tone and palette, nested ones too.
    void retone();
    // The texts these rows show, nested ones too, in reading order, their
    // paths after `prefix`. `breaks` (plain rows): whether each row's text
    // ended at a line end the split consumed.
    void units(QVector<SelectionUnit> &out, const QString &prefix,
               const QVector<bool> *breaks = nullptr) const;

  private:
    struct Row {
        Node node;
        quint64 hash = 0;
        BlockModel *children = nullptr;
        // When the row was made (ms since the epoch, as QML's Date.now()):
        // content built soon after plays its entry motion (ChatEntry.fresh()).
        qint64 born = 0;
    };
    static QString kindOf(const Node &node);
    int gap(int row) const;
    static quint64 hashOf(const Node &node);
    QVector<Node> childNodes(const Node &node) const;
    RichInline rich(const markdown::Inline &text) const;

    RichDocument *m_document;
    Container m_container;
    int m_depth; // Lists around these blocks, for bullets.
    QString m_path;
    QVector<Row> m_rows;
};

// One message's Markdown. Setting `source` parses it on a worker pool (at
// most one parse in flight; the latest text wins) and applies the result to
// `blocks`. A message that grows while `live` is revealed at OpenGhost's paced
// rate (stream-view.js); settled text drains within half a second, and a
// message shown whole (history, a row scrolled back into view) is not paced.
class RichDocument : public QObject, public QQmlParserStatus
{
    Q_OBJECT
    QML_ELEMENT
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged)
    Q_PROPERTY(QString seed READ seed WRITE setSeed NOTIFY seedChanged)
    Q_PROPERTY(
        bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY reducedMotionChanged)
    Q_PROPERTY(qreal devicePixelRatio READ devicePixelRatio WRITE setDevicePixelRatio NOTIFY
                   devicePixelRatioChanged)
    Q_PROPERTY(QObject *blocks READ blocks CONSTANT)
    Q_PROPERTY(bool renderable READ renderable NOTIFY renderableChanged)
    Q_PROPERTY(bool empty READ empty NOTIFY shownChanged)
    Q_PROPERTY(bool revealing READ revealing NOTIFY revealingChanged)
    // A display-bound notice the transcript put in the text, shown apart.
    Q_PROPERTY(QString notice READ notice NOTIFY sourceChanged)
    // Text past markdown's MaxPieces, shown plain after the blocks.
    Q_PROPERTY(QString rest READ rest NOTIFY shownChanged)
    // Over the plain rows, why they are plain; empty when there are none to
    // explain. One of the message's selectable texts ("n:t").
    Q_PROPERTY(QString plainNote READ plainNote NOTIFY shownChanged)
    // What is shown plain, as plain paragraph rows of at most PlainLines
    // lines and PlainChunk units, split at line ends: `rest`, or the whole
    // message when it is too large to format. Rows, not one text, so that
    // only those near the view are built, as for blocks.
    Q_PROPERTY(QObject *plain READ plain CONSTANT)
    // More than EagerPieces formatted pieces, or text too large to format:
    // only rows near the view are built (audit P5-02). Smaller messages are
    // built whole, so their rows have their height when placed. It changes
    // before the rows it applies to do, so no row is built only to be dropped.
    Q_PROPERTY(bool lazy READ lazy NOTIFY lazyChanged)
    // A row made below the view (ChatEntry) is `paced`: its first parse is on
    // the GUI thread only within the Pacer's slice budget, else on the pool.
    // `hold` delays parsing until its maker has decided that.
    Q_PROPERTY(bool paced READ paced WRITE setPaced NOTIFY pacedChanged)
    Q_PROPERTY(bool hold READ hold WRITE setHold NOTIFY holdChanged)

  public:
    explicit RichDocument(QObject *parent = nullptr);
    ~RichDocument() override;

    QString source() const { return m_source; }
    QString notice() const { return m_notice; }
    QString rest() const { return m_doc.rest; }
    void setSource(const QString &source);
    bool live() const { return m_live; }
    void setLive(bool live);
    QString seed() const { return m_seed; }
    void setSeed(const QString &seed);
    bool reducedMotion() const { return m_reducedMotion; }
    void setReducedMotion(bool reduced);
    qreal devicePixelRatio() const { return m_dpr; }
    void setDevicePixelRatio(qreal dpr);
    QObject *blocks() { return &m_root; }
    QObject *plain() { return &m_plain; }
    bool renderable() const { return m_renderable; }
    bool empty() const { return m_root.rowCount() == 0; }
    QString plainNote() const;
    void classBegin() override {}
    void componentComplete() override;
    bool revealing() const { return m_clock.state() == QAbstractAnimation::Running; }
    bool lazy() const { return m_lazy; }
    bool paced() const { return m_paced; }
    void setPaced(bool paced);
    bool hold() const { return m_hold; }
    void setHold(bool hold);
    static constexpr int EagerPieces = 64;
    static constexpr int PlainChunk = 2048, PlainLines = 64;
    // A live parse re-reading more than CoalesceUnits (a long streaming
    // fence) starts at most every CoalesceMs (audit P5-10).
    static constexpr int CoalesceUnits = 16 * 1024, CoalesceMs = 100;

    // A row that arrived while streaming reveals from its start.
    Q_INVOKABLE void revealFromStart();
    // How many parses have been applied (tests).
    Q_INVOKABLE int parses() const { return m_parses; }

    QColor tone(int index) const;
    QVariantList palette(int headingIndex) const;
    // What is shown, as selectable texts in reading order (the blocks, then
    // the plain rows), and a count that changes whenever they may have.
    QVector<SelectionUnit> units() const;
    // The units a message's whole text would show, parsed here and now
    // without drawing anything: a selection's copy of a row that is not
    // built (selection.h).
    static QVector<SelectionUnit> unitsFor(const QString &source);
    quint64 shownVersion() const { return m_shownVersion; }
    // The inline formulas the shown blocks draw, and only those (audit
    // P5-04): at most MathBytes of images; formulas past that show their
    // source.
    const QHash<QString, tex::Rendered> &math() const { return m_math; }
    static constexpr qint64 MathBytes = 8 * 1024 * 1024;

  signals:
    void sourceChanged();
    void liveChanged();
    void seedChanged();
    void reducedMotionChanged();
    void devicePixelRatioChanged();
    void renderableChanged();
    void shownChanged();
    void lazyChanged();
    void pacedChanged();
    void holdChanged();
    void revealingChanged();
    // The shown text grew (a reveal frame or a parse), for layout followers.
    void grew();

  private:
    struct Result;
    // `mathBudget` >= 0: formulas not drawn before cost that much at most
    // (SyncMath), and past it are shown as source, the result `partial`.
    static void parse(Result &result, const markdown::Document &previous,
                      const QHash<QString, tex::Rendered> &previousMath,
                      const std::array<int, 7> &tones, qreal dpr, int mathBudget = -1,
                      const std::atomic_bool *cancel = nullptr);
    // The parse in flight will not be shown: it stops early.
    void abandonParse();
    static constexpr int SyncParse = 32 * 1024; // UTF-16 units parsed on the GUI thread once.
    // Formula drawing a GUI-thread parse may do, in units of a formula's
    // source length plus SyncMathEach (all of it measured about 2 ms); a
    // parse past it is finished on the pool.
    static constexpr int SyncMath = 2048, SyncMathEach = 32;
    void tick(qreal elapsed);
    void want();
    void schedule();
    // Drops every parsed block, row and parse in flight.
    void reset();
    void setLazy(bool lazy);
    // Shows `text` as the plain rows, reusing unchanged ones.
    void applyPlain(const QString &text);
    void finished(const std::shared_ptr<Result> &result);
    int cut() const;
    void applyRows();
    // While revealing, at most this many new top-level blocks appear per
    // frame, so a large drain never builds thousands of rows in one frame.
    static constexpr int RowsPerFrame = 8;

    QString m_source, m_seed, m_notice;
    bool m_live = false, m_reducedMotion = false, m_renderable = true, m_started = false;
    bool m_complete = false; // All bindings set: parsing may start.
    bool m_lazy = false;
    bool m_paced = false, m_hold = false;
    qreal m_dpr = 1;
    std::array<int, 7> m_tones{0, 1, 2, 3, 4, 5, 6};
    BlockModel m_root, m_plain;
    markdown::Blocks m_plainBlocks;
    QVector<bool> m_plainBreaks; // Each plain row ended at a consumed line end.
    quint64 m_shownVersion = 0;
    // Pacing (stream-view.js PACE).
    double m_shown = 0, m_rate = 0, m_from = 0, m_settled = 0;
    bool m_done = true;      // Not live: settled or never streamed.
    bool m_draining = false; // Settled with text still to reveal.
    FrameClock m_clock;
    // Parsing: what is shown, and the one parse in flight.
    markdown::Document m_doc;
    QHash<QString, tex::Rendered> m_math;
    bool m_parsing = false;
    bool m_rowsPending = false; // Parsed blocks not shown yet (RowsPerFrame).
    bool m_partial = false;     // Shown with formulas left for a pool parse.
    bool m_clipped = false;     // The source is a display prefix (the transcript's notice).
    QString m_wantText;
    bool m_wantLive = false;
    QElapsedTimer m_parsed; // Since the last parse was applied.
    QTimer m_coalesce;
    quint64 m_generation = 0;
    int m_parses = 0;
};

// Keeps a read-only TextEdit's plain text and char formats equal to an
// inline: appends only the new suffix (keeping the reader's selection),
// restyles only what changed, and shows formulas as images it adds to the
// document itself. Links carry an anchor for linkActivated; nothing loads.
class InlineFormat : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(QVariant content READ content WRITE setContent NOTIFY contentChanged)
    Q_PROPERTY(qreal pixelSize READ pixelSize WRITE setPixelSize NOTIFY pixelSizeChanged)
    Q_PROPERTY(qreal lineHeight READ lineHeight WRITE setLineHeight NOTIFY lineHeightChanged)
    // The line box's room beyond the font's own height (CSS half-leading × 2).
    Q_PROPERTY(qreal leading READ leading NOTIFY leadingChanged)
    // What CSS paints around inline runs and Qt's text formats cannot: the
    // padded, rounded boxes of inline code and link chips, a chip's globe
    // and a link's offset underline. One map per line fragment in document
    // coordinates: kind ("code", "chip", "globe" or "underline"), x, y, w,
    // h, whether the fragment opens (`first`) and closes (`last`) its run,
    // and a link's href. The room a box's padding takes is letter spacing
    // on the characters around the run, so the text itself is unchanged.
    Q_PROPERTY(QVariantList decorations READ decorations NOTIFY decorationsChanged)
    // The hovered link (TextEdit's hoveredLink): its chip is lit.
    Q_PROPERTY(QString hoveredLink READ hoveredLink WRITE setHoveredLink NOTIFY hoveredLinkChanged)
    // New text waves in as OpenGhost's streamed words do (TextWave).
    Q_PROPERTY(TextWave *wave MEMBER m_wave NOTIFY waveChanged)
    // Set, naturalWidth is the widest block's width unwrapped (CSS
    // max-content), for boxes that shrink to their text.
    Q_PROPERTY(bool natural READ natural WRITE setNatural NOTIFY naturalChanged)
    Q_PROPERTY(qreal naturalWidth READ naturalWidth NOTIFY naturalWidthChanged)

  public:
    explicit InlineFormat(QObject *parent = nullptr);
    qreal lineHeight() const { return m_lineHeight; }
    void setLineHeight(qreal height);
    qreal leading() const { return m_leading; }
    QQuickItem *target() const { return m_target; }
    void setTarget(QQuickItem *target);
    QVariant content() const { return QVariant::fromValue(m_content); }
    void setContent(const QVariant &content);
    qreal pixelSize() const { return m_pixelSize; }
    void setPixelSize(qreal size);
    QVariantList decorations() const { return m_decorations; }
    QString hoveredLink() const { return m_hoveredLink; }
    void setHoveredLink(const QString &link);
    bool natural() const { return m_natural; }
    void setNatural(bool natural);
    qreal naturalWidth() const { return m_naturalWidth; }

    // Text of a selection, a formula's TeX in place of its image.
    static QString selected(QTextDocument *document, int start, int end);

  signals:
    void targetChanged();
    void contentChanged();
    void pixelSizeChanged();
    void lineHeightChanged();
    void leadingChanged();
    void decorationsChanged();
    void hoveredLinkChanged();
    void naturalChanged();
    void naturalWidthChanged();
    void waveChanged();
    // The target's selection stopped changing (120 ms): copy it.
    void selectionSettled();

  private slots:
    void sync();
    void measure();
    void decorate();

  private:
    QTextDocument *document() const;
    QImage aligned(int ref) const;
    void pad(QTextCursor &cursor, int from);
    void lightChips();
    void scheduleDecorate();

    QPointer<QQuickItem> m_target;
    RichInline m_content, m_shown;
    qreal m_pixelSize = 16;
    qreal m_lineHeight = 1.65; // CSS line-height: a multiple of the font size.
    qreal m_leading = 0;
    QTimer m_settle; // Started only while a selection changes.
    bool m_attached = false;
    QVariantList m_decorations;
    QString m_hoveredLink;
    bool m_natural = false;
    qreal m_naturalWidth = 0;
    bool m_decorating = false;  // A decorate() is queued.
    QPointer<QObject> m_layout; // The document layout decorate() follows.
    QPointer<TextWave> m_wave;
};

// Highlights a code TextEdit from precomputed runs (made off the GUI thread
// with the parse): per changed text block only, and never changing the text.
class CodeFormat : public QSyntaxHighlighter
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(QVariant tokens READ tokens WRITE setTokens NOTIFY tokensChanged)
    Q_PROPERTY(QColor tone READ tone WRITE setTone NOTIFY toneChanged)

  public:
    explicit CodeFormat(QObject *parent = nullptr);
    QQuickTextDocument *document() const { return m_document; }
    void setDocument(QQuickTextDocument *document);
    QVariant tokens() const { return QVariant::fromValue(m_tokens); }
    void setTokens(const QVariant &tokens);
    QColor tone() const { return m_tone; }
    void setTone(const QColor &tone);

  signals:
    void documentChanged();
    void tokensChanged();
    void toneChanged();

  protected:
    void highlightBlock(const QString &text) override;

  private:
    QPointer<QQuickTextDocument> m_document;
    QVector<markdown::Run> m_tokens;
    QColor m_tone;
};

// A display formula drawn off the GUI thread and shown as one texture (a
// diagram is a DiagramImage). Once something was drawn or asked for, source
// changes while `live` wait until they pause; the last good image stays
// until a new one is ready. Every change of what is drawn (source, kind,
// device pixel ratio, theme) starts a new request, and only the current
// request's result is shown; while `live`, a drawing of text the source
// still starts with is shown as progress, and any other drawing in flight is
// told to stop. Nothing is drawn while the parent is hidden; a change waits
// until it is shown. A failure sets `error` and shows no formula (its source
// is shown instead).
class RichImage : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY kindChanged) // "math" or "image"
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged)
    Q_PROPERTY(bool ok READ ok NOTIFY resultChanged)
    Q_PROPERTY(QString error READ error NOTIFY resultChanged)
    Q_PROPERTY(bool pending READ pending NOTIFY pendingChanged)
    Q_PROPERTY(QString shownSource READ shownSource NOTIFY resultChanged)
    Q_PROPERTY(int renders READ renders NOTIFY resultChanged)
    // kind "image": an image already decoded (an attachment preview).
    Q_PROPERTY(QImage image READ image WRITE setImage NOTIFY resultChanged)

  public:
    explicit RichImage(QQuickItem *parent = nullptr);
    ~RichImage() override;
    QString kind() const { return m_kind; }
    void setKind(const QString &kind);
    QString source() const { return m_source; }
    void setSource(const QString &source);
    bool live() const { return m_live; }
    void setLive(bool live);
    bool ok() const { return m_drawn; }
    QString error() const { return m_error; }
    bool pending() const { return m_rendering || m_timer.isActive(); }
    QString shownSource() const { return m_shownSource; }
    int renders() const { return m_renders; }
    QImage image() const { return m_image; }
    void setImage(const QImage &image);

    // Debounce before rendering a changed source that is still changing.
    static constexpr int LiveDelayMs = 150;
    // The process's drawing cache: at most CacheBytes held, in at most
    // CacheEntries drawings. Failures of sources longer than CachedFailure
    // are not cached (drawn again if needed).
    static constexpr qint64 CacheBytes = 48 * 1024 * 1024;
    static constexpr int CacheEntries = 256;
    static constexpr int CachedFailure = 4096;

  signals:
    void kindChanged();
    void sourceChanged();
    void liveChanged();
    void resultChanged();
    void pendingChanged();

  protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

  private:
    void changed(int delay);
    void render();
    qreal ratio() const;
    QByteArray drawingKey() const;
    bool fromCache();
    QString inputsKey() const;
    // The parent is visible (the item's own `visible` shows its result).
    bool shown() const;
    // A drawing of `source` with `inputs` can be shown now as progress.
    bool progress(const QString &inputs, const QString &source) const;
    void show(bool ok, const QImage &image, QSizeF size, const QString &error,
              const QString &source);
    // Drawings cached in the process (tests): entries, their accounted cost
    // and the bytes they actually hold.
    struct Cached {
        int count = 0;
        qint64 cost = 0, bytes = 0;
    };
    static Cached cached();
    friend class RichTest;

    QString m_kind, m_source, m_shownSource, m_error;
    bool m_live = false, m_rendering = false, m_again = false, m_dirty = false;
    // A job is drawing (not just waiting for a worker), of these inputs.
    bool m_flying = false;
    QString m_flightInputs, m_flightSource;
    bool m_hidden = false; // A drawing waits until the parent is visible.
    QMetaObject::Connection m_parentShown;
    QImage m_image;       // Waiting for upload; drawings drop it once uploaded.
    bool m_drawn = false; // A drawing is shown (or waits to be).
    QTimer m_timer;
    quint64 m_request = 0; // Advanced whenever what is drawn changes.
    int m_renders = 0;
    std::function<void()> m_beforeDraw; // Tests: runs on the worker before drawing.
};

// Spreads building near-view rich content over time (audit P6-07), as the
// `Pacer` singleton. A block or table row that comes near the transcript's
// view asks here (request) instead of building at once, and is built when
// this calls its admit(). Content in the window is built at once: the frame
// needs it. So is content above it, whose growth would move what the reader
// sees while a gesture holds no anchor (Main.qml). Content below the window,
// built ahead of the view, only grows the end, so it is paced: time is cut
// into slices of SliceMs, about one 240 Hz frame, and a slice admits waiting
// content nearest to the view first while it has paced fewer than PerSlice
// and spent less than SpendMs of GUI time; a fresh slice always admits one.
// A new row made below the view parses on the GUI thread (RichDocument) only
// within the same time budget and at most SyncUnits per slice; otherwise on
// the rich pool. Content no longer near withdraws. State is process-wide;
// every engine's singleton shares it.
class Pacer final : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

  public:
    static constexpr int SliceMs = 4, SpendMs = 1, PerSlice = 6, SyncUnits = 32 * 1024;
    // `item` has an admit() method. Lower `priority` (distance from the view,
    // in pixels) is admitted first. Asking again updates the priority, and
    // each slice updates a waiting item's from where it now is (Theme.ahead),
    // so content the view has since reached is built at once.
    Q_INVOKABLE void request(QObject *item, qreal priority);
    Q_INVOKABLE void withdraw(QObject *item);
    // Whether a GUI-thread parse of `units` fits the current slice; if so it
    // is counted, and the parse reports its time with spent().
    static bool takeParse(int units);
    static void spent(qint64 nanoseconds);

    // Tests: waiting content and, since reset(), the most that waited, the
    // slices that admitted or parsed and those that spent more than SliceMs,
    // the most paced in one slice, the most units parsed on the GUI thread in
    // one slice, the first parses sent to the pool, and bursts: work before
    // the event loop ran again. The most admitted and GUI time spent in one,
    // and how many admitted more than PerSlice.
    struct Stats {
        int waiting = 0, mostWaiting = 0, slices = 0, over = 0, mostPaced = 0, mostUnits = 0,
            deferred = 0, mostBurst = 0, large = 0;
        quint64 admitted = 0;
        qreal longestBurstMs = 0;
    };
    static Stats stats();
    static void reset();
    // Tests: false admits everything and allows every parse at once, as
    // before pacing, to compare against.
    static void setPaced(bool paced);
    Q_INVOKABLE int waiting() const { return stats().waiting; }
};

// Announces the palette on screen changing (theme.h's lightShown), process
// wide: `changing` comes before the change (a reveal takes its picture of the
// old theme then), `changed` after it. `origin` is the point a picked theme
// spreads from; a null one (the system switched) fades.
class ThemeSignal final : public QObject
{
    Q_OBJECT
  public:
    static ThemeSignal *instance();

  signals:
    void changing(QPointF origin);
    void changed();
    void choiceChanged();
};

// OpenGhost's palettes and motion settings for QML (theme.h's values), as the
// `Theme` singleton. The theme is OpenGhost 1.2's choice (Settings →
// Appearance): "dark" (the default), "light" or "system", which follows the
// desktop's colour scheme. Reduced motion follows OPENGHOST_REDUCED_MOTION=1/0
// when set, else KDE's AnimationDurationFactor (0 disables animation).
class Theme final : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString choice READ choice WRITE setChoice NOTIFY choiceChanged)
    Q_PROPERTY(bool light READ light NOTIFY paletteChanged)
    Q_PROPERTY(QColor fg MEMBER fg NOTIFY paletteChanged) // --fg-rgb, opaque
    Q_PROPERTY(QColor appBg MEMBER appBg NOTIFY paletteChanged)
    Q_PROPERTY(QColor chatBg MEMBER chatBg NOTIFY paletteChanged)
    Q_PROPERTY(QColor contourOuter MEMBER contourOuter NOTIFY paletteChanged)
    Q_PROPERTY(QColor contourInner MEMBER contourInner NOTIFY paletteChanged)
    Q_PROPERTY(QColor composerBg MEMBER composerBg NOTIFY paletteChanged)
    Q_PROPERTY(QColor composerBorder MEMBER composerBorder NOTIFY paletteChanged)
    Q_PROPERTY(QColor accent MEMBER accent NOTIFY paletteChanged)
    Q_PROPERTY(QColor onAccent MEMBER onAccent NOTIFY paletteChanged)
    Q_PROPERTY(QColor muted MEMBER muted NOTIFY
                   paletteChanged) // --composer-fg: placeholder and composer icons
    Q_PROPERTY(QColor text MEMBER text NOTIFY paletteChanged)
    Q_PROPERTY(QColor strong MEMBER strong NOTIFY paletteChanged)
    Q_PROPERTY(QColor secondary MEMBER secondary NOTIFY paletteChanged)
    Q_PROPERTY(QColor tertiary MEMBER tertiary NOTIFY paletteChanged)
    Q_PROPERTY(QColor quaternary MEMBER quaternary NOTIFY paletteChanged)
    Q_PROPERTY(QColor link MEMBER link NOTIFY paletteChanged)
    Q_PROPERTY(QColor success MEMBER success NOTIFY paletteChanged)
    Q_PROPERTY(QColor danger MEMBER danger NOTIFY paletteChanged)
    Q_PROPERTY(QColor warn MEMBER warn NOTIFY paletteChanged)   // --warn-rgb
    Q_PROPERTY(QColor hover MEMBER hover NOTIFY paletteChanged) // --control-hover-bg
    Q_PROPERTY(QColor rowActive MEMBER rowActive NOTIFY paletteChanged)
    Q_PROPERTY(QColor rowHover MEMBER rowHover NOTIFY paletteChanged) // --row-hover-bg
    // --frame for shaders/frame.frag: per light, its colour then its alphas
    // at its three stops (theme::Palette::frame).
    Q_PROPERTY(QVariantList frame MEMBER frame NOTIFY paletteChanged)
    // --splash-* (theme::Palette::Splash), by name: night, mistDeep, mistLit,
    // glow, core, aura, glowRgb, mistAlpha, glowAlpha, trail, motes, shade.
    Q_PROPERTY(QVariantMap splash MEMBER splash NOTIFY paletteChanged)
    Q_PROPERTY(QColor selection MEMBER selection NOTIFY paletteChanged)
    Q_PROPERTY(QColor backdrop MEMBER backdrop NOTIFY paletteChanged)
    Q_PROPERTY(QColor tipBg MEMBER tipBg NOTIFY paletteChanged)
    Q_PROPERTY(QColor wellBg MEMBER wellBg NOTIFY paletteChanged)
    Q_PROPERTY(QColor diffAdded MEMBER diffAdded NOTIFY paletteChanged)
    Q_PROPERTY(QColor diffRemoved MEMBER diffRemoved NOTIFY paletteChanged)
    Q_PROPERTY(QColor noteBg MEMBER noteBg NOTIFY paletteChanged)
    Q_PROPERTY(QColor noteHoverBg MEMBER noteHoverBg NOTIFY paletteChanged)
    Q_PROPERTY(QColor glow MEMBER glow NOTIFY paletteChanged)
    Q_PROPERTY(QColor paintShade MEMBER paintShade NOTIFY paletteChanged)
    Q_PROPERTY(qreal shadow MEMBER shadow NOTIFY paletteChanged)
    Q_PROPERTY(QVariantList tones MEMBER tones NOTIFY paletteChanged)
    Q_PROPERTY(QString mono MEMBER mono CONSTANT)
    Q_PROPERTY(QVariantList motion MEMBER motion CONSTANT) // cubic-bezier(0.32, 0.72, 0, 1)
    Q_PROPERTY(QVariantList ease MEMBER ease CONSTANT) // CSS ease: cubic-bezier(0.25, 0.1, 0.25, 1)
    Q_PROPERTY(
        bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY reducedMotionChanged)

  public:
    explicit Theme(QObject *parent = nullptr);
    bool reducedMotion() const { return m_reducedMotion; }
    void setReducedMotion(bool reduced);
    QString choice() const { return chosen(); }
    void setChoice(const QString &choice) { choose(choice); }
    bool light() const { return m_light; }
    // Picks a theme from a point on the window (a Settings card): the new
    // theme spreads from there over the old one.
    Q_INVOKABLE void pick(const QString &choice, qreal x, qreal y)
    {
        choose(choice, QPointF(x, y));
    }
    // The process-wide choice. An unknown one is ignored.
    static QString chosen();
    static void choose(const QString &choice, QPointF origin = {});
    static const QStringList &choices();
    // Whether the desktop asks for a dark scheme ("system" follows it). Tests
    // can set it; unknown (Qt::ColorScheme::Unknown) counts as dark.
    static void setSystemDark(std::optional<bool> dark);
    // A tone at an opacity, for QML (rgba(var(--tone), a)).
    Q_INVOKABLE QColor alpha(const QColor &color, qreal opacity) const;
    // QML reads the tokens as properties, so a binding follows a theme
    // change: rgba(var(--fg-rgb), a) is alpha(fg, a), a drop shadow's colour
    // alpha("black", a * shadow).
    // A palette's tokens whichever theme shows (Appearance's previews):
    // fg, appBg, chatBg, contourOuter, contourInner, composerBg,
    // composerBorder, accent, onAccent, shadow and frame.
    Q_INVOKABLE QVariantMap paletteOf(bool light) const;
    // CSS cubic-bezier(x1, y1, x2, y2) at t, for animations QML drives
    // itself (a reversible exit retraces its curve).
    Q_INVOKABLE qreal bezier(qreal x1, qreal y1, qreal x2, qreal y2, qreal t) const
    {
        return cubicBezier(x1, y1, x2, y2, t);
    }
    // --motion-easing: cubic-bezier(0.32, 0.72, 0, 1).
    Q_INVOKABLE qreal motionAt(qreal t) const { return cubicBezier(0.32, 0.72, 0, 1, t); }
    // Monotonic milliseconds (FrameClock's time base), for timelines QML reads
    // per frame: what starts mid-frame shows that frame's progress on the
    // next, as on a CSS document timeline (a FrameAnimation's first
    // frameTime after a start is 0).
    Q_INVOKABLE qreal clock() const { return FrameClock::now() * 1000; }
    // Font points for a CSS pixel size (QML pixel sizes are whole numbers).
    Q_INVOKABLE qreal points(qreal pixels) const { return pointsFor(pixels); }
    static qreal pointsFor(qreal pixels);
    // Lays out the positioners (Column, Row, Flow) in `item` now, innermost
    // first, instead of at the next polish: a transcript row then has its
    // full height when the list places it, not a partial one that grows.
    Q_INVOKABLE void settle(QQuickItem *item) const;
    // How far `item` lies below its window's bottom edge, in pixels: 0 when
    // any of it is in the window or above it. The Pacer builds content with 0
    // at once, and nearer content first.
    Q_INVOKABLE qreal ahead(QQuickItem *item) const;
    // Makes every line of `document`'s blocks from the one holding `from` on
    // `height` px tall (CSS line-height; 0 leaves the font's own), as
    // InlineFormat does; a plain TextEdit has no line height of its own.
    Q_INVOKABLE void lines(QQuickTextDocument *document, int from, qreal height) const;
    // lines() for an editable field's whole document, kept off its undo
    // history (Undo must not revert a line height). New blocks inherit the
    // format, so only a replaced document (text set, which also resets the
    // history) needs it again; only a check when nothing is missing.
    Q_INVOKABLE void fieldLines(QQuickTextDocument *document, qreal height) const;
    // --mono-font (ui-monospace, "Cascadia Code", Consolas, monospace) as
    // Chromium resolves it on Linux, which has no ui-monospace: the first
    // installed of the next two, else fontconfig's monospace.
    static QString monoFamily();
    // The face weight CSS font matching takes for `weight` among `family`'s
    // upright faces, as Chromium draws it: 600 in a family without a
    // SemiBold is Bold, not the Medium Qt's nearest match picks.
    static int cssWeight(const QString &family, int weight);
    // cssWeight in the application font's family, for QML.
    Q_INVOKABLE int weight(int css) const;
    // CSS half-leading: half of a `lineHeight` px line's room beyond
    // `font`'s own height. A FixedHeight Text draws at its line's top, so
    // CSS's text sits this much lower.
    Q_INVOKABLE qreal halfLeading(const QFont &font, qreal lineHeight) const
    {
        return (lineHeight - QFontMetricsF(font).height()) / 2;
    }

    QColor fg, appBg, chatBg, contourOuter, contourInner, composerBg, composerBorder, accent,
        onAccent, muted, text, strong, secondary, tertiary, quaternary, link, success, danger, warn,
        hover, rowActive, rowHover, selection, backdrop, tipBg, wellBg, diffAdded, diffRemoved,
        noteBg, noteHoverBg, glow, paintShade;
    qreal shadow = 1;
    QVariantList tones;
    QVariantList frame;
    QVariantMap splash;
    QString mono;
    QVariantList motion;
    QVariantList ease;

  signals:
    void reducedMotionChanged();
    void choiceChanged();
    void paletteChanged();

  private:
    void applyPalette();
    bool m_reducedMotion = false;
    bool m_light = false;
};

// A saved image decoded for a preview: PNG, JPEG or WebP only, at most
// PreviewSide pixels on its longer side (orientation applied), refusing
// images whose header declares more than PreviewSource pixels or a larger
// side than 16384, and never allocating more than 128 MiB. Pure; call it off
// the GUI thread.
struct Preview {
    QImage image;
    QString error;
};
constexpr int PreviewSide = 720;
constexpr qint64 PreviewSource = 40'000'000;
Preview decodePreview(const QByteArray &bytes, int side = PreviewSide);
// decodePreview on the rich pool; `done` runs on `receiver`'s thread unless
// it was destroyed meanwhile. A job whose `dropped` is set before it starts
// decodes nothing and answers an empty Preview.
void decodePreviewAsync(QObject *receiver, const QByteArray &bytes,
                        std::function<void(Preview)> done,
                        std::shared_ptr<const std::atomic_bool> dropped = {});

// Rich work outstanding in the process (tests): owners waiting for a worker,
// jobs running or waiting to answer, and jobs started so far.
struct RichWork {
    int queued = 0, running = 0;
    quint64 started = 0;
};
RichWork richWork();
// Ends rich work as the application's exit does: waiting work is dropped,
// running work is told to stop, and only that is waited for. Work asked for
// afterwards runs as usual.
void stopRichWork();
// Tests: runs on the worker before each rich job starts.
void setBeforeRichJob(std::function<void()> before);
// The rich pool for other owners in this module (DiagramImage): `make` runs
// here when a worker is free and returns the job (empty: nothing now); the
// job runs on the worker and returns what runs back here.
using RichCancel = std::shared_ptr<std::atomic_bool>;
using RichJob = std::function<std::function<void()>()>;
using RichMake = std::function<RichJob(const RichCancel &)>;
void wantRichWork(QObject *owner, RichMake make);
void cancelRichWork(QObject *owner);
void forgetRichWork(QObject *owner);

// Registers the OpenGhost.Native QML module (these types, the Ghost and the
// icons) before QML loads.
void registerNativeTypes();
