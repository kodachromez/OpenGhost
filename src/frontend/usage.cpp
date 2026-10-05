#include "usage.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace openghost
{
void UsageStore::record(const Usage &usage)
{
    const auto count = [](double n) { return std::isfinite(n) ? std::max(0.0, n) : 0.0; };
    const auto input = count(usage.input), output = count(usage.output);
    if ((!input && !output) || usage.provider.isEmpty() || usage.model.isEmpty())
        return;
    const QString id = QString::fromUtf8(
        QJsonDocument(QJsonArray{usage.provider, usage.model}).toJson(QJsonDocument::Compact));
    auto &row = m_days[QDate::currentDate().toString(Qt::ISODate)][id];
    row.provider = usage.provider;
    row.model = usage.model;
    row.input += input;
    row.output += output;
    row.cached += std::min(input, count(usage.cached));
    row.written += count(usage.written);
    row.requests = row.requests.value_or(0) + std::max(1.0, count(usage.requests.value_or(1)));
    m_names[id] = usage.modelName.value_or(usage.model);
    if (!m_since)
        m_since = QDateTime::currentMSecsSinceEpoch();
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
