#include "usage.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>

namespace openghost
{
namespace
{
enum Column { InputColumn, CachedColumn, WrittenColumn, OutputColumn, RequestsColumn };
// Each model is kept under its provider and model IDs written as a JSON pair, so
// any ID, `|` and all, reads back whole. Version 1 wrote `provider|model`.
std::optional<QPair<QString, QString>> idOf(const QString &key)
{
    const auto pair = QJsonDocument::fromJson(key.toUtf8()).array();
    if (pair.size() == 2 && pair.at(0).isString() && pair.at(1).isString())
        return QPair<QString, QString>{pair.at(0).toString(), pair.at(1).toString()};
    return std::nullopt;
}
QString keyOf(const QString &provider, const QString &model)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{provider, model}).toJson(QJsonDocument::Compact));
}
QJsonObject rekey(const QJsonObject &rows)
{
    QJsonObject out;
    for (auto it = rows.begin(); it != rows.end(); ++it) {
        const auto cut = it.key().indexOf(QLatin1Char('|'));
        if (cut > 0)
            out.insert(keyOf(it.key().left(cut), it.key().mid(cut + 1)), it.value());
    }
    return out;
}
double count(const QJsonValue &value)
{
    const auto n = value.toDouble();
    return std::isfinite(n) ? std::max(0.0, n) : 0.0;
}
} // namespace
UsageStore::UsageStore(KeyStore *store, QObject *parent) : QObject(parent), m_store(store)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(SaveDelay);
    connect(&m_timer, &QTimer::timeout, this, &UsageStore::flush);
    if (!m_store)
        return;
    const auto read = m_store->read(QStringLiteral("usage"));
    if (read.status == KeyStore::Status::Absent)
        return;
    auto saved = read.value;
    const auto version = saved.value("version").toInt();
    if (read.status == KeyStore::Status::Unreadable || (version != 1 && version != 2)) {
        m_error = QStringLiteral("Saved usage could not be read; it was left unchanged.");
        return;
    }
    if (version == 1) {
        QJsonObject days;
        const auto old = saved.value("days").toObject();
        for (auto it = old.begin(); it != old.end(); ++it)
            days.insert(it.key(), rekey(it.value().toObject()));
        saved.insert("days", days);
        saved.insert("names", rekey(saved.value("names").toObject()));
    }
    m_since = qint64(count(saved.value("since")));
    const auto days = saved.value("days").toObject();
    for (auto day = days.begin(); day != days.end(); ++day) {
        if (!QDate::fromString(day.key(), Qt::ISODate).isValid())
            continue;
        const auto rows = day.value().toObject();
        for (auto row = rows.begin(); row != rows.end(); ++row) {
            const auto id = idOf(row.key());
            const auto values = row.value().toArray();
            if (!id || values.size() != 5)
                continue;
            Usage &u = m_days[day.key()][row.key()];
            u.provider = id->first;
            u.model = id->second;
            u.input = count(values.at(InputColumn));
            u.cached = std::min(u.input, count(values.at(CachedColumn)));
            u.written = count(values.at(WrittenColumn));
            u.output = count(values.at(OutputColumn));
            u.requests = count(values.at(RequestsColumn));
        }
    }
    const auto names = saved.value("names").toObject();
    for (auto it = names.begin(); it != names.end(); ++it)
        if (it.value().isString() && idOf(it.key()))
            m_names.insert(it.key(), it.value().toString());
}
void UsageStore::save()
{
    if (m_store && m_error.isEmpty())
        m_timer.start();
}
bool UsageStore::flush()
{
    if (!m_timer.isActive())
        return true;
    m_timer.stop();
    QJsonObject days, names;
    for (auto day = m_days.cbegin(); day != m_days.cend(); ++day) {
        QJsonObject rows;
        for (auto row = day->cbegin(); row != day->cend(); ++row)
            rows.insert(row.key(), QJsonArray{row->input, row->cached, row->written, row->output,
                                              row->requests.value_or(0)});
        days.insert(day.key(), rows);
    }
    for (auto it = m_names.cbegin(); it != m_names.cend(); ++it)
        names.insert(it.key(), it.value());
    return m_store->write(
        QStringLiteral("usage"),
        {{"version", 2}, {"since", double(m_since)}, {"days", days}, {"names", names}});
}
QString UsageStore::nameOf(const QString &id) const
{
    if (m_names.contains(id))
        return m_names.value(id);
    const auto pair = idOf(id);
    return pair ? pair->second : id;
}
void UsageStore::record(const Usage &usage)
{
    const auto count = [](double n) { return std::isfinite(n) ? std::max(0.0, n) : 0.0; };
    const auto input = count(usage.input), output = count(usage.output);
    if ((!input && !output) || usage.provider.isEmpty() || usage.model.isEmpty())
        return;
    if (!m_error.isEmpty())
        return; // Never extend (and later overwrite) a ledger that could not be read.
    const QString id = keyOf(usage.provider, usage.model);
    auto &row = m_days[QDate::currentDate().toString(Qt::ISODate)][id];
    row.provider = usage.provider;
    row.model = usage.model;
    row.input += input;
    row.output += output;
    row.cached += std::min(input, count(usage.cached));
    row.written += count(usage.written);
    row.requests = row.requests.value_or(0) + std::max(1.0, count(usage.requests.value_or(1)));
    if (usage.modelName)
        m_names[id] = *usage.modelName;
    if (!m_since)
        m_since = QDateTime::currentMSecsSinceEpoch();
    save();
    emit changed();
}
QVariantMap UsageStore::totals(int days) const
{
    return between(days > 0 ? QDate::currentDate().addDays(1 - days).toString(Qt::ISODate)
                            : QString(),
                   "9999");
}
QVariantMap UsageStore::between(const QString &from, const QString &to) const
{
    QVariantMap result;
    for (auto day = m_days.cbegin(); day != m_days.cend(); ++day) {
        if (day.key() < from || day.key() > to)
            continue;
        for (auto row = day->cbegin(); row != day->cend(); ++row) {
            auto total = result[row->provider].toMap();
            const auto add = [&](const QString &key, double n) {
                total[key] = total[key].toDouble() + n;
            };
            add("input", row->input);
            add("cached", row->cached);
            add("written", row->written);
            add("output", row->output);
            add("tokens", row->input + row->output);
            add("requests", row->requests.value_or(0));
            auto models = total["models"].toMap();
            models[row.key()] = models[row.key()].toDouble() + row->input + row->output;
            total["models"] = models;
            result[row->provider] = total;
        }
    }
    return result;
}
QVariantMap UsageStore::day(const QDate &date) const
{
    const auto key = date.toString(Qt::ISODate);
    const auto totals = between(key, key);
    QVariantMap providers;
    for (auto it = totals.cbegin(); it != totals.cend(); ++it)
        providers[it.key()] = it->toMap()["tokens"];
    return {{"day", key}, {"providers", providers}};
}
QVariantList UsageStore::daily(int count) const
{
    QVariantList result;
    for (int i = std::clamp(count, 0, 366) - 1; i >= 0; --i)
        result.append(day(QDate::currentDate().addDays(-i)));
    return result;
}
QVariantList UsageStore::month(const QString &key) const
{
    const auto first = QDate::fromString(key + "-01", Qt::ISODate);
    QVariantList result;
    if (first.isValid())
        for (int i = 0; i < first.daysInMonth(); ++i)
            result.append(day(first.addDays(i)));
    return result;
}
QStringList UsageStore::months() const
{
    QStringList result;
    for (auto it = m_days.cbegin(); it != m_days.cend(); ++it)
        if (!result.contains(it.key().left(7)))
            result.append(it.key().left(7));
    return result;
}
} // namespace openghost
