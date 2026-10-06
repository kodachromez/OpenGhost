#include "frontend_plugins.h"
#include "example_plugin.h"
#include "preferences.h"
#include <algorithm>

namespace openghost
{
void FrontendPluginContext::subscribe(const QString &event, EventHandler handler)
{
    if (handler)
        m_events.insert(event, std::move(handler));
    else
        m_events.remove(event);
}
void FrontendPluginContext::unsubscribe(const QString &event) { m_events.remove(event); }
void FrontendPluginContext::decorateRows(RowDecorator decorator)
{
    if (!m_decorator && !decorator)
        return;
    m_decorator = std::move(decorator);
    emit m_owner->rowsChanged();
}
void FrontendPluginContext::redecorate()
{
    if (m_decorator)
        emit m_owner->rowsChanged();
}
void FrontendPluginContext::renderRows(const QString &kind, RowRenderer renderer)
{
    if (kind.isEmpty() || (renderer.delegate.isEmpty() && !m_renderers.contains(kind)))
        return;
    if (renderer.delegate.isEmpty())
        m_renderers.remove(kind);
    else
        m_renderers.insert(kind, std::move(renderer));
    ++m_owner->m_renderGeneration;
    emit m_owner->renderersChanged();
}
void FrontendPluginContext::setVisible(const QString &target, bool visible)
{
    const auto found = m_visibility.constFind(target);
    if (found != m_visibility.cend() && *found == visible)
        return;
    m_visibility.insert(target, visible);
    emit m_owner->visibilityChanged();
}
void FrontendPluginContext::clearVisible(const QString &target)
{
    if (m_visibility.remove(target))
        emit m_owner->visibilityChanged();
}
void FrontendPluginContext::update() { emit m_owner->entriesChanged(); }
bool FrontendPluginContext::option(const QString &key) const { return m_owner->option(m_id, key); }

FrontendPlugins::FrontendPlugins(PreferencesStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
}
FrontendPlugins::~FrontendPlugins()
{
    // Nothing is told: whoever listened may already be going away.
    for (auto it = m_slots.rbegin(); it != m_slots.rend(); ++it)
        if ((*it)->context)
            stop(**it, false);
}
FrontendPlugins::Slot *FrontendPlugins::find(const QString &id)
{
    for (auto &slot : m_slots)
        if (slot->info.id == id)
            return slot.get();
    return nullptr;
}
const FrontendPlugins::Slot *FrontendPlugins::find(const QString &id) const
{
    return const_cast<FrontendPlugins *>(this)->find(id);
}
bool FrontendPlugins::add(std::unique_ptr<FrontendPlugin> plugin)
{
    if (!plugin)
        return false;
    auto info = plugin->info();
    if (info.id.isEmpty() || find(info.id))
        return false;
    const auto &choices = m_store ? m_store->value().frontendPlugins : m_choices;
    const bool on = choices.value(info.id, info.enabledByDefault);
    m_slots.push_back(std::make_unique<Slot>(Slot{std::move(plugin), std::move(info), {}, 0}));
    auto &slot = *m_slots.back();
    if (on)
        start(slot);
    emit countChanged();
    emit entriesChanged();
    return true;
}
bool FrontendPlugins::remove(const QString &id)
{
    const auto at = std::find_if(m_slots.begin(), m_slots.end(),
                                 [&](const auto &slot) { return slot->info.id == id; });
    if (at == m_slots.end())
        return false;
    if ((*at)->context)
        stop(**at);
    // stop() ran plugin code: find the slot again before erasing it.
    const auto still = std::find_if(m_slots.begin(), m_slots.end(),
                                    [&](const auto &slot) { return slot->info.id == id; });
    if (still == m_slots.end())
        return true;
    m_slots.erase(still);
    emit countChanged();
    emit entriesChanged();
    return true;
}
bool FrontendPlugins::setEnabled(const QString &id, bool enabled)
{
    auto *slot = find(id);
    if (!slot)
        return false;
    if (bool(slot->context) == enabled)
        return true;
    if (m_store) {
        auto preferences = m_store->value();
        preferences.frontendPlugins.insert(id, enabled);
        if (!m_store->save(preferences))
            return false; // The store says why; the plugin stays as it was.
    } else {
        m_choices.insert(id, enabled);
    }
    if (enabled)
        start(*slot);
    else
        stop(*slot);
    return true;
}
bool FrontendPlugins::enabled(const QString &id) const
{
    const auto *slot = find(id);
    return slot && slot->context;
}
bool FrontendPlugins::option(const QString &id, const QString &key) const
{
    const auto *slot = find(id);
    if (!slot || !slot->info.options.contains(key))
        return false;
    const auto &saved = m_store ? m_store->value().frontendPluginOptions : m_options;
    return saved.value(id).value(key, slot->info.options.value(key));
}
bool FrontendPlugins::setOption(const QString &id, const QString &key, bool value)
{
    const auto *slot = find(id);
    if (!slot || !slot->info.options.contains(key))
        return false;
    if (option(id, key) == value)
        return true;
    if (m_store) {
        auto preferences = m_store->value();
        preferences.frontendPluginOptions[id].insert(key, value);
        if (!m_store->save(preferences))
            return false; // The store says why; the option stays as it was.
    } else {
        m_options[id].insert(key, value);
    }
    emit entriesChanged();
    // The plugin may have gone while Settings listened.
    if (auto *current = find(id); current && current->context)
        current->plugin->optionChanged(key);
    return true;
}
void FrontendPlugins::start(Slot &slot)
{
    slot.context.reset(new FrontendPluginContext(this, slot.info.id));
    slot.generation = ++m_generation;
    slot.plugin->enable(*slot.context);
    emit entriesChanged();
}
void FrontendPlugins::stop(Slot &slot, bool notify)
{
    const QString id = slot.info.id;
    slot.plugin->disable();
    // disable() may have changed hooks itself; the rest goes with the context.
    auto *current = find(id);
    if (!current || !current->context)
        return;
    const bool rows = bool(current->context->m_decorator);
    const bool visibility = !current->context->m_visibility.isEmpty();
    const bool renderers = !current->context->m_renderers.isEmpty();
    current->context.reset();
    if (renderers)
        ++m_renderGeneration;
    if (!notify)
        return;
    emit entriesChanged();
    if (renderers)
        emit renderersChanged();
    if (rows)
        emit rowsChanged();
    if (visibility)
        emit visibilityChanged();
}
QVariantList FrontendPlugins::entries() const
{
    QVariantList list;
    for (const auto &slot : m_slots) {
        QVariantMap options;
        for (auto it = slot->info.options.cbegin(); it != slot->info.options.cend(); ++it)
            options.insert(it.key(), option(slot->info.id, it.key()));
        list.append(QVariantMap{{"pluginId", slot->info.id},
                                {"name", slot->info.name},
                                {"description", slot->info.description},
                                {"status", slot->plugin->status()},
                                {"enabled", bool(slot->context)},
                                {"placement", slot->info.placement},
                                {"options", options}});
    }
    return list;
}
QVariantMap FrontendPlugins::visibility() const
{
    QVariantMap map;
    for (const auto &slot : m_slots) {
        if (!slot->context)
            continue;
        const auto &claims = slot->context->m_visibility;
        for (auto it = claims.cbegin(); it != claims.cend(); ++it)
            if (!map.contains(it.key()) || !it.value())
                map.insert(it.key(), it.value());
    }
    return map;
}
bool FrontendPlugins::visible(const QString &target, bool fallback) const
{
    const auto map = visibility();
    const auto found = map.constFind(target);
    return found == map.cend() ? fallback : found->toBool();
}
void FrontendPlugins::publish(const FrontendEvent &event)
{
    struct Call {
        QString id;
        quint64 generation;
        FrontendPluginContext::EventHandler handler;
    };
    QVector<Call> calls;
    for (const auto &slot : m_slots)
        if (slot->context)
            if (const auto handler = slot->context->m_events.constFind(event.name);
                handler != slot->context->m_events.cend())
                calls.append({slot->info.id, slot->generation, *handler});
    // A handler may turn plugins off (or unsubscribe): skip whatever it removed.
    for (const auto &call : calls) {
        const auto *slot = find(call.id);
        if (slot && slot->context && slot->generation == call.generation &&
            slot->context->m_events.contains(event.name))
            call.handler(event);
    }
}
bool FrontendPlugins::decorating() const
{
    return std::any_of(m_slots.cbegin(), m_slots.cend(), [](const auto &slot) {
        return slot->context && slot->context->m_decorator;
    });
}
QVariantMap FrontendPlugins::decorate(const ChatRowView &row) const
{
    QVariantMap decorations;
    for (const auto &slot : m_slots)
        if (slot->context && slot->context->m_decorator) {
            const auto decoration = slot->context->m_decorator(row);
            if (!decoration.isEmpty())
                decorations.insert(slot->info.id, decoration);
        }
    return decorations;
}
const RowRenderer *FrontendPlugins::renderer(const QString &kind) const
{
    for (const auto &slot : m_slots)
        if (slot->context)
            if (const auto found = slot->context->m_renderers.constFind(kind);
                found != slot->context->m_renderers.cend())
                return &*found;
    return nullptr;
}
QVariantMap FrontendPlugins::renderers() const
{
    QVariantMap map;
    for (const auto &slot : m_slots)
        if (slot->context)
            for (auto it = slot->context->m_renderers.cbegin();
                 it != slot->context->m_renderers.cend(); ++it)
                if (!map.contains(it.key()))
                    map.insert(it.key(), it->delegate.toString());
    return map;
}
int FrontendPlugins::hooks() const
{
    int count = 0;
    for (const auto &slot : m_slots)
        if (slot->context)
            count += slot->context->hooks();
    return count;
}

void registerBuiltinPlugins(FrontendPlugins &plugins)
{
    if (qEnvironmentVariableIntValue("OPENGHOST_EXAMPLE_PLUGIN") == 1)
        plugins.add(std::make_unique<ExamplePlugin>());
}
} // namespace openghost
