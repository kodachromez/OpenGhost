#pragma once
#include "frontend_plugin.h"
#include <QObject>
#include <QVariantList>
#include <QVector>
#include <memory>

namespace openghost
{
class PreferencesStore;

// The frontend's plugin registry. Built-in plugins are registered at startup
// (registerBuiltinPlugins); each is enabled from the saved choice, else its
// default. Turning a plugin on or off saves the choice first and applies it
// only once saved, then takes effect at once: enable() registers its hooks,
// disable() and the removal of every hook it registered undo them.
// With no plugin enabled, every hook is a no-op.
class FrontendPlugins final : public QObject
{
    Q_OBJECT
    // Settings entries in registration order: {pluginId, name, description, status, enabled}.
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // UI targets enabled plugins show (true) or hide (false); absent: no plugin's say.
    Q_PROPERTY(QVariantMap visibility READ visibility NOTIFY visibilityChanged)
  public:
    // `store`: where on/off choices are kept; null keeps them in memory.
    explicit FrontendPlugins(PreferencesStore *store, QObject *parent = nullptr);
    ~FrontendPlugins() override;

    // False for an empty or already registered ID.
    bool add(std::unique_ptr<FrontendPlugin> plugin);
    // Disables (keeping the saved choice) and forgets the plugin.
    bool remove(const QString &id);
    // False when unknown or the choice could not be saved (nothing changes).
    Q_INVOKABLE bool setEnabled(const QString &id, bool enabled);
    bool enabled(const QString &id) const;

    QVariantList entries() const;
    int count() const { return int(m_slots.size()); }
    QVariantMap visibility() const;
    // A target's visibility, or `fallback` when no enabled plugin claims it.
    Q_INVOKABLE bool visible(const QString &target, bool fallback) const;

    // Hooks, called by the window.
    void publish(const FrontendEvent &event);
    bool decorating() const;
    QVariantMap decorate(const ChatRowView &row) const; // pluginId -> decoration
    int hooks() const; // Registered hooks of every enabled plugin.

  signals:
    void entriesChanged();
    void countChanged();
    void visibilityChanged();
    void rowsChanged(); // Row decorators were added, removed or asked to rerun.

  private:
    friend class FrontendPluginContext;
    struct Slot {
        std::unique_ptr<FrontendPlugin> plugin;
        FrontendPluginInfo info;
        std::unique_ptr<FrontendPluginContext> context; // Set while enabled.
        quint64 generation = 0;                          // Moves on every enable.
    };
    Slot *find(const QString &id);
    const Slot *find(const QString &id) const;
    void start(Slot &slot);
    void stop(Slot &slot, bool notify = true);
    PreferencesStore *m_store;
    QMap<QString, bool> m_choices; // Without a store.
    std::vector<std::unique_ptr<Slot>> m_slots; // Stable: plugin code may add or remove.
    quint64 m_generation = 0;
};

// The plugins built into this OpenGhost. None is registered by default yet;
// OPENGHOST_EXAMPLE_PLUGIN=1 registers the SDK's example plugin.
void registerBuiltinPlugins(FrontendPlugins &plugins);
} // namespace openghost
