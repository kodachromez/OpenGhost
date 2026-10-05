#pragma once

// Test data only. This is NOT a production formatter, schema validator or wire
// adapter. The JS oracle checks exact fixtures; native tests check the existing
// semantic seam can forward their fields without losing metadata or images.
#include "backend/types.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

namespace browser_tools_test
{
inline QJsonArray fixture(const char *name)
{
    QFile file(QTest::qFindTestData(name, __FILE__, __LINE__));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).array();
}
inline QVector<openghost::HostToolSchema> schemas()
{
    QVector<openghost::HostToolSchema> out;
    for (const auto &value : fixture("schemas.json")) {
        const auto row = value.toObject();
        out.append({row.value("name").toString(), row.value("description").toString(),
                    row.value("parameters").toObject()});
    }
    return out;
}
inline QJsonObject result(const QString &id)
{
    for (const auto &value : fixture("results.json")) {
        const auto row = value.toObject();
        if (row.value("id").toString() == id)
            return row.value("expected").toObject();
    }
    return {};
}
inline openghost::HostToolResult body(const QJsonObject &fixture)
{
    using R = openghost::HostToolResult;
    R out;
    for (const auto &value : fixture.value("content").toArray()) {
        const auto block = value.toObject();
        if (block.value("type") == "text")
            out.content.append(R::Text{block.value("text").toString()});
        else if (block.value("type") == "image")
            out.content.append(R::Image{block.value("dataUrl").toString(),
                                        block.contains("label")
                                            ? std::optional<QString>(block.value("label").toString())
                                            : std::nullopt});
        else
            qFatal("Unknown fixture content type");
    }
    out.isError = fixture.value("isError").toBool();
    out.status = out.isError ? R::Status::Error : R::Status::Ok;
    if (fixture.contains("data"))
        out.data = fixture.value("data").toObject();
    return out;
}
inline void compare(const openghost::HostToolResult &actual,
                    const openghost::HostToolResult &expected)
{
    QCOMPARE(actual.isError, expected.isError);
    QCOMPARE(actual.status, expected.status);
    QCOMPARE(actual.reason, expected.reason);
    QCOMPARE(actual.data, expected.data);
    QCOMPARE(actual.content.size(), expected.content.size());
    for (qsizetype i = 0; i < actual.content.size(); ++i) {
        QCOMPARE(actual.content[i].index(), expected.content[i].index());
        if (const auto *text = std::get_if<openghost::HostToolResult::Text>(&actual.content[i]))
            QCOMPARE(text->text, std::get<openghost::HostToolResult::Text>(expected.content[i]).text);
        else {
            const auto &image = std::get<openghost::HostToolResult::Image>(actual.content[i]);
            const auto &other = std::get<openghost::HostToolResult::Image>(expected.content[i]);
            QCOMPARE(image.dataUrl, other.dataUrl);
            QCOMPARE(image.label, other.label);
        }
    }
}
} // namespace browser_tools_test
