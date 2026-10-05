#pragma once
#include "backend/types.h"
#include <QObject>

namespace openghost
{
// Local frontend preferences. Explicit path injection; no backend or credential
// storage. Publish the new in-memory state only after atomic save succeeds.
class PreferencesStore final : public QObject
{
    Q_OBJECT
  public:
    explicit PreferencesStore(QString path, QObject *parent = nullptr);
    const Preferences &value() const { return m_value; }
    QString error() const { return m_error; }
    bool save(const Preferences &value);
  signals:
    void changed();
    void saveFailed(const QString &error);

  private:
    QString m_path, m_error;
    Preferences m_value;
    bool fail(const QString &message);
};
QString modeName(PermissionMode mode);
std::optional<PermissionMode> parseMode(const QString &name);
} // namespace openghost
