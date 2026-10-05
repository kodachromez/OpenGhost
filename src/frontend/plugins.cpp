#include "plugins.h"
#include <QSet>
#include <utility>

namespace openghost
{
namespace
{
template <class T> const T *value(const Result &result)
{
    const auto *reply = std::get_if<Reply>(&result);
    return reply ? std::get_if<T>(reply) : nullptr;
}
QString failure(const Result &result)
{
    if (const auto *error = std::get_if<Error>(&result))
        return error->message.isEmpty() ? error->code : error->message;
    return QStringLiteral("Unexpected plugin reply. Refresh to check the backend state.");
}
bool same(const Plugin &a, const Plugin &b)
{
    return a.id == b.id && a.name == b.name && a.description == b.description &&
           a.builtIn == b.builtIn && a.enabled == b.enabled && a.state == b.state &&
           a.available == b.available && a.activeCalls == b.activeCalls &&
           a.interruption == b.interruption && a.hooks == b.hooks && a.tools == b.tools;
}
} // namespace

Plugins::Plugins(Dispatch dispatch, QObject *parent)
    : QObject(parent), m_dispatch(std::move(dispatch))
{
}
void Plugins::initialize(bool supported, const QString &connection)
{
    ++m_generation; // Abandon callbacks from an earlier connection/initialization.
    const bool flipped = m_supported != supported;
    m_supported = supported;
    m_connection = supported ? connection : QString();
    m_refreshAgain = false;
    const bool hadRows = !m_entries.isEmpty();
    m_entries.clear();
    m_index.clear();
    setState(false, false, {});
    if (hadRows)
        emit entriesChanged();
    if (flipped)
        emit supportChanged();
    if (supported)
        refresh();
}
void Plugins::setState(bool loading, bool loaded, const QString &error)
{
    if (m_loading == loading && m_loaded == loaded && m_error == error)
        return;
    m_loading = loading;
    m_loaded = loaded;
    m_error = error;
    emit stateChanged();
}
void Plugins::reindex()
{
    m_index.clear();
    m_index.reserve(m_entries.size());
    for (int i = 0; i < m_entries.size(); ++i)
        m_index.insert(m_entries.at(i).snapshot.id, i);
}
PluginEntry *Plugins::find(const QString &id)
{
    const int at = m_index.value(id, -1);
    return at < 0 ? nullptr : &m_entries[at];
}
bool Plugins::adopt(PluginEntry &entry, const Plugin &snapshot, quint64 seen)
{
    entry.seen = seen;
    bool shown = false;
    if (!same(entry.snapshot, snapshot)) {
        entry.snapshot = snapshot;
        shown = true;
    }
    if (entry.doubt != 0 && entry.doubt < seen) {
        // Observed after the failure: reconciled, so its error is stale.
        entry.doubt = 0;
        entry.error.clear();
        shown = true;
    }
    if (shown)
        touch(entry);
    return shown;
}
void Plugins::observe(const PluginChanged &event)
{
    // Only the current connection's events: an empty or other connection ID
    // is a late delivery from a replaced connection, never this one's state.
    if (!m_supported || event.plugin.id.isEmpty() || m_connection.isEmpty() ||
        event.connectionId != m_connection)
        return;
    auto *entry = find(event.plugin.id);
    if (!entry) {
        m_index.insert(event.plugin.id, int(m_entries.size()));
        m_entries.append(PluginEntry{});
        entry = &m_entries.last();
    }
    // An identical snapshot still counts as an observation (it orders later
    // lists and answers), but changes nothing shown.
    if (adopt(*entry, event.plugin, ++m_tick))
        emit entriesChanged();
}
void Plugins::refresh()
{
    if (!m_supported)
        return;
    if (m_loading) {
        m_refreshAgain = true; // Sent after this one, so it can settle newer doubt.
        return;
    }
    const auto sent = ++m_tick;
    setState(true, m_loaded, m_error);
    m_dispatch(PluginsList{}, [this, sent, generation = m_generation](const Result &result) {
        if (generation != m_generation)
            return;
        const auto *list = value<PluginsListed>(result);
        bool valid = list != nullptr;
        if (list) {
            QSet<QString> ids;
            ids.reserve(list->plugins.size());
            for (const auto &plugin : list->plugins) {
                valid &= !plugin.id.isEmpty() && !ids.contains(plugin.id);
                ids.insert(plugin.id);
            }
        }
        bool rows = false;
        if (valid) {
            // Entries move, not copy, into list order; local request
            // bookkeeping is kept, never an optimistic state.
            QVector<PluginEntry> next;
            next.reserve(list->plugins.size());
            QVector<bool> kept(m_entries.size(), false);
            for (const auto &plugin : list->plugins) {
                const int at = m_index.value(plugin.id, -1);
                PluginEntry entry;
                if (at >= 0) {
                    entry = std::move(m_entries[at]);
                    kept[at] = true;
                }
                rows |= at != int(next.size()); // New or moved.
                // A row observed after this list was sent is newer than it.
                if (entry.seen <= sent)
                    rows |= adopt(entry, plugin, sent);
                next.append(std::move(entry));
            }
            // IDs absent from the list survive only if observed after it was sent.
            for (int i = 0; i < m_entries.size(); ++i) {
                if (kept.at(i))
                    continue;
                rows = true; // Dropped, or kept after the listed rows.
                if (m_entries.at(i).seen > sent)
                    next.append(std::move(m_entries[i]));
            }
            m_entries = std::move(next);
            reindex();
            setState(false, true, list->error.value_or(QString()));
        } else {
            // Last-known states remain visible but cannot authorize another toggle.
            const auto doubt = ++m_tick;
            for (auto &entry : m_entries)
                if (entry.seen <= sent) {
                    if (entry.confirmed()) {
                        touch(entry);
                        rows = true;
                    }
                    entry.doubt = doubt;
                }
            setState(false, m_loaded, failure(result));
        }
        if (rows)
            emit entriesChanged();
        if (std::exchange(m_refreshAgain, false))
            refresh();
    });
}
void Plugins::setEnabled(const QString &id, bool enabled)
{
    auto *entry = find(id);
    if (!m_supported || !entry || !entry->canToggle() || entry->snapshot.enabled == enabled)
        return;
    entry->pending = true;
    entry->error.clear();
    entry->warning.clear();
    entry->persisted.clear();
    touch(*entry);
    const auto sent = ++m_tick;
    emit entriesChanged();
    const Command command = enabled ? Command{EnablePlugin{id}} : Command{DisablePlugin{id}};
    m_dispatch(command, [this, id, sent, generation = m_generation](const Result &result) {
        if (generation != m_generation)
            return;
        auto *entry = find(id);
        if (!entry)
            return;
        entry->pending = false;
        touch(*entry);
        const auto *update = value<PluginUpdated>(result);
        if (update && update->plugin.id == id) {
            // Anything observed since sending (an event, a newer list) is at
            // least as new as this answer: keep it.
            if (entry->seen < sent)
                adopt(*entry, update->plugin, ++m_tick);
            entry->warning = update->warning.value_or(QString());
            entry->persisted = update->persisted;
            emit entriesChanged();
            if (!m_error.isEmpty())
                refresh(); // A successful change may have repaired a saved-state load error.
        } else {
            entry->error = failure(result);
            entry->doubt = ++m_tick;
            emit entriesChanged();
            // Errors (including uncertain delivery) never imply a state. Read it
            // back with a list sent after this point; while that fails, leave
            // the last-known control non-interactive. Never retry the change.
            refresh();
        }
    });
}
} // namespace openghost
