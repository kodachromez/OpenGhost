#pragma once

#include <QMetaObject>
#include <QString>

// Settings → Appearance's choice ("light", "dark" or "system"), kept in
// `path` (appearance.json: {"theme": "<choice>"}): read once into Theme's
// process-wide choice, then written again whenever the choice changes. A
// missing, unreadable or invalid file is the default, dark; it is never
// followed through a symbolic link. The directory is made 0700 and the file
// written 0600, atomically. Returns the connection that keeps it.
QMetaObject::Connection keepAppearance(const QString &path);
// The choice `path` holds; empty when it holds none.
QString readAppearance(const QString &path);
bool writeAppearance(const QString &path, const QString &choice);
