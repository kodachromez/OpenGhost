#pragma once

#include <QColor>
#include <QImage>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

// One of OpenGhost's small line icons, by name, painted antialiased at the
// item's size. The path data is compiled in from OpenGhost's markup; QML
// chooses a name, never path data, so nothing here parses untrusted input.
class PathIcon : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    // Another glyph with the same commands, and how far toward it the path
    // is drawn (CSS `d` transitions).
    Q_PROPERTY(QString morphTo READ morphTo WRITE setMorphTo NOTIFY morphToChanged)
    Q_PROPERTY(qreal morph READ morph WRITE setMorph NOTIFY morphChanged)
    // How much of a stroked glyph is drawn from its start, as a dash that
    // stroke-dashoffset slides in: 1 all of it, 0 none, not even a cap.
    Q_PROPERTY(qreal reveal READ reveal WRITE setReveal NOTIFY revealChanged)
    // A stroked glyph's line width in its view units; 0: the glyph's own
    // (a CSS stroke-width override, as the dock's 4.6).
    Q_PROPERTY(qreal stroke READ stroke WRITE setStroke NOTIFY strokeChanged)

  public:
    explicit PathIcon(QQuickItem *parent = nullptr);
    QString name() const { return m_name; }
    void setName(const QString &name);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    QString morphTo() const { return m_morphTo; }
    void setMorphTo(const QString &name);
    qreal morph() const { return m_morph; }
    void setMorph(qreal morph);
    qreal reveal() const { return m_reveal; }
    void setReveal(qreal reveal);
    qreal stroke() const { return m_stroke; }
    void setStroke(qreal stroke);
    void paint(QPainter *painter) override;

    // True for every name paint() draws (tests).
    static bool known(const QString &name);

  signals:
    void nameChanged();
    void colorChanged();
    void morphToChanged();
    void morphChanged();
    void revealChanged();
    void strokeChanged();

  private:
    QString m_name;
    QColor m_color{255, 255, 255, 140};
    QString m_morphTo;
    qreal m_morph = 0;
    qreal m_reveal = 1;
    qreal m_stroke = 0;
};

// The sidebar toggle's glyph (sidebar-toggle.js): the frame, its split at
// `split` view units (of 60: 22 open, 11 collapsed) and the pane left of the
// split at .3, so the split can glide between them.
class ToggleIcon : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal split READ split WRITE setSplit NOTIFY splitChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)

  public:
    explicit ToggleIcon(QQuickItem *parent = nullptr);
    qreal split() const { return m_split; }
    void setSplit(qreal split);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    void paint(QPainter *painter) override;

  signals:
    void splitChanged();
    void colorChanged();

  private:
    qreal m_split = 22;
    QColor m_color{255, 255, 255};
};

// The browser toggle's globe (browser-toggle.js): its meridian narrows as
// the pointer spins it (`spin` 0 → 1) and the globe fills to .3 while the
// panel is open (`open` 0 → 1).
class GlobeIcon : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal spin READ spin WRITE setSpin NOTIFY spinChanged)
    Q_PROPERTY(qreal open READ open WRITE setOpen NOTIFY openChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)

  public:
    explicit GlobeIcon(QQuickItem *parent = nullptr);
    qreal spin() const { return m_spin; }
    void setSpin(qreal spin);
    qreal open() const { return m_open; }
    void setOpen(qreal open);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    void paint(QPainter *painter) override;

  signals:
    void spinChanged();
    void openChanged();
    void colorChanged();

  private:
    qreal m_spin = 0, m_open = 0;
    QColor m_color{255, 255, 255};
};

// A chosen file's kind icon (openghost/file-kinds.js): a page tinted in the
// kind's tone with its glyph and extension label. `kind` names the kind as
// the attachment meta line shows it ("Text", "Rust", "PDF").
class FileIcon : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString fileName READ fileName WRITE setFileName NOTIFY fileNameChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY fileNameChanged)
    // The extension label ("TXT"), an SVG <text> in OpenGhost: selectable,
    // and its box in this item's coordinates.
    Q_PROPERTY(QString label READ label NOTIFY fileNameChanged)
    Q_PROPERTY(QRectF labelRect READ labelRect NOTIFY labelRectChanged)

  public:
    explicit FileIcon(QQuickItem *parent = nullptr);
    QString fileName() const { return m_fileName; }
    void setFileName(const QString &name);
    QString kind() const { return m_kind; }
    QString label() const { return m_label; }
    QRectF labelRect() const;
    void paint(QPainter *painter) override;

    // file-kinds.js describe(): a name's kind ("Text") and label ("TXT").
    struct Described {
        QString kind, label, glyph;
        QColor tone;
    };
    static Described describe(const QString &name);

  signals:
    void fileNameChanged();
    void labelRectChanged();

  protected:
    void geometryChange(const QRectF &next, const QRectF &old) override
    {
        QQuickPaintedItem::geometryChange(next, old);
        if (next.size() != old.size())
            emit labelRectChanged();
    }

  private:
    QString m_fileName;
    QString m_kind = QStringLiteral("File");
    QString m_glyph = QStringLiteral("text");
    QString m_label = QStringLiteral("FILE");
    QColor m_tone{168, 170, 180};
};

// A decoded picture filling the item, clipped to a rounded rectangle (a
// Settings → General thumbnail: object-fit: cover, radius 8).
class ImageTile : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QImage image READ image WRITE setImage NOTIFY imageChanged)
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)

  public:
    explicit ImageTile(QQuickItem *parent = nullptr);
    QImage image() const { return m_image; }
    void setImage(const QImage &image);
    qreal radius() const { return m_radius; }
    void setRadius(qreal radius);
    void paint(QPainter *painter) override;

  signals:
    void imageChanged();
    void radiusChanged();

  private:
    QImage m_image;
    qreal m_radius = 0;
};
