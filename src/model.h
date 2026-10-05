#pragma once

#include "frontend/plugins.h"
#include "presentation.h"

#include <QAbstractListModel>
#include <QSet>

#include <algorithm>
#include <limits>

// A list model applied as keyed diffs: unchanged rows keep their delegates,
// persistent indexes and scroll position; a changed row reports only its
// changed roles, and rows out of order move without disturbing the others.
class KeyedModel : public QAbstractListModel
{
  public:
    using QAbstractListModel::QAbstractListModel;

  protected:
    // key(row) -> QString; update(index, old, new) replaces a same-key row.
    // More than maxBlocks separate removals, moves and insertions (a search
    // over a long list) reset the model instead, so a diff never costs more
    // than a rebuild. Returns false after such a reset.
    template <class Row, class Key, class Update>
    bool diff(QVector<Row> &rows, const QVector<Row> &next, Key key, Update update,
              int maxBlocks = std::numeric_limits<int>::max())
    {
        QHash<QString, int> at;
        at.reserve(next.size());
        for (int i = 0; i < next.size(); ++i)
            at.insert(key(next.at(i)), i);
        // to: each row's new position, -1 when it leaves; from: the reverse.
        QVector<int> to(rows.size(), -1), from(next.size(), -1);
        for (int i = 0; i < rows.size(); ++i) {
            const int t = at.value(key(rows.at(i)), -1);
            if (t >= 0 && from.at(t) < 0)
                to[i] = t, from[t] = i;
        }
        // The longest run of rows already in order stays; only the others
        // move, so a first-to-last move is one row, not all the rest.
        const QVector<bool> stays = inOrder(to, int(next.size()));
        const auto moves = [&](int t) { return from.at(t) >= 0 && !stays.at(t); };
        int blocks = 0;
        for (int i = 0; i < rows.size(); ++i)
            blocks += to.at(i) < 0 && (i == 0 || to.at(i - 1) >= 0);
        for (int t = 0; t < next.size(); ++t) {
            blocks += from.at(t) < 0 && (t == 0 || from.at(t - 1) >= 0);
            blocks += moves(t) && (t == 0 || !moves(t - 1) || from.at(t - 1) + 1 != from.at(t));
        }
        if (blocks > maxBlocks) {
            beginResetModel();
            rows = next;
            endResetModel();
            return false;
        }
        for (int i = int(rows.size()) - 1; i >= 0;) {
            if (to.at(i) >= 0) {
                --i;
                continue;
            }
            const int last = i;
            while (i >= 0 && to.at(i) < 0)
                --i;
            beginRemoveRows({}, i + 1, last);
            rows.remove(i + 1, last - i);
            to.remove(i + 1, last - i);
            endRemoveRows();
        }
        // Now rows are exactly the kept ones. From the end, each moving block
        // goes just before the row that follows it in next (the anchor).
        QVector<int> where(next.size(), -1); // New position -> current row.
        for (int i = 0; i < rows.size(); ++i)
            where[to.at(i)] = i;
        for (int t = int(next.size()) - 1, anchor = int(rows.size()); t >= 0; --t) {
            if (from.at(t) < 0)
                continue;
            if (!moves(t)) {
                anchor = where.at(t);
                continue;
            }
            const int last = t;
            while (t > 0 && moves(t - 1) && where.at(t - 1) + 1 == where.at(t))
                --t;
            const int begin = where.at(t), end = where.at(last) + 1;
            if (end != anchor) {
                beginMoveRows({}, begin, end - 1, {}, anchor);
                const int low = qMin(begin, anchor), middle = end < anchor ? end : begin,
                          high = qMax(end, anchor);
                std::rotate(rows.begin() + low, rows.begin() + middle, rows.begin() + high);
                std::rotate(to.begin() + low, to.begin() + middle, to.begin() + high);
                for (int i = low; i < high; ++i)
                    where[to.at(i)] = i;
                endMoveRows();
            }
            anchor = where.at(t);
        }
        for (int i = 0; i < next.size();) {
            if (from.at(i) >= 0) {
                update(i, rows[i], next.at(i));
                ++i;
                continue;
            }
            int end = i;
            while (end < next.size() && from.at(end) < 0)
                ++end;
            beginInsertRows({}, i, end - 1);
            rows.insert(i, end - i, Row());
            std::copy(next.cbegin() + i, next.cbegin() + end, rows.begin() + i);
            endInsertRows();
            i = end;
        }
        return true;
    }

  private:
    // Of the kept rows (to >= 0, in current order), one longest subsequence
    // whose new positions increase; indexed by new position.
    static QVector<bool> inOrder(const QVector<int> &to, int size);
};

// GUI-thread transcript rows for QML.
class TranscriptModel final : public KeyedModel
{
    Q_OBJECT
  public:
    enum Role {
        KindRole = Qt::UserRole + 1,
        KeyRole,
        BodyRole,
        PreviewRole,
        MessageStateRole,
        CopyableRole,
        AttachmentsRole,
        MetricsRole,
        StartedRole,
        CompletedRole,
        JoinRole,
    };

    using KeyedModel::KeyedModel;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void apply(const QVector<Entry> &rows);
    // Another conversation: every delegate and presentation edit is replaced.
    void reset(const QVector<Entry> &rows);
    Q_INVOKABLE int indexOf(const QString &key) const;
    // A diagram's edited source by row key and block path: presentation
    // only (never sent), kept while the conversation is shown. An edit may
    // be empty or not parse; none (undefined) shows the message's source.
    Q_INVOKABLE QVariant edit(const QString &key) const;
    // Anything but a string drops the edit.
    Q_INVOKABLE void setEdit(const QString &key, const QVariant &source);

  private:
    friend class ModelTest;
    QVector<Entry> m_rows;
    QHash<QString, QString> m_edits;
};

// The saved-session sidebar: OpenGhost's list, sorted and filtered by title on the
// worker. The query only records what the reader typed; the worker's answer
// for it is applied as a keyed diff.
//
// Pins are OpenGhost's (library.js): a frontend chat-index annotation, never
// sent to OpenGhost's backend. Each answer carries the saved pin; a toggle is
// shown at once and reported (pinToggled) for saving. Pinned rows lead the
// list, newest first, in their own group ("pinned:"); the others keep the
// worker's order: chats without a folder (an empty folder path) in OpenGhost
// 1.3's home group ("home:"), then the rest grouped by folder path.
class SessionModel final : public KeyedModel
{
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int pinnedCount READ pinnedCount NOTIFY groupsChanged)
    // Unpinned chats without a folder, listed right after the pinned ones.
    Q_PROPERTY(int homeCount READ homeCount NOTIFY groupsChanged)
    // The folders shown, in list order (pinned and folderless rows excluded).
    Q_PROPERTY(QStringList folders READ folders NOTIFY groupsChanged)
    // Kept folders (FolderStore) with no listed chat, latest added first.
    Q_PROPERTY(QStringList emptyFolders READ emptyFolders NOTIFY groupsChanged)
    // The folders shown and the empty ones, in OpenGhost 1.2's order.
    Q_PROPERTY(QStringList arranged READ arranged NOTIFY groupsChanged)
  public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        FolderRole,
        FolderNameRole,
        UpdatedRole,
        PinnedRole,
        GroupRole,
    };

    using KeyedModel::KeyedModel;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void apply(const QVector<Session> &sessions);
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    int pinnedCount() const { return m_pinnedCount; }
    int homeCount() const { return m_homeCount; }
    QStringList folders() const { return m_folders; }
    QStringList emptyFolders() const { return m_emptyFolders; }
    QStringList arranged() const { return m_arranged; }
    // `order`: every folder, listed or empty, by activity.
    void setEmptyFolders(const QStringList &folders, const QStringList &order);
    // The empty folders drawn just before the shown `folder`; with "", those
    // after the last.
    Q_INVOKABLE QStringList emptyBefore(const QString &folder) const
    {
        return m_emptyBefore.value(folder);
    }
    Q_INVOKABLE void togglePin(const QString &id);
    // A deleted session's memory-only pin goes with it; its row leaves with
    // the worker's next list.
    void forget(const QString &id) { m_pinned.remove(id); }
    Q_INVOKABLE QString folderAt(int row) const;
    // A listed session's folder: empty for a chat without one, null when it
    // is not listed.
    QString folderOf(const QString &id) const;

  signals:
    void queryChanged();
    void groupsChanged();
    void pinToggled(QString id, bool pinned);

  private:
    void place();
    void arrange();

    QVector<Session> m_source; // The worker's answer, in its order.
    QVector<Session> m_rows;
    QSet<QString> m_pinned;
    int m_pinnedCount = 0;
    int m_homeCount = 0;
    QStringList m_folders;
    QStringList m_emptyFolders;
    QStringList m_order;
    QStringList m_arranged;
    QHash<QString, QStringList> m_emptyBefore;
    QString m_query;
};

// Settings' runtime plugin rows, formatted from the backend's snapshots. A
// keyed diff by plugin ID: an update changes roles in place, so a row's
// delegate (and its keyboard focus) survives pending flips and events. Only
// entries whose revision moved are formatted again.
class PluginModel final : public KeyedModel
{
    Q_OBJECT
  public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        DescriptionRole,
        StatusRole,
        EnabledRole,
        AvailableRole,
        PendingRole,
        CanToggleRole,
        NoteRole,
        NoteErrorRole,
    };
    struct Row {
        QString id, name, description, status, note;
        bool enabled = false, available = false, pending = false, canToggle = false;
        bool noteError = false;
        quint64 revision = 0; // PluginEntry::revision it was formatted from.
    };

    using KeyedModel::KeyedModel;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void sync(const QVector<openghost::PluginEntry> &entries);

  private:
    static Row format(const openghost::PluginEntry &entry);
    void update(int i, Row &old, const Row &now);
    QVector<Row> m_rows;
};
