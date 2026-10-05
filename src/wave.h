#pragma once

#include "motion.h"

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QTextFormat>
#include <QTextLayout>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

class QSGOpacityNode;
class QSGTransformNode;
class QTextCursor;
class QTextDocument;

// OpenGhost 1.2's streamed word reveal (stream-view.js fragment() and
// stagger(), styles.css .md-w): each grapheme a live reveal adds to a text
// fades in over 0.22 s (md-wave) while it sharpens from a 2 px blur in
// 0.14 s (md-focus), both on cubic-bezier(0.33, 1, 0.68, 1), and sits
// 0.35 em low until its fade is half done (rise()). Those one frame adds start spread over
// that frame (at most 50 ms), the last at once; past the newest 90 they show
// at once. Spaces never wave.
//
// The text keeps its own layout, height, selection and copy throughout:
// while a grapheme is in flight its characters are transparent in the
// document (a char format mark, Hidden, keeps the foreground they had), and
// this item, laid over the text as its child, draws it moving: the glyph as
// the scene graph draws text, under a blurred copy that fades out. Once it
// has landed the text gets its colour back, at the latest with the next
// edit, so the glyph drawn here at rest is replaced by the same glyph.
// Nothing waves with reduced motion or while the text holds a selection; a
// selection, reduced motion or a new target lands everything at once.
class TextWave : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    // The TextEdit (or TextArea) whose document is waved.
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY targetChanged)
    // New text waves: the text belongs to a reply being revealed.
    Q_PROPERTY(bool active MEMBER m_active NOTIFY activeChanged)
    // The text present at the first edit waves too: the text's block was
    // just added to a revealing reply, not built again by scrolling.
    Q_PROPERTY(bool fresh MEMBER m_fresh NOTIFY freshChanged)
    Q_PROPERTY(
        bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY reducedMotionChanged)
    // The text holds part of a reply's selection (selection.h): as a
    // selection of its own, everything lands and nothing new waves.
    Q_PROPERTY(bool selected READ selected WRITE setSelected NOTIFY selectedChanged)
    // Graphemes in flight.
    Q_PROPERTY(int count READ count NOTIFY countChanged)

  public:
    // The char format property marking a hidden character: it holds the
    // foreground (a QBrush) the character had, or nothing when it had none.
    static constexpr int Hidden = QTextFormat::UserProperty + 40;
    static constexpr double Duration = 0.22, Focus = 0.14, Rise = 0.35, Blur = 2;
    static constexpr int Most = 90;
    static constexpr double Spread = 0.05;
    static constexpr int BlurCache = 4 * 1024 * 1024; // Bytes of blurred copies kept.

    explicit TextWave(QQuickItem *parent = nullptr);
    ~TextWave() override;
    QQuickItem *target() const { return m_target; }
    void setTarget(QQuickItem *target);
    bool reducedMotion() const { return m_reducedMotion; }
    void setReducedMotion(bool reduced);
    bool selected() const { return m_selected; }
    void setSelected(bool selected);
    int count() const { return int(m_flying.size()); }

    // Called by the text's producer inside its edit block (`cursor`), after
    // its text and formats are set: the text from `from` on is new. In-flight
    // graphemes before it whose formats the producer set again are hidden
    // again; those from it on were replaced.
    void edited(QTextCursor &cursor, int from);
    // edited() in an edit block of its own (LiveText inserts by itself).
    Q_INVOKABLE void grew(int from);
    // Lands everything now.
    Q_INVOKABLE void settle();
    // The in-flight grapheme at `position` (tests, recordings): its rise in
    // pixels, opacity and blur in pixels; empty when none is in flight there.
    Q_INVOKABLE QVariantMap at(int position) const;

    // The CSS easing both keyframes use.
    static double ease(double t);
    // How far below its place a grapheme of `em` px is at eased progress `eased`.
    static double rise(qreal em, double eased);

  signals:
    void targetChanged();
    void activeChanged();
    void freshChanged();
    void reducedMotionChanged();
    void selectedChanged();
    void countChanged();

  protected:
    void componentComplete() override;
    void updatePolish() override;
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void releaseResources() override;

  private:
    struct Grapheme {
        int position = 0, length = 0;
        double start = 0;
        qreal em = 16; // The font's CSS pixel size: the rise is 0.35 em.
        QColor color;
        std::unique_ptr<QTextLayout> layout; // The glyph alone, origin at its line's top.
        qreal ascent = 0;
        QImage blurred;  // Waiting for upload.
        QRectF blurRect; // The blurred copy, relative to the layout's origin.
        QPointF origin;  // The layout's origin in this item, this frame.
        QSGTransformNode *node = nullptr;
        QSGOpacityNode *sharp = nullptr, *soft = nullptr;
    };
    QTextDocument *document() const;
    void tick();
    // Gives the characters of `g` their colour back (in `cursor`'s edit block).
    static void restore(QTextCursor &cursor, int from, int to);
    // Hides [from, to) (in `cursor`'s edit block), keeping each character's
    // foreground in its mark; already hidden characters are left alone.
    static void hide(QTextCursor &cursor, int from, int to);
    // The grapheme's look from the document as it is now.
    void dress(Grapheme &g, QTextDocument *doc);
    // Lands the graphemes that finished at least `after` seconds ago.
    void land(QTextCursor &cursor, double after);
    void place();
    void dropAll();
    bool selecting() const;

    QPointer<QQuickItem> m_target;
    QMetaObject::Connection m_selection;
    bool m_active = false, m_fresh = false, m_reducedMotion = false, m_selected = false;
    bool m_started = false; // The first edit has been seen.
    std::vector<Grapheme> m_flying;
    // Scene graph nodes whose graphemes landed, removed at the next update.
    std::vector<QSGTransformNode *> m_dead;
    bool m_rebuild = false; // The scene graph lost its nodes.
    FrameClock m_clock;
};
