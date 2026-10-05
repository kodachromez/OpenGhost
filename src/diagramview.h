#pragma once

#include "diagram.h"
#include "diagram_scene.h"
#include "motion.h"

#include <QImage>
#include <QQuickItem>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <memory>

// One drawing in a reply (diagram.js DiagramView): compiled and painted off
// the GUI thread, shown at the stage's width the way viewSpec places it,
// and animated as upstream's Scene does: a drawing built fresh comes in item
// by item, one that changes (streamed, edited, resized) springs to its new
// layout, and everything settles on the worker's settled image. Under the
// pointer, charts answer with tips and a rule, rows with the shared glide,
// blocks light their arrows, rings focus a slice. A drawing that was built
// before (a row scrolled back) or under reduced motion shows at once.
//
// As RichImage: every change of what is drawn starts a new request; only the
// current request's result is shown (while `live`, a drawing of text the
// source still starts with is shown as progress); a source that does not
// draw keeps the last good drawing and says why in `error`.
class DiagramImage : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString lang READ lang WRITE setLang NOTIFY langChanged)
    Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged)
    // The stage's width (the chat's wide width) and the text column in it.
    Q_PROPERTY(qreal availableWidth READ availableWidth WRITE setAvailableWidth NOTIFY
                   availableWidthChanged)
    Q_PROPERTY(qreal column READ column WRITE setColumn NOTIFY columnChanged)
    // The section's colour (theme::tones index; -1 neutral).
    Q_PROPERTY(int tone READ tone WRITE setTone NOTIFY toneChanged)
    // Built fresh: its first drawing may come in item by item.
    Q_PROPERTY(bool animate READ animate WRITE setAnimate NOTIFY animateChanged)
    Q_PROPERTY(
        bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY reducedMotionChanged)
    Q_PROPERTY(bool ok READ ok NOTIFY resultChanged)
    Q_PROPERTY(QString error READ error NOTIFY resultChanged)
    Q_PROPERTY(bool pending READ pending NOTIFY pendingChanged)
    Q_PROPERTY(QString shownSource READ shownSource NOTIFY resultChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY resultChanged)
    Q_PROPERTY(int renders READ renders NOTIFY resultChanged)
    // The stage as it stands now (springing while a drawing grows): its
    // size, and where the Edit and Copy tools go.
    Q_PROPERTY(qreal stageWidth READ stageWidth NOTIFY stageChanged)
    Q_PROPERTY(qreal stageHeight READ stageHeight NOTIFY stageChanged)
    Q_PROPERTY(QPointF tools READ tools NOTIFY stageChanged)
    // The drawing's box in the stage (x, y, width, height).
    Q_PROPERTY(QRectF drawing READ drawing NOTIFY stageChanged)
    // Still coming in or springing (tests and the GPU qualification).
    Q_PROPERTY(bool animating READ animating NOTIFY animatingChanged)
    // The pointer is over the drawing or its tools (.dg-host.is-hover).
    Q_PROPERTY(bool hovering READ hovering NOTIFY hoveringChanged)
    // The tip to show (.dg-tip): shown, title, rows [{name, value, cls,
    // color, mark}], mode "mark" (box: the mark's box; px, py: the pointer;
    // large) or "probe" (x, top, bottom: the rule), and its key (sig).
    Q_PROPERTY(QVariantMap tip READ tip NOTIFY tipChanged)
    // Each line of drawn text, in this item's coordinates: what a selection
    // paints (SelectionWash).
    Q_PROPERTY(QVariantList selectionRects READ selectionRects NOTIFY resultChanged)

  public:
    explicit DiagramImage(QQuickItem *parent = nullptr);
    ~DiagramImage() override;

    QString source() const { return m_source; }
    void setSource(const QString &source);
    QString lang() const { return m_lang; }
    void setLang(const QString &lang);
    bool live() const { return m_live; }
    void setLive(bool live);
    qreal availableWidth() const { return m_room; }
    void setAvailableWidth(qreal width);
    qreal column() const { return m_column; }
    void setColumn(qreal column);
    int tone() const { return m_tone; }
    void setTone(int tone);
    bool animate() const { return m_animate; }
    void setAnimate(bool animate);
    bool reducedMotion() const { return m_reducedMotion; }
    void setReducedMotion(bool reduced);
    bool ok() const { return bool(m_result); }
    QString error() const { return m_error; }
    bool pending() const { return m_rendering || m_timer.isActive(); }
    QString shownSource() const { return m_shownSource; }
    QString kind() const;
    int renders() const { return m_renders; }
    qreal stageWidth() const;
    qreal stageHeight() const;
    QPointF tools() const;
    QRectF drawing() const;
    bool animating() const { return m_clock.state() == QAbstractAnimation::Running; }
    bool hovering() const { return m_hovering; }
    QVariantMap tip() const { return m_tip; }
    QVariantList selectionRects() const;

    // Moves the pointer to (x, y) in this item, or away (tests).
    Q_INVOKABLE void pointAt(qreal x, qreal y);
    Q_INVOKABLE void pointAway();
    // The keys lit now (tests).
    Q_INVOKABLE QStringList hotKeys() const { return m_hotKeys; }

    static constexpr int LiveDelayMs = 150;   // A streamed source pauses.
    static constexpr int ResizeDelayMs = 120; // A width that is still changing.
    // The process's drawing cache: at most CacheBytes in CacheEntries.
    static constexpr qint64 CacheBytes = 48 * 1024 * 1024;
    static constexpr int CacheEntries = 256;
    // Failures of sources longer than this are not cached.
    static constexpr int CachedFailure = 4096;

  signals:
    void sourceChanged();
    void langChanged();
    void liveChanged();
    void availableWidthChanged();
    void columnChanged();
    void toneChanged();
    void animateChanged();
    void reducedMotionChanged();
    void resultChanged();
    void pendingChanged();
    void stageChanged();
    void animatingChanged();
    void hoveringChanged();
    void tipChanged();

  protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;

  public:
    // A drawing as the worker made it (the settled one).
    struct Drawing {
        std::shared_ptr<const diagram::Result> result;
        QImage image;
        double left = 0;
        diagram::View view;
        QVector<QRectF> texts;
        QString error;
    };

  private:
    // Drawings cached in the process (tests): entries, their accounted cost
    // and the bytes they actually hold.
    struct Cached {
        int count = 0;
        qint64 cost = 0, bytes = 0;
    };
    static Cached cached();
    friend class RichTest;
    std::function<void()> m_beforeDraw; // Tests: runs on the worker before drawing.

    void changed(int delay);
    void render();
    QString inputsKey() const;
    QByteArray drawingKey() const;
    bool fromCache();
    bool shown() const;
    qreal ratio() const;
    diagram::Options options() const;
    void show(const Drawing &drawing, const QString &source);
    void fail(const QString &error);
    void tick();
    void frame();
    void paintNext();
    void settle();
    bool plain() const; // The settled image shows exactly what the scene does.
    void updateStage();
    void pointer(QPointF at, bool inside);
    void probe(double cx, double cy, const QString &target);
    void showTip(const diagram::Tip *tip, const QVariantMap &at, const QStringList &keys);
    void focusPie(int index);
    const QVector<diagram::Hit> &hits();
    QString hitAt(QPointF at, bool *node = nullptr);

    // Inputs.
    QString m_source, m_lang;
    bool m_live = false, m_animate = false, m_reducedMotion = false;
    qreal m_room = 0, m_column = 0;
    int m_tone = -1;
    // Requests.
    bool m_rendering = false, m_again = false, m_hidden = false, m_flying = false;
    quint64 m_request = 0;
    QString m_flightInputs, m_flightSource;
    QTimer m_timer;
    QMetaObject::Connection m_parentShown;
    // What is shown.
    std::shared_ptr<const diagram::Result> m_result;
    Drawing m_settled;
    QString m_error, m_shownSource;
    int m_renders = 0;
    diagram::Hints m_hints;
    QString m_sideways;
    // The scene and its frames.
    diagram::Scene m_scene;
    FrameClock m_clock;
    bool m_first = true, m_useFrame = false, m_dirty = false;
    // A frame is being painted off the GUI thread; another is wanted after it.
    bool m_framing = false, m_frameWanted = false;
    quint64 m_generation = 0;
    QImage m_frame;
    double m_frameLeft = 0;
    // The pointer.
    bool m_hovering = false, m_hitsValid = false;
    QVector<diagram::Hit> m_hits;
    QStringList m_hotKeys;
    QString m_hotSig, m_lit, m_hovered;
    int m_focused = -1;
    QVariantMap m_tip;
    // The rows' shared highlight (Glide).
    struct Glide {
        double x = 0, y = 0, w = 0, h = 0, o = 0;
    };
    Glide m_glide, m_glideVel;
    std::optional<QRectF> m_glideBox;
    bool m_glideOn = false;
    double m_last = 0;
};
