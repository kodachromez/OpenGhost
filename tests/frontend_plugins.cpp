#include "frontend/example_plugin.h"
#include "frontend/frontend_plugins.h"
#include "frontend/preferences.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace openghost;

namespace
{
// A plugin whose hooks are scripted by the test.
class Probe final : public FrontendPlugin
{
  public:
    explicit Probe(QString id, bool byDefault = false) : m_id(std::move(id)), m_default(byDefault)
    {
    }
    FrontendPluginInfo info() const override { return {m_id, m_id, {}, m_default}; }
    void enable(FrontendPluginContext &context) override
    {
        ++enables;
        context.subscribe(events::ChatChanged, [this](const FrontendEvent &event) {
            received.append(event.name);
            if (onEvent)
                onEvent();
        });
        context.decorateRows([](const ChatRowView &row) {
            return row.role == "assistant" ? QVariantMap{{"tag", row.key}} : QVariantMap{};
        });
        context.setVisible("browser", visible);
    }
    void disable() override { ++disables; }
    QString m_id;
    bool m_default, visible = true;
    int enables = 0, disables = 0;
    QStringList received;
    std::function<void()> onEvent;
};
FrontendEvent changed() { return {events::ChatChanged, "s1", {}}; }
// A plugin with an option, offered under Appearance → Chat Settings.
class Optioned final : public FrontendPlugin
{
  public:
    FrontendPluginInfo info() const override
    {
        FrontendPluginInfo info{"o", "Optioned", {}, true};
        info.placement = "chat";
        info.options.insert("shut", true);
        return info;
    }
    void enable(FrontendPluginContext &context) override
    {
        m_context = &context;
        seen = context.option("shut");
    }
    void disable() override { m_context = nullptr; }
    void optionChanged(const QString &key) override
    {
        changes.append(key);
        seen = m_context->option(key);
    }
    FrontendPluginContext *m_context = nullptr;
    bool seen = false;
    QStringList changes;
};
} // namespace

class FrontendPluginsTest final : public QObject
{
    Q_OBJECT
  private slots:
    void emptyRegistryIsInert()
    {
        FrontendPlugins plugins(nullptr);
        QCOMPARE(plugins.count(), 0);
        QVERIFY(plugins.entries().isEmpty());
        QVERIFY(plugins.visibility().isEmpty());
        QVERIFY(plugins.visible("browser", true) && !plugins.visible("browser", false));
        QVERIFY(!plugins.decorating());
        QVERIFY(plugins.decorate({"s1", "k", "assistant", "hi", {}}).isEmpty());
        plugins.publish(changed());
        QCOMPARE(plugins.hooks(), 0);
        QVERIFY(!plugins.setEnabled("missing", true));
        QVERIFY(!plugins.remove("missing"));
    }
    void registrationAndIdentity()
    {
        FrontendPlugins plugins(nullptr);
        QSignalSpy count(&plugins, &FrontendPlugins::countChanged);
        QVERIFY(!plugins.add(nullptr));
        QVERIFY(!plugins.add(std::make_unique<Probe>(QString())));
        QVERIFY(plugins.add(std::make_unique<Probe>("a")));
        QVERIFY(!plugins.add(std::make_unique<Probe>("a")));
        QVERIFY(plugins.add(std::make_unique<Probe>("b", true)));
        QCOMPARE(count.count(), 2);
        const auto entries = plugins.entries();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[0].toMap().value("pluginId"), "a");
        QCOMPARE(entries[0].toMap().value("enabled"), false);
        QCOMPARE(entries[1].toMap().value("enabled"), true); // its default
        QVERIFY(plugins.remove("b"));
        QCOMPARE(plugins.count(), 1);
        QCOMPARE(plugins.hooks(), 0);
    }
    void enableDisableIsLiveAndClean()
    {
        FrontendPlugins plugins(nullptr);
        auto owned = std::make_unique<Probe>("a");
        auto *probe = owned.get();
        plugins.add(std::move(owned));
        QSignalSpy rows(&plugins, &FrontendPlugins::rowsChanged);
        QSignalSpy shown(&plugins, &FrontendPlugins::visibilityChanged);
        plugins.publish(changed());
        QVERIFY(probe->received.isEmpty());

        QVERIFY(plugins.setEnabled("a", true));
        QVERIFY(plugins.enabled("a") && probe->enables == 1);
        QCOMPARE(plugins.hooks(), 3); // event, decorator, visibility
        QVERIFY(rows.count() >= 1 && shown.count() == 1);
        plugins.publish(changed());
        plugins.publish({events::ReplyDelta, "s1", {}}); // not subscribed
        QCOMPARE(probe->received, QStringList{events::ChatChanged});
        QVERIFY(plugins.decorating());
        QCOMPARE(plugins.decorate({"s1", "k1", "assistant", "hi", {}}),
                 (QVariantMap{{"a", QVariantMap{{"tag", "k1"}}}}));
        QVERIFY(plugins.decorate({"s1", "k2", "user", "hi", {}}).isEmpty());
        QCOMPARE(plugins.visibility(), (QVariantMap{{"browser", true}}));
        QVERIFY(plugins.setEnabled("a", true)); // already on: no second enable
        QCOMPARE(probe->enables, 1);

        rows.clear();
        shown.clear();
        QVERIFY(plugins.setEnabled("a", false));
        QVERIFY(!plugins.enabled("a") && probe->disables == 1);
        QCOMPARE(plugins.hooks(), 0);
        QCOMPARE(rows.count(), 1);
        QCOMPARE(shown.count(), 1);
        QVERIFY(!plugins.decorating());
        QVERIFY(plugins.visibility().isEmpty());
        plugins.publish(changed());
        QCOMPARE(probe->received.size(), 1);

        QVERIFY(plugins.setEnabled("a", true)); // again, with fresh hooks
        QCOMPARE(plugins.hooks(), 3);
        plugins.publish(changed());
        QCOMPARE(probe->received.size(), 2);
    }
    void removeWhileEnabledCleansUp()
    {
        FrontendPlugins plugins(nullptr);
        auto owned = std::make_unique<Probe>("a", true);
        auto *probe = owned.get();
        plugins.add(std::move(owned));
        QCOMPARE(plugins.hooks(), 3);
        QCOMPARE(probe->disables, 0);
        QSignalSpy shown(&plugins, &FrontendPlugins::visibilityChanged);
        QVERIFY(plugins.remove("a"));
        QCOMPARE(plugins.hooks(), 0);
        QCOMPARE(shown.count(), 1);
        QVERIFY(plugins.visibility().isEmpty());
    }
    void hiddenWinsOverShown()
    {
        FrontendPlugins plugins(nullptr);
        auto hide = std::make_unique<Probe>("b", true);
        hide->visible = false;
        plugins.add(std::make_unique<Probe>("a", true));
        plugins.add(std::move(hide));
        QVERIFY(!plugins.visible("browser", true));
        plugins.setEnabled("b", false);
        QVERIFY(plugins.visible("browser", false));
    }
    void handlerMayTurnPluginsOff()
    {
        FrontendPlugins plugins(nullptr);
        auto first = std::make_unique<Probe>("a", true);
        auto second = std::make_unique<Probe>("b", true);
        auto *a = first.get();
        auto *b = second.get();
        plugins.add(std::move(first));
        plugins.add(std::move(second));
        a->onEvent = [&] { plugins.setEnabled("b", false); };
        plugins.publish(changed());
        QCOMPARE(a->received.size(), 1);
        QVERIFY(b->received.isEmpty()); // turned off before its turn: not called
        b->onEvent = [&] { plugins.setEnabled("b", false); };
        plugins.setEnabled("b", true);
        a->onEvent = nullptr;
        plugins.publish(changed()); // b turns itself off inside its handler
        QCOMPARE(b->received.size(), 1);
        QVERIFY(!plugins.enabled("b"));
        QCOMPARE(plugins.hooks(), 3);
    }
    void examplePlugin()
    {
        FrontendPlugins plugins(nullptr);
        auto owned = std::make_unique<ExamplePlugin>();
        auto *example = owned.get();
        QVERIFY(plugins.add(std::move(owned)));
        QVERIFY(!plugins.enabled(ExamplePlugin::Id)); // off by default
        QSignalSpy entries(&plugins, &FrontendPlugins::entriesChanged);
        plugins.setEnabled(ExamplePlugin::Id, true);
        plugins.publish(changed());
        plugins.publish(changed());
        QVERIFY(entries.count() >= 3);
        QCOMPARE(plugins.entries()[0].toMap().value("status"), "Active · 2 chat updates seen");
        QCOMPARE(plugins.visibility(), (QVariantMap{{"example", true}}));
        QCOMPARE(plugins.decorate({"s1", "k", "user", "hi", {}}).value(ExamplePlugin::Id),
                 (QVariantMap{{"seen", true}}));
        plugins.setEnabled(ExamplePlugin::Id, false);
        QVERIFY(!example->active());
        QCOMPARE(plugins.hooks(), 0);
        QCOMPARE(plugins.entries()[0].toMap().value("status"), QString());
    }
    void choicesAreSavedAndRestored()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("preferences.json");
        {
            PreferencesStore store(path);
            FrontendPlugins plugins(&store);
            plugins.add(std::make_unique<Probe>("a"));
            plugins.add(std::make_unique<Probe>("b", true));
            QVERIFY(!QFile::exists(path)); // registering saves nothing
            QVERIFY(plugins.setEnabled("a", true));
            QVERIFY(plugins.setEnabled("b", false));
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("plugins").toObject(),
                 (QJsonObject{{"a", true}, {"b", false}}));
        // A restart: the saved choice wins over the default.
        PreferencesStore store(path);
        QVERIFY(store.error().isEmpty());
        FrontendPlugins plugins(&store);
        plugins.add(std::make_unique<Probe>("a"));
        plugins.add(std::make_unique<Probe>("b", true));
        QVERIFY(plugins.enabled("a") && !plugins.enabled("b"));
        // Choices for plugins not registered now survive other saves.
        plugins.remove("b");
        QVERIFY(plugins.setEnabled("a", false));
        QCOMPARE(store.value().frontendPlugins, (QMap<QString, bool>{{"a", false}, {"b", false}}));
    }
    void optionsAreSavedAndToldOnlyWhileOn()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("preferences.json");
        {
            PreferencesStore store(path);
            FrontendPlugins plugins(&store);
            auto owned = std::make_unique<Optioned>();
            auto *plugin = owned.get();
            plugins.add(std::move(owned));
            QVERIFY(plugin->seen && plugins.option("o", "shut"));
            const auto entry = plugins.entries().value(0).toMap();
            QCOMPARE(entry.value("placement").toString(), QStringLiteral("chat"));
            QCOMPARE(entry.value("options").toMap(), (QVariantMap{{"shut", true}}));
            QVERIFY(!plugins.setOption("o", "other", false)); // undeclared
            QVERIFY(!plugins.setOption("x", "shut", false));  // unknown plugin
            QVERIFY(!plugins.option("o", "other"));
            QVERIFY(!QFile::exists(path));
            QSignalSpy entries(&plugins, &FrontendPlugins::entriesChanged);
            QVERIFY(plugins.setOption("o", "shut", false));
            QCOMPARE(plugin->changes, QStringList{"shut"});
            QVERIFY(!plugin->seen && entries.count() == 1);
            QVERIFY(!plugins.entries().value(0).toMap().value("options").toMap().value("shut").toBool());
            QVERIFY(plugins.setOption("o", "shut", false)); // unchanged: not told again
            QCOMPARE(plugin->changes.size(), 1);
            // Off: kept and settable, but the plugin is not told.
            QVERIFY(plugins.setEnabled("o", false));
            QVERIFY(plugins.setOption("o", "shut", true));
            QCOMPARE(plugin->changes.size(), 1);
            QVERIFY(plugins.setOption("o", "shut", false));
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(saved.value("plugins").toObject(), (QJsonObject{{"o", false}}));
        QCOMPARE(saved.value("pluginOptions").toObject(),
                 (QJsonObject{{"o", QJsonObject{{"shut", false}}}}));
        file.close();
        // A restart reads both back; the plugin starts with its saved option.
        {
            PreferencesStore store(path);
            QVERIFY(store.error().isEmpty());
            FrontendPlugins plugins(&store);
            auto owned = std::make_unique<Optioned>();
            auto *plugin = owned.get();
            plugins.add(std::move(owned));
            QVERIFY(!plugins.enabled("o") && !plugins.option("o", "shut"));
            QVERIFY(plugins.setEnabled("o", true) && !plugin->seen);
        }
        // Malformed options are refused, never guessed.
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(R"({"version":1,"provider":"","model":"","thinking":null,"mode":"ask",)"
                   R"("instructions":"","pluginOptions":{"o":{"shut":"no"}}})");
        file.close();
        PreferencesStore invalid(path);
        QVERIFY(!invalid.error().isEmpty());
    }
    void unsavedChoiceChangesNothing()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("preferences.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"version\": 99}");
        file.close();
        PreferencesStore store(path);
        QVERIFY(!store.error().isEmpty());
        FrontendPlugins plugins(&store);
        plugins.add(std::make_unique<Probe>("a"));
        QSignalSpy failed(&store, &PreferencesStore::saveFailed);
        QVERIFY(!plugins.setEnabled("a", true));
        QCOMPARE(failed.count(), 1);
        QVERIFY(!plugins.enabled("a"));
        QCOMPARE(plugins.hooks(), 0);
    }
    void preferencesWithoutPluginsAreUnchanged()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("preferences.json");
        PreferencesStore store(path);
        QVERIFY(store.save(Preferences{}));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!QJsonDocument::fromJson(file.readAll()).object().contains("plugins"));
        file.close();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(R"({"version":1,"provider":"","model":"","thinking":null,"mode":"ask",)"
                   R"("instructions":"","plugins":{"a":"yes"}})");
        file.close();
        PreferencesStore invalid(path);
        QVERIFY(!invalid.error().isEmpty()); // malformed choices are refused, never guessed
    }
};

QTEST_GUILESS_MAIN(FrontendPluginsTest)
#include "frontend_plugins.moc"
