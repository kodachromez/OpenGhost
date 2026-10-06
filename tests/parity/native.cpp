// Test-only presentation injection. No backend, wire, credentials or host dialogs.
#include "../video_fixture.h"
#include "diagram.h"
#include "medialoader.h"
#include "rich.h"
#include "window.h"
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtTest>

namespace
{
QQuickItem *visual(QQuickItem *item, const QString &name)
{
    if (item->objectName() == name)
        return item;
    for (auto *child : item->childItems())
        if (auto *found = visual(child, name))
            return found;
    return nullptr;
}
} // namespace
int parityTest(QQmlApplicationEngine &engine, WindowController &controller, const QString &manifest,
               const QString &output)
{
    if (QGuiApplication::platformName() != "offscreen") {
        qCritical("Parity refuses a visible platform");
        return 1;
    }
    QFile file(manifest);
    if (!file.open(QIODevice::ReadOnly))
        return 1;
    const auto fixtures = QJsonDocument::fromJson(file.readAll()).object()["fixtures"].toArray();
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (!window)
        return 1;
    auto run = [&](const QString &source) {
        QQmlExpression expression(qmlContext(window), window, source);
        auto result = expression.evaluate();
        if (expression.hasError())
            qFatal("Parity QML: %s", qPrintable(expression.error().toString()));
        return result;
    };
    QTest::qWait(200);
    // These are presentation snapshots, not semantic backend fixtures. An
    // explicit picker choice must not ask the disconnected service to replace
    // the injected catalog while a screenshot is settling.
    QObject::disconnect(controller.settings(), &Settings::chosen, nullptr, nullptr);
    QObject::disconnect(controller.sessions(), &SessionModel::queryChanged, &controller, nullptr);
    qInfo() << "Parity renderer" << window->rendererInterface()->graphicsApi() << "font"
            << QFontInfo(QGuiApplication::font()).family() << "DPR" << window->devicePixelRatio();
    if (QFontInfo(QGuiApplication::font()).family() != "Noto Sans" ||
        QFontInfo(QFont("monospace")).family() != "Noto Sans Mono")
        qFatal("Parity requires Noto Sans and monospace resolved to Noto Sans Mono");
    if (window->rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL)
        qFatal("Parity requires offscreen OpenGL (including shaders), not a software scenegraph "
               "fallback");
    QDir().mkpath(output);
    // Replace only the settings page's presentation ledger, never a service.
    // Each fixture gets a fresh in-memory ledger, preventing cross-case counts.
    std::unique_ptr<openghost::UsageStore> usage;
    for (int pass = 0; pass < 2; ++pass) {
        for (auto value : fixtures) {
            const auto f = value.toObject();
            if (f["manual"].toBool())
                continue;
            const QString id = f["id"].toString();
            window->setWidth(f["width"].toInt(1280));
            window->setHeight(f["height"].toInt(840));
            QTest::mouseMove(window, QPoint(window->width() - 2, 2));
            run("splashLoader.active=false; Theme.reducedMotion=true; Selection.clear();");
            run("thinkingChoice.popup.close();");
            QTest::qWait(30);
            Theme::setSystemDark(f["systemDark"].toBool(true));
            Theme::choose(f["theme"].toString("dark"));
            run("settingsDialog.close(); modelStage.close(); modeDock.close(false); "
                "approvalModel.clear(); composerFiles.clear(); "
                "window.notice = ''; window.working = false; window.sidebarOpen = true; "
                "sidebar.renamingId=''; sidebar.confirmingId=''; "
                "composer.text = ''; transcript.follow = false; window.approvalDetails = false;");
            if (auto *search = visual(window->contentItem(), "search")) {
                search->setProperty("text", "");
                search->setProperty("focus", false);
            }
            auto previousUsage = std::move(usage);
            usage = std::make_unique<openghost::UsageStore>();
            auto *dialog = window->findChild<QObject *>("settingsDialog");
            for (auto *object : dialog->findChildren<QObject *>()) {
                if (QByteArray(object->metaObject()->className()).startsWith("UsagePage_"))
                    object->setProperty(
                        "frontend",
                        QVariantMap{{"usage", QVariant::fromValue(usage.get())},
                                    {"settings", QVariant::fromValue(controller.settings())}});
            }
            for (auto c : f["usageCounts"].toArray()) {
                auto value = c.toObject();
                openghost::Usage count;
                count.provider = value["provider"].toString();
                count.model = value["model"].toString();
                count.input = value["input"].toDouble();
                count.output = value["output"].toDouble();
                count.cached = value["cached"].toDouble();
                count.requests = value["requests"].toDouble();
                usage->record(count);
            }
            if (f["usage"].toBool()) {
                openghost::Usage count;
                count.provider = "fixture";
                count.model = "example";
                count.modelName = "Example";
                count.input = 1200;
                count.output = 340;
                count.cached = 400;
                count.requests = 2;
                usage->record(count);
            }
            controller.sessions()->apply({});
            controller.transcript()->reset({});
            // A reply's pictures come only from the fixture's bytes, for the
            // addresses it serves; every other load fails. Never the network.
            {
                const QByteArray bytes =
                    QByteArray::fromBase64(f["imageData"].toString().toLatin1());
                QStringList served{"https://parity.invalid/image.png"};
                if (f.contains("mediaUrls")) {
                    served.clear();
                    for (auto url : f["mediaUrls"].toArray())
                        served << url.toString();
                }
                const auto images = f["mediaImages"].toObject();
                const auto pending = f["mediaPending"].toArray();
                MediaLoader::instance()->setFetch(
                    [bytes, served, images, pending](const QString &url, bool,
                                                     const MediaLoader::Done &done) {
                        if (pending.contains(url))
                            return;
                        done(images.contains(url)
                                 ? QByteArray::fromBase64(images[url].toString().toLatin1())
                             : served.contains(url) ? bytes
                                                    : QByteArray());
                    });
                auto host = std::make_shared<FixtureVideoInfo>();
                const auto values = f["videoInfo"].toObject();
                for (auto it = values.begin(); it != values.end(); ++it) {
                    const auto info = it.value().toObject();
                    host->values[it.key()] = {info["title"].toString(), info["by"].toString()};
                }
                host->hold = f["videoInfoPending"].toBool();
                VideoTitles::instance()->setService(host);
            }
            QTest::qWait(30);
            QVector<Entry> rows;
            for (auto r : f["rows"].toArray()) {
                auto row = r.toObject();
                Entry e;
                const auto role = row["role"].toString();
                e.kind = role == "user"        ? Entry::User
                         : role == "assistant" ? Entry::Assistant
                                               : Entry::Note;
                e.key = row["key"].toString("parity");
                e.text = row["text"].toString();
                e.state = row["state"].toString("done");
                e.copyable = row["copyable"].toBool(role == "assistant");
                e.preview = row["preview"].toString();
                e.metrics = row["metrics"].toString();
                e.started = 1000000;
                e.completed = 1002400;
                e.join = row["joined"].toBool() ? Entry::Joined : Entry::Apart;
                for (auto a : row["attachments"].toArray()) {
                    auto aObj = a.toObject();
                    e.attachments.append({aObj["name"].toString(), aObj["mime"].toString(),
                                          aObj["size"].toInteger()});
                }
                rows.append(e);
            }
            controller.transcript()->reset(rows);
            Account account;
            account.providersLoaded = true;
            account.providersError = "No backend is connected.";
            for (auto m : f["models"].toArray()) {
                auto model = m.toObject();
                ModelInfo info;
                info.provider = model["provider"].toString();
                info.id = model["id"].toString();
                info.name = model["name"].toString();
                info.available = true;
                info.imageInput = true;
                info.levels = {"off", "low", "medium", "high"};
                info.defaultThinking = "medium";
                account.models.append(info);
            }
            if (!account.models.isEmpty())
                account.defaults = {account.models[0].provider, account.models[0].id, "medium",
                                    false};
            if (f.contains("providers")) {
                account.providersError.clear();
                account.providers = f["providers"].toArray().toVariantList();
                account.login = f["login"].toObject().toVariantMap();
            }
            controller.settings()->apply(account);
            controller.settings()->use(account.defaults);
            QVector<Session> sessions;
            for (auto s : f["sessions"].toArray()) {
                auto row = s.toObject();
                sessions.append({row["id"].toString(), row["title"].toString(),
                                 row["folder"].toString(), QDateTime::currentMSecsSinceEpoch(),
                                 QDateTime::currentMSecsSinceEpoch(), row["pinned"].toBool()});
            }
            controller.sessions()->apply(sessions);
            run("composer.text = " +
                QString::fromUtf8(QJsonDocument(QJsonArray{f["draft"].toString()})
                                      .toJson(QJsonDocument::Compact)) +
                "[0];");
            if (f.contains("cards")) {
                const auto json = QString::fromUtf8(
                    QJsonDocument(f["cards"].toArray()).toJson(QJsonDocument::Compact));
                run("{ const cards=" + json +
                    "; for(let i=0;i<cards.length;i++) "
                    "composerFiles.append({token:'fixture'+i,name:cards[i].name,size:cards[i].size,"
                    "picture:false}); }");
            }
            if (f.contains("approval")) {
                const auto json = QString::fromUtf8(
                    QJsonDocument(f["approval"].toObject()).toJson(QJsonDocument::Compact));
                run("approvalModel.append({requestId:'fixture', approval:{requestId:'fixture', "
                    "card:" +
                    json + ",answered:false},leaving:false});");
                window->setProperty("approvalDetails", f["details"].toBool());
            }
            if (f.contains("diagram") && !diagram::render(f["diagram"].toString(), {}, {}).ok)
                qFatal("Native diagram fixture did not compile: %s", qPrintable(id));
            if (f.contains("native"))
                run(f["native"].toString());
            for (auto p : f["properties"].toArray()) {
                const auto property = p.toObject();
                auto *item = visual(window->contentItem(), property["object"].toString());
                if (!item || !item->setProperty(qPrintable(property["name"].toString()),
                                                property["value"].toVariant()))
                    qFatal("Missing fixture property target: %s", qPrintable(id));
            }
            if (f["motion"].toBool()) {
                run("Theme.reducedMotion=false;");
                if (f["splash"].toBool())
                    run("splashLoader.active=true;");
            }
            QTest::qWait(f["wait"].toInt(450));
            run("transcript.follow = false; transcript.positionViewAtBeginning(); composer.focus = "
                "false;");
            if (f.contains("nativeAfter"))
                run(f["nativeAfter"].toString());
            // Real pointer clicks on every visible control of that name (a
            // picture's consent plate), as the reference clicks its own. A
            // click may rebuild the others, so each round finds them afresh.
            if (f.contains("nativeClick")) {
                const QString name = f["nativeClick"].toString();
                const auto first = [&]() -> QQuickItem * {
                    QQuickItem *found = nullptr;
                    std::function<void(QQuickItem *)> look = [&](QQuickItem *item) {
                        if (!found && item->objectName() == name && item->isVisible())
                            found = item;
                        for (auto *child : item->childItems())
                            look(child);
                    };
                    look(window->contentItem());
                    return found;
                };
                int clicks = 0;
                for (QQuickItem *target = first(); target && clicks < 16; target = first()) {
                    const QPointF at =
                        target->mapToScene(QPointF(target->width() / 2, target->height() / 2));
                    QTest::mouseClick(window, Qt::LeftButton, {}, at.toPoint());
                    ++clicks;
                    QTest::qWait(150);
                }
                if (!clicks || first())
                    qFatal("Fixture click targets did not all respond: %s", qPrintable(id));
                // The reference clicks without a pointer: nothing stays hovered.
                QTest::mouseMove(window, QPoint(window->width() - 2, window->height() - 2));
                QTest::qWait(250);
            }
            for (auto p : f["propertiesAfter"].toArray()) {
                const auto property = p.toObject();
                auto *item = visual(window->contentItem(), property["object"].toString());
                if (!item || !item->setProperty(qPrintable(property["name"].toString()),
                                                property["value"].toVariant()))
                    qFatal("Missing settled fixture property: %s", qPrintable(id));
            }
            if (f.contains("invoke")) {
                auto action = f["invoke"].toObject();
                auto *item = visual(window->contentItem(), action["object"].toString());
                if (!item ||
                    !QMetaObject::invokeMethod(item, qPrintable(action["method"].toString())))
                    qFatal("Missing fixture action target: %s", qPrintable(id));
            }
            if (f.contains("mediaIndex")) {
                auto *stack = visual(window->contentItem(), "mediaStack");
                if (!stack || !QMetaObject::invokeMethod(stack, "go",
                                                         Q_ARG(QVariant, f["mediaIndex"].toInt())))
                    qFatal("Missing media stack for navigation");
            }
            if (f.contains("mediaHover")) {
                auto *target = visual(window->contentItem(), f["mediaHover"].toString());
                if (!target)
                    qFatal("Missing media hover target");
                QTest::mouseMove(
                    window, target->mapToScene(QPointF(target->width() / 2, target->height() / 2))
                                .toPoint());
            }
            QTest::qWait(f["afterWait"].toInt(100));
            if (f.contains("imageData") && f["mediaUrls"].toArray({QJsonValue("x")}).size() > 0) {
                bool shown = false;
                std::function<void(QQuickItem *)> find = [&](QQuickItem *item) {
                    if ((item->objectName() == "mediaImage" ||
                         item->objectName() == "mediaVideoImage") &&
                        item->isVisible() && item->property("status").toInt() == 1 /* Ready */ &&
                        item->property("sourceSize").toSize().width() == 240)
                        shown = true;
                    for (auto *child : item->childItems())
                        find(child);
                };
                find(window->contentItem());
                if (!shown && !f["skipImageCheck"].toBool())
                    qFatal("Synthetic media image did not load: %s", qPrintable(id));
            }
            if (dialog && dialog->property("visible").toBool()) {
                const QStringList pages{"general", "providers", "usage", "appearance"};
                auto *glide = visual(window->contentItem(), "settingsGlide");
                const int index = pages.indexOf(dialog->property("page").toString());
                if (!glide || index < 0 || qAbs(glide->y() - index * 38) > 0.01)
                    qFatal("Settings fixture did not settle its actual navigation: %s",
                           qPrintable(id));
            }
            for (const auto &entry : f["nativeValues"].toArray()) {
                const auto check = entry.toObject();
                auto *item = visual(window->contentItem(), check["object"].toString());
                if (!item || item->property(qPrintable(check["property"].toString())) !=
                                 check["value"].toVariant())
                    qFatal("Media fixture %s property assertion failed", qPrintable(id));
            }
            const auto expectedText = f["nativeText"].toObject();
            for (auto it = expectedText.begin(); it != expectedText.end(); ++it) {
                auto *text = visual(window->contentItem(), it.key());
                if (!text || text->property("text").toString() != it.value().toString())
                    qFatal("Media fixture %s has incorrect %s", qPrintable(id),
                           qPrintable(it.key()));
            }
            const auto image = window->grabWindow();
            if (image.isNull() || !image.save(output + "/" + id + (pass ? ".repeat.png" : ".png")))
                qFatal("Could not capture %s", qPrintable(id));
            auto *effort = visual(window->contentItem(), "thinkingChoice");
            auto *effortPanel = effort ? effort->property("popup").value<QObject *>() : nullptr;
            QJsonObject geometry{
                {"model", controller.settings()->model()},
                {"levels", QJsonArray::fromStringList(controller.settings()->levels())},
                {"effortPanel", effortPanel && effortPanel->property("visible").toBool()}};
            if (id.startsWith("effort-") && controller.settings()->levels().size() != 4)
                qFatal("Fixture catalog was lost while opening effort");
            if (!id.startsWith("effort-") && effortPanel &&
                effortPanel->property("visible").toBool())
                qFatal("Previous fixture left its effort popup open");
            for (const QString name : {"sidebar", "composerFrame", "transcript", "welcome"}) {
                if (auto *item = visual(window->contentItem(), name)) {
                    auto point = item->mapToScene(QPointF());
                    geometry[name] =
                        QJsonArray{point.x(), point.y(), item->width(), item->height()};
                }
            }
            QJsonArray mediaRects;
            QHash<QString, int> counts;
            std::function<void(QQuickItem *)> mediaGeometry = [&](QQuickItem *item) {
                if (item->isVisible()) {
                    counts[item->objectName()]++;
                    if (item->objectName() == "mediaBlock") {
                        const auto at = item->mapToScene(QPointF());
                        mediaRects.append(
                            QJsonArray{at.x(), at.y(), item->width(), item->height()});
                    }
                }
                for (auto *child : item->childItems())
                    mediaGeometry(child);
            };
            mediaGeometry(window->contentItem());
            geometry["media"] = mediaRects;
            const auto expectedCounts = f["nativeCounts"].toObject();
            for (auto it = expectedCounts.begin(); it != expectedCounts.end(); ++it)
                if (counts.value(it.key()) != it.value().toInt())
                    qFatal("Media fixture %s expected %s=%d, got %d", qPrintable(id),
                           qPrintable(it.key()), it.value().toInt(), counts.value(it.key()));
            QFile meta(output + "/" + id + (pass ? ".repeat.json" : ".json"));
            if (!meta.open(QIODevice::WriteOnly))
                return 1;
            meta.write(QJsonDocument(geometry).toJson());
            qInfo().noquote() << "captured" << pass << id;
        }
    }
    VideoTitles::instance()->setService({});
    MediaLoader::instance()->setFetch({});
    // The window outlives this function. Its last injected ledger must too.
    if (usage) {
        usage->setParent(&controller);
        usage.release();
    }
    return engine.property("smokeWarnings").toBool() ? 1 : 0;
}
