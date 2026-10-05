#include "toolcard.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <cmath>

namespace toolcard
{
namespace
{
// JavaScript's String(value) for a JSON value.
QString jsNumber(double d)
{
    if (std::isnan(d))
        return QStringLiteral("NaN");
    if (std::isinf(d))
        return d > 0 ? QStringLiteral("Infinity") : QStringLiteral("-Infinity");
    if (d == std::floor(d) && std::abs(d) < 1e21)
        return QString::number(d, 'f', 0);
    return QString::number(d, 'g', QLocale::FloatingPointShortest);
}

QString jsString(const QJsonValue &value)
{
    switch (value.type()) {
    case QJsonValue::String:
        return value.toString();
    case QJsonValue::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Double:
        return jsNumber(value.toDouble());
    case QJsonValue::Array: {
        QStringList parts;
        for (const QJsonValue &item : value.toArray())
            parts << (item.isNull() || item.isUndefined() ? QString() : jsString(item));
        return parts.join(QLatin1Char(','));
    }
    case QJsonValue::Object:
        return QStringLiteral("[object Object]");
    default:
        return {};
    }
}

// tool-card.js text(): strings as they are, nothing for null or undefined.
QString text(const QJsonValue &value)
{
    return value.isNull() || value.isUndefined() ? QString() : jsString(value);
}

// "x" + value, as JavaScript concatenates.
QString concat(const QJsonValue &value)
{
    if (value.isUndefined())
        return QStringLiteral("undefined");
    if (value.isNull())
        return QStringLiteral("null");
    return jsString(value);
}

// a ?? b
QJsonValue either(const QJsonValue &a, const QJsonValue &b)
{
    return a.isNull() || a.isUndefined() ? b : a;
}

double toNumber(const QJsonValue &value)
{
    switch (value.type()) {
    case QJsonValue::Double:
        return value.toDouble();
    case QJsonValue::Bool:
        return value.toBool() ? 1 : 0;
    case QJsonValue::Null:
        return 0;
    case QJsonValue::String: {
        const QString s = value.toString().trimmed();
        if (s.isEmpty())
            return 0;
        bool ok = false;
        const double d = s.toDouble(&ok);
        return ok ? d : std::nan("");
    }
    default:
        return std::nan("");
    }
}

QString joined(const QStringList &parts)
{
    QStringList kept;
    for (const QString &part : parts)
        if (!part.isEmpty())
            kept << part;
    return kept.join(QStringLiteral(" · "));
}

QString range(const QJsonValue &from, const QJsonValue &to = QJsonValue(QJsonValue::Undefined))
{
    if (from.isUndefined())
        return {};
    return QStringLiteral("Lines ") + concat(from) + QStringLiteral("–") +
           (to.isNull() || to.isUndefined() ? QStringLiteral("…") : concat(to));
}

// JSON.stringify(JSON.parse(json)) for valid JSON: no space outside strings.
QString compact(const QString &json)
{
    QString out;
    out.reserve(json.size());
    bool quoted = false, escaped = false;
    for (const QChar c : json) {
        if (quoted) {
            out += c;
            if (escaped)
                escaped = false;
            else if (c == QLatin1Char('\\'))
                escaped = true;
            else if (c == QLatin1Char('"'))
                quoted = false;
            continue;
        }
        if (c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\n') ||
            c == QLatin1Char('\r'))
            continue;
        if (c == QLatin1Char('"'))
            quoted = true;
        out += c;
    }
    return out;
}

Lines lines(const QString &value)
{
    if (value.isEmpty())
        return {};
    QString all = value;
    all.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (all.endsWith(QLatin1Char('\n')))
        all.chop(1);
    const QStringList split = all.split(QLatin1Char('\n'));
    return {split.mid(0, 14), int(std::max<qsizetype>(0, split.size() - 14))};
}
/// Claude Code's built-in tools and MCP tools (exact, capitalized names,
/// which never collide with Ghosty's lowercase ones). Returns false for any
/// other name, which keeps the generic card.
bool claudeCode(const QString &name, const QJsonObject &a, Info &found)
{
    const auto field = [&a](const char *key) { return text(a.value(QLatin1String(key))); };
    if (name == QLatin1String("Bash")) {
        found.kind = QStringLiteral("command");
        found.title = QStringLiteral("Run a command");
        found.code = field("command");
        found.text = a.value(QLatin1String("run_in_background")).toBool()
                         ? joined({QStringLiteral("In the background"), field("description")})
                         : field("description");
    } else if (name == QLatin1String("BashOutput") || name == QLatin1String("TaskOutput")) {
        found.kind = QStringLiteral("command");
        found.title = QStringLiteral("Read command output");
        found.text = either(a.value(QLatin1String("bash_id")), a.value(QLatin1String("task_id"))).toString();
    } else if (name == QLatin1String("KillShell") || name == QLatin1String("TaskStop")) {
        found.kind = QStringLiteral("command");
        found.title = QStringLiteral("Stop a background command");
        found.text = either(a.value(QLatin1String("shell_id")), a.value(QLatin1String("task_id"))).toString();
    } else if (name == QLatin1String("Read") || name == QLatin1String("NotebookRead")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Read a file");
        found.path = either(a.value(QLatin1String("file_path")), a.value(QLatin1String("notebook_path"))).toString();
        const QJsonValue offset = a.value(QLatin1String("offset")), limit = a.value(QLatin1String("limit"));
        if (!offset.isUndefined() || !limit.isUndefined()) {
            const double from = offset.isUndefined() ? 1 : toNumber(offset);
            found.text = limit.isUndefined() ? range(offset)
                                             : range(QJsonValue(from), QJsonValue(from + toNumber(limit) - 1));
        }
    } else if (name == QLatin1String("Write")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Write a file");
        found.path = field("file_path");
        found.added = field("content");
    } else if (name == QLatin1String("Edit") || name == QLatin1String("MultiEdit")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Edit a file");
        found.path = field("file_path");
        QStringList removed, added;
        const QJsonArray edits = a.value(QLatin1String("edits")).toArray();
        if (edits.isEmpty()) {
            removed << field("old_string");
            added << field("new_string");
        }
        for (const QJsonValue &edit : edits) {
            removed << text(edit.toObject().value(QLatin1String("old_string")));
            added << text(edit.toObject().value(QLatin1String("new_string")));
        }
        found.removed = removed.join(QLatin1Char('\n'));
        found.added = added.join(QLatin1Char('\n'));
        if (a.value(QLatin1String("replace_all")).toBool())
            found.text = QStringLiteral("Every occurrence");
    } else if (name == QLatin1String("NotebookEdit")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Edit a notebook");
        found.path = field("notebook_path");
        found.added = field("new_source");
    } else if (name == QLatin1String("Grep")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Search the code");
        found.text = joined({field("pattern"), field("glob")});
        found.path = field("path");
    } else if (name == QLatin1String("Glob") || name == QLatin1String("LS")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Find files");
        found.text = field("pattern");
        found.path = field("path");
    } else if (name == QLatin1String("WebFetch")) {
        found.kind = QStringLiteral("web");
        found.title = QStringLiteral("Open a web page");
        found.url = field("url");
    } else if (name == QLatin1String("WebSearch")) {
        found.kind = QStringLiteral("web");
        found.title = QStringLiteral("Search the web");
        found.text = field("query");
    } else if (name == QLatin1String("Agent") || name == QLatin1String("Task")) {
        found.kind = QStringLiteral("command");
        found.title = QStringLiteral("Claude subagent");
        found.text = joined({field("description"), field("subagent_type")});
    } else if (name == QLatin1String("TodoWrite")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Update the plan");
        const qsizetype items = a.value(QLatin1String("todos")).toArray().size();
        found.text = items == 1 ? QStringLiteral("1 item") : QString::number(items) + QStringLiteral(" items");
    } else if (name == QLatin1String("Skill")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Load a skill");
        found.text = either(a.value(QLatin1String("skill")), a.value(QLatin1String("name"))).toString();
    } else if (name.startsWith(QLatin1String("mcp__"))) {
        // mcp__<server>__<tool>: the exact source name, shown readably.
        const QStringList parts = name.mid(5).split(QStringLiteral("__"));
        found.kind = QStringLiteral("command");
        found.title = parts.size() >= 2 ? parts.mid(1).join(QStringLiteral("__")) + QStringLiteral(" (MCP ")
                                              + parts.first() + QLatin1Char(')')
                                        : name;
    } else {
        return false;
    }
    return true;
}
} // namespace

Info describe(const QString &name, const QString &json, bool known)
{
    QJsonValue args(QJsonValue::Null);
    QString raw;
    if (known && !json.isEmpty()) {
        QJsonParseError error{};
        const QJsonDocument doc =
            QJsonDocument::fromJson(QByteArray("[") + json.toUtf8() + QByteArray("]"), &error);
        if (error.error == QJsonParseError::NoError && doc.isArray() && doc.array().size() == 1)
            args = doc.array().at(0);
        else
            raw = json; // Past the arguments' display bound.
    }
    const QJsonObject a = args.isObject() ? args.toObject() : QJsonObject();
    const QString path = text(a.value(QLatin1String("path")));
    Info found;
    if (name == QLatin1String("subagent")) {
        found.title = a.value(QLatin1String("background")).toBool()
                          ? QStringLiteral("Background subagent")
                          : QStringLiteral("Foreground subagent");
        // The prompt alone: the job's identity is not part of it.
        found.text = text(a.value(QLatin1String("task")));
    } else if (name == QLatin1String("bash")) {
        found.kind = QStringLiteral("command");
        found.title = QStringLiteral("Run a command");
        found.code = text(either(a.value(QLatin1String("script")), a.value(QLatin1String("command"))));
    } else if (name == QLatin1String("bash_output")) {
        found.kind = QStringLiteral("command");
        found.title = QStringLiteral("Read command output");
        const QJsonValue offset = a.value(QLatin1String("offset"));
        found.text = joined({text(a.value(QLatin1String("id"))), text(a.value(QLatin1String("stream"))),
                             offset.isUndefined() ? QString() : text(offset)});
    } else if (name == QLatin1String("read")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Read a file");
        found.path = path;
        if (path.isEmpty() && a.value(QLatin1String("paths")).isArray()) {
            QStringList paths;
            for (const QJsonValue &p : a.value(QLatin1String("paths")).toArray())
                paths << text(p);
            found.path = paths.join(QStringLiteral(", "));
        }
        const QJsonValue offset = a.value(QLatin1String("offset")),
                         limit = a.value(QLatin1String("limit"));
        QString lines;
        if (limit.isUndefined()) {
            lines = range(offset);
        } else {
            const QJsonValue from = either(offset, QJsonValue(1));
            // (offset ?? 1) + limit - 1, as JavaScript adds.
            const double to = from.isString() || limit.isString()
                                  ? toNumber(QJsonValue(concat(from) + concat(limit))) - 1
                                  : toNumber(from) + toNumber(limit) - 1;
            lines = range(from, QJsonValue(to));
        }
        found.text = joined({text(a.value(QLatin1String("symbol"))), lines});
    } else if (name == QLatin1String("write")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Write a file");
        found.path = path;
        found.added = text(a.value(QLatin1String("content")));
    } else if (name == QLatin1String("edit")) {
        // One block, several `edits`, or several `files` each with its own.
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Edit a file");
        const auto objects = [](const QJsonValue &list) {
            QVector<QJsonValue> out;
            if (list.isArray())
                for (const QJsonValue &item : list.toArray())
                    if (item.isObject() || item.isArray())
                        out << item;
            return out;
        };
        const QVector<QJsonValue> files = objects(a.value(QLatin1String("files")));
        QVector<QJsonValue> blocks{QJsonValue(a)};
        blocks += objects(a.value(QLatin1String("edits")));
        for (const QJsonValue &file : files) {
            blocks << file;
            blocks += objects(file.toObject().value(QLatin1String("edits")));
        }
        const auto all = [&blocks](const char *key) {
            QStringList parts;
            for (const QJsonValue &block : blocks)
                if (const QString part = text(block.toObject().value(QLatin1String(key)));
                    !part.isEmpty())
                    parts << part;
            return parts.join(QLatin1Char('\n'));
        };
        found.path = path;
        if (path.isEmpty()) {
            QStringList paths;
            for (const QJsonValue &file : files)
                if (const QString p = text(file.toObject().value(QLatin1String("path"))); !p.isEmpty())
                    paths << p;
            found.path = paths.join(QStringLiteral(", "));
        }
        found.removed = all("search");
        if (a.value(QLatin1String("lines")).isArray()) {
            QStringList added;
            for (const QJsonValue &line : a.value(QLatin1String("lines")).toArray())
                added << text(line);
            found.added = added.join(QLatin1Char('\n'));
        } else {
            found.added = all("replace");
        }
        const QString pos = text(a.value(QLatin1String("pos")));
        found.text = joined({range(a.value(QLatin1String("start_line")), a.value(QLatin1String("end_line"))),
                             pos.isEmpty() ? QString()
                                           : joined({pos, text(a.value(QLatin1String("end")))})});
    } else if (name == QLatin1String("code_search")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Search the code");
        found.text = text(a.value(QLatin1String("query")));
    } else if (name == QLatin1String("ast_search")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Search symbols");
        found.text = joined({text(a.value(QLatin1String("name"))), text(a.value(QLatin1String("kind"))),
                             text(a.value(QLatin1String("filePattern")))});
    } else if (name == QLatin1String("lsp")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Ask the language server");
        found.path = path;
        const QJsonValue line = a.value(QLatin1String("line"));
        found.text = joined({text(a.value(QLatin1String("action"))), text(a.value(QLatin1String("symbol"))),
                             line.isUndefined() ? QString() : QStringLiteral("Line ") + concat(line)});
    } else if (name == QLatin1String("get_repo_map")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Map the repository");
    } else if (name == QLatin1String("memory")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Update memory");
        found.text = joined({text(a.value(QLatin1String("action"))),
                             text(either(a.value(QLatin1String("fact")), a.value(QLatin1String("heading"))))});
    } else if (name == QLatin1String("skill")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Load a skill");
        found.text = text(a.value(QLatin1String("name")));
    } else if (name == QLatin1String("obs_recall")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Recall an observation");
        found.text = text(a.value(QLatin1String("id")));
    } else if (name == QLatin1String("attach_file")) {
        found.kind = QStringLiteral("file");
        found.title = QStringLiteral("Attach a file");
        found.path = path;
    } else if (name == QLatin1String("web_search")) {
        found.kind = QStringLiteral("web");
        found.title = QStringLiteral("Search the web");
        found.text = text(a.value(QLatin1String("query")));
    } else if (name == QLatin1String("web_fetch")) {
        found.kind = QStringLiteral("web");
        found.title = QStringLiteral("Open a web page");
        found.url = text(a.value(QLatin1String("url")));
    } else if (claudeCode(name, a, found)) {
        // A Claude Code run's own tool, described from its own arguments.
    } else {
        found.kind = QStringLiteral("command");
        found.title = name.isEmpty() ? QStringLiteral("Tool") : name;
        found.code = args.isNull() ? QString() : compact(json);
    }
    if (found.code.isEmpty() && name.startsWith(QLatin1String("mcp__")) && !args.isNull())
        found.code = compact(json);
    if (!raw.isEmpty())
        found.code = raw;
    const QString summary = (!found.path.isEmpty()   ? found.path
                             : !found.code.isEmpty() ? found.code
                             : !found.text.isEmpty() ? found.text
                                                     : found.url)
                                .simplified();
    found.summary = summary.size() > 160 ? summary.left(159) + QStringLiteral("…") : summary;
    static const QRegularExpression url(QStringLiteral("^[a-z][a-z0-9+.-]*://([^/?#]*)([^#]*)"),
                                        QRegularExpression::CaseInsensitiveOption);
    QString host, rest = found.url;
    if (const QRegularExpressionMatch m = url.match(rest); m.hasMatch()) {
        host = m.captured(1);
        if (host.startsWith(QLatin1String("www."), Qt::CaseSensitive))
            host.remove(0, 4);
        rest = m.captured(2) == QLatin1String("/") ? QString() : m.captured(2);
    }
    found.link = host + rest;
    found.removedLines = lines(found.removed);
    found.addedLines = lines(found.added);
    return found;
}

QStringList endLines(const QString &state, const QString &ending)
{
    if (!ending.isEmpty() && (state == QLatin1String("done") || state == QLatin1String("error")))
        return ending.split(QLatin1Char('\n'));
    if (state == QLatin1String("done"))
        return {QStringLiteral("Done")};
    if (state == QLatin1String("error"))
        return {QStringLiteral("Failed")};
    if (state == QLatin1String("running"))
        return {QStringLiteral("Running…")};
    if (state == QLatin1String("cancelled"))
        return {QStringLiteral("Cancelled")};
    if (state == QLatin1String("missing"))
        return {QStringLiteral("No result was saved")};
    if (state == QLatin1String("unconfirmed"))
        return {QStringLiteral("Result unconfirmed")};
    return {state};
}

QString liveNote(const QString &state, bool hasOutput)
{
    if (!hasOutput)
        return {};
    if (state == QLatin1String("running"))
        return QStringLiteral("Live output, incomplete until the result arrives");
    if (state == QLatin1String("missing") || state == QLatin1String("unconfirmed"))
        return QStringLiteral("Partial live output");
    return {};
}

QString omission(double lines, double characters)
{
    const QString l = lines == 1 ? QStringLiteral("1 earlier line")
                                 : jsNumber(lines) + QStringLiteral(" earlier lines");
    const QString c = characters == 1 ? QStringLiteral("1 character")
                                      : jsNumber(characters) + QStringLiteral(" characters");
    if (lines > 0 && characters > 0)
        return l + QStringLiteral(" and ") + c + QStringLiteral(" omitted");
    if (lines > 0)
        return l + QStringLiteral(" omitted");
    if (characters > 0)
        return c + QStringLiteral(" of this line omitted");
    return {};
}

QString plain(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

QVector<SelectionUnit> units(const Row &row)
{
    QVector<SelectionUnit> out;
    if (!row.expanded)
        return out;
    // Each is a block of the card's body (a <pre> or a <div>): a line end after.
    const auto add = [&out](const QString &path, const QString &text) {
        if (text.isEmpty())
            return;
        SelectionUnit unit;
        unit.path = path;
        unit.text.text = plain(text);
        unit.after = 'l';
        out << std::move(unit);
    };
    const Info info = describe(row.name, row.arguments, row.known);
    add(QStringLiteral("t/c:t"), info.code);
    const auto diff = [&add](const Lines &side, QChar letter) {
        for (int i = 0; i < side.shown.size(); ++i)
            add(QStringLiteral("t/%1%2:t").arg(letter).arg(i),
                side.shown.at(i).isEmpty() ? QStringLiteral(" ") : side.shown.at(i));
        if (side.more > 0)
            add(QStringLiteral("t/%1m:t").arg(letter), QString::number(side.more) +
                                                          QStringLiteral(" more lines"));
    };
    diff(info.removedLines, QLatin1Char('r'));
    diff(info.addedLines, QLatin1Char('a'));
    add(QStringLiteral("t/q:t"), info.text);
    add(QStringLiteral("t/l:t"), info.link);
    if (row.name == QLatin1String("subagent")) {
        // The prompt, then the activity panel: its line and each step's
        // heading and text. The steps stand in for the output.
        add(QStringLiteral("t/sn:t"), row.note);
        for (const auto &value : row.steps) {
            const QVariantMap step = value.toMap();
            const QString id = QString::number(step.value(QStringLiteral("id")).toLongLong());
            add(QStringLiteral("t/s%1h:t").arg(id), step.value(QStringLiteral("head")).toString());
            add(QStringLiteral("t/s%1:t").arg(id), step.value(QStringLiteral("text")).toString());
        }
    } else {
        add(QStringLiteral("t/o:t"), omission(row.omittedLines, row.omittedCharacters));
        add(QStringLiteral("t/b:t"), row.body);
        add(QStringLiteral("t/v:t"), liveNote(row.state, !row.body.isEmpty()));
    }
    if (row.state != QLatin1String("running")) {
        const QStringList end = endLines(row.state, row.ending);
        for (int i = 0; i < end.size(); ++i)
            add(QStringLiteral("t/e%1:t").arg(i), end.at(i).isEmpty() ? QStringLiteral(" ") : end.at(i));
    }
    return out;
}

QVariantList steps(const QVector<ActivityItem> &items)
{
    QVariantList out;
    out.reserve(items.size());
    for (const auto &item : items) {
        QString head, kind, text = item.text;
        if (item.kind == ActivityItem::Call) {
            kind = QStringLiteral("call");
            const Info info = describe(item.name, item.arguments, true);
            head = info.summary.isEmpty() ? info.title
                                          : info.title + QStringLiteral(" · ") + info.summary;
        } else if (item.kind == ActivityItem::Thinking) {
            kind = QStringLiteral("thinking");
            head = QStringLiteral("Thinking");
        } else {
            kind = QStringLiteral("text");
        }
        if (item.clipped > 0) {
            // Live output keeps its end, anything else its beginning.
            const bool tail = item.kind == ActivityItem::Call && item.state == QLatin1String("running");
            const QString cut = QStringLiteral("%1 %2 %3 not shown")
                                    .arg(QString::number(item.clipped),
                                         tail ? QStringLiteral("earlier") : QStringLiteral("more"),
                                         item.clipped == 1 ? QStringLiteral("character")
                                                           : QStringLiteral("characters"));
            text = tail ? QStringLiteral("… ") + cut + QLatin1Char('\n') + text
                        : text + QStringLiteral("\n… ") + cut;
        }
        out << QVariantMap{{QStringLiteral("id"), item.id},
                           {QStringLiteral("kind"), kind},
                           {QStringLiteral("head"), head},
                           {QStringLiteral("text"), text},
                           {QStringLiteral("state"), item.state},
                           {QStringLiteral("revision"), double(item.revision)}};
    }
    return out;
}
} // namespace toolcard

QVariantMap ToolText::describe(const QString &name, const QString &json, bool known) const
{
    const toolcard::Info info = toolcard::describe(name, json, known);
    const auto lines = [](const toolcard::Lines &side) {
        return QVariantMap{{QStringLiteral("shown"), side.shown}, {QStringLiteral("more"), side.more}};
    };
    return {{QStringLiteral("kind"), info.kind},
            {QStringLiteral("title"), info.title},
            {QStringLiteral("path"), info.path},
            {QStringLiteral("code"), info.code},
            {QStringLiteral("text"), info.text},
            {QStringLiteral("url"), info.url},
            {QStringLiteral("removed"), info.removed},
            {QStringLiteral("added"), info.added},
            {QStringLiteral("summary"), info.summary},
            {QStringLiteral("link"), info.link},
            {QStringLiteral("removedLines"), lines(info.removedLines)},
            {QStringLiteral("addedLines"), lines(info.addedLines)}};
}
