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

#ifdef OPENGHOST_SMOKE_TEST
int smokeTest(QQmlApplicationEngine &engine, WindowController &controller);
int parityTest(QQmlApplicationEngine &engine, WindowController &controller, const QString &manifest,
               const QString &output);
#endif

int main(int argc, char *argv[])
{
#ifdef OPENGHOST_SMOKE_TEST
    // Refuse before constructing an application: a mistaken direct invocation
    // must never open a desktop window. The runner supplies an offscreen
    // renderer on a private memory-only display, with no visible fallback.
    bool parityRequested = false, platformOverride = false;
    for (int i = 1; i < argc; ++i) {
        parityRequested |= std::strncmp(argv[i], "--parity-", 9) == 0;
        platformOverride |= std::strncmp(argv[i], "-platform", 9) == 0 ||
                            std::strncmp(argv[i], "--platform", 10) == 0;
    }
    if (parityRequested && (qgetenv("QT_QPA_PLATFORM") != "offscreen" || platformOverride)) {
        fprintf(stderr, "Parity requires QT_QPA_PLATFORM=offscreen and no platform override\n");
        return 2;
    }
#endif
    platform::beforeApplication();
#ifdef OPENGHOST_BROWSER
    // The browser panel's pages (Qt WebEngine) share the window's GL context.
    QtWebEngineQuick::initialize();
#endif
    QGuiApplication app(argc, argv);
    platform::afterApplication();
    app.setApplicationName(QStringLiteral("openghost-native"));
    app.setApplicationDisplayName(QStringLiteral("OpenGhost Native"));
    app.setApplicationVersion(QStringLiteral("0.1"));
    app.setWindowIcon(QIcon(QStringLiteral(":/openghost.png")));
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Standalone native frontend. No Rust backend or transport."));
    parser.addOption(
        {QStringLiteral("fake-backend"),
         QStringLiteral("Use the in-memory fake backend (no model, tools or credentials).")});
    parser.addOption({QStringLiteral("pi"),
                      QStringLiteral("Proof of concept: chat through `pi --mode rpc --no-session`.")});
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
    QString dataPath;
    QString browserPath, browserStorage, browserDownloads;
#ifdef OPENGHOST_SMOKE_TEST
    QTemporaryDir testSettings;
    if (parser.isSet(QStringLiteral("smoke-test")) ||
        parser.isSet(QStringLiteral("parity-manifest"))) {
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
        if (!parser.isSet(QStringLiteral("fake-backend")) && !parser.isSet(QStringLiteral("pi")))
            dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                       QStringLiteral("/library");
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
        !parser.isSet(QStringLiteral("parity-manifest")))
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
        backend = std::make_unique<openghost::PiBackend>();
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
    WindowController controller(backend.get(), preferencesPath, dataPath, browser.get());
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
#endif
    return app.exec();
}
