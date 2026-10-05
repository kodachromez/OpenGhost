#pragma once
#include "backend/types.h"
#include <QDate>
#include <QMap>
#include <QObject>
#include <QVariantMap>

namespace openghost
{
// Frontend-local, session-lifetime ledger. Only identity-checked live events
// reach record(); replay updates reply metrics but must not charge this ledger.
class UsageStore final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString thisMonth READ thisMonth NOTIFY changed)
    Q_PROPERTY(double since READ since NOTIFY changed)
  public:
    using QObject::QObject;
    QString thisMonth() const { return QDate::currentDate().toString("yyyy-MM"); }
    double since() const { return m_since; }
    void record(const Usage &usage);
    Q_INVOKABLE QVariantMap totals(int days = 0) const;
    Q_INVOKABLE QVariantMap between(const QString &from, const QString &to) const;
    Q_INVOKABLE QVariantList daily(int count) const;
    Q_INVOKABLE QVariantList month(const QString &key) const;
    Q_INVOKABLE QStringList months() const;
    Q_INVOKABLE QString nameOf(const QString &id) const { return m_names.value(id, id); }
  signals:
    void changed();

  private:
    QVariantMap day(const QDate &date) const;
    QMap<QString, QMap<QString, Usage>> m_days;
    QMap<QString, QString> m_names;
    qint64 m_since = 0;
};
} // namespace openghost
