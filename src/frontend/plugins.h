#pragma once
#include "backend/types.h"
#include <QObject>
#include <functional>

namespace openghost
{
struct PluginEntry {
    Plugin snapshot;
    bool pending = false;
    QString error, warning;
    // The last answer's `persisted` ("saved", "memory", "ambiguous", or a
    // future value), kept until the next request; empty before any answer.
    QString persisted;
    // Local causal bookkeeping on Plugins' tick, not a backend version.
    // seen: when the shown snapshot was observed (a list counts from when it
    // was sent). doubt: when the row stopped being trustworthy (a failed
    // request or list); only an observation made after that clears it.
    quint64 seen = 0, doubt = 0;
    bool confirmed() const { return doubt == 0; }
    // Disabling can be undone: the backend re-enables and keeps the settling calls.
    bool canToggle() const
    {
        return confirmed() && !pending && snapshot.available &&
               (snapshot.state == "enabled" || snapshot.state == "disabled" ||
                snapshot.state == "disabling");
    }
};

// Connection-scoped frontend projection. Nothing is persisted or optimistically
// enabled here. Dispatch shares ChatService's request correlation, but plugin
// failures/pending work never affect the chat's status or admission flags.
//
// Ordering (port contract, ABP's plugin.changed rules): events and answers
// arrive in backend order, and an enable/disable answer is never older than
// a plugin.changed delivered before it. Each observation is stamped with a
// local tick. An answer is adopted only when nothing about its plugin was
// observed after the request was sent; a list replaces only rows not
// observed after the list was sent, and clears only doubt that predates it.
class Plugins final : public QObject
{
    Q_OBJECT
  public:
    using Completion = std::function<void(const Result &)>;
    using Dispatch = std::function<void(const Command &, Completion)>;
    explicit Plugins(Dispatch dispatch, QObject *parent = nullptr);
    void initialize(bool supported);
    void refresh();
    void setEnabled(const QString &id, bool enabled);
    void observe(const PluginChanged &event);
    bool supported() const { return m_supported; }
    bool loading() const { return m_loading; }
    bool loaded() const { return m_loaded; }
    QString error() const { return m_error; }
    const QVector<PluginEntry> &entries() const { return m_entries; }
  signals:
    void changed();
    void supportChanged(); // Only when supported() flips, so pages are not rebuilt per update.
  private:
    PluginEntry *find(const QString &id);
    static void adopt(PluginEntry &entry, const Plugin &snapshot, quint64 seen);
    Dispatch m_dispatch;
    QVector<PluginEntry> m_entries;
    QString m_error;
    quint64 m_generation = 0, m_tick = 0;
    bool m_supported = false, m_loading = false, m_loaded = false, m_refreshAgain = false;
};
} // namespace openghost
