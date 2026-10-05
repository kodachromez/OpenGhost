#pragma once

#include "rich.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QVariantAnimation>
#include <QtQml/qqmlregistration.h>

#include <unordered_map>

// One text selection across the conversation (OpenGhost's DOM selection):
// from an anchor to a focus, each a text of a transcript row
// (SelectionUnit) and an offset into what it shows, so it crosses
// paragraphs, headings, lists, quotes, code, tables, formulas and diagrams,
// and the replies, user messages and notes around them, as one range. Each
// text is its own item, so Qt's per-item selection stops at its edges; here the
// range is data, kept by row keys and text paths rather than by items. Items
// come and go as the transcript builds blocks near the view (Block.qml) and
// rows near it (ListView), and a streaming reply grows; the range survives
// both, and what it copies comes from the rows' documents (or, for a row not
// built, from its text in the transcript model), never from items, so a
// selection reaching far outside the view copies exactly what it covers.
//
// The items that show a row's texts enrol with its key and their unit's
// path; each paints its own part of the range (SelectionWash). Block boxes
// enrol too: the veil's cut-out (selection-focus.js) takes the box of every
// block wholly inside the range, as Range.getClientRects() does.
//
// selection-menu.js's rule decides the menu and the veil: a range inside one
// reply brings them; one that runs on from a reply into nothing but the
// start of what follows (or the end of what precedes) is cut back to the
// reply and brings them; one that takes in another message's words brings
// neither.
//
// One selection exists in the window (a QML singleton): starting one ends any
// other, as a new DOM selection does.
class ReplySelection final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Selection)
    QML_SINGLETON
    // The row (a transcript key) holding the whole range, empty when there is
    // none or the range spans several rows.
    Q_PROPERTY(QString row READ row NOTIFY changed)
    // A range that is not empty.
    Q_PROPERTY(bool active READ active NOTIFY changed)
    // A press is extending the range (the pointer is down).
    Q_PROPERTY(bool dragging READ dragging NOTIFY changed)
    // Let go with a reply's text selected: the menu and the veil are up
    // (selection-menu.js's is-shown). Hiding them keeps the range.
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)
    // The range's wash has stepped back (.select-held): from the menu showing
    // until the range is gone, even after the menu has been hidden.
    Q_PROPERTY(bool held READ held NOTIFY heldChanged)
    // --selection-alpha: 0.3, easing to 0 in 0.45 s (CSS ease) once held and
    // back to 0.3 at once when the range goes.
    Q_PROPERTY(qreal wash READ wash NOTIFY washChanged)
    // Changes with the range or with what its rows show.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    // Changes when the rows' items, as enrolled, may have moved.
    Q_PROPERTY(int layout READ layout NOTIFY layoutChanged)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    // The transcript's rows (TranscriptModel: key, kind, body, attachments,
    // copyable): their order, and the text of rows that are not built.
    Q_PROPERTY(QAbstractItemModel *model READ model WRITE setModel NOTIFY modelChanged)
    // The width a diagram is laid out at when its row is not built.
    Q_PROPERTY(qreal diagramWidth MEMBER m_diagramWidth NOTIFY diagramWidthChanged)
    // A link dragged starts a system drag (off in tests: linkDragged only).
    Q_PROPERTY(bool systemDrag MEMBER m_systemDrag NOTIFY systemDragChanged)
    Q_PROPERTY(int dragDistance READ dragDistance CONSTANT)

  public:
    explicit ReplySelection(QObject *parent = nullptr);

    static constexpr qreal Wash = 0.3;
    static constexpr int WashMs = 450;
    // Chromium's drag threshold: a press on a link that moves this far on
    // either axis drags the link; less, and it is a click.
    static constexpr int DragDistance = 4;

    int dragDistance() const { return DragDistance; }
    QString row() const;
    bool active() const;
    bool dragging() const { return m_dragging; }
    bool shown() const { return m_shown; }
    bool held() const { return m_held; }
    qreal wash() const { return m_wash; }
    int revision() const { return m_revision; }
    int layout() const { return m_layout; }
    // The range as OpenGhost 1.2's Ctrl+C copies it (Writer in
    // selection.cpp): each text's part, formulas and diagrams as their
    // drawn text, with the separators of SelectionUnit::after between
    // consecutive texts and a line end more after a reply's copy button.
    // Exact: no trimming.
    QString text() const;
    QAbstractItemModel *model() const { return m_model; }
    void setModel(QAbstractItemModel *model);

    // A row's text item (`unit` its path), an element's box (`prefix` the
    // path its texts start with) and the row's surface (the item hit tests
    // and geometry are in), with its document (a reply's; none for a user
    // message). Each is forgotten when destroyed.
    Q_INVOKABLE void enroll(QQuickItem *item, const QString &row, const QString &unit);
    Q_INVOKABLE void enrollBox(QQuickItem *item, const QString &row, const QString &prefix);
    Q_INVOKABLE void surface(QQuickItem *item, const QString &row, QObject *document);

    // Where a point of `row`'s surface falls, as Chromium's hit test places
    // a caret there: {unit, offset}, or an empty unit when the row shows no
    // text. In a text, the nearest position; in the gap between blocks, the
    // start of the block below; past the last, the end of the last. Those
    // two are `outside` any text (a caret beside the text, not in it).
    Q_INVOKABLE QVariantMap hit(const QString &row, QQuickItem *surface, qreal x, qreal y) const;
    // The same, only inside a text (an empty map elsewhere), with the top of
    // the line it is on (`line`): keyboard line steps.
    Q_INVOKABLE QVariantMap hitInside(const QString &row, QQuickItem *surface, qreal x,
                                      qreal y) const;
    // Where a point of `view` (the transcript) falls across its rows:
    // {row, unit, offset, line}. Over a row's surface, hit() there; above a
    // row (the gap between messages), that row's start; past the last row,
    // its end. Rows that show no text are passed over.
    // `inside`: only inside a text.
    Q_INVOKABLE QVariantMap hitView(QQuickItem *view, qreal x, qreal y, bool inside = false) const;
    // The innermost scrolling box (an interactive Flickable) under a point
    // of `view`, inside it; null if none. A range dragged
    // past its edge scrolls it (SelectDriver), as Chromium autoscrolls the
    // scroller a selection started in.
    Q_INVOKABLE QQuickItem *scrollerAt(QQuickItem *view, qreal x, qreal y) const;
    // The link (an href) under a point of `row`'s surface, if any.
    Q_INVOKABLE QString linkAt(const QString &row, QQuickItem *surface, qreal x, qreal y) const;
    // Whether a press at (x, y) of `root` belongs to a control (a button, a
    // mouse area, an editable field) rather than to the text around it.
    Q_INVOKABLE bool control(QQuickItem *root, qreal x, qreal y) const;
    // Whether a press there leaves the selection in place: a button's or a
    // scroll bar's does, and a row's (its SelectArea starts, extends or
    // keeps the range); one in a text field moves the selection into it,
    // and one on other text or empty space collapses it.
    Q_INVOKABLE bool keeps(QQuickItem *root, qreal x, qreal y) const;

    // A press (1–3 clicks: character, word or block) starts a range at a
    // point of `row`; `extend` (Shift) keeps the anchor instead, in any row.
    // Dragging moves the focus, into any row (the window's pointer moves are
    // reported, moved(), while it lasts); release() lets go and settles the
    // menu (returning whether it shows).
    Q_INVOKABLE void press(const QString &row, QObject *document, const QString &unit, int offset,
                           int clicks, bool extend, bool outside = false);
    Q_INVOKABLE void drag(const QString &row, const QString &unit, int offset,
                          bool outside = false);
    Q_INVOKABLE bool release();
    // A row's whole text selected (Ctrl+A), settled.
    Q_INVOKABLE void selectAll(const QString &row, QObject *document);
    // The focus moved by `count` characters (graphemes) or, with `words`, by
    // words, across texts and rows: keyboard selection. Settles the range.
    Q_INVOKABLE void step(int count, bool words);
    // The focus moved to a point (keyboard Shift+Up/Down): settles the range.
    Q_INVOKABLE void extendTo(const QString &row, const QString &unit, int offset);
    // Reports every left press in `window` (pressed()) before it is
    // delivered, and while a range is dragged its moves and release.
    Q_INVOKABLE void watch(QQuickWindow *window);
    // The menu and the veil go; the range stays.
    Q_INVOKABLE void hide();
    // The range goes (and with it the menu and the veil).
    Q_INVOKABLE void clear();
    // A text a row shows that the transcript does not hold (a file card's
    // preview error, which the window keeps), as `unit` of `row`; empty
    // text: none.
    Q_INVOKABLE void note(const QString &row, const QString &unit, const QString &text);
    // A link pressed on `source` dragged past DragDistance: a drag of its
    // URL (text/uri-list and text/plain), or, when the link lies inside the
    // range, of the range's text, as Chromium drags them. The press's grab
    // ends; the range stays.
    Q_INVOKABLE void dragLink(QQuickItem *source, const QString &url, bool selected);

    // `unit`'s part of the range in `row`: QPoint(start, end), end -1 when
    // the range runs on past its end; (-1, -1) when it has none.
    Q_INVOKABLE QPoint range(const QString &row, const QString &unit) const;
    // Whether the block at `prefix` of `row` holds one of the range's ends:
    // while the range exists, the transcript keeps it built (Block.qml), so
    // the text being selected from or to is never released.
    Q_INVOKABLE bool holds(const QString &row, const QString &prefix) const;
    // Whether `offset` of `unit` lies inside the range.
    Q_INVOKABLE bool covers(const QString &row, const QString &unit, int offset) const;
    // The focus's caret in `item`'s coordinates (an invalid rect when its
    // text is not built).
    Q_INVOKABLE QRectF caretIn(QQuickItem *item) const;
    // In the row surface's coordinates: the veil's cut-outs
    // (selection-focus.js draw()) as {x, y, w, h, kind: band|rise|fall}, and
    // the first and last of the range's client rects, which place the menu.
    // Only for a range within one row.
    Q_INVOKABLE QVariantMap geometry() const;
    Q_INVOKABLE QQuickItem *rowSurface() const;

    // selection-focus.js's cut-out bands from client rects, exposed for tests.
    struct Band {
        qreal left = 0, right = 0, top = 0, bottom = 0;
        bool joinedTop = false, joinedBottom = false;
    };
    static QVector<Band> bands(QVector<QRectF> rects);
    static QVariantList cuts(const QVector<Band> &bands);
    static constexpr qreal PadX = 2, PadY = 3, FeatherX = 56, FeatherY = 5, Join = 16;
    // A user message's texts as 1.2 shows them (.message.is-user): each
    // file card's extension label, name and kind · size (and the card's
    // note, "u/fN:e", when `notes` has one), then the bubble.
    static QVector<SelectionUnit> userUnits(const QString &body, const QVariantList &files,
                                            const QMap<QString, QString> &notes = {});
    // file-kinds.js formatSize().
    static QString formatSize(double bytes);

  signals:
    void changed();
    void shownChanged();
    void heldChanged();
    void washChanged();
    void layoutChanged();
    void modelChanged();
    void diagramWidthChanged();
    void systemDragChanged();
    void pressed(qreal x, qreal y);
    // While a range is dragged: the pointer's scene position, and its release.
    void moved(qreal x, qreal y);
    void released(qreal x, qreal y);
    // dragLink(): what a link drag carries.
    void linkDragged(const QString &url, const QString &text);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    struct Pos {
        QString row, unit;
        int offset = 0;
        bool outside = false; // Hit beside the texts (hit()): no message's words.
    };
    // A place in reading order: the row's transcript index, the unit's
    // index in the row, an offset.
    struct At {
        int row = -1, unit = -1, offset = 0;
        QString key;
        bool operator<(const At &o) const
        {
            return row != o.row ? row < o.row : unit != o.unit ? unit < o.unit : offset < o.offset;
        }
        bool operator==(const At &o) const
        {
            return row == o.row && unit == o.unit && offset == o.offset;
        }
        bool valid() const { return row >= 0 && unit >= 0; }
    };
    // A row's texts, from its document while built, else from the model.
    struct Row {
        QPointer<RichDocument> doc;
        quint64 seen = 0;
        QString source; // The body they were made from (a row not built).
        QString extra;  // And the rest of what it shows (extraOf()).
        QVector<SelectionUnit> units;
        QHash<QString, int> index;
        // Each block path's first and last unit (paths cut at a delimiter).
        QHash<QString, std::pair<int, int>> spans;
        bool reply = false, copyable = false;
    };
    struct Enrolled {
        QPointer<QQuickItem> item;
        QString row, unit;
    };
    const Row *rowOf(const QString &key) const;
    Row build(const QString &key) const;
    int indexOf(const QString &key) const;
    QString keyAt(int index) const;
    QVariant field(int index, const char *role) const;
    QString extraOf(int index) const;
    void watchDocument(RichDocument *doc);
    void documentChanged(RichDocument *doc); // Follows a row's units.
    At resolve(const Pos &pos) const;
    Pos at(const At &at) const;
    std::pair<At, At> ordered() const;
    int unitLength(const QString &key, int unit) const;
    // The next (dir 1) or previous row with text from `index`, -1 if none.
    int neighbour(int index, int dir) const;
    void touch(); // The range changed.
    void followTrims(int first, int last);
    void noteTrims();
    // Not dragging: the menu, the veil and the held wash follow the range,
    // which a reply's menu cuts back to the reply first.
    bool settle();
    bool hasText(const At &from, const At &to) const;
    void setShown(bool shown);
    void setHeld(bool held);
    // Word or block around a position, by granularity.
    std::pair<At, At> around(const At &at, int granularity) const;
    QVector<QQuickItem *> itemsOf(const QString &row, const QString &unit) const;
    QVariantMap find(const QString &row, QQuickItem *surface, qreal x, qreal y, bool inside) const;
    QString copied(const At &from, const At &to) const;

    QPointer<QAbstractItemModel> m_model;
    QHash<QString, int> m_roles; // Role names to roles.
    // Rows' texts while needed: the range's rows (pointers stay valid
    // across insertions).
    mutable std::unordered_map<QString, Row> m_rows;
    // Bumped whenever the order or the texts of rows may have changed: the
    // range's ends in order and rows' indexes are kept for one epoch.
    mutable quint64 m_epoch = 0, m_orderedEpoch = ~quint64(0), m_indexEpoch = ~quint64(0);
    mutable std::pair<At, At> m_ordered;
    mutable QHash<QString, int> m_indexes;
    Pos m_anchor, m_focus;
    // A word or block selection keeps its first word or block whole.
    int m_granularity = 0;
    Pos m_spanFrom, m_spanTo;
    bool m_dragging = false, m_shown = false, m_held = false;
    qreal m_wash = Wash;
    QVariantAnimation m_washing;
    int m_revision = 0, m_layout = 0;
    qreal m_diagramWidth = 640;
    bool m_systemDrag = true;
    QHash<QQuickItem *, Enrolled> m_items, m_boxes;
    QHash<QString, QPointer<QQuickItem>> m_surfaces;
    QHash<QString, QPointer<RichDocument>> m_documents;
    QSet<RichDocument *> m_watched;
    // What the transcript had trimmed from the output the range's ends are
    // in, when last seen (followTrims()).
    QHash<QString, double> m_trims;
    // note(): by row, each unit's text.
    QHash<QString, QMap<QString, QString>> m_notes;
    // Rows registered without a model, in order (tests): their indexes.
    QHash<QString, int> m_order;
    // Diagram texts by source, language and width.
    mutable QHash<QString, std::pair<QStringList, bool>> m_diagrams;
};

// Paints one text's part of the range under its glyphs, in rgba(selection,
// wash), as Chromium paints a selection: each line's run over the whole
// line box; a line the range runs past to its wrap reaching the line box's
// end, and the end of a text the range runs past a space's width more.
// The text's own colours, layout and glyphs are untouched. Set on a TextEdit
// (`target`), or on an image or a label: a display formula or a short label
// whose whole box it covers, a diagram whose lines of text
// (`selectionRects`) it covers.
class SelectionWash : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(QPoint range READ range WRITE setRange NOTIFY rangeChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    // The rects painted, in this item's coordinates (tests).
    Q_PROPERTY(QVariantList rects READ rects NOTIFY rectsChanged)

  public:
    explicit SelectionWash(QQuickItem *parent = nullptr);
    QQuickItem *target() const { return m_target; }
    void setTarget(QQuickItem *target);
    QPoint range() const { return m_range; }
    void setRange(QPoint range);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    QVariantList rects() const;

    // A text's lines over [start, end) (end -1: to its end, the range running
    // on): the selection highlight's rects (`highlight`) or the selected
    // glyph runs' boxes (Range.getClientRects()), in the text item's
    // coordinates. An item without a document is one box.
    static QVector<QRectF> lines(QQuickItem *text, int start, int end, bool highlight);

  signals:
    void targetChanged();
    void rangeChanged();
    void colorChanged();
    void rectsChanged();

  protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void updatePolish() override { relayout(); }

  private slots:
    void schedule() { polish(); }

  private:
    void relayout();
    QPointer<QQuickItem> m_target;
    QPoint m_range{-1, -1};
    QColor m_color;
    QVector<QRectF> m_rects;
    QMetaObject::Connection m_layoutUpdate, m_documentChange;
};
