#include "preferences.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace openghost
{
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
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        m_error = QStringLiteral("Cannot read native preferences.");
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto o = document.object();
    const auto mode = parseMode(o.value("mode").toString());
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        o.value("version").toInt() != 1 || !mode || !o.value("instructions").isString() ||
        o.value("instructions").toString().size() > 8000 || !o.value("provider").isString() ||
        !o.value("model").isString() ||
        (!o.value("thinking").isNull() && !o.value("thinking").isString())) {
        m_error = QStringLiteral("Invalid native preferences; file left unchanged.");
        return;
    }
    m_value.model.provider = o.value("provider").toString();
    m_value.model.model = o.value("model").toString();
    if (o.value("thinking").isString())
        m_value.preferredThinking = o.value("thinking").toString();
    m_value.mode = *mode;
    m_value.userContext.instructions = o.value("instructions").toString();
}
bool PreferencesStore::fail(const QString &message)
{
    emit saveFailed(message);
    return false;
}
bool PreferencesStore::save(const Preferences &value)
{
    // Do not overwrite unreadable/unknown versions with defaults, or silently
    // drop pinned payloads before their persistence implementation exists.
    if (!m_error.isEmpty())
        return fail(m_error);
    if (value.userContext.instructions.size() > 8000 || !value.userContext.files.isEmpty())
        return fail(QStringLiteral(
            "Instructions exceed 8,000 characters or pinned files are unsupported."));
    const QJsonObject object{{"version", 1},
                             {"provider", value.model.provider},
                             {"model", value.model.model},
                             {"thinking", value.preferredThinking
                                              ? QJsonValue(*value.preferredThinking)
                                              : QJsonValue(QJsonValue::Null)},
                             {"mode", modeName(value.mode)},
                             {"instructions", value.userContext.instructions}};
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
