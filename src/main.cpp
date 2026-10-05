#include "appearance.h"
#include "backend/fake_backend.h"
#include "platform/platform.h"
#include "window.h"
#include <memory>

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QStandardPaths>
#include <QTemporaryDir>

#ifdef OPENGHOST_SMOKE_TEST
int smokeTest(QQmlApplicationEngine &engine, WindowController &controller);
#endif

int main(int argc, char *argv[])
{
    platform::beforeApplication();
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
    parser.addHelpOption();
    parser.addVersionOption();
#ifdef OPENGHOST_SMOKE_TEST
    parser.addOption(
        {QStringLiteral("smoke-test"), QStringLiteral("Run the isolated UI smoke test and exit.")});
#endif
    parser.process(app);
    selectControlsStyle();
    QString appearancePath;
    QString preferencesPath;
    QString dataPath;
#ifdef OPENGHOST_SMOKE_TEST
    QTemporaryDir testSettings;
    if (parser.isSet(QStringLiteral("smoke-test"))) {
        if (!testSettings.isValid())
            return 1;
        appearancePath = testSettings.path() + QStringLiteral("/appearance.json");
        preferencesPath = testSettings.path() + QStringLiteral("/preferences.json");
        dataPath = testSettings.path() + QStringLiteral("/library");
    } else
#endif
    {
        const QString config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        appearancePath = config + QStringLiteral("/appearance.json");
        preferencesPath = config + QStringLiteral("/preferences.json");
        // Display caches, recovery markers, chat index and usage ledger: local
        // frontend state, never backend history or credentials. The fake's
        // sessions die with the process, so its chats never enter the profile.
        if (!parser.isSet(QStringLiteral("fake-backend")))
            dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                       QStringLiteral("/library");
    }
    keepAppearance(appearancePath);
    std::unique_ptr<openghost::Backend> backend;
    if (parser.isSet(QStringLiteral("fake-backend")))
        backend = std::make_unique<openghost::FakeBackend>();
    WindowController controller(backend.get(), preferencesPath, dataPath);
    QObject::connect(&controller, &WindowController::closeRequested, &app, &QCoreApplication::quit);
    QQmlApplicationEngine engine;
#ifdef OPENGHOST_SMOKE_TEST
    QObject::connect(
        &engine, &QQmlApplicationEngine::warnings, &engine,
        [&engine](const QList<QQmlError> &) { engine.setProperty("smokeWarnings", true); });
#endif
    engine.setNetworkAccessManagerFactory(denyNetwork());
    engine.setInitialProperties({{"frontend", QVariant::fromValue(&controller)}});
    engine.load(QUrl(QStringLiteral("qrc:/OpenGhost/Ui/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
#ifdef OPENGHOST_SMOKE_TEST
    if (parser.isSet(QStringLiteral("smoke-test")))
        return smokeTest(engine, controller);
#endif
    return app.exec();
}
