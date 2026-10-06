#include "preferences.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace openghost
{
namespace
{
// General's pinned files (user-context.js LIMITS): text only, at most 20, and
// 200,000 characters together. Storage guardrails, not model limits.
constexpr int MaxFiles = 20, MaxPinnedChars = 200000;
bool pinnable(const QVector<ContextFile> &files)
{
    qint64 chars = 0;
    for (const auto &file : files) {
        if (file.kind != ContextFile::Kind::Text || !file.text || file.id.isEmpty() ||
            file.name.isEmpty())
            return false;
        chars += file.text->size();
    }
    return files.size() <= MaxFiles && chars <= MaxPinnedChars;
}
QJsonArray filesJson(const QVector<ContextFile> &files)
{
    QJsonArray out;
    for (const auto &file : files) {
        QJsonObject o{{"id", file.id},
                      {"name", file.name},
                      {"size", double(file.size)},
                      {"text", file.text.value_or(QString())}};
        if (file.path)
            o.insert("path", *file.path);
        out.append(o);
    }
    return out;
}
std::optional<QVector<ContextFile>> filesFrom(const QJsonValue &value)
{
    QVector<ContextFile> files;
    if (value.isUndefined())
        return files;
    if (!value.isArray())
        return std::nullopt;
    for (const auto &item : value.toArray()) {
        const auto o = item.toObject();
        if (!o.value("id").isString() || !o.value("name").isString() ||
            !o.value("text").isString() || !o.value("size").isDouble() ||
            (o.contains("path") && !o.value("path").isString()))
            return std::nullopt;
        ContextFile file;
        file.id = o.value("id").toString();
        file.name = o.value("name").toString();
        file.size = qint64(o.value("size").toDouble());
        file.kind = ContextFile::Kind::Text;
        file.text = o.value("text").toString();
        if (o.contains("path"))
            file.path = o.value("path").toString();
        files.append(file);
    }
    if (!pinnable(files))
        return std::nullopt;
    return files;
}
// Frontend plugin choices: {"<plugin id>": true|false}; absent means none.
std::optional<QMap<QString, bool>> pluginsFrom(const QJsonValue &value)
{
    QMap<QString, bool> plugins;
    if (value.isUndefined())
        return plugins;
    if (!value.isObject())
        return std::nullopt;
    const auto o = value.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (it.key().isEmpty() || !it.value().isBool())
            return std::nullopt;
        plugins.insert(it.key(), it.value().toBool());
    }
    return plugins;
}
} // namespace
QString modeName(PermissionMode mode)
{
    switch (mode) {
    case PermissionMode::Ask:
        return QStringLiteral("ask");
    case PermissionMode::Auto:
        return QStringLiteral("auto");
    case PermissionMode::Full:
        return QStringLiteral("full");
    }
    return {};
}
std::optional<PermissionMode> parseMode(const QString &name)
{
    if (name == QStringLiteral("ask"))
        return PermissionMode::Ask;
    if (name == QStringLiteral("auto"))
        return PermissionMode::Auto;
    if (name == QStringLiteral("full"))
        return PermissionMode::Full;
    return {};
}
PreferencesStore::PreferencesStore(QString path, QObject *parent)
    : QObject(parent), m_path(std::move(path))
{
    if (m_path.isEmpty())
        return; // Explicit ephemeral preferences, useful for tests.
    QFile file(m_path);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
        m_error = QStringLiteral("Cannot read native preferences.");
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto o = document.object();
    const auto mode = parseMode(o.value("mode").toString());
    const auto files = filesFrom(o.value("files"));
    const auto plugins = pluginsFrom(o.value("plugins"));
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        o.value("version").toInt() != 1 || !mode || !o.value("instructions").isString() ||
        o.value("instructions").toString().size() > 8000 || !o.value("provider").isString() ||
        !o.value("model").isString() ||
        (!o.value("thinking").isNull() && !o.value("thinking").isString()) || !files ||
        !plugins) {
        m_error = QStringLiteral("Invalid native preferences; file left unchanged.");
        return;
    }
    m_value.model.provider = o.value("provider").toString();
    m_value.model.model = o.value("model").toString();
    if (o.value("thinking").isString())
        m_value.preferredThinking = o.value("thinking").toString();
    m_value.mode = *mode;
    m_value.userContext.instructions = o.value("instructions").toString();
    m_value.userContext.files = *files;
    m_value.frontendPlugins = *plugins;
}
bool PreferencesStore::fail(const QString &message)
{
    emit saveFailed(message);
    return false;
}
bool PreferencesStore::save(const Preferences &value)
{
    // Do not overwrite unreadable/unknown versions with defaults.
    if (!m_error.isEmpty())
        return fail(m_error);
    if (value.userContext.instructions.size() > 8000)
        return fail(QStringLiteral("Instructions exceed 8,000 characters."));
    if (!pinnable(value.userContext.files))
        return fail(QStringLiteral("Files kept for every chat are limited to 20 text files and "
                                   "200,000 characters together."));
    QJsonObject object{{"version", 1},
                       {"provider", value.model.provider},
                       {"model", value.model.model},
                       {"thinking", value.preferredThinking
                                        ? QJsonValue(*value.preferredThinking)
                                        : QJsonValue(QJsonValue::Null)},
                       {"mode", modeName(value.mode)},
                       {"instructions", value.userContext.instructions},
                       {"files", filesJson(value.userContext.files)}};
    // Written only once a choice exists, so a profile without plugins is unchanged.
    if (!value.frontendPlugins.isEmpty()) {
        QJsonObject plugins;
        for (auto it = value.frontendPlugins.cbegin(); it != value.frontendPlugins.cend(); ++it)
            plugins.insert(it.key(), it.value());
        object.insert("plugins", plugins);
    }
    if (!m_path.isEmpty()) {
        if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
            return fail(QStringLiteral("Cannot create native preferences directory."));
        QSaveFile file(m_path);
        const auto bytes = QJsonDocument(object).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            return fail(QStringLiteral("OpenGhost C++ preferences could not be saved."));
    }
    m_value = value;
    emit changed();
    return true;
}
} // namespace openghost
