#include "browser_tools.h"
#include "browser.h"
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cmath>

// The resource is also used from a static QtCore-only library.
static void browserResources() { Q_INIT_RESOURCE(browser_contract); }
namespace openghost
{
namespace
{
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString quoted(const QString &s)
{
    auto json = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}
QString key(const HostToolRequest &r)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{r.sessionId, r.turnId, r.toolCallId})
                                 .toJson(QJsonDocument::Compact));
}
const QSet<QString> supported = {"browser_navigate", "browser_snapshot",   "browser_click",
                                 "browser_type",     "browser_select",     "browser_press",
                                 "browser_scroll",   "browser_screenshot", "browser_read",
                                 "browser_wait",     "browser_tabs"};
const QSet<QString> inputs = {"browser_click", "browser_type", "browser_select", "browser_press",
                              "browser_scroll"};
// JavaScript conversions the reference applies to direct-call arguments.
bool truthy(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::Bool:
        return v.toBool();
    case QJsonValue::Double:
        return v.toDouble() != 0 && !std::isnan(v.toDouble());
    case QJsonValue::String:
        return !v.toString().isEmpty();
    case QJsonValue::Array:
    case QJsonValue::Object:
        return true;
    default:
        return false;
    }
}
double number(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::Null:
        return 0;
    case QJsonValue::Bool:
        return v.toBool();
    case QJsonValue::Double:
        return v.toDouble();
    case QJsonValue::String: {
        const auto text = v.toString().trimmed();
        if (text.isEmpty())
            return 0;
        bool ok = false;
        const double value = text.toDouble(&ok);
        return ok ? value : std::nan("");
    }
    default:
        return std::nan("");
    }
}
QString string(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::String:
        return v.toString();
    case QJsonValue::Bool:
        return v.toBool() ? "true" : "false";
    case QJsonValue::Double: {
        auto json = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
        return QString::fromUtf8(json.mid(1, json.size() - 2));
    }
    case QJsonValue::Null:
        return "null";
    case QJsonValue::Undefined:
        return "undefined";
    default:
        return "[object Object]";
    }
}
// A ref other than undefined/null/empty string selects the element.
bool present(const QJsonValue &ref)
{
    return !ref.isUndefined() && !ref.isNull() && ref != QJsonValue("");
}
// desktop/browser.js KEYS/MODIFIERS/keyOf(): never an OS-dependent guess.
struct Stroke {
    BrowserAutomation::Input event;
    QString error;
};
Stroke keyOf(const QString &combo)
{
    struct Named {
        const char *key, *code;
        int vk;
        const char *text;
    };
    static const QHash<QString, Named> keys = {{"enter", {"Enter", "Enter", 13, "\r"}},
                                               {"return", {"Enter", "Enter", 13, "\r"}},
                                               {"tab", {"Tab", "Tab", 9, ""}},
                                               {"escape", {"Escape", "Escape", 27, ""}},
                                               {"esc", {"Escape", "Escape", 27, ""}},
                                               {"backspace", {"Backspace", "Backspace", 8, ""}},
                                               {"delete", {"Delete", "Delete", 46, ""}},
                                               {"space", {" ", "Space", 32, " "}},
                                               {"arrowup", {"ArrowUp", "ArrowUp", 38, ""}},
                                               {"arrowdown", {"ArrowDown", "ArrowDown", 40, ""}},
                                               {"arrowleft", {"ArrowLeft", "ArrowLeft", 37, ""}},
                                               {"arrowright", {"ArrowRight", "ArrowRight", 39, ""}},
                                               {"up", {"ArrowUp", "ArrowUp", 38, ""}},
                                               {"down", {"ArrowDown", "ArrowDown", 40, ""}},
                                               {"left", {"ArrowLeft", "ArrowLeft", 37, ""}},
                                               {"right", {"ArrowRight", "ArrowRight", 39, ""}},
                                               {"pageup", {"PageUp", "PageUp", 33, ""}},
                                               {"pagedown", {"PageDown", "PageDown", 34, ""}},
                                               {"home", {"Home", "Home", 36, ""}},
                                               {"end", {"End", "End", 35, ""}}};
    static const QHash<QString, int> modifiers = {
        {"alt", 1}, {"control", 2}, {"ctrl", 2}, {"meta", 4}, {"cmd", 4}, {"win", 4}, {"shift", 8}};
    Stroke out;
    QStringList parts;
    for (const auto &part : combo.split('+'))
        if (!part.trimmed().isEmpty())
            parts << part.trimmed();
    if (parts.isEmpty()) {
        out.error = "key is empty";
        return out;
    }
    auto &event = out.event;
    for (const auto &part : parts.mid(0, parts.size() - 1)) {
        const int bit = modifiers.value(part.toLower());
        if (!bit) {
            out.error = "Unknown modifier " + part + ". Use Control, Alt, Shift or Meta.";
            return out;
        }
        event.modifiers |= bit;
    }
    const auto last = parts.last();
    if (const auto known = keys.find(last.toLower()); known != keys.end()) {
        event.key = known->key;
        event.code = known->code;
        event.vk = known->vk;
        event.text = event.modifiers & 7 ? QString() : QString::fromLatin1(known->text);
        return out;
    }
    if (last.size() != 1) {
        out.error = "Unknown key " + last +
                    ". Use Enter, Tab, Escape, Backspace, Delete, Space, arrows, PageUp, "
                    "PageDown, Home, End or a single character.";
        return out;
    }
    static const QRegularExpression letter(QStringLiteral("[A-Z]")), digit(QStringLiteral("[0-9]"));
    const auto upper = last.toUpper();
    event.key = last;
    event.code = letter.match(upper).hasMatch() ? "Key" + upper
                 : digit.match(last).hasMatch() ? "Digit" + last
                                                : QString();
    event.vk = upper.at(0).unicode();
    event.text = event.modifiers & 7 ? QString() : last;
    return out;
}
} // namespace
struct BrowserTools::Job {
    RequestId id;
    HostToolRequest request;
    QString tab, page, document;
    Sequence receiptRevision = 0;
    quint64 lease = 0, call = 0, generation = 0;
    int incarnation = 0;
    QElapsedTimer elapsed;
    qint64 deadline = 0, loadDeadline = 0;
    bool explicitTab = false, done = false, started = false, navigating = false;
    // follow: a navigation-permissive step was issued; later barriers follow the
    // replacement document (navigating additionally stops loading on abandon).
    bool follow = false, dirty = false, handed = false, held = false, addTabs = false;
    QPointer<QObject> back; // browser-panel.js run(): document.activeElement at receipt
};
BrowserTools::BrowserTools(Browser &browser, BrowserAutomation &engine)
    : QObject(nullptr), m_browser(browser), m_engine(engine)
{
    browserResources();
    // Prompt loss/cancellation detection even while waiting on an engine callback.
    auto *timer = new QTimer(this);
    timer->setInterval(20);
    connect(timer, &QTimer::timeout, this, [this] {
        if (m_active && !m_active->held)
            check(m_active);
        const auto queued = m_queue;
        for (const auto &job : queued)
            if (!job->done && !job->held && job->elapsed.elapsed() >= m_limits.queue)
                fail(job, "timeout", "Browser queue timed out");
        pump();
    });
    timer->start();
    connect(&browser, &Browser::changed, this, [this] {
        for (auto it = m_reads.begin(); it != m_reads.end();) {
            const auto *tab = m_browser.tab(it.key());
            if (!tab || tab->pageId != it->page)
                it = m_reads.erase(it);
            else
                ++it;
        }
    });
}
BrowserTools::~BrowserTools()
{
    m_closing = true;
    const auto jobs = m_jobs.values();
    for (const auto &job : jobs)
        abandon(job);
}
QVector<HostToolSchema> BrowserTools::schemas() const
{
    QFile file(":/browser/schemas.json");
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Missing browser schemas");
    QVector<HostToolSchema> out;
    for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
        const auto row = value.toObject();
        if (supported.contains(row["name"].toString()))
            out.append({row["name"].toString(), row["description"].toString(),
                        row["parameters"].toObject()});
    }
    return out;
}
HostToolResult BrowserTools::format(const QJsonValue &value)
{
    const QJsonObject answer =
        value.isObject() ? value.toObject() : QJsonObject{{"error", "the browser did not answer"}};
    HostToolResult result;
    QJsonObject data;
    for (const auto *name :
         {"code", "tabId", "pageId", "readId", "refs", "tabs", "truncated", "coverage", "scroll",
          "width", "height", "scale", "pageWidth", "pageHeight", "downloads"})
        if (answer.contains(name))
            data[name] = answer[name];
    if (answer.contains("pagination")) {
        const auto pagination = answer["pagination"].toObject();
        for (auto it = pagination.begin(); it != pagination.end(); ++it)
            data[it.key()] = it.value();
    }
    result.isError = !answer["error"].toString().isEmpty();
    result.status = result.isError ? HostToolResult::Status::Error : HostToolResult::Status::Ok;
    if (result.isError) {
        if (data["code"].toString().isEmpty())
            data["code"] = "browser_error";
        result.content.append(HostToolResult::Text{"Error: " + answer["error"].toString()});
    } else {
        result.content.append(HostToolResult::Text{answer["text"].toString()});
        if (!answer["image"].toString().isEmpty())
            result.content.append(HostToolResult::Image{
                answer["image"].toString(), QStringLiteral("Screenshot of the built-in browser")});
    }
    result.data = data;
    return result;
}
void BrowserTools::run(RequestId id, const HostToolRequest &request)
{
    if (m_jobs.contains(id))
        return; // never replace an owned reverse request
    auto job = std::make_shared<Job>();
    m_jobs.insert(id, job);
    job->id = id;
    job->request = request;
    job->elapsed.start();
    job->explicitTab = !request.args["tabId"].toString().isEmpty();
    job->tab = job->explicitTab                 ? request.args["tabId"].toString()
               : request.name == "browser_tabs" ? QString()
                                                : m_browser.activeHandle();
    if (const auto *tab = m_browser.tab(job->tab))
        job->receiptRevision = tab->revision;
    job->lease = m_browser.m_selectionRevision;
    job->back = m_engine.focused();
    // ChatService admits live turns. This owner also refuses conflicting/reused
    // identities so a queued step can never silently acquire another turn.
    QString error;
    if (request.sessionId.isEmpty() || request.turnId.isEmpty() || request.toolCallId.isEmpty())
        error = "A browser step needs an owned session, turn and tool call";
    else if ((m_turns.contains(request.sessionId) &&
              m_turns[request.sessionId] != request.turnId) ||
             m_seen.contains(key(request)))
        error = "The browser turn or tool call is stale";
    else if (!supported.contains(request.name))
        error = "Unsupported browser tool";
    if (!error.isEmpty()) {
        QTimer::singleShot(0, this, [this, job, error] { fail(job, "invalid_request", error); });
        return;
    }
    m_turns[request.sessionId] = request.turnId;
    m_seen.insert(key(request));
    m_browser.drive(request.sessionId, true);
    job->held = m_browser.userHas();
    job->handed = job->held;
    m_queue.enqueue(job);
    QTimer::singleShot(0, this, [this] { pump(); });
}
void BrowserTools::abandon(const Work &job)
{
    if (!job || job->done)
        return;
    job->done = true;
    m_jobs.remove(job->id);
    ++job->generation;
    m_engine.cancel(job->call);
    if (job->navigating)
        m_engine.navigate(target(job), "stop", {});
    if (job->dirty) {
        if (auto *tab = m_browser.find(job->tab, job->incarnation)) {
            m_browser.invalidatePage(*tab);
            m_browser.update(tab->handle);
        }
        m_reads.remove(job->tab);
    }
    // browser-panel.js giveBack(): after any step, a keyboard the step moved
    // into a guest returns to where the user was, unless the user has control.
    if (!m_closing && !m_browser.m_user)
        if (const auto lent = m_engine.giveBack(job->back.data()); !lent.isEmpty())
            m_browser.m_lent = lent;
}
void BrowserTools::cancel(RequestId id)
{
    abandon(m_jobs.value(id));
    QTimer::singleShot(0, this, [this] { pump(); });
}
void BrowserTools::turnEnded(const QString &session)
{
    const auto jobs = m_jobs.values();
    for (const auto &job : jobs)
        if (job->request.sessionId == session)
            abandon(job);
    m_turns.remove(session);
    for (auto it = m_seen.begin(); it != m_seen.end();) {
        if (QJsonDocument::fromJson(it->toUtf8()).array().first().toString() == session)
            it = m_seen.erase(it);
        else
            ++it;
    }
    QTimer::singleShot(0, this, [this] { pump(); });
}
void BrowserTools::inputQueued(const QString &session)
{
    const auto jobs = m_jobs.values();
    for (const auto &job : jobs) {
        if (!job->held || job->request.sessionId != session)
            continue;
        abandon(job);
        HostToolResult result;
        result.status = HostToolResult::Status::Cancelled;
        result.reason = "message";
        emit m_browser.finished(job->id, result);
    }
    QTimer::singleShot(0, this, [this] { pump(); });
}
void BrowserTools::controlChanged()
{
    QList<Work> jobs = m_queue;
    if (m_active)
        jobs.prepend(m_active);
    for (const auto &job : jobs) {
        if (job->done)
            continue;
        if (m_browser.userHas()) {
            m_engine.cancel(job->call);
            ++job->generation;
            if (job->navigating)
                m_engine.navigate(target(job), "stop", {});
            if (job->dirty) {
                if (auto *tab = m_browser.find(job->tab, job->incarnation)) {
                    m_browser.invalidatePage(*tab);
                    m_browser.update(tab->handle);
                }
            }
            job->dirty = job->navigating = job->follow = false;
            job->held = job->handed = true;
        } else if (job->held) {
            job->held = false;
            job->started = false;
            job->page.clear();
            job->incarnation = 0;
            job->addTabs = false;
            job->elapsed.restart();
            job->request.name = "browser_snapshot";
            job->request.args = {};
            job->explicitTab = false;
            job->tab = m_browser.activeHandle();
            job->lease = m_browser.m_selectionRevision;
            job->back = m_engine.focused();
            if (const auto *tab = m_browser.tab(job->tab))
                job->receiptRevision = tab->revision;
        }
    }
    // The interrupted step keeps its place ahead of every queued step.
    if (m_active && !m_active->held) {
        m_queue.prepend(m_active);
        m_active.reset();
    }
    QTimer::singleShot(0, this, [this] { pump(); });
}
void BrowserTools::pump()
{
    if (m_pumping)
        return;
    m_pumping = true;
    if (m_active && m_active->done)
        m_active.reset();
    while (!m_active && !m_queue.isEmpty()) {
        auto job = m_queue.dequeue();
        if (job->done)
            continue;
        m_active = job;
        if (job->held)
            break;
        if (job->elapsed.elapsed() >= m_limits.queue) {
            fail(job, "timeout", "Browser queue timed out");
            break;
        }
        job->deadline = m_limits.queue; // renderer budget includes queue + readiness
        if (!job->tab.isEmpty()) {
            const auto *tab = m_browser.tab(job->tab);
            if (!tab) {
                fail(job, "tab_gone", "This browser tab was closed");
                break;
            }
            if (tab->revision != job->receiptRevision) {
                fail(job, "stale_page",
                     "The page changed while the step was queued. Take a new snapshot.");
                break;
            }
        }
        if (job->request.name != "browser_tabs" && !job->explicitTab &&
            job->lease != m_browser.m_selectionRevision) {
            fail(job, "stale_tab", "The active browser target changed. Take a new snapshot.");
            break;
        }
        job->started = true;
        if (job->request.name == "browser_tabs")
            tabs(job);
        else {
            if (job->tab.isEmpty())
                job->tab = m_browser.newTab({}, false, false);
            m_browser.select(job->tab);
            job->lease = m_browser.m_selectionRevision;
            ensure(job);
        }
    }
    m_pumping = false;
}
BrowserAutomation::Target BrowserTools::target(const Work &job) const
{
    return {job->tab, job->page, job->incarnation, job->document};
}
bool BrowserTools::check(const Work &job, bool page)
{
    if (job->done || job->held || m_active != job)
        return false;
    if (job->elapsed.elapsed() >= std::min<qint64>(job->deadline, m_limits.queue)) {
        fail(job, "timeout", "Browser operation timed out");
        return false;
    }
    if (!job->started || job->tab.isEmpty())
        return true;
    const auto *tab = m_browser.tab(job->tab);
    if (!tab) {
        fail(job, "tab_gone", "This browser tab was closed");
        return false;
    }
    if (tab->state == "gone" && job->incarnation) {
        fail(job, "guest_crashed", "The browser page crashed. Open it again.");
        return false;
    }
    if (job->lease != m_browser.m_selectionRevision) {
        fail(job, "stale_tab", "The active browser target changed. Take a new snapshot.");
        return false;
    }
    if (page && !job->page.isEmpty() && !job->follow && tab->pageId != job->page) {
        fail(job, "stale_page", "The page changed. Take a new snapshot.");
        return false;
    }
    return true;
}
void BrowserTools::finish(const Work &job, QJsonObject answer)
{
    if (job->done || job->held)
        return;
    if (job->addTabs)
        answer["tabs"] = tabData();
    auto result = format(answer);
    if (job->handed)
        result.status = HostToolResult::Status::HandedBack;
    abandon(job);
    emit m_browser.finished(job->id, result);
    QTimer::singleShot(0, this, [this] { pump(); });
}
void BrowserTools::fail(const Work &job, const QString &code, const QString &message)
{
    QJsonObject answer{{"error", message}, {"code", code}};
    if (!job->page.isEmpty())
        answer["tabId"] = job->tab;
    finish(job, answer);
}
void BrowserTools::later(const Work &job, int ms, std::function<void()> action)
{
    const auto generation = job->generation;
    QTimer::singleShot(ms, this, [this, job, generation, action = std::move(action)] {
        if (generation == job->generation && check(job))
            action();
    });
}
void BrowserTools::ensure(const Work &job)
{
    if (!check(job, false))
        return;
    auto *tab = m_browser.find(job->tab);
    if (!tab)
        return;
    if (!job->incarnation && (!tab->view || tab->state == "failed")) {
        tab->error.clear();
        m_browser.createView(*tab, Browser::isBlank(tab->url) ? "about:blank" : tab->url);
    }
    job->incarnation = tab->incarnation;
    if (tab->pending) {
        later(job, 20, [this, job] { ensure(job); });
        return;
    }
    if (!tab->guest || tab->state == "failed") {
        fail(job, tab->error.isEmpty() ? "timeout" : "navigation_failed",
             tab->error.isEmpty() ? "The browser did not become ready in time" : tab->error);
        return;
    }
    job->deadline = job->elapsed.elapsed() + m_limits.operation;
    job->page = tab->pageId;
    job->document = tab->document;
    const auto supplied = job->request.args["pageId"].toString();
    if (!supplied.isEmpty() && supplied != job->page) {
        fail(job, "stale_page", "The page changed. Take a new snapshot.");
        return;
    }
    dispatch(job);
}
void BrowserTools::dispatch(const Work &job)
{
    if (!check(job))
        return;
    const auto name = job->request.name;
    const auto &args = job->request.args;
    if (inputs.contains(name) && !truthy(args["pageId"])) {
        fail(job, "stale_page", "Pass pageId from a fresh snapshot with browser input.");
        return;
    }
    // A step that types where the page's focus is gets the lent keyboard back.
    const bool keys =
        name == "browser_press" ||
        (name == "browser_type" && (args["ref"].isUndefined() || args["ref"].isNull()));
    if (keys && m_browser.lent() == job->tab)
        emit m_browser.act(job->tab, QStringLiteral("focus"), {});
    if (name == "browser_click")
        click(job);
    else if (name == "browser_type")
        type(job);
    else if (name == "browser_select")
        select(job);
    else if (name == "browser_press")
        press(job);
    else if (name == "browser_snapshot")
        observe(job);
    else if (name == "browser_navigate")
        navigate(job);
    else if (name == "browser_read")
        read(job);
    else if (name == "browser_screenshot")
        screenshot(job);
    else if (name == "browser_scroll")
        scroll(job);
    else if (name == "browser_wait") {
        double seconds = job->request.args["seconds"].toDouble();
        if (!seconds)
            seconds = job->request.args["text"].toString().isEmpty() ? 2 : 15;
        seconds = std::clamp(seconds, 0.5, 60.0);
        wait(job, job->elapsed.elapsed() + qint64(seconds * 1000), seconds);
    }
}
void BrowserTools::query(const Work &job, BrowserAutomation::Query kind, QJsonObject args,
                         std::function<void(QJsonObject)> next)
{
    if (!check(job))
        return;
    job->call = ++m_serial;
    const auto call = job->call, generation = job->generation;
    QTimer::singleShot(m_limits.call, this, [this, job, call, generation] {
        if (!job->done && !job->held && job->call == call && job->generation == generation)
            fail(job, "timeout", "The page is not responding");
    });
    auto queryTarget = target(job);
    // Navigation and a final input deliberately follow the replacement document
    // while settling. All other steps retain the exact document they admitted.
    if (job->follow)
        queryTarget.document = m_browser.tab(job->tab)->document;
    m_engine.query(call, queryTarget, kind, args,
                   now() + std::min<qint64>(m_limits.call, job->deadline - job->elapsed.elapsed()),
                   [this, job, call, generation, next = std::move(next)](QJsonObject answer) {
                       if (job->generation != generation || job->call != call || !check(job))
                           return;
                       job->call = 0;
                       if (!answer["error"].toString().isEmpty())
                           fail(job, answer["code"].toString("browser_error"),
                                answer["error"].toString());
                       else
                           next(answer);
                   });
}
void BrowserTools::observe(const Work &job, const QString &note)
{
    if (!check(job))
        return;
    if (job->dirty) {
        auto *tab = m_browser.find(job->tab);
        m_browser.invalidatePage(*tab);
        job->page = tab->pageId;
        job->dirty = false;
        m_browser.update(tab->handle);
        m_reads.remove(job->tab);
    }
    const bool full = job->request.args["full"].toBool();
    query(job, BrowserAutomation::Query::Snapshot, {{"full", full}},
          [this, job, note, full](QJsonObject snap) {
              const auto *tab = m_browser.tab(job->tab);
              const auto scroll = snap["scroll"].toObject();
              const double height = scroll["height"].toDouble(), vh = scroll["vh"].toDouble();
              const double screens = height / std::max(1.0, vh);
              QStringList head;
              if (!note.isEmpty())
                  head << note;
              head << "Page: " + (tab->title.isEmpty() ? "(no title)" : tab->title)
                   << "URL: " + (tab->url.isEmpty() ? "about:blank" : tab->url);
              if (tab->loading)
                  head << "The page is still loading.";
              const QString viewport =
                  QStringLiteral("Viewport %1×%2").arg(scroll["vw"].toInt()).arg(vh);
              head << (screens > 1.05
                           ? viewport + QString(", scrolled %1% of a page %2 screens tall.")
                                            .arg(height - vh > 4
                                                     ? int(std::round(scroll["top"].toDouble() /
                                                                      (height - vh) * 100))
                                                     : 0)
                                            .arg(screens, 0, 'f', 1)
                           : viewport + ", the whole page fits on screen.");
              QStringList lines;
              for (const auto &line : snap["lines"].toArray())
                  lines << line.toString();
              QString text = head.join('\n') + "\n\n" +
                             (lines.isEmpty() ? "(nothing readable on screen)" : lines.join('\n'));
              const int skipped = snap["skipped"].toInt();
              if (skipped)
                  text += QString("\n[… %1 more lines not shown. %2.]")
                              .arg(skipped)
                              .arg(full ? "Scroll to them and take a snapshot"
                                        : "Call browser_snapshot with full true or scroll");
              else if (!full && scroll["below"].toDouble() > 8)
                  text += "\n[More content below: scroll down to see it.]";
              finish(job, {{"text", text},
                           {"tabId", job->tab},
                           {"pageId", job->page},
                           {"refs", snap["refs"]},
                           {"truncated", snap["truncated"]},
                           {"coverage", full ? "full-dom-heuristic" : "viewport-dom-heuristic"},
                           {"scroll", scroll},
                           {"downloads", QJsonArray{}}});
          });
}
QJsonArray BrowserTools::tabData() const
{
    QJsonArray out;
    for (const auto &tab : m_browser.snapshot().tabs)
        out.append(QJsonObject{{"n", tab.n},
                               {"tabId", tab.tabId},
                               {"state", tab.state},
                               {"loading", tab.loading},
                               {"revision", double(tab.revision)},
                               {"title", tab.title},
                               {"url", tab.url},
                               {"active", tab.active}});
    return out;
}
QString BrowserTools::tabText() const
{
    QStringList lines;
    for (const auto &tab : m_browser.snapshot().tabs) {
        QString label = tab.title;
        if (label.isEmpty())
            label = Browser::isBlank(tab.url) ? "New tab" : Browser::hostOf(tab.url);
        if (label.isEmpty())
            label = tab.url;
        lines << QString::number(tab.n) + ". " + label +
                     (tab.url.isEmpty() ? "" : " (" + tab.url + ")") +
                     (tab.active ? " active" : "");
    }
    return lines.isEmpty() ? "No tabs are open." : lines.join('\n');
}
void BrowserTools::tabs(const Work &job)
{
    const auto action = job->request.args["action"].toString("list");
    if (action == "list") {
        finish(job, {{"text", "Tabs:\n" + tabText()}, {"tabs", tabData()}});
        return;
    }
    if (action == "new") {
        if (m_browser.tabList().size() >= Browser::TabsMax) {
            fail(job, "tab_limit", "The browser already has 12 tabs. Close one first.");
            return;
        }
        job->tab = m_browser.newTab({}, false, false);
        if (job->request.args["url"].toString().isEmpty()) {
            finish(job, {{"text", "Opened a new empty tab.\n\nTabs:\n" + tabText()},
                         {"tabs", tabData()},
                         {"tabId", job->tab}});
            return;
        }
        job->addTabs = true;
        job->request.name = "browser_navigate";
    } else if (action == "switch" || action == "close") {
        if (!job->explicitTab) {
            fail(job, "invalid_request", "Pass a stable tabId, not a display position");
            return;
        }
        if (action == "close") {
            m_browser.close(job->tab);
            job->tab.clear();
            finish(job, {{"text", "Closed. Tabs:\n" + tabText()}, {"tabs", tabData()}});
            return;
        }
        m_browser.select(job->tab);
        job->request.name = "browser_snapshot";
    } else {
        fail(job, "invalid_request", "Unknown browser tab action");
        return;
    }
    job->request.args.remove("pageId"); // tabs deliberately ignore page preconditions
    job->lease = m_browser.m_selectionRevision;
    ensure(job);
}
void BrowserTools::settle(const Work &job, std::function<void()> next, bool history)
{
    later(job, history ? 250 : 120, [this, job, next = std::move(next)] {
        job->loadDeadline = job->elapsed.elapsed() + 15000;
        auto poll = std::make_shared<std::function<void()>>();
        // Capture a weak self: timers own continuations, never a callback cycle.
        std::weak_ptr<std::function<void()>> weak = poll;
        *poll = [this, job, next, weak] {
            if (m_browser.tab(job->tab)->loading) {
                if (job->elapsed.elapsed() >= job->loadDeadline) {
                    fail(job, "timeout", "The page did not finish loading");
                    return;
                }
                if (auto keep = weak.lock())
                    later(job, 40, [keep] { (*keep)(); });
            } else
                quiet(job, job->elapsed.elapsed() + 2000, next);
        };
        (*poll)();
    });
}
void BrowserTools::quiet(const Work &job, qint64 until, std::function<void()> next)
{
    query(job, BrowserAutomation::Query::Quiet, {}, [this, job, until, next](QJsonObject answer) {
        if (answer["quiet"].toBool() || job->elapsed.elapsed() >= until)
            next();
        else
            later(job, 50, [this, job, until, next] { quiet(job, until, next); });
    });
}
void BrowserTools::navigate(const Work &job)
{
    QString raw = job->request.args["url"].toString().trimmed(), verb = raw.toLower();
    if (raw.isEmpty()) {
        fail(job, "browser_error", "url is empty");
        return;
    }
    const auto *tab = m_browser.tab(job->tab);
    const bool history = verb == "back" || verb == "forward" || verb == "reload";
    if ((verb == "back" && !tab->back) || (verb == "forward" && !tab->forward)) {
        fail(job, "browser_error", "There is no page to go " + verb + " to");
        return;
    }
    QString url;
    if (!history) {
        // The host normalizer, unlike the toolbar, also accepts IPv6 loopback.
        static const QRegularExpression ipv6(QStringLiteral("^\\[::1\\](:\\d+)?(/|$)"));
        url = ipv6.match(raw).hasMatch() ? "http://" + raw : Browser::normalize(raw);
        verb = "load";
    }
    job->navigating = job->follow = true;
    m_browser.find(job->tab)->error.clear();
    const auto before = tab->revision;
    m_engine.navigate(target(job), verb, url);
    job->loadDeadline = job->elapsed.elapsed() + m_limits.load;
    auto poll = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = poll;
    *poll = [this, job, before, weak, history] {
        const auto *t = m_browser.tab(job->tab);
        if (!t->error.isEmpty()) {
            fail(job, "navigation_failed", "The page could not be opened: " + t->error);
            return;
        }
        if (t->loading || t->revision == before) {
            if (job->elapsed.elapsed() >= job->loadDeadline) {
                fail(job, "timeout", "The page did not load in time");
                return;
            }
            if (auto keep = weak.lock())
                later(job, 25, [keep] { (*keep)(); });
            return;
        }
        settle(
            job,
            [this, job] {
                const auto *current = m_browser.tab(job->tab);
                if (!current->error.isEmpty()) {
                    fail(job, "navigation_failed",
                         "The page could not be opened: " + current->error);
                    return;
                }
                job->page = current->pageId;
                job->document = current->document;
                job->navigating = job->follow = false;
                observe(job);
            },
            history);
    };
    later(job, 25, [poll] { (*poll)(); });
}
void BrowserTools::wait(const Work &job, qint64 until, double seconds)
{
    const auto text = job->request.args["text"].toString();
    if (job->elapsed.elapsed() >= until) {
        if (!text.isEmpty())
            fail(job, "wait_timeout",
                 QString("\"%1\" did not appear within %2 s.")
                     .arg(text, QString::number(seconds, 'g', 16)));
        else
            settle(job, [this, job] { observe(job); });
        return;
    }
    if (text.isEmpty()) {
        later(job, int(std::min<qint64>(250, until - job->elapsed.elapsed())),
              [this, job, until, seconds] { wait(job, until, seconds); });
        return;
    }
    query(job, BrowserAutomation::Query::Has, {{"text", text}},
          [this, job, until, seconds, text](QJsonObject answer) {
              if (answer["found"].toBool())
                  settle(job,
                         [this, job, text] { observe(job, "\"" + text + "\" is on the page."); });
              else
                  later(job, 400, [this, job, until, seconds] { wait(job, until, seconds); });
          });
}
void BrowserTools::read(const Work &job)
{
    const auto args = job->request.args;
    if (args["start"].toDouble() > 0 || !args["readId"].toString().isEmpty()) {
        const auto saved = m_reads.value(job->tab);
        if (saved.id.isEmpty() || saved.id != args["readId"].toString() || saved.page != job->page)
            fail(job, "stale_read", "The read snapshot expired. Read again from start=0.");
        else
            readResult(job, saved);
        return;
    }
    query(job, BrowserAutomation::Query::Read, {}, [this, job](QJsonObject answer) {
        Read saved{job->page, uuid(), answer["text"].toString(), answer["url"].toString(),
                   answer["sourceTruncated"].toBool()};
        m_reads[job->tab] = saved;
        readResult(job, saved);
    });
}
void BrowserTools::readResult(const Work &job, const Read &saved)
{
    // QString offsets are UTF-16, including intentionally split surrogate pairs.
    const double from = std::max(0.0, std::floor(job->request.args["start"].toDouble()));
    const QString part =
        from < saved.text.size() ? saved.text.mid(qsizetype(from), 40000) : QString();
    const double end = from + part.size();
    const bool more = end < saved.text.size();
    QString text = saved.url + "\n\n" + (part.isEmpty() ? "(empty page)" : part);
    if (more)
        text += QString("\n\n[Characters %1–%2 of %3. Call browser_read with tabId=%4, readId=%5, "
                        "start=%2 to read further.]")
                    .arg(from, 0, 'f', 0)
                    .arg(end, 0, 'f', 0)
                    .arg(saved.text.size())
                    .arg(quoted(job->tab), quoted(saved.id));
    if (saved.truncated)
        text += "\n\n[Source HTML truncated at 4 Mi UTF-16 code units.]";
    finish(job, {{"text", text},
                 {"tabId", job->tab},
                 {"pageId", job->page},
                 {"readId", saved.id},
                 {"pagination",
                  QJsonObject{{"start", from},
                              {"end", end},
                              {"total", double(saved.text.size())},
                              {"hasMore", more},
                              {"offsetUnit", "utf16"},
                              {"sourceTruncated", saved.truncated},
                              {"truncated", saved.truncated || more},
                              {"coverage", "html-derived; excludes form controls, shadow roots and "
                                           "iframe content; may include hidden text"}}}});
}
void BrowserTools::screenshot(const Work &job)
{
    query(job, BrowserAutomation::Query::Metrics, {}, [this, job](QJsonObject metrics) {
        job->call = ++m_serial;
        const auto call = job->call, generation = job->generation;
        QTimer::singleShot(m_limits.call, this, [this, job, call, generation] {
            if (!job->done && job->generation == generation && job->call == call)
                fail(job, "timeout", "The page did not draw a screenshot");
        });
        const bool full = job->request.args["full_page"].toBool();
        m_engine.capture(
            call, target(job), metrics, full,
            [this, job, call, generation, full](QJsonObject shot) {
                if (job->generation != generation || job->call != call || !check(job))
                    return;
                job->call = 0;
                if (shot.contains("error")) {
                    fail(job, shot["code"].toString("browser_error"), shot["error"].toString());
                    return;
                }
                const auto *tab = m_browser.tab(job->tab);
                const double scale = shot["scale"].toDouble();
                const QString guidance =
                    std::abs(scale - 1) < 0.01
                        ? "one screenshot pixel is one page pixel, so x and y for browser_click "
                          "can be read from it"
                        : "to click by coordinates divide screenshot pixels by " +
                              QString::number(scale, 'f', 3);
                shot["text"] = QStringLiteral("Screenshot of %1: %2, %3×%4; %5.")
                                   .arg(full ? "the page from the top" : "the viewport",
                                        tab->title.isEmpty() ? tab->url : tab->title,
                                        QString::number(shot["width"].toInt()),
                                        QString::number(shot["height"].toInt()), guidance);
                shot["tabId"] = job->tab;
                shot["pageId"] = job->page;
                // A native grab has no document token. Revalidate in the isolated
                // world after capture as well, before publishing its pixels.
                query(job, BrowserAutomation::Query::Metrics, {},
                      [this, job, shot](QJsonObject) { finish(job, shot); });
            });
    });
}
void BrowserTools::scroll(const Work &job)
{
    const auto after = [this, job] {
        later(job, 250, [this, job] { settle(job, [this, job] { observe(job); }); });
    };
    const auto args = job->request.args;
    const auto ref = args["ref"];
    if (present(ref)) {
        job->dirty = true;
        query(job, BrowserAutomation::Query::Reveal, {{"ref", ref}},
              [after](QJsonObject) { after(); });
        return;
    }
    const double requested = number(args["amount"]);
    const double amount =
        std::min(10.0, std::max(0.1, requested && !std::isnan(requested) ? requested : 0.8));
    const double sign =
        (truthy(args["direction"]) ? string(args["direction"]) : "down").toLower() == "up" ? -1 : 1;
    query(job, BrowserAutomation::Query::Metrics, {},
          [this, job, amount, sign, after](QJsonObject metrics) {
              // A mouse wheel at the visual viewport's centre, by its client height.
              BrowserAutomation::Input wheel;
              wheel.kind = BrowserAutomation::Input::Kind::Wheel;
              wheel.x = metrics["viewWidth"].toDouble() / 2;
              wheel.y = metrics["viewHeight"].toDouble() / 2;
              wheel.deltaY = sign * metrics["viewHeight"].toDouble() * amount;
              send(job, wheel, false, after);
          });
}
void BrowserTools::send(const Work &job, const BrowserAutomation::Input &event, bool mayNavigate,
                        std::function<void()> next)
{
    if (!check(job))
        return;
    job->dirty = true;
    job->call = ++m_serial;
    const auto call = job->call, generation = job->generation;
    QTimer::singleShot(m_limits.call, this, [this, job, call, generation] {
        if (!job->done && !job->held && job->call == call && job->generation == generation)
            fail(job, "timeout", "Browser input timed out");
    });
    m_engine.input(call, target(job), event,
                   [this, job, call, generation, next = std::move(next)](QJsonObject answer) {
                       if (job->generation != generation || job->call != call || !check(job))
                           return;
                       job->call = 0;
                       if (answer.contains("error"))
                           fail(job, answer["code"].toString("browser_error"),
                                answer["error"].toString());
                       else
                           next();
                   });
    // The final input may itself navigate before it is acknowledged. Only its
    // release and observation may follow, never input to a replacement page.
    if (mayNavigate)
        job->follow = true;
}
void BrowserTools::barrier(const Work &job, std::function<void()> next)
{
    // Between dispatches of one sequence: a round trip to the admitted
    // document and page. A page change stops the remaining input.
    query(job, BrowserAutomation::Query::Probe, {}, [next](QJsonObject) { next(); });
}
void BrowserTools::pointer(const Work &job, double x, double y, std::function<void()> next)
{
    if (!check(job))
        return;
    emit m_browser.pointer(job->tab, x, y);
    // The pointer is shown moving only on a guest the open panel shows.
    if (m_browser.isOpen() && m_browser.activeHandle() == job->tab)
        later(job, 420, std::move(next));
    else
        next();
}
void BrowserTools::mouse(const Work &job, double x, double y, int count, bool mayNavigate,
                         std::function<void()> next)
{
    using Kind = BrowserAutomation::Input::Kind;
    const auto event = [x, y](Kind kind, int k) {
        BrowserAutomation::Input e;
        e.kind = kind;
        e.x = x;
        e.y = y;
        e.count = k;
        return e;
    };
    auto step = std::make_shared<std::function<void(int)>>();
    std::weak_ptr<std::function<void(int)>> weak = step;
    *step = [this, job, event, count, mayNavigate, next, weak](int k) {
        auto keep = weak.lock();
        if (!keep)
            return;
        const bool final = k == count;
        send(job, event(Kind::Press, k), mayNavigate && final,
             [this, job, event, k, final, next, keep] {
                 send(job, event(Kind::Release, k), false, [this, job, k, final, next, keep] {
                     if (final)
                         next();
                     else
                         barrier(job, [keep, k] { (*keep)(k + 1); });
                 });
             });
    };
    send(job, event(Kind::Move, 1), false, [step] { (*step)(1); });
}
void BrowserTools::stroke(const Work &job, const QString &combo, bool mayNavigate,
                          std::function<void()> next)
{
    const auto parsed = keyOf(combo);
    if (!parsed.error.isEmpty()) {
        fail(job, "browser_error", parsed.error);
        return;
    }
    auto down = parsed.event, up = parsed.event;
    down.kind = BrowserAutomation::Input::Kind::KeyDown;
    up.kind = BrowserAutomation::Input::Kind::KeyUp;
    send(job, down, mayNavigate,
         [this, job, up, next = std::move(next)] { send(job, up, false, next); });
}
void BrowserTools::afterInput(const Work &job, const QString &note)
{
    job->follow = true;
    settle(job, [this, job, note] {
        const auto *tab = m_browser.tab(job->tab);
        job->page = tab->pageId;
        job->document = tab->document;
        job->follow = false;
        observe(job, note);
    });
}
void BrowserTools::click(const Work &job)
{
    const auto args = job->request.args;
    const auto ref = args["ref"];
    const int count = truthy(args["double"]) ? 2 : 1;
    const auto proceed = [this, job, ref, count](double x, double y) {
        if (!check(job))
            return;
        pointer(job, x, y, [this, job, ref, count, x, y] {
            const auto go = [this, job, count, x, y] {
                mouse(job, x, y, count, true, [this, job] { afterInput(job); });
            };
            // Loose `ref != null`, exactly as the reference rechecks.
            if (ref.isUndefined() || ref.isNull()) {
                go();
                return;
            }
            job->dirty = true;
            query(job, BrowserAutomation::Query::Point, {{"ref", ref}},
                  [this, job, x, y, go](QJsonObject spot) {
                      if (!spot["covered"].toString().isEmpty() || spot["x"].toDouble() != x ||
                          spot["y"].toDouble() != y)
                          fail(job, "stale_target",
                               "The click target moved or is covered. Take a new snapshot.");
                      else
                          go();
                  });
        });
    };
    if (present(ref)) {
        job->dirty = true;
        query(job, BrowserAutomation::Query::Point, {{"ref", ref}},
              [this, job, ref, proceed](QJsonObject spot) {
                  if (!spot["covered"].toString().isEmpty())
                      fail(job, "element_covered",
                           "Element [" + string(ref) + "] is covered by " +
                               spot["covered"].toString() + ". No click was sent.");
                  else
                      proceed(spot["x"].toDouble(), spot["y"].toDouble());
              });
        return;
    }
    const double x = number(args["x"]), y = number(args["y"]);
    if (!std::isfinite(x) || !std::isfinite(y)) {
        fail(job, "browser_error", "Pass ref from the snapshot, or x and y in page pixels");
        return;
    }
    proceed(x, y);
}
void BrowserTools::type(const Work &job)
{
    const auto args = job->request.args;
    const auto ref = args["ref"];
    const QString text =
        args["text"].isUndefined() || args["text"].isNull() ? QString() : string(args["text"]);
    const bool clear = args["clear"] != QJsonValue(false), submit = truthy(args["submit"]);
    const auto finish = [this, job, submit] {
        if (!submit) {
            afterInput(job);
            return;
        }
        later(job, 60,
              [this, job] { stroke(job, "Enter", true, [this, job] { afterInput(job); }); });
    };
    const auto insert = [this, job, text, submit, finish] {
        if (!check(job))
            return;
        if (text.isEmpty()) {
            finish();
            return;
        }
        BrowserAutomation::Input commit;
        commit.kind = BrowserAutomation::Input::Kind::Text;
        commit.text = text;
        send(job, commit, !submit, [this, job, submit, finish] {
            if (submit)
                barrier(job, finish);
            else
                finish();
        });
    };
    const auto keys = [this, job, text, clear, submit, insert] {
        if (!check(job))
            return;
        if (!clear) {
            insert();
            return;
        }
        stroke(job, "Control+A", false, [this, job, text, submit, insert] {
            barrier(job, [this, job, text, submit, insert] {
                if (!text.isEmpty()) {
                    insert();
                    return;
                }
                stroke(job, "Delete", !submit, [this, job, submit, insert] {
                    if (submit)
                        barrier(job, insert);
                    else
                        insert();
                });
            });
        });
    };
    if (!present(ref)) {
        keys();
        return;
    }
    job->dirty = true;
    query(job, BrowserAutomation::Query::Point, {{"ref", ref}},
          [this, job, ref, keys](QJsonObject spot) {
              const double x = spot["x"].toDouble(), y = spot["y"].toDouble();
              if (!check(job))
                  return;
              pointer(job, x, y, [this, job, ref, keys, x, y] {
                  query(job, BrowserAutomation::Query::Point, {{"ref", ref}},
                        [this, job, keys, x, y](QJsonObject now) {
                            if (!now["covered"].toString().isEmpty() || now["x"].toDouble() != x ||
                                now["y"].toDouble() != y) {
                                fail(job, "stale_target",
                                     "The field moved or is covered. Take a new snapshot.");
                                return;
                            }
                            mouse(job, x, y, 1, false, [this, job, keys] {
                                barrier(job, [this, job, keys] { later(job, 80, keys); });
                            });
                        });
              });
          });
}
void BrowserTools::select(const Work &job)
{
    const auto args = job->request.args;
    job->dirty = true;
    query(job, BrowserAutomation::Query::Choose, {{"ref", args["ref"]}, {"option", args["option"]}},
          [this, job](QJsonObject answer) {
              afterInput(job, "Chose \"" + answer["chosen"].toString() + "\".");
          });
    // The whole choice may navigate (a change handler); only observation follows.
    job->follow = true;
}
void BrowserTools::press(const Work &job)
{
    const auto args = job->request.args;
    const double times = number(args["times"]);
    const int count = int(
        std::min(20.0, std::max(1.0, std::floor((times && !std::isnan(times) ? times : 1) + 0.5))));
    const auto combo = truthy(args["key"]) ? string(args["key"]) : QString();
    auto step = std::make_shared<std::function<void(int)>>();
    std::weak_ptr<std::function<void(int)>> weak = step;
    *step = [this, job, combo, count, weak](int k) {
        auto keep = weak.lock();
        if (!keep || !check(job))
            return;
        const bool final = k == count - 1;
        stroke(job, combo, final, [this, job, final, keep, k] {
            if (final)
                afterInput(job);
            else
                barrier(job, [keep, k] { (*keep)(k + 1); });
        });
    };
    (*step)(0);
}
} // namespace openghost
