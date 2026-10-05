#include "selection.h"

#include "diagram.h"
#include "icon.h"
#include "tex.h"

#include <QAbstractTextDocumentLayout>
#include <QDrag>
#include <QEasingCurve>
#include <QFontMetricsF>
#include <QMimeData>
#include <QMouseEvent>
#include <QQuickTextDocument>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace
{
QTextDocument *documentOf(const QQuickItem *item)
{
    if (!item)
        return nullptr;
    auto *quick = item->property("textDocument").value<QQuickTextDocument *>();
    return quick ? quick->textDocument() : nullptr;
}

// A path's prefix ends at a delimiter: "r/1" covers "r/1:t" and "r/1/0:t",
// never "r/10:t"; a table row "r/2:c1" covers its cells "r/2:c1.0".
bool within(const QString &path, const QString &prefix)
{
    if (!path.startsWith(prefix))
        return false;
    if (path.size() == prefix.size())
        return true;
    const QChar next = path.at(prefix.size());
    return next == QLatin1Char('/') || next == QLatin1Char(':') || next == QLatin1Char('.');
}

QQuickItem *deepest(QQuickItem *item, QPointF point)
{
    QList<QQuickItem *> children = item->childItems();
    std::stable_sort(children.begin(), children.end(),
                     [](const QQuickItem *a, const QQuickItem *b) { return a->z() < b->z(); });
    for (auto it = children.crbegin(); it != children.crend(); ++it) {
        QQuickItem *child = *it;
        if (!child->isVisible() || !child->isEnabled() ||
            child->objectName() == QLatin1String("selectArea"))
            continue;
        const QPointF inChild = child->mapFromItem(item, point);
        if (child->clip() && !child->contains(inChild))
            continue;
        if (QQuickItem *hit = deepest(child, inChild))
            return hit;
    }
    return item->contains(point) ? item : nullptr;
}

// The item a left press at a point reaches first, as Qt delivers it: the
// topmost that accepts the button (items that only draw, a veil, pass it on).
QQuickItem *pressTarget(QQuickItem *item, QPointF point)
{
    QList<QQuickItem *> children = item->childItems();
    std::stable_sort(children.begin(), children.end(),
                     [](const QQuickItem *a, const QQuickItem *b) { return a->z() < b->z(); });
    for (auto it = children.crbegin(); it != children.crend(); ++it) {
        QQuickItem *child = *it;
        if (!child->isVisible() || !child->isEnabled())
            continue;
        const QPointF inChild = child->mapFromItem(item, point);
        if (child->clip() && !child->contains(inChild))
            continue;
        if (QQuickItem *hit = pressTarget(child, inChild))
            return hit;
    }
    return item->contains(point) && (item->acceptedMouseButtons() & Qt::LeftButton) ? item : nullptr;
}

// Chromium's plain text of a range over OpenGhost's DOM (TextIterator), as
// recorded on 1.2's Ctrl+C:
// - a box's edge (a Break: a formula's flex and grid boxes, a diagram's
//   <text>s) copies as one line end between texts, however many meet, and
//   nothing before the first text copied or after the last; an empty
//   delimiter box (a Mark) counts as a text there without copying anything;
// - a block's end copies a line end at once (even first); a paragraph's,
//   then an empty line, and a diagram's, two, once anything has been
//   copied;
// - a reply's copy button under it, one more line end.
class Writer
{
  public:
    void text(const QString &s)
    {
        if (s.isEmpty())
            return;
        flush();
        out += s;
        emitted = true;
    }
    void brk() { pending = true; }
    void mark()
    {
        flush();
        marked = true;
    }
    void separator(char after)
    {
        switch (after) {
        case 'p':
            newline();
            if (emitted)
                out += QLatin1Char('\n');
            break;
        case 'l':
            newline();
            break;
        case 't':
            pending = false;
            out += QLatin1Char('\t');
            break;
        case 'd':
            newline();
            if (emitted)
                out += QStringLiteral("\n\n");
            break;
        default:
            break;
        }
    }
    void message()
    {
        pending = false;
        out += QLatin1Char('\n');
    }
    QString out;

  private:
    void newline()
    {
        pending = false;
        if (!out.endsWith(QLatin1Char('\n')))
            out += QLatin1Char('\n');
    }
    void flush()
    {
        if (pending && (emitted || marked) && !out.endsWith(QLatin1Char('\n')))
            out += QLatin1Char('\n');
        pending = false;
    }
    bool emitted = false, marked = false, pending = false;
};

bool blank(const QString &s, int from, int to)
{
    for (int i = std::max(0, from); i < to && i < s.size(); ++i)
        if (!s.at(i).isSpace())
            return false;
    return true;
}
} // namespace

// ---- Selection -------------------------------------------------------------

ReplySelection::ReplySelection(QObject *parent) : QObject(parent)
{
    QEasingCurve ease(QEasingCurve::BezierSpline); // CSS ease
    ease.addCubicBezierSegment(QPointF(0.25, 0.1), QPointF(0.25, 1), QPointF(1, 1));
    m_washing.setEasingCurve(ease);
    m_washing.setDuration(WashMs);
    connect(&m_washing, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_wash = value.toReal();
        emit washChanged();
    });
}

void ReplySelection::setModel(QAbstractItemModel *model)
{
    if (model == m_model)
        return;
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    clear();
    m_rows.clear();
    m_model = model;
    m_roles.clear();
    if (model) {
        const auto names = model->roleNames();
        for (auto it = names.cbegin(); it != names.cend(); ++it)
            m_roles.insert(QString::fromUtf8(it.value()), it.key());
        // A row that leaves takes the range with it; another conversation
        // takes everything.
        const auto gone = [this] {
            ++m_epoch;
            if ((!m_anchor.row.isEmpty() && indexOf(m_anchor.row) < 0) ||
                (!m_focus.row.isEmpty() && indexOf(m_focus.row) < 0))
                clear();
            m_rows.clear();
            touch();
        };
        connect(model, &QAbstractItemModel::rowsRemoved, this, gone);
        connect(model, &QAbstractItemModel::rowsMoved, this, gone);
        connect(model, &QAbstractItemModel::rowsInserted, this, [this] { ++m_epoch; });
        connect(model, &QAbstractItemModel::modelReset, this, [this] {
            ++m_epoch;
            clear();
            m_rows.clear();
        });
        // Rows not built follow their text when read (rowOf()).
        connect(model, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex &first, const QModelIndex &last) {
                    ++m_epoch;
                    followTrims(first.row(), last.row());
                });
    }
    emit modelChanged();
}

int ReplySelection::indexOf(const QString &key) const
{
    if (key.isEmpty())
        return -1;
    if (!m_model)
        return m_order.value(key, -1);
    if (m_indexEpoch != m_epoch) {
        m_indexes.clear();
        m_indexEpoch = m_epoch;
    }
    if (const auto found = m_indexes.constFind(key); found != m_indexes.cend())
        return *found;
    int index = -1;
    QMetaObject::invokeMethod(m_model, "indexOf", Q_RETURN_ARG(int, index), Q_ARG(QString, key));
    m_indexes.insert(key, index);
    return index;
}

QString ReplySelection::keyAt(int index) const
{
    if (!m_model)
        return m_order.key(index);
    return field(index, "key").toString();
}

QVariant ReplySelection::field(int index, const char *role) const
{
    if (!m_model || index < 0 || index >= m_model->rowCount())
        return {};
    return m_model->data(m_model->index(index, 0), m_roles.value(QString::fromLatin1(role), -1));
}

QString ReplySelection::extraOf(int index) const
{
    // What a row shows besides its body: a user message's receipt.
    const QString kind = field(index, "kind").toString();
    QStringList parts{kind};
    if (kind == QLatin1String("user")) {
        parts << field(index, "preview").toString();
        const auto notes = m_notes.value(field(index, "key").toString());
        for (auto it = notes.cbegin(); it != notes.cend(); ++it)
            parts << it.key() << it.value();
    }
    return parts.join(QChar(0x1f));
}

QString ReplySelection::formatSize(double bytes)
{
    if (bytes < 1000)
        return QString::number(qint64(bytes)) + QStringLiteral(" B");
    static const char *const units[] = {"KB", "MB", "GB"};
    double value = bytes / 1000;
    int unit = 0;
    while (value >= 1000 && unit < 2) {
        value /= 1000;
        ++unit;
    }
    QString number = value < 10 ? QString::number(value, 'f', 1)
                                : QString::number(std::floor(value + 0.5), 'f', 0);
    if (number.endsWith(QLatin1String(".0")))
        number.chop(2);
    return number + QLatin1Char(' ') + QLatin1String(units[unit]);
}

QVector<SelectionUnit> ReplySelection::userUnits(const QString &body, const QVariantList &files,
                                                const QMap<QString, QString> &notes)
{
    QVector<SelectionUnit> out;
    const auto add = [&out](const QString &path, const QString &text, char after) {
        SelectionUnit unit;
        unit.path = path;
        unit.text.text = text;
        unit.after = after;
        out << std::move(unit);
    };
    for (int i = 0; i < files.size(); ++i) {
        const QVariantMap file = files.at(i).toMap();
        const QString name = file.value(QStringLiteral("name")).toString();
        const double size = file.value(QStringLiteral("size")).toDouble();
        const QString card = QStringLiteral("u/f%1").arg(i);
        // A counted paste shows its name alone (ChatEntry's file card).
        if (size < 0) {
            add(card + QStringLiteral(":n"), name, 'l');
            continue;
        }
        const FileIcon::Described kind = FileIcon::describe(name);
        add(card + QStringLiteral(":i"), kind.label, 'l');
        add(card + QStringLiteral(":n"), name, 'l');
        add(card + QStringLiteral(":m"), kind.kind + QStringLiteral(" · ") + formatSize(size), 'l');
        if (const QString note = notes.value(card + QStringLiteral(":e")); !note.isEmpty())
            add(card + QStringLiteral(":e"), note, 'l');
    }
    if (!body.isEmpty()) {
        // The bubble's document: one position per line end.
        QString text = body;
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        add(QStringLiteral("u:b"), text, 'p');
    }
    return out;
}

ReplySelection::Row ReplySelection::build(const QString &key) const
{
    Row r;
    const int index = indexOf(key);
    if (RichDocument *doc = m_documents.value(key)) {
        r.doc = doc;
        r.seen = doc->shownVersion();
        r.units = doc->units();
        r.reply = true;
    } else if (index >= 0 && m_model) {
        const QString kind = field(index, "kind").toString();
        r.source = field(index, "body").toString();
        r.extra = extraOf(index);
        const auto add = [&r](const QString &path, const QString &text, char after) {
            if (text.isEmpty())
                return;
            SelectionUnit unit;
            unit.path = path;
            unit.text.text = text;
            unit.text.text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            unit.text.text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            unit.after = after;
            r.units << std::move(unit);
        };
        if (kind == QLatin1String("assistant")) {
            r.units = RichDocument::unitsFor(r.source);
            r.reply = true;
        } else if (kind == QLatin1String("user")) {
            r.units = userUnits(r.source, field(index, "attachments").toList(), m_notes.value(key));
            // A steering receipt under the bubble, the message's last text.
            add(QStringLiteral("u:r"), field(index, "preview").toString(), 'p');
        } else if (kind == QLatin1String("note")) {
            add(QStringLiteral("n:t"), r.source, 'l');
        }
    }
    r.copyable = r.reply && field(index, "copyable").toBool();
    r.index.reserve(r.units.size());
    for (int k = 0; k < r.units.size(); ++k) {
        const QString &path = r.units.at(k).path;
        r.index.insert(path, k);
        for (int i = 1; i <= path.size(); ++i) {
            if (i < path.size() && path.at(i) != QLatin1Char('/') && path.at(i) != QLatin1Char(':') &&
                path.at(i) != QLatin1Char('.'))
                continue;
            const QString prefix = path.left(i);
            if (auto span = r.spans.find(prefix); span != r.spans.end())
                span->second = k; // Units come in order: the first stays.
            else
                r.spans.insert(prefix, {k, k});
        }
    }
    return r;
}

const ReplySelection::Row *ReplySelection::rowOf(const QString &key) const
{
    if (key.isEmpty())
        return nullptr;
    RichDocument *doc = m_documents.value(key);
    if (const auto it = m_rows.find(key); it != m_rows.end()) {
        const Row &row = it->second;
        if (doc ? row.doc == doc && row.seen == doc->shownVersion()
                // Not built: what it showed last when there is no model to
                // read, else its text in the model while unchanged.
                : !m_model || (row.doc.isNull() && !row.source.isNull() &&
                               field(indexOf(key), "body").toString() == row.source &&
                               extraOf(indexOf(key)) == row.extra))
            return &row;
    }
    ++m_epoch;
    return &(m_rows[key] = build(key));
}

void ReplySelection::watchDocument(RichDocument *doc)
{
    if (!doc || m_watched.contains(doc))
        return;
    m_watched.insert(doc);
    connect(doc, &RichDocument::shownChanged, this, [this, doc] { documentChanged(doc); });
    connect(doc, &RichDocument::grew, this, [this, doc] { documentChanged(doc); });
    connect(doc, &QObject::destroyed, this, [this, doc] { m_watched.remove(doc); });
}

void ReplySelection::documentChanged(RichDocument *doc)
{
    QString key;
    for (auto it = m_documents.cbegin(); it != m_documents.cend(); ++it)
        if (it.value() == doc)
            key = it.key();
    const auto cached = m_rows.find(key);
    if (key.isEmpty() || cached == m_rows.end() || cached->second.seen == doc->shownVersion())
        return;
    const Row before = cached->second;
    const auto [wasFrom, wasTo] = ordered();
    Row next = build(key);
    // An end whose text went (the tail of a growing reply re-parsed into
    // another kind) moves to where that text was.
    const auto keep = [&](Pos &pos) {
        if (pos.row != key || pos.unit.isEmpty())
            return;
        if (const int k = next.index.value(pos.unit, -1); k >= 0) {
            pos.offset = std::clamp(pos.offset, 0, int(next.units.at(k).text.text.size()));
            return;
        }
        const int was = before.index.value(pos.unit, -1);
        if (next.units.isEmpty() || was < 0) {
            pos = {};
            return;
        }
        const int k = std::min(was, int(next.units.size()) - 1);
        pos = Pos{key, next.units.at(k).path,
                  was < next.units.size() ? 0 : int(next.units.at(k).text.text.size())};
    };
    keep(m_anchor);
    keep(m_focus);
    keep(m_spanFrom);
    keep(m_spanTo);
    m_rows[key] = next;
    ++m_epoch;
    // Text appended past the range (a reply streaming on) changes nothing
    // the range shows or copies: no item needs to look again.
    const auto [from, to] = ordered();
    const int row = indexOf(key);
    bool same = from == wasFrom && to == wasTo && from.valid();
    if (same && row >= from.row && row <= to.row) {
        const int last = row == to.row ? to.unit : int(before.units.size()) - 1;
        same = last < before.units.size() && last < next.units.size();
        for (int k = 0; same && k <= last; ++k)
            same = before.units.at(k).path == next.units.at(k).path &&
                   before.units.at(k).text.text == next.units.at(k).text.text &&
                   before.units.at(k).after == next.units.at(k).after;
    }
    if (same)
        return;
    touch();
}

bool ReplySelection::active() const
{
    const auto [from, to] = ordered();
    return from.valid() && to.valid() && from < to;
}

QString ReplySelection::row() const
{
    return active() && m_anchor.row == m_focus.row ? m_anchor.row : QString();
}

void ReplySelection::enroll(QQuickItem *item, const QString &row, const QString &unit)
{
    if (!item)
        return;
    const bool known = m_items.contains(item);
    m_items.insert(item, Enrolled{item, row, unit});
    const auto ours = [this](const QString &row) {
        return !row.isEmpty() && (row == m_anchor.row || row == m_focus.row);
    };
    if (!known)
        connect(item, &QObject::destroyed, this, [this, item, ours] {
            const Enrolled gone = m_items.take(item);
            if (ours(gone.row)) {
                ++m_layout;
                emit layoutChanged();
            }
        });
    if (ours(row)) {
        ++m_layout;
        emit layoutChanged();
    }
}

void ReplySelection::enrollBox(QQuickItem *item, const QString &row, const QString &prefix)
{
    if (!item)
        return;
    const bool known = m_boxes.contains(item);
    m_boxes.insert(item, Enrolled{item, row, prefix});
    if (!known)
        connect(item, &QObject::destroyed, this, [this, item] { m_boxes.remove(item); });
    if (!row.isEmpty() && row == this->row()) {
        ++m_layout;
        emit layoutChanged();
    }
}

void ReplySelection::surface(QQuickItem *item, const QString &row, QObject *document)
{
    if (row.isEmpty())
        return;
    // A row released far from the view leaves nothing behind here.
    if (item && m_surfaces.value(row) != item)
        connect(item, &QObject::destroyed, this, [this, row] {
            // Unless the row was built again since.
            if (m_surfaces.value(row).isNull()) {
                m_surfaces.remove(row);
                m_documents.remove(row);
            }
        });
    m_surfaces.insert(row, item);
    auto *rich = qobject_cast<RichDocument *>(document);
    m_documents.insert(row, rich);
    watchDocument(rich);
    ++m_epoch;
    if (!m_model && !m_order.contains(row))
        m_order.insert(row, int(m_order.size()));
    // A row built again (scrolled back into view) brings its document back:
    // the range, kept by path, applies to it as it did.
    if (row == m_anchor.row || row == m_focus.row) {
        if (rich)
            documentChanged(rich);
        ++m_layout;
        emit layoutChanged();
    }
}

QQuickItem *ReplySelection::rowSurface() const
{
    const QString one = row();
    return one.isEmpty() ? nullptr : m_surfaces.value(one).data();
}

int ReplySelection::unitLength(const QString &key, int unit) const
{
    const Row *row = rowOf(key);
    return row && unit >= 0 && unit < row->units.size() ? int(row->units.at(unit).text.text.size())
                                                        : 0;
}

ReplySelection::At ReplySelection::resolve(const Pos &pos) const
{
    const Row *row = pos.unit.isEmpty() ? nullptr : rowOf(pos.row);
    const int k = row ? row->index.value(pos.unit, -1) : -1;
    const int index = k >= 0 ? indexOf(pos.row) : -1;
    if (index < 0)
        return {};
    return At{index, k, std::clamp(pos.offset, 0, int(row->units.at(k).text.text.size())), pos.row};
}

ReplySelection::Pos ReplySelection::at(const At &at) const
{
    const Row *row = at.valid() ? rowOf(at.key) : nullptr;
    if (!row || at.unit >= row->units.size())
        return {};
    return Pos{at.key, row->units.at(at.unit).path, at.offset};
}

std::pair<ReplySelection::At, ReplySelection::At> ReplySelection::ordered() const
{
    if (m_orderedEpoch == m_epoch)
        return m_ordered;
    const At a = resolve(m_anchor), f = resolve(m_focus);
    m_ordered = !a.valid() || !f.valid() ? std::pair{At{}, At{}}
                : f < a                  ? std::pair{f, a}
                                         : std::pair{a, f};
    m_orderedEpoch = m_epoch;
    return m_ordered;
}

int ReplySelection::neighbour(int index, int dir) const
{
    const int rows = m_model ? m_model->rowCount() : int(m_order.size());
    for (int i = index + dir; i >= 0 && i < rows; i += dir) {
        const Row *row = rowOf(keyAt(i));
        if (row && !row->units.isEmpty())
            return i;
    }
    return -1;
}

std::pair<ReplySelection::At, ReplySelection::At> ReplySelection::around(const At &at, int granularity) const
{
    const Row *row = at.valid() ? rowOf(at.key) : nullptr;
    if (!row || granularity <= 0)
        return {at, at};
    const auto place = [&at](int unit, int offset) { return At{at.row, unit, offset, at.key}; };
    if (granularity >= 2) { // A paragraph, through to the next block's start.
        if (at.unit + 1 < row->units.size())
            return {place(at.unit, 0), place(at.unit + 1, 0)};
        return {place(at.unit, 0), place(at.unit, unitLength(at.key, at.unit))};
    }
    const QString &text = row->units.at(at.unit).text.text;
    if (text.isEmpty())
        return {at, at};
    QTextBoundaryFinder words(QTextBoundaryFinder::Word, text);
    int offset = std::min(at.offset, int(text.size()) - 1);
    words.setPosition(offset);
    int from = words.isAtBoundary() ? offset : int(words.toPreviousBoundary());
    words.setPosition(offset);
    int to = int(words.toNextBoundary());
    if (from < 0)
        from = 0;
    if (to < 0)
        to = int(text.size());
    return {place(at.unit, from), place(at.unit, to)};
}

void ReplySelection::followTrims(int first, int last)
{
    // Output a live tail dropped from its front takes the range's ends in
    // it along, as the output's document keeps its own selection (LiveText).
    bool moved = false;
    for (Pos *pos : {&m_anchor, &m_focus, &m_spanFrom, &m_spanTo}) {
        if (pos->unit != QLatin1String("t/b:t"))
            continue;
        const int index = indexOf(pos->row);
        if (index < first || index > last || !m_trims.contains(pos->row))
            continue;
        const double cut = field(index, "trimmed").toDouble() - m_trims.value(pos->row);
        if (cut > 0) {
            pos->offset = std::max(0, pos->offset - int(cut));
            moved = true;
        }
    }
    for (auto it = m_trims.begin(); it != m_trims.end(); ++it)
        if (const int index = indexOf(it.key()); index >= first && index <= last)
            it.value() = field(index, "trimmed").toDouble();
    if (moved)
        touch();
}

void ReplySelection::noteTrims()
{
    m_trims.clear();
    for (const Pos *pos : {&m_anchor, &m_focus, &m_spanFrom, &m_spanTo})
        if (pos->unit == QLatin1String("t/b:t") && !m_trims.contains(pos->row))
            m_trims.insert(pos->row, field(indexOf(pos->row), "trimmed").toDouble());
}

void ReplySelection::touch()
{
    ++m_epoch;
    ++m_revision;
    noteTrims();
    // The wash comes back once the range itself is gone (release()).
    if (!active() && m_held)
        setHeld(false);
    // Rows outside the range keep no texts.
    const auto [from, to] = ordered();
    if (m_rows.size() > 8) {
        for (auto it = m_rows.begin(); it != m_rows.end();) {
            const int index = indexOf(it->first);
            const bool kept = it->first == m_anchor.row || it->first == m_focus.row ||
                              (from.valid() && index >= from.row && index <= to.row);
            it = kept ? std::next(it) : m_rows.erase(it);
        }
    }
    emit changed();
}

void ReplySelection::setShown(bool shown)
{
    if (shown == m_shown)
        return;
    m_shown = shown;
    emit shownChanged();
}

void ReplySelection::setHeld(bool held)
{
    if (held == m_held)
        return;
    m_held = held;
    emit heldChanged();
    m_washing.stop();
    if (held) {
        m_washing.setStartValue(m_wash);
        m_washing.setEndValue(0.0);
        m_washing.start();
    } else if (m_wash != Wash) {
        m_wash = Wash;
        emit washChanged();
    }
}

void ReplySelection::press(const QString &row, QObject *document, const QString &unit, int offset,
                           int clicks, bool extend, bool outside)
{
    if (auto *rich = qobject_cast<RichDocument *>(document); rich && m_documents.value(row) != rich)
        surface(m_surfaces.value(row), row, rich);
    const At hit = resolve(Pos{row, unit, offset});
    if (!hit.valid())
        return;
    m_dragging = true;
    setShown(false);
    if (extend && resolve(m_anchor).valid()) {
        m_granularity = 0;
        m_focus = at(hit);
        m_focus.outside = outside;
    } else {
        m_granularity = clicks >= 3 ? 2 : clicks == 2 ? 1 : 0;
        const auto [from, to] = around(hit, m_granularity);
        m_anchor = m_spanFrom = at(from);
        m_focus = m_spanTo = at(to);
        m_anchor.outside = m_focus.outside = outside && m_granularity == 0;
    }
    touch();
}

void ReplySelection::drag(const QString &row, const QString &unit, int offset, bool outside)
{
    if (!m_dragging)
        return;
    const At hit = resolve(Pos{row, unit, offset});
    if (!hit.valid())
        return;
    if (m_granularity > 0) {
        const At from = resolve(m_spanFrom), to = resolve(m_spanTo);
        const auto [start, end] = around(hit, m_granularity);
        if (hit < from) {
            m_anchor = at(to);
            m_focus = at(start);
        } else {
            m_anchor = at(from);
            m_focus = at(std::max(end, to));
        }
    } else {
        m_focus = at(hit);
        m_focus.outside = outside;
    }
    touch();
}

bool ReplySelection::release()
{
    const bool was = m_dragging;
    m_dragging = false;
    const bool show = settle();
    if (was)
        emit changed();
    return show;
}

bool ReplySelection::hasText(const At &from, const At &to) const
{
    if (!from.valid() || !(from < to))
        return false;
    for (int r = from.row; r <= to.row; ++r) {
        const QString key = r == from.row ? from.key : r == to.row ? to.key : keyAt(r);
        const Row *row = rowOf(key);
        if (!row)
            continue;
        const int first = r == from.row ? from.unit : 0;
        const int last = r == to.row ? to.unit : int(row->units.size()) - 1;
        for (int k = first; k <= last && k < row->units.size(); ++k) {
            const int start = r == from.row && k == from.unit ? from.offset : 0;
            const int end = r == to.row && k == to.unit ? to.offset : unitLength(key, k);
            if (!blank(row->units.at(k).text.text, start, end))
                return true;
        }
    }
    return false;
}

bool ReplySelection::settle()
{
    // selection-menu.js update(): only a reply's range holding more than
    // whitespace brings the menu, the veil and the held wash. A range that
    // runs from a reply on to a place beside the texts (Chromium's caret in
    // the gap below it, or in the space past the last message) with no other
    // message's words between is cut back to the reply; one that takes in
    // another message's words, or ends in one, brings neither.
    auto [from, to] = ordered();
    QString reply;
    if (from.valid() && from < to) {
        const Row *first = rowOf(from.key), *last = rowOf(to.key);
        if (from.row == to.row) {
            if (first && first->reply)
                reply = from.key;
        } else if (first && last) {
            const bool forward = !(resolve(m_focus) < resolve(m_anchor));
            const Pos &early = forward ? m_anchor : m_focus, &late = forward ? m_focus : m_anchor;
            const int end = int(first->units.size()) - 1;
            const At firstEnd{from.row, end, unitLength(from.key, end), from.key};
            const At lastStart{to.row, 0, 0, to.key};
            if (first->reply && late.outside && !hasText(firstEnd, to)) {
                (forward ? m_focus : m_anchor) = at(firstEnd);
                reply = from.key;
                touch();
            } else if (last->reply && early.outside && !hasText(from, lastStart)) {
                (forward ? m_anchor : m_focus) = at(lastStart);
                reply = to.key;
                touch();
            }
        }
    }
    std::tie(from, to) = ordered();
    const bool show = !reply.isEmpty() && hasText(from, to);
    setShown(show);
    if (show)
        setHeld(true);
    return show;
}

void ReplySelection::extendTo(const QString &row, const QString &unit, int offset)
{
    const At hit = resolve(Pos{row, unit, offset});
    if (!hit.valid() || !resolve(m_anchor).valid())
        return;
    m_granularity = 0;
    m_focus = at(hit);
    touch();
    settle();
}

void ReplySelection::selectAll(const QString &row, QObject *document)
{
    if (auto *rich = qobject_cast<RichDocument *>(document); rich && m_documents.value(row) != rich)
        surface(m_surfaces.value(row), row, rich);
    const Row *r = rowOf(row);
    if (!r || r->units.isEmpty())
        return;
    m_granularity = 0;
    m_anchor = Pos{row, r->units.first().path, 0};
    m_focus = Pos{row, r->units.last().path, int(r->units.last().text.text.size())};
    touch();
    settle();
}

void ReplySelection::step(int count, bool words)
{
    At focus = resolve(m_focus);
    if (!focus.valid())
        return;
    for (int n = 0; n < std::abs(count); ++n) {
        const Row *row = rowOf(focus.key);
        const QString &text = row->units.at(focus.unit).text.text;
        if (count > 0) {
            if (focus.offset >= text.size()) {
                if (focus.unit + 1 < row->units.size()) {
                    focus = At{focus.row, focus.unit + 1, 0, focus.key};
                } else if (const int next = neighbour(focus.row, 1); next >= 0) {
                    focus = At{next, 0, 0, keyAt(next)};
                } else {
                    break;
                }
                continue;
            }
            QTextBoundaryFinder finder(words ? QTextBoundaryFinder::Word
                                             : QTextBoundaryFinder::Grapheme,
                                       text);
            finder.setPosition(focus.offset);
            qsizetype next = finder.toNextBoundary();
            // A word step ends after a word, past the spaces before it.
            while (words && next > 0 && next < text.size() &&
                   !(finder.boundaryReasons() & QTextBoundaryFinder::EndOfItem))
                next = finder.toNextBoundary();
            focus.offset = next < 0 ? int(text.size()) : int(next);
        } else {
            if (focus.offset <= 0) {
                if (focus.unit > 0) {
                    focus = At{focus.row, focus.unit - 1, unitLength(focus.key, focus.unit - 1),
                               focus.key};
                } else if (const int previous = neighbour(focus.row, -1); previous >= 0) {
                    const QString key = keyAt(previous);
                    const int last = int(rowOf(key)->units.size()) - 1;
                    focus = At{previous, last, unitLength(key, last), key};
                } else {
                    break;
                }
                continue;
            }
            QTextBoundaryFinder finder(words ? QTextBoundaryFinder::Word
                                             : QTextBoundaryFinder::Grapheme,
                                       text);
            finder.setPosition(focus.offset);
            qsizetype previous = finder.toPreviousBoundary();
            while (words && previous > 0 &&
                   !(finder.boundaryReasons() & QTextBoundaryFinder::StartOfItem))
                previous = finder.toPreviousBoundary();
            focus.offset = previous < 0 ? 0 : int(previous);
        }
    }
    m_granularity = 0;
    m_focus = at(focus);
    touch();
    settle();
}

void ReplySelection::watch(QQuickWindow *window)
{
    if (window)
        window->installEventFilter(this);
}

bool ReplySelection::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        const auto *press = static_cast<QMouseEvent *>(event);
        if (press->button() == Qt::LeftButton)
            emit pressed(press->scenePosition().x(), press->scenePosition().y());
        break;
    }
    case QEvent::MouseMove:
        if (m_dragging) {
            const auto *move = static_cast<QMouseEvent *>(event);
            emit moved(move->scenePosition().x(), move->scenePosition().y());
        }
        break;
    case QEvent::MouseButtonRelease: {
        const auto *release = static_cast<QMouseEvent *>(event);
        if (m_dragging && release->button() == Qt::LeftButton)
            emit released(release->scenePosition().x(), release->scenePosition().y());
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void ReplySelection::note(const QString &row, const QString &unit, const QString &text)
{
    if (row.isEmpty() || unit.isEmpty() || m_notes.value(row).value(unit) == text)
        return;
    if (text.isEmpty()) {
        auto it = m_notes.find(row);
        if (it != m_notes.end() && it->remove(unit) && it->isEmpty())
            m_notes.erase(it);
    } else {
        m_notes[row][unit] = text;
    }
    // The row's texts are read again (rowOf() compares extraOf()).
    touch();
}

void ReplySelection::hide()
{
    setShown(false);
}

void ReplySelection::clear()
{
    const bool had = !m_anchor.row.isEmpty() || m_dragging;
    m_anchor = m_focus = m_spanFrom = m_spanTo = {};
    m_granularity = 0;
    m_dragging = false;
    setShown(false);
    setHeld(false);
    m_rows.clear();
    ++m_epoch;
    if (had)
        touch();
}

void ReplySelection::dragLink(QQuickItem *source, const QString &url, bool selected)
{
    const QString carried = selected ? text() : url;
    emit linkDragged(url, carried);
    if (source)
        source->ungrabMouse();
    if (!m_systemDrag || !source || carried.isEmpty())
        return;
    auto *mime = new QMimeData;
    if (!selected)
        mime->setUrls({QUrl(url)});
    mime->setText(carried);
    // Owned by its source (or by the platform's drag, which may end it).
    auto *drag = new QDrag(source);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction | Qt::LinkAction);
}

QString ReplySelection::text() const
{
    const auto [from, to] = ordered();
    if (!from.valid() || !(from < to))
        return {};
    return copied(from, to);
}

QString ReplySelection::copied(const At &from, const At &to) const
{
    Writer w;
    for (int r = from.row; r <= to.row; ++r) {
        const QString key = r == from.row ? from.key : r == to.row ? to.key : keyAt(r);
        const Row *row = rowOf(key);
        if (!row || row->units.isEmpty())
            continue; // No selectable text in this row.
        const int first = r == from.row ? from.unit : 0;
        const int last = r == to.row ? to.unit : int(row->units.size()) - 1;
        for (int k = first; k <= last; ++k) {
            const SelectionUnit &unit = row->units.at(k);
            const int start = r == from.row && k == from.unit ? from.offset : 0;
            const int end = r == to.row && k == to.unit ? to.offset : int(unit.text.text.size());
            char after = unit.after;
            const QString &shown = unit.copy.isEmpty() ? unit.text.text : unit.copy;
            QString run;
            for (int i = start; i < end && i < shown.size(); ++i) {
                if (shown.at(i) != markdown::Object) {
                    run += shown.at(i);
                    continue;
                }
                w.text(run);
                run.clear();
                if (!unit.diagram.isNull()) {
                    // Its texts as drawn: from the edit shown, at its width.
                    QString source = unit.diagram;
                    if (m_model) {
                        const QString block =
                            unit.path.mid(1, unit.path.lastIndexOf(QLatin1Char(':')) - 1);
                        QVariant edit;
                        QMetaObject::invokeMethod(m_model, "edit", Q_RETURN_ARG(QVariant, edit),
                                                  Q_ARG(QString, key + block));
                        if (edit.typeId() == QMetaType::QString)
                            source = edit.toString();
                    }
                    qreal width = m_diagramWidth, column = 0;
                    for (QQuickItem *item : itemsOf(key, unit.path)) {
                        if (const qreal w = item->property("availableWidth").toReal(); w > 0)
                            width = w;
                        column = item->property("column").toReal();
                    }
                    const QString cacheKey = unit.lang + QLatin1Char('\x1f') +
                                             QString::number(qRound(width)) + QLatin1Char('\x1f') +
                                             QString::number(qRound(column)) + QLatin1Char('\x1f') +
                                             source;
                    auto found = m_diagrams.find(cacheKey);
                    if (found == m_diagrams.end()) {
                        if (m_diagrams.size() >= 32)
                            m_diagrams.clear();
                        diagram::Options options;
                        options.width = width;
                        options.column = column;
                        options.zoom = 16.0 / 14;
                        const diagram::Texts drawn = diagram::texts(source, unit.lang, options);
                        found = m_diagrams.insert(cacheKey, {drawn.texts, drawn.ok});
                    }
                    if (found->second) {
                        for (const QString &t : std::as_const(found->first)) {
                            w.brk();
                            w.text(t);
                        }
                        w.brk();
                    } else {
                        // Never drawn: 1.2 shows (and copies) its source as code.
                        w.text(unit.lang.isEmpty() ? QStringLiteral("mermaid")
                                                   : unit.lang.section(QLatin1Char(' '), 0, 0));
                        w.brk();
                        w.text(source);
                        after = 'l';
                    }
                    continue;
                }
                const auto run_ = std::find_if(unit.text.runs.cbegin(), unit.text.runs.cend(),
                                               [i](const markdown::Run &r) {
                                                   return (r.format & markdown::Math) &&
                                                          r.start <= i && i < r.start + r.length;
                                               });
                if (run_ == unit.text.runs.cend() || run_->ref < 0 ||
                    run_->ref >= unit.text.math.size())
                    continue;
                for (const tex::Piece &piece :
                     tex::copy(unit.text.math.at(run_->ref), unit.display)) {
                    if (piece.kind == tex::Piece::Text)
                        w.text(piece.text);
                    else if (piece.kind == tex::Piece::Break)
                        w.brk();
                    else
                        w.mark();
                }
            }
            w.text(run);
            if (r == to.row && k == to.unit)
                break;
            w.separator(after);
            // The copy button under a run's reply (.message-tools).
            if (k == row->units.size() - 1 && row->copyable)
                w.message();
        }
    }
    return w.out;
}

QPoint ReplySelection::range(const QString &row, const QString &unit) const
{
    if (row.isEmpty() || (row != m_anchor.row && row != m_focus.row && !active()))
        return {-1, -1};
    const auto [from, to] = ordered();
    if (!from.valid() || !(from < to))
        return {-1, -1};
    const int index = indexOf(row);
    if (index < from.row || index > to.row)
        return {-1, -1};
    if (index > from.row && index < to.row)
        return {0, -1}; // A row between the ends: all of it.
    const Row *r = rowOf(row);
    const int k = r ? r->index.value(unit, -1) : -1;
    if (k < 0)
        return {-1, -1};
    int start = 0;
    if (index == from.row) {
        if (k < from.unit)
            return {-1, -1};
        start = k == from.unit ? from.offset : 0;
    }
    if (index == to.row) {
        if (k > to.unit)
            return {-1, -1};
        if (k == to.unit)
            return to.offset <= start ? QPoint(-1, -1) : QPoint(start, to.offset);
    }
    return {start, -1};
}

bool ReplySelection::holds(const QString &row, const QString &prefix) const
{
    if (row.isEmpty() || prefix.isEmpty() || !active())
        return false;
    return (row == m_anchor.row && within(m_anchor.unit, prefix)) ||
           (row == m_focus.row && within(m_focus.unit, prefix));
}

bool ReplySelection::covers(const QString &row, const QString &unit, int offset) const
{
    const QPoint part = range(row, unit);
    return part.x() >= 0 && offset >= part.x() && (part.y() < 0 || offset < part.y());
}

QVector<QQuickItem *> ReplySelection::itemsOf(const QString &row, const QString &unit) const
{
    QVector<QQuickItem *> out;
    for (const Enrolled &e : m_items)
        if (e.item && e.row == row && e.unit == unit)
            out << e.item;
    return out;
}

QVariantMap ReplySelection::hit(const QString &row, QQuickItem *surface, qreal x, qreal y) const
{
    return find(row, surface, x, y, false);
}

QVariantMap ReplySelection::hitInside(const QString &row, QQuickItem *surface, qreal x,
                                      qreal y) const
{
    return find(row, surface, x, y, true);
}

QVariantMap ReplySelection::hitView(QQuickItem *view, qreal x, qreal y, bool inside) const
{
    if (!view)
        return {};
    struct Place {
        QString row;
        QQuickItem *item;
        QRectF rect;
    };
    QVector<Place> places;
    for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
        QQuickItem *item = it.value();
        if (!item || !item->isVisible() || item->window() != view->window())
            continue;
        places << Place{it.key(), item,
                        item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height()))};
    }
    std::sort(places.begin(), places.end(),
              [](const Place &a, const Place &b) { return a.rect.top() < b.rect.top(); });
    const auto answer = [&](const Place &p, QPointF local, bool within) {
        QVariantMap out = find(p.row, p.item, local.x(), local.y(), within);
        if (!out.isEmpty()) {
            out.insert(QStringLiteral("row"), p.row);
            if (out.contains(QStringLiteral("line")))
                out.insert(
                    QStringLiteral("line"),
                    p.item->mapToItem(view, QPointF(0, out.value(QStringLiteral("line")).toReal()))
                        .y());
        }
        return out;
    };
    for (const Place &p : std::as_const(places)) {
        const QPointF local = p.item->mapFromItem(view, QPointF(x, y));
        if (y < p.rect.top()) {
            // The gap above a row: its start.
            if (inside)
                return {};
            if (QVariantMap out = answer(p, QPointF(local.x(), -1), false); !out.isEmpty())
                return out;
            continue;
        }
        if (y < p.rect.bottom()) {
            if (QVariantMap out = answer(p, local, inside); !out.isEmpty() || inside)
                return out;
        }
    }
    if (inside)
        return {};
    // Past the last row: its end.
    for (auto it = places.crbegin(); it != places.crend(); ++it)
        if (QVariantMap out = answer(
                *it,
                QPointF(it->item->mapFromItem(view, QPointF(x, y)).x(), it->item->height() + 1),
                false);
            !out.isEmpty())
            return out;
    return {};
}

QVariantMap ReplySelection::find(const QString &row, QQuickItem *surface, qreal x, qreal y,
                                 bool inside) const
{
    const Row *r = surface ? rowOf(row) : nullptr;
    if (!r)
        return {};
    struct Candidate {
        int index;
        QQuickItem *item;
        QString unit;
        QRectF rect;
    };
    QVector<Candidate> candidates;
    for (const Enrolled &e : m_items) {
        if (!e.item || e.row != row || !e.item->isVisible())
            continue;
        const int k = r->index.value(e.unit, -1);
        if (k < 0)
            continue;
        // What shows of it: a text in a scrolling well is cut to the well.
        QRectF rect = e.item->mapRectToItem(surface, QRectF(0, 0, e.item->width(), e.item->height()));
        for (QQuickItem *p = e.item->parentItem(); p && p != surface; p = p->parentItem())
            if (p->clip())
                rect &= p->mapRectToItem(surface, QRectF(0, 0, p->width(), p->height()));
        if (rect.isEmpty())
            continue;
        candidates << Candidate{k, e.item, e.unit, rect};
    }
    if (candidates.isEmpty())
        return {};
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.index < b.index; });
    const Candidate *chosen = nullptr;
    qreal nearest = 0;
    for (const Candidate &c : std::as_const(candidates)) {
        if (y < c.rect.top() || y >= c.rect.bottom())
            continue;
        const qreal away = x < c.rect.left() ? c.rect.left() - x
                           : x > c.rect.right() ? x - c.rect.right()
                                                : 0;
        if (!chosen || away < nearest) {
            chosen = &c;
            nearest = away;
        }
    }
    QVariantMap out;
    if (chosen) {
        QPointF local = chosen->item->mapFromItem(surface, QPointF(x, y));
        int offset = 0;
        QRectF line(0, 0, 0, chosen->item->height());
        if (QTextDocument *doc = documentOf(chosen->item)) {
            // Above its first line or below its last (in its padding), the
            // nearest line, at x.
            QRectF first, last;
            QMetaObject::invokeMethod(chosen->item, "positionToRectangle",
                                      Q_RETURN_ARG(QRectF, first), Q_ARG(int, 0));
            QMetaObject::invokeMethod(chosen->item, "positionToRectangle",
                                      Q_RETURN_ARG(QRectF, last),
                                      Q_ARG(int, doc->characterCount() - 1));
            if (first.isValid() && last.isValid())
                local.setY(std::clamp(local.y(), first.top() + 0.5,
                                      std::max(first.top() + 0.5, last.bottom() - 0.5)));
            QMetaObject::invokeMethod(chosen->item, "positionAt", Q_RETURN_ARG(int, offset),
                                      Q_ARG(qreal, local.x()), Q_ARG(qreal, local.y()));
            QMetaObject::invokeMethod(chosen->item, "positionToRectangle",
                                      Q_RETURN_ARG(QRectF, line), Q_ARG(int, offset));
        } else {
            // A drawing or a short label: before or after it whole.
            offset = local.x() < chosen->item->width() / 2 ? 0 : unitLength(row, chosen->index);
        }
        out.insert(QStringLiteral("unit"), chosen->unit);
        out.insert(QStringLiteral("offset"), offset);
        // The line it is on (its caret's top), for keyboard line steps.
        out.insert(QStringLiteral("line"), chosen->item->mapRectToItem(surface, line).top());
        return out;
    }
    if (inside)
        return out;
    // Between blocks: the start of the block below; past the last block, its end.
    out.insert(QStringLiteral("outside"), true);
    for (const Candidate &c : std::as_const(candidates)) {
        if (c.rect.top() > y) {
            out.insert(QStringLiteral("unit"), c.unit);
            out.insert(QStringLiteral("offset"), 0);
            return out;
        }
    }
    const Candidate &last = candidates.last();
    out.insert(QStringLiteral("unit"), last.unit);
    out.insert(QStringLiteral("offset"), unitLength(row, last.index));
    return out;
}

QQuickItem *ReplySelection::scrollerAt(QQuickItem *view, qreal x, qreal y) const
{
    if (!view)
        return nullptr;
    for (QQuickItem *item = deepest(view, QPointF(x, y)); item && item != view;
         item = item->parentItem())
        if (item->inherits("QQuickFlickable") && item->property("interactive").toBool())
            return item;
    return nullptr;
}

QString ReplySelection::linkAt(const QString &row, QQuickItem *surface, qreal x, qreal y) const
{
    if (!surface)
        return {};
    for (const Enrolled &e : m_items) {
        if (!e.item || e.row != row || !e.item->isVisible() || !documentOf(e.item))
            continue;
        const QPointF local = e.item->mapFromItem(surface, QPointF(x, y));
        if (!e.item->contains(local))
            continue;
        QString link;
        QMetaObject::invokeMethod(e.item, "linkAt", Q_RETURN_ARG(QString, link),
                                  Q_ARG(qreal, local.x()), Q_ARG(qreal, local.y()));
        if (!link.isEmpty())
            return link;
    }
    return {};
}

bool ReplySelection::control(QQuickItem *root, qreal x, qreal y) const
{
    if (!root)
        return false;
    for (QQuickItem *item = deepest(root, QPointF(x, y)); item && item != root;
         item = item->parentItem()) {
        if (item->inherits("QQuickAbstractButton") || item->inherits("QQuickMouseArea") ||
            item->inherits("QQuickTextInput") || item->property("selectionControl").toBool())
            return true;
        if (item->inherits("QQuickTextEdit") && !item->property("readOnly").toBool())
            return true;
    }
    return false;
}

bool ReplySelection::keeps(QQuickItem *root, qreal x, qreal y) const
{
    if (!root)
        return false;
    // A press that reaches a row's SelectArea is its own: a new range, Shift
    // extending this one (as Chromium's), or a link's click or drag.
    if (const QQuickItem *top = pressTarget(root, QPointF(x, y));
        top && top->objectName() == QLatin1String("selectArea"))
        return true;
    for (QQuickItem *item = deepest(root, QPointF(x, y)); item && item != root;
         item = item->parentItem()) {
        if (item->inherits("QQuickTextInput") ||
            (item->inherits("QQuickTextEdit") && !item->property("readOnly").toBool()))
            return false;
        if (item->inherits("QQuickAbstractButton") || item->inherits("QQuickMouseArea") ||
            item->property("selectionControl").toBool())
            return true;
    }
    return false;
}

QRectF ReplySelection::caretIn(QQuickItem *target) const
{
    const At focus = resolve(m_focus);
    if (!target || !focus.valid())
        return {};
    for (QQuickItem *item : itemsOf(m_focus.row, m_focus.unit)) {
        if (!item->isVisible())
            continue;
        QRectF rect(0, 0, 1, item->height());
        if (documentOf(item))
            QMetaObject::invokeMethod(item, "positionToRectangle", Q_RETURN_ARG(QRectF, rect),
                                      Q_ARG(int, focus.offset));
        else if (focus.offset > 0)
            rect.moveLeft(item->width());
        return item->mapRectToItem(target, rect);
    }
    return {};
}

QVariantMap ReplySelection::geometry() const
{
    QQuickItem *surface = rowSurface();
    const QString one = row();
    const Row *r = rowOf(one);
    const auto [from, to] = ordered();
    if (!surface || !r || !from.valid() || !(from < to))
        return {};
    // Range.getClientRects(): the selected glyph runs of each text...
    struct Text {
        int index;
        QQuickItem *item;
    };
    QVector<Text> texts;
    for (const Enrolled &e : m_items) {
        if (!e.item || e.row != one || !e.item->isVisible())
            continue;
        const int k = r->index.value(e.unit, -1);
        if (k >= from.unit && k <= to.unit)
            texts << Text{k, e.item};
    }
    std::sort(texts.begin(), texts.end(),
              [](const Text &a, const Text &b) { return a.index < b.index; });
    QVector<QRectF> rects;
    QRectF first, last;
    for (const Text &t : std::as_const(texts)) {
        const int start = t.index == from.unit ? from.offset : 0;
        const int end = t.index == to.unit ? to.offset : -1;
        if (end >= 0 && end <= start)
            continue;
        for (const QRectF &rect : SelectionWash::lines(t.item, start, end, false)) {
            const QRectF mapped = t.item->mapRectToItem(surface, rect);
            if (mapped.width() <= 0 || mapped.height() <= 0)
                continue;
            rects << mapped;
            if (first.isNull() && t.index == from.unit)
                first = mapped;
            last = mapped;
        }
    }
    // ...and the box of every block wholly inside the range whose parent is not.
    QVector<std::pair<QString, QQuickItem *>> boxes;
    for (const Enrolled &e : m_boxes) {
        if (!e.item || e.row != one || !e.item->isVisible())
            continue;
        const auto span = r->spans.constFind(e.unit);
        // Its texts all lie strictly between the range's ends: a range
        // starting or ending inside a block does not select the block.
        if (span == r->spans.cend() || span->first <= from.unit || span->second >= to.unit)
            continue;
        boxes.append({e.unit, e.item});
    }
    for (const auto &[prefix, item] : std::as_const(boxes)) {
        const bool inner = std::any_of(boxes.cbegin(), boxes.cend(), [&prefix](const auto &other) {
            return other.first.size() < prefix.size() && within(prefix, other.first);
        });
        if (inner)
            continue;
        const QRectF mapped = item->mapRectToItem(surface, QRectF(0, 0, item->width(), item->height()));
        if (mapped.width() > 0 || mapped.height() > 0)
            rects << mapped;
    }
    if (first.isNull() && !rects.isEmpty())
        first = rects.first();
    QVariantMap out;
    out.insert(QStringLiteral("cuts"), cuts(bands(rects)));
    out.insert(QStringLiteral("first"), first);
    out.insert(QStringLiteral("last"), last);
    return out;
}

QVector<ReplySelection::Band> ReplySelection::bands(QVector<QRectF> rects)
{
    rects.erase(std::remove_if(rects.begin(), rects.end(),
                               [](const QRectF &r) { return r.width() <= 0 || r.height() <= 0; }),
                rects.end());
    std::stable_sort(rects.begin(), rects.end(),
                     [](const QRectF &a, const QRectF &b) { return a.top() < b.top(); });
    QVector<Band> lines;
    for (const QRectF &rect : std::as_const(rects)) {
        // Rects on one line merge into a band.
        Band *line = nullptr;
        for (Band &item : lines) {
            const qreal overlap =
                std::min(item.bottom, rect.bottom()) - std::max(item.top, rect.top());
            if (overlap > 0.5 * std::min(item.bottom - item.top, rect.height())) {
                line = &item;
                break;
            }
        }
        if (line) {
            line->left = std::min(line->left, rect.left());
            line->right = std::max(line->right, rect.right());
            line->top = std::min(line->top, rect.top());
            line->bottom = std::max(line->bottom, rect.bottom());
        } else {
            lines << Band{rect.left(), rect.right(), rect.top(), rect.bottom()};
        }
    }
    std::stable_sort(lines.begin(), lines.end(),
                     [](const Band &a, const Band &b) { return a.top < b.top; });
    // Lines closer than Join close up into one lit block.
    for (int i = 0; i + 1 < lines.size(); ++i) {
        Band &a = lines[i], &b = lines[i + 1];
        const qreal gap = b.top - a.bottom;
        if (gap > Join)
            continue;
        if (gap > 0)
            a.bottom = b.top = (a.bottom + b.top) / 2;
        a.joinedBottom = b.joinedTop = true;
    }
    return lines;
}

QVariantList ReplySelection::cuts(const QVector<Band> &bands)
{
    // px(): to half pixels, as selection-focus.js places mask layers.
    const auto px = [](qreal v) { return std::floor(v * 2 + 0.5) / 2; }; // JS Math.round
    QVariantList out;
    const auto cut = [&out, &px](const char *kind, qreal x, qreal y, qreal w, qreal h) {
        out << QVariantMap{{QStringLiteral("kind"), QString::fromLatin1(kind)},
                           {QStringLiteral("x"), px(x)},
                           {QStringLiteral("y"), px(y)},
                           {QStringLiteral("w"), px(w)},
                           {QStringLiteral("h"), px(h)}};
    };
    for (const Band &line : bands) {
        const qreal x = line.left - PadX - FeatherX;
        const qreal width = line.right - line.left + 2 * (PadX + FeatherX);
        const qreal top = line.top - (line.joinedTop ? 0 : PadY);
        const qreal bottom = line.bottom + (line.joinedBottom ? 0 : PadY);
        cut("band", x, top, width, bottom - top);
        if (!line.joinedTop)
            cut("rise", x + FeatherX, top - FeatherY, width - 2 * FeatherX, FeatherY);
        if (!line.joinedBottom)
            cut("fall", x + FeatherX, bottom, width - 2 * FeatherX, FeatherY);
    }
    return out;
}

// ---- SelectionWash ---------------------------------------------------------

SelectionWash::SelectionWash(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents);
}

void SelectionWash::setTarget(QQuickItem *target)
{
    if (target == m_target)
        return;
    if (m_target)
        disconnect(m_target, nullptr, this, nullptr);
    disconnect(m_layoutUpdate);
    disconnect(m_documentChange);
    m_target = target;
    if (m_target) {
        const QMetaObject *meta = m_target->metaObject();
        const int schedule = metaObject()->indexOfSlot("schedule()");
        for (const char *signal : {"widthChanged()", "heightChanged()", "topPaddingChanged()",
                                   "leftPaddingChanged()"}) {
            if (const int index = meta->indexOfSignal(signal); index >= 0)
                QMetaObject::connect(m_target, index, this, schedule);
        }
        if (QTextDocument *doc = documentOf(m_target)) {
            m_documentChange =
                connect(doc, &QTextDocument::contentsChanged, this, &SelectionWash::schedule);
            m_layoutUpdate = connect(doc->documentLayout(), &QAbstractTextDocumentLayout::update,
                                     this, &SelectionWash::schedule);
        }
    }
    emit targetChanged();
    polish();
}

void SelectionWash::setRange(QPoint range)
{
    if (range == m_range)
        return;
    m_range = range;
    emit rangeChanged();
    polish();
}

void SelectionWash::setColor(const QColor &color)
{
    if (color == m_color)
        return;
    m_color = color;
    emit colorChanged();
    update();
}

QVariantList SelectionWash::rects() const
{
    QVariantList out;
    for (const QRectF &rect : m_rects)
        out << rect;
    return out;
}

void SelectionWash::relayout()
{
    QVector<QRectF> next;
    if (m_target && m_range.x() >= 0) {
        for (const QRectF &rect : lines(m_target, m_range.x(), m_range.y(), true))
            next << mapRectFromItem(m_target, rect);
    }
    if (next != m_rects) {
        m_rects = next;
        emit rectsChanged();
        update();
    }
}

QVector<QRectF> SelectionWash::lines(QQuickItem *text, int start, int end, bool highlight)
{
    QVector<QRectF> out;
    if (!text || start < 0)
        return out;
    QTextDocument *doc = documentOf(text);
    if (!doc) {
        if (end >= 0 && end <= start)
            return out;
        // A drawing's lines of text (a diagram's labels), else one box.
        for (const QVariant &rect : text->property("selectionRects").toList())
            out << rect.toRectF();
        if (out.isEmpty())
            out << QRectF(0, 0, text->width(), text->height());
        return out;
    }
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    layout->documentSize(); // Laid out.
    const int length = doc->characterCount() - 1;
    const bool runsOn = end < 0;
    const int from = std::clamp(start, 0, length);
    const int to = runsOn ? length : std::clamp(end, from, length);
    const qreal left = text->property("leftPadding").toReal();
    const qreal top = text->property("topPadding").toReal();
    for (QTextBlock block = doc->findBlock(from); block.isValid() && block.position() <= to;
         block = block.next()) {
        const QTextLayout *lay = block.layout();
        if (!lay)
            continue;
        const QPointF origin = lay->position();
        const QTextBlockFormat format = block.blockFormat();
        const bool lastBlock = !block.next().isValid() || block.next().position() > length;
        for (int i = 0; i < lay->lineCount(); ++i) {
            const QTextLine line = lay->lineAt(i);
            const int lineStart = block.position() + line.textStart();
            const bool lastLine = i == lay->lineCount() - 1;
            const int lineEnd = lineStart + line.textLength();
            const int a = std::max(from, lineStart), b = std::min(to, lineEnd);
            if (b < a)
                continue;
            // The range runs past this line's end: on to the next line of
            // the block, past the block's end, or past the text.
            const bool past = lastLine ? (runsOn && lastBlock) || to > lineEnd : to > lineEnd;
            if (a == b && !(highlight && past && lastLine))
                continue;
            const qreal ascent = line.ascent(), descent = line.descent();
            const qreal baseline = origin.y() + line.y() + ascent;
            qreal x0 = origin.x() + line.cursorToX(a);
            qreal x1 = origin.x() + line.cursorToX(b);
            qreal y0 = baseline - ascent, y1 = baseline + descent;
            if (highlight) {
                // The CSS line box (Theme.lines and InlineFormat set a
                // fixed or least height), centred on the glyphs.
                qreal box = line.height();
                if (format.lineHeightType() == QTextBlockFormat::FixedHeight)
                    box = format.lineHeight();
                else if (format.lineHeightType() == QTextBlockFormat::MinimumHeight)
                    box = std::max(format.lineHeight(), line.height());
                const qreal halfLeading = (box - ascent - descent) / 2;
                y0 -= halfLeading;
                y1 = y0 + box;
                if (past) {
                    if (!lastLine) {
                        x1 = origin.x() + line.x() + line.width();
                    } else {
                        QTextCursor cursor(block);
                        cursor.setPosition(lineEnd);
                        x1 = origin.x() + line.cursorToX(lineEnd) +
                             QFontMetricsF(cursor.charFormat().font())
                                 .horizontalAdvance(QLatin1Char(' '));
                    }
                }
            }
            if (x1 < x0)
                std::swap(x0, x1);
            out << QRectF(left + x0, top + y0, x1 - x0, y1 - y0);
        }
    }
    return out;
}

QSGNode *SelectionWash::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    if (m_rects.isEmpty() || m_color.alpha() == 0) {
        delete old;
        return nullptr;
    }
    auto *node = static_cast<QSGGeometryNode *>(old);
    if (!node) {
        node = new QSGGeometryNode;
        auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGFlatColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    QSGGeometry *geometry = node->geometry();
    geometry->allocate(int(m_rects.size()) * 6);
    QSGGeometry::Point2D *v = geometry->vertexDataAsPoint2D();
    for (const QRectF &r : std::as_const(m_rects)) {
        const float l = float(r.left()), t = float(r.top()), rr = float(r.right()),
                    b = float(r.bottom());
        *v++ = {l, t};
        *v++ = {rr, t};
        *v++ = {l, b};
        *v++ = {rr, t};
        *v++ = {rr, b};
        *v++ = {l, b};
    }
    node->markDirty(QSGNode::DirtyGeometry);
    auto *material = static_cast<QSGFlatColorMaterial *>(node->material());
    if (material->color() != m_color) {
        material->setColor(m_color);
        node->markDirty(QSGNode::DirtyMaterial);
    }
    return node;
}
