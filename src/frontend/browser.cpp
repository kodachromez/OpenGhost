#include "browser.h"
#include "browser_tools.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QTimerEvent>
#include <QUrl>
#include <QUuid>
#include <cmath>

namespace openghost
{
namespace
{
// browser-panel.js WIDTH: the panel's share of the room, its least width and
// the least the chat keeps beside it.
constexpr qreal WidthShare = 0.44;
constexpr int WidthMin = 360, ChatMin = 400;

QString label(const Browser::Tab &tab)
{
    if (!tab.title.isEmpty())
        return tab.title;
    if (Browser::isBlank(tab.url))
        return QStringLiteral("New tab");
    const QString host = Browser::hostOf(tab.url);
    return host.isEmpty() ? tab.url : host;
}

// desktop/browser.js guard(): a guest is only ever created on these schemes.
bool guarded(const QString &url)
{
    static const QRegularExpression allowed(QStringLiteral("^(https?|file|about|data):"),
                                            QRegularExpression::CaseInsensitiveOption);
    return allowed.match(url).hasMatch();
}
} // namespace

int BrowserTabs::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_browser->m_tabs.size());
}

QVariant BrowserTabs::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_browser->m_tabs.size())
        return {};
    const auto &tab = m_browser->m_tabs.at(index.row());
    const bool blank = Browser::isBlank(tab.url);
    switch (role) {
    case Handle:
        return tab.handle;
    case Label:
        return label(tab);
    case Tooltip:
        return blank ? label(tab) : label(tab) + QLatin1Char('\n') + tab.url;
    case Url:
        return blank ? QString() : tab.url;
    case Icon:
        return tab.icon;
    case Loading:
        return tab.loading;
    case Active:
        return tab.handle == m_browser->m_active;
    case View:
        return tab.view;
    case Incarnation:
        return tab.incarnation;
    case Source:
        return tab.source;
    case State:
        return tab.state;
    default:
        return {};
    }
}

QHash<int, QByteArray> BrowserTabs::roleNames() const
{
    return {{Handle, "handle"}, {Label, "label"}, {Tooltip, "tooltip"},
            {Url, "url"},       {Icon, "icon"},   {Loading, "loading"},
            {Active, "active"}, {View, "view"},   {Incarnation, "incarnation"},
            {Source, "source"}, {State, "state"}};
}

Browser::Browser(QString statePath, QString storagePath, QObject *parent, QString downloadsPath)
    : HostServices(parent), m_statePath(std::move(statePath)),
      m_storagePath(std::move(storagePath)), m_downloadsPath(std::move(downloadsPath)),
      m_model(this)
{
    load();
}

Browser::~Browser() = default;

void Browser::setAutomation(std::unique_ptr<BrowserAutomation> automation)
{
    if (m_automation || !automation)
        qFatal("Browser automation must be composed exactly once");
    m_automation = std::move(automation);
    m_tools = std::make_unique<BrowserTools>(*this, *m_automation);
    m_automation->observeDownloads(
        [this](quint64 id, const QString &handle, int incarnation, const QString &name) {
            return downloadStarting(id, handle, incarnation, name);
        },
        [this](quint64 id, bool completed) { downloadEnded(id, completed); });
    report();
}

void Browser::attachGuest(const QString &handle, int incarnation, QObject *view)
{
    if (m_automation && find(handle, incarnation))
        m_automation->attach(handle, incarnation, view);
}

QVector<HostToolSchema> Browser::tools() const
{
    return m_tools ? m_tools->schemas() : QVector<HostToolSchema>{};
}

void Browser::cancel(RequestId id)
{
    if (m_tools)
        m_tools->cancel(id);
    m_runs.remove(id);
}

void Browser::turnEnded(const QString &session)
{
    if (m_tools)
        m_tools->turnEnded(session);
    drive(session, false);
}

void Browser::inputQueued(const QString &session)
{
    if (m_tools)
        m_tools->inputQueued(session);
}

void Browser::revise(Tab &tab)
{
    ++tab.revision;
    invalidatePage(tab);
}

void Browser::invalidatePage(Tab &tab)
{
    // Input invalidates observations, not the panel's navigation revision.
    // A queued observation without pageId may still observe the settled page.
    tab.pageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// browser-panel.js normalize(): a URL as typed, a local path, a local server,
// a host name, else a search.
QString Browser::normalize(const QString &value)
{
    const QString text = value.trimmed();
    if (text.isEmpty())
        return {};
    static const QRegularExpression scheme(QStringLiteral("^(https?|file|about|data):"),
                                           QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression drive(QStringLiteral("^[a-zA-Z]:[\\\\/]"));
    static const QRegularExpression local(
        QStringLiteral("^(localhost|127\\.0\\.0\\.1|\\d{1,3}(\\.\\d{1,3}){3})(:\\d+)?(\\/|$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression space(QStringLiteral("\\s"));
    static const QRegularExpression host(
        QStringLiteral("^[^\\s/]+\\.[a-z]{2,}(:\\d+)?(\\/|$|\\?|#)"),
        QRegularExpression::CaseInsensitiveOption);
    if (scheme.match(text).hasMatch())
        return text;
    if (text.startsWith(QLatin1Char('/')))
        return QStringLiteral("file://") + text;
    if (drive.match(text).hasMatch())
        return QStringLiteral("file:///") +
               QString(text).replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (local.match(text).hasMatch())
        return QStringLiteral("http://") + text;
    if (!space.match(text).hasMatch() && host.match(text).hasMatch())
        return QStringLiteral("https://") + text;
    // encodeURIComponent leaves !'()* as they are.
    return QStringLiteral("https://www.google.com/search?q=") +
           QString::fromLatin1(QUrl::toPercentEncoding(text, "!'()*"));
}

QString Browser::hostOf(const QString &url)
{
    const QUrl parsed(url, QUrl::TolerantMode);
    if (!parsed.isValid() || parsed.scheme().isEmpty())
        return {};
    QString host = parsed.host();
    if (host.startsWith(QStringLiteral("www.")))
        host.remove(0, 4);
    return host;
}

QStringList Browser::urlParts(const QString &url) const
{
    if (isBlank(url))
        return {};
    const QUrl parsed(url, QUrl::TolerantMode);
    if (!parsed.isValid() || parsed.scheme().isEmpty())
        return {url};
    const QString lead =
        parsed.scheme() == "https" ? QString() : parsed.scheme() + QStringLiteral("://");
    QString host = parsed.host(QUrl::FullyEncoded);
    if (parsed.port() >= 0)
        host += QLatin1Char(':') + QString::number(parsed.port());
    if (host.startsWith(QStringLiteral("www.")))
        host.remove(0, 4);
    const QString path = parsed.path(QUrl::FullyEncoded);
    QString rest = path == "/" ? QString() : path;
    if (!parsed.query(QUrl::FullyEncoded).isEmpty())
        rest += QLatin1Char('?') + parsed.query(QUrl::FullyEncoded);
    if (!parsed.fragment(QUrl::FullyEncoded).isEmpty())
        rest += QLatin1Char('#') + parsed.fragment(QUrl::FullyEncoded);
    return {lead, host, QUrl::fromPercentEncoding(rest.toUtf8())};
}

int Browser::fit(qreal room, int wanted)
{
    const qreal most = std::max<qreal>(WidthMin, room - ChatMin);
    return int(std::round(
        std::min(most, std::max<qreal>(WidthMin, wanted > 0 ? wanted : room * WidthShare))));
}

void Browser::resize(qreal room, qreal wanted)
{
    const int width = fit(room, int(std::round(wanted)));
    if (width == m_width)
        return;
    m_width = width;
    emit changed();
}

int Browser::indexOf(const QString &handle) const
{
    if (handle.isEmpty())
        return -1;
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs.at(i).handle == handle)
            return i;
    return -1;
}

const Browser::Tab *Browser::tab(const QString &handle) const
{
    const int at = indexOf(handle);
    return at < 0 ? nullptr : &m_tabs.at(at);
}

Browser::Tab *Browser::find(const QString &handle, int incarnation)
{
    const int at = indexOf(handle);
    if (at < 0)
        return nullptr;
    Tab &tab = m_tabs[at];
    // A replaced or removed guest's late events describe no current page.
    if (incarnation >= 0 && (!tab.view || tab.incarnation != incarnation))
        return nullptr;
    return &tab;
}

QString Browser::url() const
{
    const Tab *t = tab(m_active);
    return !t || isBlank(t->url) ? QString() : t->url;
}
bool Browser::blank() const
{
    const Tab *t = tab(m_active);
    return !t || !t->view;
}
bool Browser::loading() const
{
    const Tab *t = tab(m_active);
    return t && t->loading;
}
QString Browser::error() const
{
    const Tab *t = tab(m_active);
    if (!t || t->error.isEmpty())
        return {};
    const QString host = hostOf(t->url);
    return (host.isEmpty() ? t->url : host) + QStringLiteral(" · ") + t->error;
}
bool Browser::ready() const
{
    const Tab *t = tab(m_active);
    return t && t->view && t->guest;
}
bool Browser::canGoBack() const { return ready() && tab(m_active)->back; }
bool Browser::canGoForward() const { return ready() && tab(m_active)->forward; }

QString Browser::addTab(const QString &url, const QString &title, int after)
{
    Tab tab;
    tab.handle = QUuid::createUuid().toString(QUuid::WithoutBraces);
    tab.url = url;
    tab.title = title;
    const int at = after >= 0 ? after + 1 : int(m_tabs.size());
    m_model.beginInsertRows({}, at, at);
    m_tabs.insert(at, tab);
    m_model.endInsertRows();
    // A manually opened tab beyond the limit closes an older one.
    while (m_tabs.size() > TabsMax) {
        int victim = 0;
        for (int i = 0; i < m_tabs.size(); ++i)
            if (m_tabs.at(i).handle != tab.handle && m_tabs.at(i).handle != m_active) {
                victim = i;
                break;
            }
        closeAt(victim, true);
    }
    return tab.handle;
}

void Browser::createView(Tab &tab, const QString &url)
{
    const QString target = url.isEmpty() ? QStringLiteral("about:blank") : url;
    tab.view = true;
    ++tab.incarnation;
    tab.document.clear();
    revise(tab);
    // guard(): a guest on another scheme is never attached; its readiness
    // then times out as the reference's refused webview does.
    tab.source = guarded(target) ? target : QString();
    if (!isBlank(url))
        tab.url = url;
    tab.state = QStringLiteral("loading");
    tab.guest = 0;
    tab.pending = true;
    tab.back = tab.forward = false;
    stopTimer(tab.handle);
    m_timers.insert(startTimer(m_readyMs), tab.handle);
    row(tab.handle);
}

void Browser::stopTimer(const QString &handle)
{
    for (auto it = m_timers.begin(); it != m_timers.end();) {
        if (it.value() == handle) {
            killTimer(it.key());
            it = m_timers.erase(it);
        } else
            ++it;
    }
}

void Browser::timerEvent(QTimerEvent *event)
{
    const QString handle = m_timers.take(event->timerId());
    if (handle.isEmpty())
        return HostServices::timerEvent(event);
    killTimer(event->timerId());
    if (Tab *t = find(handle))
        failReady(*t); // "The browser did not become ready in time"
}

void Browser::failReady(Tab &tab)
{
    if (!tab.pending)
        return;
    tab.pending = false;
    stopTimer(tab.handle);
    tab.state = QStringLiteral("failed");
    row(tab.handle);
    emit changed();
    report();
}

void Browser::row(const QString &handle)
{
    const int at = indexOf(handle);
    if (at >= 0)
        emit m_model.dataChanged(m_model.index(at), m_model.index(at));
}

void Browser::update(const QString &handle)
{
    row(handle);
    emit changed();
    report();
}

void Browser::setOpen(bool open)
{
    if (m_open == open)
        return;
    m_open = open;
    if (open) {
        if (Tab *t = find(m_active); t && !t->view && !isBlank(t->url))
            createView(*t, t->url);
        const Tab *t = tab(m_active);
        if (m_tabs.isEmpty() || !t || isBlank(t->url))
            emit focusAddress();
    }
    save();
    emit changed();
    report();
}

QString Browser::newTab(const QString &url, bool background, bool focus)
{
    return openTab(url, -1, background, focus);
}

QString Browser::openTab(const QString &url, int after, bool background, bool focus)
{
    const QString handle = addTab(url, {}, after);
    if (!isBlank(url) && (m_open || background))
        createView(*find(handle), url);
    if (!background)
        selectAt(indexOf(handle), false);
    row(handle);
    save();
    if (!background && focus && isBlank(url) && m_open)
        emit focusAddress();
    emit changed();
    report();
    return handle;
}

void Browser::select(const QString &handle)
{
    const int at = indexOf(handle);
    if (at >= 0)
        selectAt(at, false);
}

void Browser::selectAt(int index, bool lazy)
{
    const auto active = index >= 0 && index < m_tabs.size() ? m_tabs.at(index).handle : QString();
    if (active != m_active)
        ++m_selectionRevision;
    m_active = active;
    if (Tab *t = find(m_active); t && !t->view && !isBlank(t->url) && !lazy && m_open)
        createView(*t, t->url);
    if (!m_tabs.isEmpty())
        emit m_model.dataChanged(m_model.index(0), m_model.index(int(m_tabs.size()) - 1));
    if (!lazy)
        save();
    emit changed();
    report();
}

void Browser::close(const QString &handle)
{
    const int at = indexOf(handle);
    if (at >= 0)
        closeAt(at, false);
}

void Browser::closeAt(int at, bool quiet)
{
    const QString handle = m_tabs.at(at).handle;
    stopTimer(handle); // its readiness is no longer awaited: "This browser tab was closed"
    m_model.beginRemoveRows({}, at, at);
    m_tabs.removeAt(at);
    m_model.endRemoveRows();
    if (m_lent == handle)
        m_lent.clear();
    if (m_active == handle)
        selectAt(std::min(at, int(m_tabs.size()) - 1), false);
    if (!quiet) {
        save();
        emit changed();
    }
    report();
}

void Browser::go(const QString &text)
{
    const QString target = normalize(text);
    if (target.isEmpty())
        return;
    const QString handle = m_active.isEmpty() ? newTab({}, false, false) : m_active;
    Tab *t = find(handle);
    t->error.clear();
    if (t->view)
        emit act(handle, QStringLiteral("load"), target);
    else
        createView(*t, target);
    t->url = target;
    update(handle);
    save();
}

void Browser::back()
{
    if (const Tab *t = tab(m_active); t && t->view)
        emit act(m_active, QStringLiteral("back"), {});
}

void Browser::forward()
{
    if (const Tab *t = tab(m_active); t && t->view)
        emit act(m_active, QStringLiteral("forward"), {});
}

void Browser::reloadOrStop()
{
    const Tab *t = tab(m_active);
    if (!t || !t->view)
        return;
    emit act(m_active, t->loading ? QStringLiteral("stop") : QStringLiteral("reload"), {});
}

void Browser::retry()
{
    Tab *t = find(m_active);
    if (!t)
        return;
    t->error.clear();
    if (t->view)
        emit act(m_active, QStringLiteral("reload"), {});
    else
        createView(*t, t->url);
    update(m_active);
}

void Browser::documentState(const QString &handle, int incarnation, const QString &document,
                            bool ready)
{
    auto *t = find(handle, incarnation);
    if (!t || document.isEmpty())
        return;
    if (t->document != document) {
        t->document = document;
        revise(*t);
        update(handle);
    }
    if (ready)
        domReady(handle, incarnation);
}

void Browser::domReady(const QString &handle, int incarnation)
{
    Tab *t = find(handle, incarnation);
    if (!t || !t->pending)
        return;
    t->pending = false;
    stopTimer(handle);
    t->guest = ++m_guests;
    t->state = QStringLiteral("ready");
    update(handle);
}

void Browser::loadStarted(const QString &handle, int incarnation)
{
    Tab *t = find(handle, incarnation);
    if (!t)
        return;
    revise(*t); // did-start-navigation (main frame)
    t->loading = true;
    t->error.clear();
    update(handle);
}

void Browser::loadStopped(const QString &handle, int incarnation, const QString &url,
                          const QString &title)
{
    Tab *t = find(handle, incarnation);
    if (!t)
        return;
    // A finished replacement document invalidates observations made in-load,
    // including same-URL reloads whose URL signal does not change.
    if (t->loading)
        revise(*t);
    t->loading = false;
    if (!isBlank(url))
        t->url = url;
    if (!title.isEmpty())
        t->title = title;
    update(handle);
    save();
}

void Browser::loadFailed(const QString &handle, int incarnation, int code,
                         const QString &description)
{
    Tab *t = find(handle, incarnation);
    if (!t || code == -3) // ERR_ABORTED: a navigation replaced by another
        return;
    QString text = description;
    if (text.startsWith(QStringLiteral("net::")))
        text.remove(0, 5); // Chromium's errorDescription, as Electron reports it
    t->error = text.isEmpty() ? QStringLiteral("Error %1").arg(code) : text;
    failReady(*t); // "navigation_failed"
    t->loading = false;
    update(handle);
}

void Browser::navigated(const QString &handle, int incarnation, const QString &url, bool inPage)
{
    Tab *t = find(handle, incarnation);
    if (!t)
        return;
    Q_UNUSED(inPage)
    revise(*t); // commits and same-document history both invalidate observations
    if (hostOf(url) != hostOf(t->url))
        t->icon.clear();
    if (!isBlank(url))
        t->url = url;
    update(handle);
}

void Browser::titleChanged(const QString &handle, int incarnation, const QString &title)
{
    if (Tab *t = find(handle, incarnation)) {
        t->title = title;
        update(handle);
    }
}

void Browser::iconChanged(const QString &handle, int incarnation, const QString &icon)
{
    if (Tab *t = find(handle, incarnation)) {
        t->icon = icon;
        update(handle);
    }
}

void Browser::history(const QString &handle, int incarnation, bool back, bool forward)
{
    Tab *t = find(handle, incarnation);
    if (!t || (t->back == back && t->forward == forward))
        return;
    t->back = back;
    t->forward = forward;
    emit changed();
}

void Browser::crashed(const QString &handle, int incarnation)
{
    Tab *t = find(handle, incarnation);
    if (!t)
        return;
    failReady(*t); // "guest_crashed"
    revise(*t);
    t->state = QStringLiteral("gone");
    t->error = QStringLiteral("The browser page crashed. Open the page again.");
    t->guest = 0;
    t->view = false; // the guest is removed; the next attempt creates another
    if (m_lent == handle)
        m_lent.clear();
    update(handle);
}

void Browser::openFrom(const QString &handle, int incarnation, const QString &url, bool background)
{
    if (url.isEmpty())
        return;
    // newTab(url, {after: from, background}): next to the page that asked.
    openTab(url, find(handle, incarnation) ? indexOf(handle) : -1, background, true);
}

void Browser::signedIn(const QString &handle, int incarnation, const QString &host)
{
    // browser-panel.js signedIn(): the host without www., most recent first.
    static const QRegularExpression name(QStringLiteral("^[A-Za-z0-9.:\\[\\]-]{1,253}$"));
    if (!find(handle, incarnation) || !name.match(host).hasMatch())
        return;
    QString site = host.toLower();
    if (site.startsWith(QStringLiteral("www.")))
        site.remove(0, 4);
    if (site.isEmpty())
        return;
    m_accounts.removeIf([&](const auto &seen) { return seen.host == site; });
    m_accounts.prepend({site, double(QDateTime::currentMSecsSinceEpoch())});
    if (m_accounts.size() > AccountsMax)
        m_accounts.resize(AccountsMax);
    save();
    report();
}

QVariantList Browser::menu(const QVariantMap &context)
{
    QVariantList items;
    const auto add = [&](const QString &action, const QString &label, bool enabled = true) {
        items.append(QVariantMap{{"action", action}, {"label", label}, {"enabled", enabled}});
    };
    const auto line = [&] {
        if (!items.isEmpty() && !items.last().toMap().value("separator").toBool())
            items.append(QVariantMap{{"separator", true}});
    };
    if (!context.value("link").toString().isEmpty()) {
        add("openLink", "Open link in new tab");
        add("copyLink", "Copy link address");
        line();
    }
    if (!context.value("image").toString().isEmpty()) {
        add("openImage", "Open image in new tab");
        add("copyImage", "Copy image");
        line();
    }
    if (context.value("editable").toBool()) {
        add("cut", "Cut", context.value("canCut").toBool());
        add("copy", "Copy", context.value("canCopy").toBool());
        add("paste", "Paste", context.value("canPaste").toBool());
        add("selectAll", "Select all");
        line();
    } else if (!context.value("selection").toString().isEmpty()) {
        add("copy", "Copy");
        line();
    }
    add("back", "Back", context.value("back").toBool());
    add("forward", "Forward", context.value("forward").toBool());
    add("reload", "Reload");
    line();
    add("inspect", "Inspect");
    return items;
}

QString Browser::downloadStarting(quint64 id, const QString &handle, int incarnation,
                                  const QString &suggested)
{
    if (m_downloadsPath.isEmpty() || m_downloads.contains(id))
        return {};
    // desktop/browser.js uniqueFile(): NAME, else "STEM (K)EXT", never replacing a file.
    QString name = QFileInfo(suggested).fileName();
    if (name == "." || name == "..")
        name.clear();
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    const QString ext = dot > 0 ? name.mid(dot) : QString();
    QString stem = dot > 0 ? name.left(dot) : name;
    if (stem.isEmpty())
        stem = QStringLiteral("download");
    QDir dir(m_downloadsPath);
    if (!dir.mkpath(QStringLiteral(".")))
        return {};
    QString file = dir.absoluteFilePath(name.isEmpty() ? QStringLiteral("download") : name);
    for (int k = 1; QFileInfo::exists(file) || m_reserved.contains(file); ++k)
        file = dir.absoluteFilePath(stem + QStringLiteral(" (%1)").arg(k) + ext);
    PendingDownload pending;
    pending.file = file;
    // Only a panel guest owns a download; the step running on it at the
    // start is the one it belongs to (owner.running).
    if (find(handle, incarnation)) {
        pending.tab = handle;
        pending.incarnation = incarnation;
        if (m_tools) {
            const auto [operation, session] = m_tools->running(handle);
            pending.operation = operation;
            pending.session = session;
        }
    }
    m_reserved.insert(file);
    m_downloads.insert(id, pending);
    return file;
}

void Browser::downloadEnded(quint64 id, bool completed)
{
    const auto found = m_downloads.find(id);
    if (found == m_downloads.end())
        return; // exactly once: a repeated or unknown report changes nothing
    const PendingDownload pending = found.value();
    m_downloads.erase(found);
    m_reserved.remove(pending.file);
    // Cancelled and failed downloads are neither recorded nor announced.
    if (!completed || pending.tab.isEmpty())
        return;
    if (Tab *t = find(pending.tab, pending.incarnation)) {
        t->downloads.append({pending.file, double(QDateTime::currentMSecsSinceEpoch()),
                             pending.operation, pending.session, pending.incarnation});
        if (t->downloads.size() > DownloadsMax)
            t->downloads.remove(0, t->downloads.size() - DownloadsMax);
    }
    emit downloaded(QFileInfo(pending.file).fileName());
}

QVector<Browser::Download> Browser::downloadsOf(const QString &handle,
                                                const QString &operation) const
{
    QVector<Download> out;
    if (const Tab *t = tab(handle); t && !operation.isEmpty())
        for (const auto &item : t->downloads)
            if (item.operation == operation && item.incarnation == t->incarnation)
                out.append(item);
    return out;
}

void Browser::releaseDownloads(const QString &session)
{
    for (auto &pending : m_downloads)
        if (pending.session == session)
            pending.operation.clear(), pending.session.clear();
    for (auto &t : m_tabs)
        for (auto &item : t.downloads)
            if (item.session == session)
                item.operation.clear(), item.session.clear();
}

void Browser::take()
{
    if (m_user)
        return;
    m_user = true;
    if (m_tools)
        m_tools->controlChanged();
    emit changed();
    report();
    if (const Tab *t = tab(m_active); t && t->view)
        emit act(m_active, QStringLiteral("focus"), {});
}

void Browser::handBack()
{
    if (!m_user)
        return;
    m_user = false;
    if (m_tools)
        m_tools->controlChanged();
    // The keyboard leaves the page with the user; a later agent step that
    // types gets it back first.
    const Tab *t = tab(m_active);
    m_lent = t && t->view ? m_active : QString();
    if (!m_lent.isEmpty())
        emit act(m_active, QStringLiteral("blur"), {});
    emit changed();
    report();
    release();
}

void Browser::drive(const QString &key, bool on)
{
    const bool had = !m_drivers.isEmpty();
    if (on)
        m_drivers.insert(key);
    else
        m_drivers.remove(key);
    if (m_drivers.isEmpty()) {
        m_user = false;
        release();
    }
    if (on && !had && !m_user)
        emit yieldFocus();
    emit changed();
    report();
}

void Browser::release()
{
    const auto waiters = std::move(m_waiters);
    m_waiters.clear();
    for (const auto &resolve : waiters)
        resolve();
}

void Browser::run(RequestId id, const HostToolRequest &request)
{
    if (m_tools) {
        m_tools->run(id, request);
        return;
    }
    // Not composed, so ChatService refuses before calling; settle anyway,
    // exactly once and never inline, unless cancelled first.
    m_runs.insert(id);
    QTimer::singleShot(0, this, [this, id] {
        if (!m_runs.remove(id))
            return;
        HostToolResult result;
        result.content.append(
            HostToolResult::Text{QStringLiteral("The browser has no host tools yet.")});
        result.isError = true;
        result.status = HostToolResult::Status::Error;
        emit finished(id, result);
    });
}

BrowserState Browser::snapshot() const
{
    BrowserState state;
    state.available = bool(m_tools);
    state.status = BrowserState::Status::Unavailable;
    if (m_tools) {
        state.status = BrowserState::Status::Empty;
        if (const auto *t = tab(m_active)) {
            if (t->state == "gone")
                state.status = BrowserState::Status::Gone;
            else if (t->state == "failed")
                state.status = BrowserState::Status::Failed;
            else if (t->state == "loading")
                state.status = BrowserState::Status::Loading;
            else if (t->guest)
                state.status = BrowserState::Status::Ready;
            else
                state.status = BrowserState::Status::Lazy;
        }
    }
    state.control = userHas() ? BrowserState::Control::User : BrowserState::Control::Agent;
    state.open = m_open;
    for (int k = 0; k < m_tabs.size(); ++k) {
        const Tab &tab = m_tabs.at(k);
        state.tabs.append({k + 1, tab.handle, tab.state, tab.loading, tab.revision, tab.title,
                           isBlank(tab.url) ? QString() : tab.url, tab.handle == m_active});
    }
    state.signedIn = m_accounts;
    return state;
}

void Browser::report()
{
    const BrowserState state = snapshot();
    QJsonArray tabs, accounts;
    for (const auto &tab : state.tabs)
        tabs.append(QJsonArray{tab.n, tab.tabId, tab.state, tab.loading, double(tab.revision),
                               tab.title, tab.url, tab.active});
    for (const auto &seen : state.signedIn)
        accounts.append(QJsonArray{seen.host, seen.at});
    const QString key =
        QString::fromUtf8(QJsonDocument(QJsonArray{state.available, int(state.status),
                                                   int(state.control), state.open, tabs, accounts})
                              .toJson(QJsonDocument::Compact));
    if (key == m_reported)
        return;
    m_reported = key;
    emit browserChanged(state);
}

void Browser::save() const
{
    if (m_statePath.isEmpty())
        return;
    QJsonArray tabs;
    int active = -1;
    for (const auto &tab : m_tabs) {
        if (isBlank(tab.url))
            continue;
        if (tab.handle == m_active)
            active = int(tabs.size());
        tabs.append(QJsonObject{{"url", tab.url}, {"title", tab.title}});
    }
    QJsonArray accounts;
    for (const auto &seen : m_accounts)
        accounts.append(QJsonObject{{"host", seen.host}, {"at", seen.at}});
    // openghost.browser and openghost.browser.accounts, in one local record.
    const QJsonObject saved{{"open", m_open},
                            {"width", m_width},
                            {"tabs", tabs},
                            {"active", std::max(0, active)},
                            {"accounts", accounts}};
    QDir().mkpath(QFileInfo(m_statePath).absolutePath());
    QSaveFile file(m_statePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(saved).toJson(QJsonDocument::Compact));
        file.commit(); // A failed save keeps the previous layout, as localStorage would.
    }
}

void Browser::load()
{
    QJsonObject saved;
    if (!m_statePath.isEmpty()) {
        QFile file(m_statePath);
        if (file.open(QIODevice::ReadOnly))
            saved = QJsonDocument::fromJson(file.read(1 << 20)).object();
    }
    m_width = std::max(0, saved.value("width").toInt());
    const QJsonArray tabs = saved.value("tabs").toArray();
    for (int i = 0; i < std::min<qsizetype>(tabs.size(), TabsMax); ++i) {
        const QJsonObject item = tabs.at(i).toObject();
        addTab(item.value("url").toString(), item.value("title").toString());
    }
    for (const auto &value : saved.value("accounts").toArray()) {
        const QJsonObject item = value.toObject();
        const QString host = item.value("host").toString();
        if (!host.isEmpty() && m_accounts.size() < AccountsMax &&
            std::none_of(m_accounts.begin(), m_accounts.end(),
                         [&](const auto &seen) { return seen.host == host; }))
            m_accounts.append({host, item.value("at").toDouble()});
    }
    const int active = saved.value("active").toInt();
    selectAt(active >= 0 && active < m_tabs.size() ? active : m_tabs.isEmpty() ? -1 : 0, true);
    if (saved.value("open").toBool())
        setOpen(true);
    report();
}
} // namespace openghost
