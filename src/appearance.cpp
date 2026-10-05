#include "appearance.h"

#include "rich.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace
{
constexpr qint64 MaxBytes = 4096;
}

QString readAppearance(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || info.size() > MaxBytes)
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(MaxBytes));
    const QString choice = doc.object().value(QLatin1String("theme")).toString();
    return Theme::choices().contains(choice) ? choice : QString();
}

bool writeAppearance(const QString &path, const QString &choice)
{
    const QFileInfo info(path);
    if (info.isSymLink() || !QDir().mkpath(info.absolutePath()))
        return false;
    QFile::setPermissions(info.absolutePath(),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("theme"), choice}}).toJson());
    return file.commit();
}

QMetaObject::Connection keepAppearance(const QString &path)
{
    const QString saved = readAppearance(path);
    if (!saved.isEmpty())
        Theme::choose(saved);
    return QObject::connect(ThemeSignal::instance(), &ThemeSignal::choiceChanged,
                            ThemeSignal::instance(), [path] {
                                if (!writeAppearance(path, Theme::chosen()))
                                    qWarning("OpenGhost: the theme choice could not be saved.");
                            });
}
