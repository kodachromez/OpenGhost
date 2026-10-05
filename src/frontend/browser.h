#pragma once
#include "browser_automation.h"
#include "host.h"
#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QVariantList>
#include <functional>
#include <memory>

namespace openghost
{
class Browser;
class BrowserTools;

// The panel's tabs (browser-panel.js `tabs`), in strip order. One row per tab
// for the strip and for the guest views: a row keeps its delegate while the
// tab lives, so a guest is never rebuilt because another tab changed.
class BrowserTabs final : public QAbstractListModel
{
    Q_OBJECT
  public:
    enum Role {
        Handle = Qt::UserRole + 1,
        Label,       // title, else "New tab" or the host
        Tooltip,     // label, and the URL under it
        Url,         // "" for a blank tab
        Icon,        // the page's favicon, "" for none
        Loading,     // between did-start-loading and did-stop-loading
        Active,      // the selected tab
        View,        // a guest exists (not lazy, not crashed or closed)
        Incarnation, // which guest: a new one after a crash or failed readiness
        Source,      // the URL the guest was created with
        State,       // lazy, loading, ready, failed, gone
    };
    explicit BrowserTabs(Browser *browser) : QAbstractListModel(nullptr), m_browser(browser) {}
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  private:
    friend class Browser;
    Browser *m_browser;
};

// browser-panel.js's BrowserPanel for the desktop build: the panel's open
// state and width, its tabs and their guests' lifecycle, the agent's driving
// and the user's Take Control / Hand Back, its saved layout, and the
// `host.browser` snapshot. It owns the automation seam and serial tool owner;
// the panel's guest views (Qt WebEngine, only with OPENGHOST_BROWSER) execute
// native navigation and report page lifecycle through the methods below.
//
// A BrowserAutomation implementation is owned by this host when composed by
// the desktop application. Without one, no tools are advertised.
class Browser final : public HostServices
{
    Q_OBJECT
    Q_PROPERTY(QObject *automation READ automation CONSTANT)
    Q_PROPERTY(bool open READ isOpen NOTIFY changed)
    Q_PROPERTY(int savedWidth READ savedWidth NOTIFY changed)
    Q_PROPERTY(QAbstractItemModel *tabs READ tabs CONSTANT)
    Q_PROPERTY(QString active READ activeHandle NOTIFY changed)
    Q_PROPERTY(QString url READ url NOTIFY changed)
    Q_PROPERTY(bool blank READ blank NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY changed)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY changed)
    Q_PROPERTY(bool agent READ agent NOTIFY changed)
    Q_PROPERTY(bool driving READ driving NOTIFY changed)
    Q_PROPERTY(bool user READ user NOTIFY changed)
    Q_PROPERTY(QString storagePath READ storagePath CONSTANT)

  public:
    static constexpr int TabsMax = 12;
    static constexpr int ReadyMs = 15000;
    static constexpr int AccountsMax = 30;   // openghost.browser.accounts
    static constexpr int DownloadsMax = 100; // per guest, desktop/browser.js
    // A completed download of a tab's guest (desktop/browser.js owner.downloads):
    // `operation` is the browser step that was running on that guest when the
    // download started, cleared when that step's turn ends.
    struct Download {
        QString file;
        double at = 0;
        QString operation, session;
        int incarnation = 0;
    };
    struct Tab {
        QString handle, url, title, icon, error;
        bool loading = false;
        bool view = false;    // a guest exists
        bool pending = false; // its readiness is still awaited (tab.ready)
        int incarnation = 0;
        int guest = 0; // tab.id: nonzero once its guest is ready
        QString source;
        QString state = QStringLiteral("lazy");
        Sequence revision = 0;
        QString pageId, document;
        bool back = false, forward = false;
        QVector<Download> downloads;
    };
    // `statePath`: the saved layout (openghost.browser), empty for none.
    // `storagePath`: the guests' persistent site storage (persist:browser).
    // `downloadsPath`: where guests' downloads are saved (the OS downloads
    // folder); empty refuses every download rather than guessing a folder.
    explicit Browser(QString statePath = {}, QString storagePath = {}, QObject *parent = nullptr,
                     QString downloadsPath = {});
    ~Browser() override;

    // Set once, before publishing the host or constructing the guest QML.
    void setAutomation(std::unique_ptr<BrowserAutomation> automation);
    QObject *automation() const { return m_automation.get(); }
    BrowserTools *toolOwner() const { return m_tools.get(); }
    Q_INVOKABLE void attachGuest(const QString &handle, int incarnation, QObject *view);
    std::optional<BrowserState> browser() const override { return snapshot(); }
    QVector<HostToolSchema> tools() const override;
    void run(RequestId id, const HostToolRequest &request) override;
    void cancel(RequestId id) override;
    void turnEnded(const QString &sessionId) override;
    void inputQueued(const QString &sessionId) override;

    BrowserState snapshot() const;
    static QString normalize(const QString &text);
    static QString hostOf(const QString &url);
    static bool isBlank(const QString &url) { return url.isEmpty() || url == "about:blank"; }

    bool isOpen() const { return m_open; }
    int savedWidth() const { return m_width; }
    QAbstractItemModel *tabs() { return &m_model; }
    const QVector<Tab> &tabList() const { return m_tabs; }
    const Tab *tab(const QString &handle) const;
    QString activeHandle() const { return m_active; }
    QString url() const;
    bool blank() const;
    bool loading() const;
    QString error() const;
    bool ready() const;
    bool canGoBack() const;
    bool canGoForward() const;
    bool agent() const { return !m_drivers.isEmpty(); }
    bool driving() const { return agent() && !m_user; }
    bool user() const { return agent() && m_user; }
    bool userHas() const { return user(); }
    QString storagePath() const { return m_storagePath; }
    QString downloadsPath() const { return m_downloadsPath; }
    QString lent() const { return m_lent; }
    const QVector<BrowserState::SignInObservation> &accounts() const { return m_accounts; }

    // The panel (browser-panel.js build/setOpen/fit/newTab/select/close/go).
    Q_INVOKABLE void setOpen(bool open);
    Q_INVOKABLE void toggle() { setOpen(!m_open); }
    // fit(): the panel's width in `room` (the window less the sidebar and
    // three gaps): the saved width, else 44 % of the room, at least 360 px
    // and leaving the chat 400 px.
    Q_INVOKABLE int widthFor(qreal room) const { return fit(room, m_width); }
    Q_INVOKABLE void resize(qreal room, qreal wanted);
    Q_INVOKABLE QString newTab(const QString &url = {}, bool background = false, bool focus = true);
    Q_INVOKABLE void select(const QString &handle);
    Q_INVOKABLE void close(const QString &handle);
    Q_INVOKABLE void go(const QString &text);
    Q_INVOKABLE void back();
    Q_INVOKABLE void forward();
    Q_INVOKABLE void reloadOrStop();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void take();
    Q_INVOKABLE void handBack();
    Q_INVOKABLE void save() const;
    // The address view's parts: the dimmed scheme (none for https), the host
    // and the dimmed rest (syncBar).
    Q_INVOKABLE QStringList urlParts(const QString &url) const;

    // What a guest's page did (webview events). `incarnation` names the guest;
    // a report from a guest that has since been replaced is ignored.
    Q_INVOKABLE void documentState(const QString &handle, int incarnation, const QString &document,
                                   bool ready);
    Q_INVOKABLE void domReady(const QString &handle, int incarnation);
    Q_INVOKABLE void loadStarted(const QString &handle, int incarnation);
    Q_INVOKABLE void loadStopped(const QString &handle, int incarnation, const QString &url,
                                 const QString &title);
    Q_INVOKABLE void loadFailed(const QString &handle, int incarnation, int code,
                                const QString &description);
    Q_INVOKABLE void navigated(const QString &handle, int incarnation, const QString &url,
                               bool inPage);
    Q_INVOKABLE void titleChanged(const QString &handle, int incarnation, const QString &title);
    Q_INVOKABLE void iconChanged(const QString &handle, int incarnation, const QString &icon);
    Q_INVOKABLE void history(const QString &handle, int incarnation, bool back, bool forward);
    Q_INVOKABLE void crashed(const QString &handle, int incarnation);
    // A page asked for a tab (window.open/target=_blank/middle click, or the
    // page menu's Open link/image in new tab): browser-panel.js onEvent 'open'.
    Q_INVOKABLE void openFrom(const QString &handle, int incarnation, const QString &url,
                              bool background);
    // A guest's isolated observer saw a filled password field being sent
    // (desktop/browser-preload.js): the site's host name only, never verified.
    Q_INVOKABLE void signedIn(const QString &handle, int incarnation, const QString &host);
    // The page menu (desktop/browser.js context-menu) for what was clicked:
    // {link, image, editable, canCut, canCopy, canPaste, selection, back,
    // forward}. Rows are {action, label, enabled} or {separator: true}.
    Q_INVOKABLE static QVariantList menu(const QVariantMap &context);

    // Downloads, as the engine reports them (BrowserAutomation::observeDownloads).
    QString downloadStarting(quint64 id, const QString &handle, int incarnation,
                             const QString &name);
    void downloadEnded(quint64 id, bool completed);
    // Completed downloads attributed to one browser step of a tab's guest.
    QVector<Download> downloadsOf(const QString &handle, const QString &operation) const;
    // The turn ended: its steps no longer own their downloads.
    void releaseDownloads(const QString &session);

    // The agent's use of the browser (chat.js onHostTool/end): `key` is the
    // chat driving it. While any chat drives, the overlays show; the last one
    // to stop gives the browser back to the agent and wakes its waiters.
    void drive(const QString &key, bool on);
    // Resolved when the user hands the browser back, or no chat drives it.
    void waitForAgent(std::function<void()> resolve) { m_waiters.append(std::move(resolve)); }
    int waiting() const { return int(m_waiters.size()); }
    // The readiness deadline (READY_MS); tests shorten it.
    void setReadyMs(int ms) { m_readyMs = ms; }

  signals:
    void changed();
    // For the guest `handle`: load (url), back, forward, reload, stop, focus, blur.
    void act(const QString &handle, const QString &verb, const QString &url);
    void focusAddress();
    // Keyboard focus leaves the guests for the app (view.blur()).
    void yieldFocus();
    // A panel guest's download finished (browser-panel.js notify).
    void downloaded(const QString &name);

  protected:
    void timerEvent(QTimerEvent *event) override;

  private:
    friend class BrowserTabs;
    friend class BrowserTools;
    void revise(Tab &tab);
    void invalidatePage(Tab &tab);
    static int fit(qreal room, int wanted);
    int indexOf(const QString &handle) const;
    Tab *find(const QString &handle, int incarnation = -1);
    QString addTab(const QString &url, const QString &title, int after = -1);
    QString openTab(const QString &url, int after, bool background, bool focus);
    void createView(Tab &tab, const QString &url);
    void failReady(Tab &tab);
    void selectAt(int index, bool lazy);
    void closeAt(int index, bool quiet);
    void row(const QString &handle);
    void update(const QString &handle);
    void report();
    void release();
    void stopTimer(const QString &handle);
    void load();

    struct PendingDownload {
        QString tab;
        int incarnation = 0;
        QString file, operation, session;
    };
    QString m_statePath, m_storagePath, m_downloadsPath;
    QHash<quint64, PendingDownload> m_downloads;
    QSet<QString> m_reserved; // files chosen for downloads still running
    QVector<BrowserState::SignInObservation> m_accounts;
    QVector<Tab> m_tabs;
    BrowserTabs m_model;
    QString m_active;
    quint64 m_selectionRevision = 0;
    bool m_open = false, m_user = false;
    int m_width = 0;
    QSet<QString> m_drivers;
    QString m_lent;
    QVector<std::function<void()>> m_waiters;
    QHash<int, QString> m_timers; // readiness deadline -> tab handle
    QSet<RequestId> m_runs;
    QString m_reported;
    int m_guests = 0;
    int m_readyMs = ReadyMs;
    std::unique_ptr<BrowserAutomation> m_automation;
    std::unique_ptr<BrowserTools> m_tools;
};
} // namespace openghost
