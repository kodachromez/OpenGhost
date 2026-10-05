#include "appearance.h"
#include "platform/platform.h"
#include "window.h"

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
    app.setApplicationDisplayName(QStringLiteral("OpenGhost Native (UI shell)"));
    app.setApplicationVersion(QStringLiteral("0.1"));
    app.setWindowIcon(QIcon(QStringLiteral(":/openghost.png")));
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Standalone native frontend; no backend connected."));
    parser.addHelpOption();
    parser.addVersionOption();
#ifdef OPENGHOST_SMOKE_TEST
    parser.addOption(
        {QStringLiteral("smoke-test"), QStringLiteral("Run the isolated UI smoke test and exit.")});
#endif
    parser.process(app);
    selectControlsStyle();
    QString appearancePath;
#ifdef OPENGHOST_SMOKE_TEST
    QTemporaryDir testSettings;
    if (parser.isSet(QStringLiteral("smoke-test"))) {
        if (!testSettings.isValid())
            return 1;
        appearancePath = testSettings.path() + QStringLiteral("/appearance.json");
    } else
#endif
        appearancePath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                         QStringLiteral("/appearance.json");
    keepAppearance(appearancePath);
    WindowController controller;
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
