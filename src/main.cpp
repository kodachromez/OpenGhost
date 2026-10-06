#include "appearance.h"
#include "backend/fake_backend.h"
#include "backend/pi_backend.h"
#include "frontend/browser.h"
#include "medialoader.h"
#include "platform/platform.h"
#include "videoinfo.h"
#include "window.h"
#include <cstdio>
#include <cstring>
#include <memory>

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QStandardPaths>
#include <QTemporaryDir>
#ifdef OPENGHOST_BROWSER
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include "browser/qt_browser_automation.h"
#endif

namespace
{
// One-time move of the profile written under the pre-rename application name.
// Each location moves only when the new one does not exist yet; afterwards
// every read and write uses the openghost-cpp locations.
void migrateProfile(QGuiApplication &app)
{
    const auto locations = {QStandardPaths::AppConfigLocation, QStandardPaths::AppDataLocation,
                            QStandardPaths::CacheLocation};
    const QString current = app.applicationName();
    app.setApplicationName(QStringLiteral("openghost-native")); // legacy name, migration only
    QStringList old;
    for (const auto location : locations)
        old.append(QStandardPaths::writableLocation(location));
    app.setApplicationName(current);
    qsizetype i = 0;
    for (const auto location : locations) {
        const QString from = old[i++];
        const QString to = QStandardPaths::writableLocation(location);
        if (from.isEmpty() || to.isEmpty() || from == to || !QFileInfo(from).isDir() ||
            QFileInfo::exists(to))
            continue;
        QDir().mkpath(QFileInfo(to).absolutePath());
        if (!QDir().rename(from, to))
            fprintf(stderr, "Could not migrate %s to %s\n", qPrintable(from), qPrintable(to));
    }
}
} // namespace

#ifdef OPENGHOST_SMOKE_TEST
int smokeTest(QQmlApplicationEngine &engine, WindowController &controller);
int parityTest(QQmlApplicationEngine &engine, WindowController &controller, const QString &manifest,
               const QString &output);
int splashFrames(QQmlApplicationEngine &engine, const QString &output, int every);
void prepareSplashSteps();
int splashSteps(QQmlApplicationEngine &engine, const QString &output, const QString &times);
int splashCheck(QQmlApplicationEngine &engine);
#endif

int main(int argc, char *argv[])
{
#ifdef OPENGHOST_SMOKE_TEST
    // Refuse before constructing an application: a mistaken direct invocation
    // must never open a desktop window. The runner supplies an offscreen
    // renderer on a private memory-only display, with no visible fallback.
    bool parityRequested = false, platformOverride = false;
    for (int i = 1; i < argc; ++i) {
        parityRequested |= std::strncmp(argv[i], "--parity-", 9) == 0 ||
                           std::strncmp(argv[i], "--splash-frames", 15) == 0 ||
                           std::strncmp(argv[i], "--splash-check", 14) == 0;
        platformOverride |= std::strncmp(argv[i], "-platform", 9) == 0 ||
                            std::strncmp(argv[i], "--platform", 10) == 0;
    }
    // A frame capture may also run on a private headless compositor (the
    // runner's own `kwin_wayland --virtual` socket), never the desktop's.
    const bool privateWayland = qgetenv("QT_QPA_PLATFORM") == "wayland" &&
                                qgetenv("WAYLAND_DISPLAY").startsWith("openghost-private-");
    if (parityRequested &&
        ((qgetenv("QT_QPA_PLATFORM") != "offscreen" && !privateWayland) || platformOverride)) {
        fprintf(stderr, "Parity requires QT_QPA_PLATFORM=offscreen and no platform override\n");
        return 2;
    }
    // A stepped splash drives its own animation timeline on the GUI thread.
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--splash-at", 11) == 0 ||
            std::strncmp(argv[i], "--splash-check", 14) == 0)
            qputenv("QSG_RENDER_LOOP", "basic");
#endif
    platform::beforeApplication();
#ifdef OPENGHOST_BROWSER
    // The browser panel's pages (Qt WebEngine) share the window's GL context.
    QtWebEngineQuick::initialize();
#endif
    QGuiApplication app(argc, argv);
    platform::afterApplication();
    app.setApplicationName(QStringLiteral("openghost-cpp"));
    app.setApplicationDisplayName(QStringLiteral("OpenGhost C++"));
    app.setApplicationVersion(QStringLiteral("0.1"));
    app.setWindowIcon(QIcon(QStringLiteral(":/openghost.png")));
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Standalone OpenGhost C++ frontend. No Rust backend or transport."));
    parser.addOption(
        {QStringLiteral("fake-backend"),
         QStringLiteral("Use the in-memory fake backend (no model, tools or credentials).")});
    parser.addOption({QStringLiteral("pi"),
                      QStringLiteral("Chat through Pi (`pi --mode rpc`), one Pi session per chat.")});
    parser.addHelpOption();
    parser.addVersionOption();
#ifdef OPENGHOST_SMOKE_TEST
    parser.addOption(
        {QStringLiteral("smoke-test"), QStringLiteral("Run the isolated UI smoke test and exit.")});
    parser.addOption({QStringLiteral("parity-manifest"),
                      QStringLiteral("Offscreen-only visual fixture manifest."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("parity-output"),
                      QStringLiteral("Visual fixture output directory."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("splash-frames"),
                      QStringLiteral("Read back the splash's rendered frames into a directory."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("splash-every"),
                      QStringLiteral("Save every nth splash frame."), QStringLiteral("n"),
                      QStringLiteral("1")});
    parser.addOption({QStringLiteral("splash-check"),
                      QStringLiteral("Check the splash against splash.js frame by frame and exit.")});
    parser.addOption({QStringLiteral("splash-at"),
                      QStringLiteral("Step the splash deterministically at 240 Hz and grab it at "
                                     "these scene times (ms, comma-separated)."),
                      QStringLiteral("times")});
#endif
    parser.process(app);
#ifdef OPENGHOST_SMOKE_TEST
    if (parser.isSet(QStringLiteral("parity-output")) !=
        parser.isSet(QStringLiteral("parity-manifest")))
        parser.showHelp(2);
#endif
    selectControlsStyle();
    QString appearancePath;
    QString preferencesPath;
    QString dataPath, usagePath, piSessions;
    QString browserPath, browserStorage, browserDownloads;
#ifdef OPENGHOST_SMOKE_TEST
    QTemporaryDir testSettings;
    if (parser.isSet(QStringLiteral("smoke-test")) ||
        parser.isSet(QStringLiteral("parity-manifest")) ||
        parser.isSet(QStringLiteral("splash-frames")) ||
        parser.isSet(QStringLiteral("splash-check"))) {
        if (!testSettings.isValid())
            return 1;
        appearancePath = testSettings.path() + QStringLiteral("/appearance.json");
        preferencesPath = testSettings.path() + QStringLiteral("/preferences.json");
        dataPath = testSettings.path() + QStringLiteral("/library");
        browserPath = testSettings.path() + QStringLiteral("/browser.json");
        browserStorage = testSettings.path() + QStringLiteral("/browser");
        browserDownloads = testSettings.path() + QStringLiteral("/downloads");
    } else
#endif
    {
        migrateProfile(app);
        const QString config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        appearancePath = config + QStringLiteral("/appearance.json");
        preferencesPath = config + QStringLiteral("/preferences.json");
        // The browser panel's layout and tabs (openghost.browser), and its
        // sites' own storage (persist:browser): signed-in sites stay signed in.
        browserPath = config + QStringLiteral("/browser.json");
        browserStorage = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                         QStringLiteral("/browser");
        // Pages' downloads go where the reference's do (app.getPath('downloads')).
        browserDownloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        // Display caches, recovery markers, chat index and usage ledger: local
        // frontend state, never backend history or credentials. The fake's
        // sessions die with the process, so its chats never enter the profile.
        // Pi keeps each chat's history in its own session file (piSessions), so
        // Pi's chats share the profile's library.
        if (!parser.isSet(QStringLiteral("fake-backend")))
            dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                       QStringLiteral("/library");
        // Pi's spend is real: its usage ledger is kept apart from the profile's.
        if (parser.isSet(QStringLiteral("pi")) && !parser.isSet(QStringLiteral("fake-backend"))) {
            usagePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                        QStringLiteral("/pi-usage");
            piSessions = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                         QStringLiteral("/pi-sessions");
        }
    }
#ifdef OPENGHOST_SMOKE_TEST
    if (parser.isSet(QStringLiteral("parity-manifest"))) {
        QFont font(QStringLiteral("Noto Sans"));
        font.setPixelSize(15);
        app.setFont(font);
    }
#endif
    // Frontend-only metadata host. Test adapters install their own host and
    // never read the real metadata cache or fall back to live networking.
#ifdef OPENGHOST_SMOKE_TEST
    if (!parser.isSet(QStringLiteral("smoke-test")) &&
        !parser.isSet(QStringLiteral("parity-manifest")) &&
        !parser.isSet(QStringLiteral("splash-frames")) &&
        !parser.isSet(QStringLiteral("splash-check")))
#endif
        VideoTitles::instance()->setService(
            std::make_shared<NetworkVideoInfo>(),
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                QStringLiteral("/media-info.json"));
    keepAppearance(appearancePath);
    std::unique_ptr<openghost::Backend> backend;
    if (parser.isSet(QStringLiteral("fake-backend")))
        backend = std::make_unique<openghost::FakeBackend>();
    else if (parser.isSet(QStringLiteral("pi")))
        backend = std::make_unique<openghost::PiBackend>(piSessions);
    std::unique_ptr<openghost::Browser> browser;
#ifdef OPENGHOST_BROWSER
    // The desktop panel exists with or without an agent backend.
    browser = std::make_unique<openghost::Browser>(browserPath, browserStorage, nullptr,
                                                   browserDownloads);
    browser->setAutomation(std::make_unique<openghost::QtBrowserAutomation>());
#else
    Q_UNUSED(browserStorage)
    Q_UNUSED(browserDownloads)
#endif
    WindowController controller(backend.get(), preferencesPath, dataPath, browser.get(),
                                usagePath);
    openghost::registerBuiltinPlugins(*controller.frontendPlugins());
    QObject::connect(&controller, &WindowController::closeRequested, &app, &QCoreApplication::quit);
    QQmlApplicationEngine engine;
#ifdef OPENGHOST_SMOKE_TEST
    QObject::connect(
        &engine, &QQmlApplicationEngine::warnings, &engine,
        [&engine](const QList<QQmlError> &) { engine.setProperty("smokeWarnings", true); });
#endif
    engine.setNetworkAccessManagerFactory(denyNetwork());
    // A reply's pictures, which MediaLoader alone fetches under its rules.
    engine.addImageProvider(QStringLiteral("openghost-media"), new MediaImages);
    QVariantMap initial{{"frontend", QVariant::fromValue(&controller)}};
#ifdef OPENGHOST_SMOKE_TEST
    // The offscreen GLX backing surface is allocated on first show and does
    // not grow on resize. Allocate the fixture ceiling before its first frame.
    if (parser.isSet(QStringLiteral("parity-manifest"))) {
        initial.insert(QStringLiteral("width"), 2048);
        initial.insert(QStringLiteral("height"), 1400);
    }
    // A stepped splash: its clock, animation driver and Ghosts' choices, from the start.
    if (parser.isSet(QStringLiteral("splash-at")) || parser.isSet(QStringLiteral("splash-check")))
        prepareSplashSteps();
#endif
    engine.setInitialProperties(initial);
    engine.load(QUrl(QStringLiteral("qrc:/OpenGhost/Ui/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
#ifdef OPENGHOST_SMOKE_TEST
    if (parser.isSet(QStringLiteral("smoke-test")))
        return smokeTest(engine, controller);
    if (parser.isSet(QStringLiteral("parity-manifest")))
        return parityTest(engine, controller, parser.value(QStringLiteral("parity-manifest")),
                          parser.value(QStringLiteral("parity-output")));
    if (parser.isSet(QStringLiteral("splash-check")))
        return splashCheck(engine);
    if (parser.isSet(QStringLiteral("splash-frames")) && parser.isSet(QStringLiteral("splash-at")))
        return splashSteps(engine, parser.value(QStringLiteral("splash-frames")),
                           parser.value(QStringLiteral("splash-at")));
    if (parser.isSet(QStringLiteral("splash-frames")))
        return splashFrames(engine, parser.value(QStringLiteral("splash-frames")),
                            parser.value(QStringLiteral("splash-every")).toInt());
#endif
    return app.exec();
}
