#include "store.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace openghost
{
bool KeyStore::validKey(const QString &key)
{
    if (key.isEmpty() || key.size() > 512)
        return false;
    for (const auto &part : key.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral(".."))
            return false;
        for (const QChar c : part)
            if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != QLatin1Char('-') &&
                c != QLatin1Char('_') && c != QLatin1Char('.') && c != QLatin1Char(':'))
                return false;
    }
    return true;
}
KeyStore::Read MemoryKeyStore::read(const QString &key) const
{
    if (raw.contains(key))
        return {Status::Unreadable, {}};
    const auto it = values.constFind(key);
    return it == values.cend() ? Read{} : Read{Status::Ok, it.value()};
}
bool MemoryKeyStore::write(const QString &key, const QJsonObject &value)
{
    if (!validKey(key) || (!failWrites.isEmpty() && key.startsWith(failWrites)))
        return false;
    raw.remove(key);
    values.insert(key, value);
    return true;
}
bool MemoryKeyStore::remove(const QString &key)
{
    if (!validKey(key) || (!failWrites.isEmpty() && key.startsWith(failWrites)))
        return false;
    raw.remove(key);
    values.remove(key);
    return true;
}
QString FileKeyStore::path(const QString &key) const
{
    // ':' is not portable in Windows file names; it stays a key character only.
    QString name = key;
    name.replace(QLatin1Char(':'), QStringLiteral("%3A"));
    return m_directory + QLatin1Char('/') + name + QStringLiteral(".json");
}
KeyStore::Read FileKeyStore::read(const QString &key) const
{
    if (!validKey(key) || m_directory.isEmpty())
        return {Status::Unreadable, {}};
    QFile file(path(key));
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxBytes)
        return {Status::Unreadable, {}};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {Status::Unreadable, {}};
    return {Status::Ok, document.object()};
}
bool FileKeyStore::write(const QString &key, const QJsonObject &value)
{
    if (!validKey(key) || m_directory.isEmpty())
        return false;
    const auto target = path(key);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return false;
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaxBytes)
        return false;
    QSaveFile file(target);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
bool FileKeyStore::remove(const QString &key)
{
    if (!validKey(key) || m_directory.isEmpty())
        return false;
    QFile file(path(key));
    return !file.exists() || file.remove();
}
} // namespace openghost
