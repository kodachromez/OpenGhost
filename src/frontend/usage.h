#pragma once
#include "backend/types.h"
#include "store.h"
#include <QDate>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

namespace openghost
{
// Frontend-local ledger (usage.js, schema version 2; version 1 upgraded on
// read). Only identity-checked live events reach record(); replay updates reply
// metrics but must not charge it. Saves are debounced and flushed on exit. Failed
// writes report saveFailed and stay pending for a later record/flush or exit;
// there is no automatic retry loop. An unreadable/unknown saved ledger is left
// on disk untouched and not extended.
class UsageStore final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString thisMonth READ thisMonth NOTIFY changed)
    Q_PROPERTY(double since READ since NOTIFY changed)
  public:
    explicit UsageStore(QObject *parent = nullptr) : UsageStore(nullptr, parent) {}
    explicit UsageStore(KeyStore *store, QObject *parent = nullptr);
    ~UsageStore() override { flush(); }
    QString error() const { return m_error; } // Load error; blocks recording.
    bool flush(); // Writes pending changes; false + saveFailed leaves them pending.
    static constexpr int SaveDelay = 800;
    QString thisMonth() const { return QDate::currentDate().toString("yyyy-MM"); }
    double since() const { return m_since; }
    void record(const Usage &usage);
    Q_INVOKABLE QVariantMap totals(int days = 0) const;
    Q_INVOKABLE QVariantMap between(const QString &from, const QString &to) const;
    Q_INVOKABLE QVariantList daily(int count) const;
    Q_INVOKABLE QVariantList month(const QString &key) const;
    Q_INVOKABLE QStringList months() const;
    Q_INVOKABLE QString nameOf(const QString &id) const;
  signals:
    void changed();
    void saveFailed(const QString &error);

  private:
    QVariantMap day(const QDate &date) const;
    void save();
    KeyStore *m_store = nullptr;
    QTimer m_timer;
    bool m_dirty = false;
    QString m_error;
    QMap<QString, QMap<QString, Usage>> m_days;
    QMap<QString, QString> m_names;
    qint64 m_since = 0;
};
} // namespace openghost
