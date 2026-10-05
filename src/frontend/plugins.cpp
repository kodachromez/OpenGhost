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
} // namespace

Plugins::Plugins(Dispatch dispatch, QObject *parent)
    : QObject(parent), m_dispatch(std::move(dispatch))
{
}
void Plugins::initialize(bool supported)
{
    ++m_generation; // Abandon callbacks from an earlier connection/initialization.
    const bool flipped = m_supported != supported;
    m_supported = supported;
    m_loading = m_loaded = m_refreshAgain = false;
    m_entries.clear();
    m_error.clear();
    emit changed();
    if (flipped)
        emit supportChanged();
    if (supported)
        refresh();
}
PluginEntry *Plugins::find(const QString &id)
{
    for (auto &entry : m_entries)
        if (entry.snapshot.id == id)
            return &entry;
    return nullptr;
}
void Plugins::adopt(PluginEntry &entry, const Plugin &snapshot, quint64 seen)
{
    entry.snapshot = snapshot;
    entry.seen = seen;
    if (entry.doubt != 0 && entry.doubt < seen) {
        // Observed after the failure: reconciled, so its error is stale.
        entry.doubt = 0;
        entry.error.clear();
    }
}
void Plugins::observe(const PluginChanged &event)
{
    if (!m_supported || event.plugin.id.isEmpty())
        return;
    auto *entry = find(event.plugin.id);
    if (!entry) {
        m_entries.append(PluginEntry{});
        entry = &m_entries.last();
    }
    adopt(*entry, event.plugin, ++m_tick);
    emit changed();
}
void Plugins::refresh()
{
    if (!m_supported)
        return;
    if (m_loading) {
        m_refreshAgain = true; // Sent after this one, so it can settle newer doubt.
        return;
    }
    m_loading = true;
    const auto sent = ++m_tick;
    emit changed();
    m_dispatch(PluginsList{}, [this, sent, generation = m_generation](const Result &result) {
        if (generation != m_generation)
            return;
        m_loading = false;
        const auto *list = value<PluginsListed>(result);
        QSet<QString> ids;
        bool valid = list != nullptr;
        if (list)
            for (const auto &plugin : list->plugins) {
                valid &= !plugin.id.isEmpty() && !ids.contains(plugin.id);
                ids.insert(plugin.id);
            }
        if (valid) {
            QVector<PluginEntry> entries;
            entries.reserve(list->plugins.size());
            for (const auto &plugin : list->plugins) {
                // Retain local request bookkeeping, never an optimistic state.
                const auto *previous = find(plugin.id);
                auto entry = previous ? *previous : PluginEntry{};
                // A row observed after this list was sent is newer than it.
                if (entry.seen <= sent)
                    adopt(entry, plugin, sent);
                entries.append(entry);
            }
            // IDs absent from the list survive only if observed after it was sent.
            for (const auto &entry : std::as_const(m_entries))
                if (!ids.contains(entry.snapshot.id) && entry.seen > sent)
                    entries.append(entry);
            m_entries = std::move(entries);
            m_loaded = true;
            m_error = list->error.value_or(QString());
        } else {
            m_error = failure(result);
            // Last-known states remain visible but cannot authorize another toggle.
            const auto doubt = ++m_tick;
            for (auto &entry : m_entries)
                if (entry.seen <= sent)
                    entry.doubt = doubt;
        }
        const bool again = std::exchange(m_refreshAgain, false);
        emit changed();
        if (again)
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
    const auto sent = ++m_tick;
    emit changed();
    const Command command = enabled ? Command{EnablePlugin{id}} : Command{DisablePlugin{id}};
    m_dispatch(command, [this, id, sent, generation = m_generation](const Result &result) {
        if (generation != m_generation)
            return;
        auto *entry = find(id);
        if (!entry)
            return;
        entry->pending = false;
        const auto *update = value<PluginUpdated>(result);
        if (update && update->plugin.id == id) {
            // Anything observed since sending (an event, a newer list) is at
            // least as new as this answer: keep it.
            if (entry->seen < sent)
                adopt(*entry, update->plugin, ++m_tick);
            entry->warning = update->warning.value_or(QString());
            entry->persisted = update->persisted;
            emit changed();
            if (!m_error.isEmpty())
                refresh(); // A successful change may have repaired a saved-state load error.
        } else {
            entry->error = failure(result);
            entry->doubt = ++m_tick;
            emit changed();
            // Errors (including uncertain delivery) never imply a state. Read it
            // back with a list sent after this point; while that fails, leave
            // the last-known control non-interactive. Never retry the change.
            refresh();
        }
    });
}
} // namespace openghost
