#include "settings.h"

void Settings::apply(const Account &account)
{
    const bool providers = account.providers != m_account.providers ||
                           account.login != m_account.login ||
                           account.providersLoaded != m_account.providersLoaded ||
                           account.providersError != m_account.providersError;
    m_account = account;
    if (providers)
        emit providersChanged();
    m_providerIds.clear();
    for (const auto &model : std::as_const(m_account.models)) {
        if (model.available && !m_providerIds.contains(model.provider))
            m_providerIds << model.provider;
    }
    m_notice.clear();
    // Adopt a saved frontend preference once; later catalogs preserve canonical display state.
    if (!m_used && !account.defaults.model.isEmpty() && m_selected.model.isEmpty() && !m_invalid) {
        m_used = true;
        use(account.defaults);
        return;
    }
    emit changed();
}

void Settings::use(const Selection &selection)
{
    m_used = true;
    m_selected = {selection.provider, selection.model, selection.thinking};
    m_provider = selection.provider;
    m_invalid = false;
    // Canonical session configuration may set/clear even unadvertised levels.
    emit changed();
}

Selection Settings::selection() const
{
    Selection selection = m_selected;
    selection.invalid = m_invalid;
    return selection;
}

const ModelInfo *Settings::find(const QString &provider, const QString &id) const
{
    for (const auto &model : std::as_const(m_account.models)) {
        if (model.provider == provider && model.id == id)
            return &model;
    }
    return nullptr;
}

// OpenGhost: supported user preference, then supported advertised default.
// Never infer medium or the first level.
QString Settings::resolve(const ModelInfo &model, const QString &wanted) const
{
    for (const auto &level : {wanted, model.defaultThinking}) {
        if (!level.isEmpty() && model.levels.contains(level))
            return level;
    }
    return {};
}

// Keeps the selection on an advertised level once its model is known.
void Settings::resolveThinking()
{
    if (const auto *model = find(m_selected.provider, m_selected.model))
        m_selected.thinking = resolve(*model, m_selected.thinking);
}

QString Settings::name(const QString &provider) const
{
    for (const auto &row : m_account.providers) {
        const auto map = row.toMap();
        if (map.value("id").toString() == provider)
            return map.value("name").toString();
    }
    return provider;
}

QStringList Settings::providerNames() const
{
    QStringList names;
    for (const auto &id : m_providerIds)
        names << name(id);
    return names;
}

QStringList Settings::modelIds() const
{
    QStringList ids;
    for (const auto &model : std::as_const(m_account.models)) {
        if (model.available && model.provider == m_provider)
            ids << model.id;
    }
    return ids;
}

QStringList Settings::modelNames() const
{
    QStringList names;
    for (const auto &model : std::as_const(m_account.models)) {
        if (model.available && model.provider == m_provider)
            names << model.name;
    }
    return names;
}

QVariantList Settings::choices() const
{
    QVariantList rows;
    for (const auto &provider : m_providerIds) {
        const QString providerName = name(provider);
        for (const auto &model : std::as_const(m_account.models)) {
            if (model.available && model.provider == provider)
                rows << QVariantMap{{"provider", provider},
                                    {"providerName", providerName},
                                    {"id", model.id},
                                    {"name", model.name},
                                    {"imageInput", model.imageInput}};
        }
    }
    return rows;
}

QString Settings::modelLabel() const
{
    if (m_invalid)
        return QStringLiteral("Choose a model");
    if (m_selected.model.isEmpty())
        return QStringLiteral("No model yet");
    const auto *model = find(m_selected.provider, m_selected.model);
    if (model && model->available)
        return model->name;
    return m_selected.provider + QLatin1Char('/') + m_selected.model +
           (m_account.models.isEmpty() ? QString() : QStringLiteral(" (unavailable)"));
}

QStringList Settings::levels() const
{
    const auto *model = find(m_selected.provider, m_selected.model);
    return model ? model->levels : QStringList();
}

bool Settings::canSave() const
{
    const auto *model = find(m_selected.provider, m_selected.model);
    if (m_invalid || !model || !model->available || m_account.saving)
        return false;
    const auto &saved = m_account.defaults;
    return saved.provider != m_selected.provider || saved.model != m_selected.model ||
           (!m_selected.thinking.isEmpty() && saved.thinking != m_selected.thinking);
}

QString Settings::defaultsText() const
{
    const auto &saved = m_account.defaults;
    // The Settings Default row's hint (openghost/i18n.js settings.default.*).
    if (saved.model.isEmpty())
        return QStringLiteral("No preferred model selected yet.");
    QStringList parts{name(saved.provider), saved.model};
    if (!saved.thinking.isEmpty())
        parts << saved.thinking;
    return QStringLiteral("OpenGhost uses %1 for new chats and after restarts.")
        .arg(parts.join(QStringLiteral(" · ")));
}

void Settings::chooseProvider(const QString &provider)
{
    if (!m_providerIds.contains(provider))
        return;
    if (provider == m_selected.provider && !m_invalid) {
        m_provider = provider;
        emit changed();
        return;
    }
    m_provider = provider;
    m_selected = {};
    m_invalid = true;
    m_notice.clear();
    emit changed();
    emit modelWanted();
}

void Settings::chooseModel(const QString &id) { choose(m_provider, id); }

void Settings::choose(const QString &provider, const QString &id)
{
    const auto *model = find(provider, id);
    if (!model || !model->available)
        return;
    // Re-picking the current selection is not a new explicit preference.
    if (!m_invalid && model->provider == m_selected.provider && model->id == m_selected.model)
        return;
    const QString wanted = m_account.defaults.thinking;
    m_provider = model->provider;
    m_selected = {model->provider, model->id, wanted};
    m_invalid = false;
    m_notice.clear();
    resolveThinking();
    emit changed();
    emit chosen(selection());
}

bool Settings::chooseThinking(const QString &level)
{
    if (level == m_selected.thinking)
        return true;
    if (!levels().contains(level)) {
        m_notice = QStringLiteral("The selected model does not advertise this thinking level.");
        emit changed();
        return false;
    }
    m_selected.thinking = level;
    m_notice.clear();
    emit changed();
    // An adopted model that is no longer available keeps the level for its
    // next run but is never saved: canSave()'s gate.
    const auto *model = find(m_selected.provider, m_selected.model);
    if (!m_invalid && model && model->available)
        emit chosen(selection());
    return true;
}
