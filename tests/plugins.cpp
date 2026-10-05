#include "frontend/chat_service.h"
#include "plugin_fixture.h"
#include <QtTest>

using namespace openghost;
class PluginsTest final : public QObject
{
    Q_OBJECT
  private slots:
    void initialListAndCapability()
    {
        PreferencesStore prefs({});
        ChatService offline(nullptr, &prefs);
        offline.initialize();
        QVERIFY(!offline.plugins()->supported());
        PluginBackend backend;
        backend.runtimePlugins = false;
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QVERIFY(!chat.plugins()->supported());
        QCOMPARE(backend.count<PluginsList>(), 0);
        backend.publish(plugin());
        chat.plugins()->setEnabled("vendor.future", true);
        chat.plugins()->refresh();
        QVERIFY(chat.plugins()->entries().isEmpty());
        QCOMPARE(backend.count<EnablePlugin>(), 0);
        QCOMPARE(backend.count<PluginsList>(), 0);

        backend.disconnectBackend();
        backend.runtimePlugins = true;
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QCOMPARE(backend.count<PluginsList>(), 1);
        QVERIFY(chat.plugins()->loading());
        QVERIFY(chat.plugins()->entries().isEmpty());
        QVector<Plugin> all;
        for (const auto &id :
             {"shell", "read", "edit", "write", "attachments", "web", "vendor.future/v3"})
            all.append(plugin(id, "enabled"));
        all.last().name = "<b>A new third-party plugin</b>";
        backend.finish(backend.next<PluginsList>(), listed(all));
        QVERIFY(chat.plugins()->loaded());
        QCOMPARE(chat.plugins()->entries().size(), all.size());
        QCOMPARE(chat.plugins()->entries().last().snapshot.id, "vendor.future/v3");
        QCOMPARE(chat.plugins()->entries().last().snapshot.name, all.last().name);
        QVERIFY(!chat.plugins()->loading());
    }
    void togglesAreAuthoritativeAndIndependentOfChat()
    {
        PluginBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        auto *plugins = chat.plugins();
        backend.finish(backend.next<PluginsList>(), listed({plugin()}));
        QVERIFY(chat.send("Keep chatting") != 0);
        QTRY_VERIFY(!chat.pending());
        QVERIFY(chat.busy());
        const auto catalogRequests = backend.count<ModelsList>();
        plugins->setEnabled("vendor.future", true);
        QCOMPARE(backend.count<EnablePlugin>(), 1);
        QCOMPARE(backend.command<EnablePlugin>().pluginId, "vendor.future");
        QVERIFY(plugins->entries().first().pending);
        QVERIFY(!plugins->entries().first().snapshot.enabled);
        QVERIFY(!plugins->entries().first().canToggle());
        plugins->setEnabled("vendor.future", true);
        plugins->setEnabled("vendor.future", false);
        QCOMPARE(backend.count<EnablePlugin>(), 1);
        QCOMPARE(backend.count<DisablePlugin>(), 0);
        backend.finish(backend.next<EnablePlugin>(), updated(plugin("vendor.future", "enabled")));
        QVERIFY(plugins->entries().first().snapshot.enabled);
        QVERIFY(plugins->entries().first().canToggle());
        QVERIFY(chat.busy());
        QVERIFY(!chat.pending());
        plugins->setEnabled("vendor.future", false);
        QCOMPARE(backend.command<DisablePlugin>().pluginId, "vendor.future");
        QVERIFY(plugins->entries().first().snapshot.enabled); // no optimistic flip
        backend.finish(backend.next<DisablePlugin>(),
                       updated(plugin(), "Saved, but sync was uncertain"));
        QVERIFY(!plugins->entries().first().snapshot.enabled);
        QVERIFY(!plugins->entries().first().warning.isEmpty());
        QCOMPARE(backend.count<ModelsList>(), catalogRequests);
        QCOMPARE(backend.count<Initialize>(), 1); // no restart/reinitialization
        for (int i = 0; i < 200; ++i)
            backend.fake.advance();
        QVERIFY(!chat.busy());
        QVERIFY(chat.ready());
    }
    void notificationsAndDisabling()
    {
        PluginBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        auto *plugins = chat.plugins();
        QSignalSpy changes(plugins, &Plugins::changed);
        backend.finish(backend.next<PluginsList>(), listed({plugin("vendor.future", "enabled")}));
        plugins->setEnabled("vendor.future", false);
        backend.publish(plugin("vendor.future", "disabling"));
        QVERIFY(!plugins->entries().first().canToggle()); // its own request is pending
        QCOMPARE(plugins->entries().first().snapshot.state, "disabling");
        backend.finish(backend.next<DisablePlugin>(),
                       updated(plugin("vendor.future", "disabling")));
        QVERIFY(!plugins->entries().first().pending);
        QVERIFY(plugins->entries().first().canToggle()); // can be turned back on
        plugins->setEnabled("vendor.future", false);     // already off: nothing to send
        QCOMPARE(backend.count<DisablePlugin>(), 1);
        backend.publish(plugin());
        QCOMPARE(plugins->entries().first().snapshot.state, "disabled");
        QVERIFY(plugins->entries().first().canToggle());
        backend.publish(plugin("vendor.future", "enabled")); // external origin, no request
        QVERIFY(plugins->entries().first().snapshot.enabled);
        plugins->setEnabled("vendor.future", false);
        backend.publish(plugin("vendor.future", "disabling"));
        backend.publish(plugin()); // final event even before the earlier reply is delivered
        QVERIFY(plugins->entries().first().pending);
        backend.finish(backend.next<DisablePlugin>(),
                       updated(plugin("vendor.future", "disabling")));
        QCOMPARE(plugins->entries().first().snapshot.state, "disabled");
        QVERIFY(plugins->entries().first().canToggle());
        backend.publish(plugin("another.future/id"));
        QCOMPARE(plugins->entries().size(), 2);
        QVERIFY(changes.size() >= 10);
    }
    void unavailableAndUnknownStates()
    {
        PluginBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        backend.finish(backend.next<PluginsList>(),
                       listed({plugin("unconfigured", "enabled", false),
                               plugin("future-state", "suspended"), plugin("failed", "error")}));
        for (const auto &p : chat.plugins()->entries()) {
            QVERIFY(!p.canToggle());
            chat.plugins()->setEnabled(p.snapshot.id, !p.snapshot.enabled);
        }
        chat.plugins()->setEnabled("unknown-id", true);
        QCOMPARE(backend.count<EnablePlugin>(), 0);
        QCOMPARE(backend.count<DisablePlugin>(), 0);
    }
    void failureReconciles_data()
    {
        QTest::addColumn<bool>("enable");
        QTest::addColumn<bool>("malformed");
        QTest::newRow("enable") << true << false;
        QTest::newRow("disable") << false << false;
        QTest::newRow("wrong-plugin-reply") << true << true;
    }
    void failureReconciles()
    {
        QFETCH(bool, enable);
        QFETCH(bool, malformed);
        PluginBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        auto before = plugin("vendor.future", enable ? "disabled" : "enabled");
        backend.finish(backend.next<PluginsList>(), listed({before}));
        const auto status = chat.status();
        chat.plugins()->setEnabled(before.id, enable);
        backend.finish(
            enable ? backend.next<EnablePlugin>() : backend.next<DisablePlugin>(),
            malformed
                ? Result{updated(plugin("wrong-id"))}
                : Result{Error{"plugin_save_failed", "Cannot save plugin state", {}, {}, {}, {}}});
        const auto &row = chat.plugins()->entries().first();
        QCOMPARE(row.snapshot.state, before.state);
        QVERIFY(!row.error.isEmpty());
        QVERIFY(!row.pending);
        QVERIFY(!row.canToggle());
        QVERIFY(chat.ready());
        QCOMPARE(chat.status(), status);
        // Another actor may have changed the state; the refresh, not the failed
        // operation's desired value or an optimistic rollback, wins.
        auto actual = plugin("vendor.future", enable ? "enabled" : "disabled");
        backend.finish(backend.next<PluginsList>(), listed({actual}));
        QCOMPARE(chat.plugins()->entries().first().snapshot.state, actual.state);
        QVERIFY(chat.plugins()->entries().first().canToggle());
        QVERIFY(chat.plugins()->entries().first().error.isEmpty()); // reconciled: no stale error
        QVERIFY(chat.send("Settings failure did not break chat") != 0);
    }
    void failedChangeAndFailedRefreshNeverGuessState()
    {
        PluginBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        auto *plugins = chat.plugins();
        backend.finish(backend.next<PluginsList>(), listed({plugin(), plugin("second")}));
        plugins->setEnabled("vendor.future", true);
        plugins->setEnabled("second", true); // only this ID is blocked by its own request
        QCOMPARE(backend.count<EnablePlugin>(), 2);
        backend.finish(backend.next<EnablePlugin>(),
                       Error{"plugin_activation_failed", "Activation refused", {}, {}, {}, {}});
        QVERIFY(!plugins->entries().first().snapshot.enabled);
        QVERIFY(plugins->entries().last().pending);
        // Sent after second's enable, so served after it: it already reports the change.
        backend.finish(backend.next<PluginsList>(),
                       listed({plugin(), plugin("second", "enabled")}));
        QVERIFY(!plugins->entries().first().snapshot.enabled);
        QVERIFY(plugins->entries().first().canToggle());
        QVERIFY(plugins->entries().last().pending); // list cannot release an in-flight toggle
        backend.finish(backend.next<EnablePlugin>(), updated(plugin("second", "enabled")));
        QVERIFY(plugins->entries().last().snapshot.enabled);
        plugins->setEnabled("vendor.future", true);
        backend.finish(backend.next<EnablePlugin>(),
                       Error{"timeout", "Outcome not confirmed", {}, {}, {}, {}});
        backend.finish(backend.next<PluginsList>(),
                       Error{"timeout", "Read failed", {}, {}, {}, {}});
        QVERIFY(!plugins->entries().first().canToggle());
        QVERIFY(!plugins->entries().first().snapshot.enabled);
        QVERIFY(!plugins->entries().first().error.isEmpty());
        plugins->setEnabled("vendor.future", true);
        QCOMPARE(backend.count<EnablePlugin>(), 3); // no blind retry after an uncertain outcome
        backend.publish(plugin("vendor.future", "enabled"));
        QVERIFY(plugins->entries().first().snapshot.enabled);
        QVERIFY(plugins->entries().first().canToggle());
        QVERIFY(plugins->entries().first().error.isEmpty());
        QVERIFY(chat.ready());
    }
    void listErrorsAndRacedSnapshots()
    {
        PluginBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        auto *plugins = chat.plugins();
        backend.finish(backend.next<PluginsList>(),
                       Error{"plugin_error", "List failed", {}, {}, {}, {}});
        QVERIFY(!plugins->loaded());
        QCOMPARE(plugins->error(), "List failed");
        QVERIFY(chat.ready());
        plugins->refresh();
        backend.publish(plugin("vendor.future", "enabled"));
        backend.publish(plugin("brand.new"));
        backend.finish(backend.next<PluginsList>(),
                       listed({plugin()}, "Saved state was unreadable"));
        QCOMPARE(plugins->entries().size(), 2);
        QVERIFY(plugins->entries().first().snapshot.enabled);
        QCOMPARE(plugins->error(), "Saved state was unreadable");
        QVERIFY(plugins->entries().first().canToggle()); // no local persistence policy
        plugins->setEnabled("vendor.future", false);
        // changed:false can still reconcile a state changed by someone else;
        // it need not be accompanied by a notification.
        backend.finish(backend.next<DisablePlugin>(),
                       Reply{PluginUpdated{plugin(), false, "memory", {}}});
        QVERIFY(!plugins->entries().first().snapshot.enabled);
        QVERIFY(plugins->loading()); // reread a possibly repaired load error
        backend.finish(backend.next<PluginsList>(), listed({plugin()}));
        QVERIFY(plugins->error().isEmpty());
        plugins->refresh();
        backend.finish(backend.next<PluginsList>(), Reply{Null{}});
        QVERIFY(!plugins->entries().first().canToggle());
        QVERIFY(!plugins->error().isEmpty());
        plugins->refresh();
        backend.finish(backend.next<PluginsList>(), listed({plugin(), plugin()}));
        QVERIFY(!plugins->entries().first().canToggle()); // malformed duplicate IDs
        plugins->refresh();
        backend.finish(backend.next<PluginsList>(), listed({plugin()}));
        QVERIFY(plugins->error().isEmpty());
        QCOMPARE(plugins->entries().size(), 1);
        QVERIFY(plugins->entries().first().canToggle());
    }
    void stopCancelsOnlyTheChatTurn()
    {
        PluginBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        auto *plugins = chat.plugins();
        backend.finish(backend.next<PluginsList>(),
                       listed({plugin(), plugin("second", "enabled")}));
        plugins->setEnabled("vendor.future", true);
        plugins->refresh();
        const auto enable = backend.next<EnablePlugin>();
        const auto list = backend.next<PluginsList>();
        QVERIFY(enable && list);
        QVERIFY(chat.send("Stop before acceptance") != 0);
        QVERIFY(chat.pending()); // StartTurn not yet acknowledged
        const auto start = backend.last<StartTurn>();
        QVERIFY(start != 0);
        plugins->setEnabled("second", false); // dispatched while the start is outstanding
        const auto disable = backend.next<DisablePlugin>();
        chat.stop();
        QCOMPARE(backend.cancelled, QVector<RequestId>{start});
        QVERIFY(!backend.cancelled.contains(enable));
        QVERIFY(!backend.cancelled.contains(list));
        QVERIFY(!backend.cancelled.contains(disable));
        QVERIFY(plugins->entries().first().pending);
        QVERIFY(plugins->entries().first().error.isEmpty());
        QVERIFY(plugins->loading());
        backend.finish(enable, updated(plugin("vendor.future", "enabled")));
        backend.finish(disable, updated(plugin("second")));
        backend.finish(list, listed({plugin("vendor.future", "enabled"), plugin("second")}));
        QVERIFY(plugins->entries().first().snapshot.enabled);
        QVERIFY(plugins->entries().first().canToggle());
        QVERIFY(!plugins->entries().last().snapshot.enabled);
        QVERIFY(plugins->entries().last().canToggle());
    }
    void reenableWhileDisabling()
    {
        PluginBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        auto *plugins = chat.plugins();
        backend.finish(backend.next<PluginsList>(), listed({plugin("vendor.future", "enabled")}));
        auto disabling = plugin("vendor.future", "disabling");
        disabling.activeCalls = 1;
        // Rapid disable -> enable: answered disabling, then turned back on at once.
        plugins->setEnabled("vendor.future", false);
        backend.publish(disabling);
        backend.finish(backend.next<DisablePlugin>(), updated(disabling));
        QVERIFY(plugins->entries().first().canToggle());
        plugins->refresh(); // an older read races the re-enable
        const auto olderList = backend.next<PluginsList>();
        plugins->setEnabled("vendor.future", true);
        QCOMPARE(backend.count<EnablePlugin>(), 1);
        QVERIFY(plugins->entries().first().pending);
        QCOMPARE(plugins->entries().first().snapshot.state, "disabling"); // not optimistic
        plugins->setEnabled("vendor.future", true);
        QCOMPARE(backend.count<EnablePlugin>(), 1); // one request at a time
        backend.publish(plugin("vendor.future", "enabled"));
        backend.finish(backend.next<EnablePlugin>(), updated(plugin("vendor.future", "enabled")));
        QVERIFY(plugins->entries().first().snapshot.enabled);
        backend.finish(olderList, listed({disabling})); // sent before the change
        QVERIFY(plugins->entries().first().snapshot.enabled);
        QVERIFY(plugins->entries().first().canToggle());

        // The old call settled before the enable was served: disabled, then
        // enabled again. An answer is never older than an event before it.
        plugins->setEnabled("vendor.future", false);
        backend.publish(disabling);
        backend.finish(backend.next<DisablePlugin>(), updated(disabling));
        plugins->setEnabled("vendor.future", true);
        backend.publish(plugin());
        QVERIFY(plugins->entries().first().pending);
        backend.publish(plugin("vendor.future", "enabled"));
        backend.finish(backend.next<EnablePlugin>(), updated(plugin("vendor.future", "enabled")));
        QCOMPARE(plugins->entries().first().snapshot.state, "enabled");
        QVERIFY(plugins->entries().first().canToggle());

        // A failed re-enable never guesses: blocked until a fresh read.
        plugins->setEnabled("vendor.future", false);
        backend.finish(backend.next<DisablePlugin>(), updated(disabling));
        plugins->setEnabled("vendor.future", true);
        backend.finish(backend.next<EnablePlugin>(),
                       Error{"timeout", "Outcome unknown", {}, {}, {}, {}});
        QVERIFY(!plugins->entries().first().canToggle());
        backend.finish(backend.next<PluginsList>(), listed({plugin("vendor.future", "enabled")}));
        QVERIFY(plugins->entries().first().snapshot.enabled);
        QVERIFY(plugins->entries().first().error.isEmpty());
        QCOMPARE(backend.count<EnablePlugin>(), 3);
    }
    void olderListsNeverWin()
    {
        PluginBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        auto *plugins = chat.plugins();
        backend.finish(backend.next<PluginsList>(), listed({plugin()}));

        // A list sent before an uncertain failure cannot reauthorize the toggle.
        plugins->refresh();
        const auto older = backend.next<PluginsList>();
        plugins->setEnabled("vendor.future", true);
        backend.finish(backend.next<EnablePlugin>(),
                       Error{"timeout", "Outcome not confirmed", {}, {}, {}, {}});
        QVERIFY(!plugins->entries().first().canToggle());
        backend.finish(older, listed({plugin()}));
        QVERIFY(!plugins->entries().first().canToggle());
        QVERIFY(!plugins->entries().first().error.isEmpty());
        QVERIFY(plugins->loading()); // the reconciliation read, sent after the failure
        plugins->setEnabled("vendor.future", true);
        QCOMPARE(backend.count<EnablePlugin>(), 1); // no retry before reconciliation
        backend.finish(backend.next<PluginsList>(), listed({plugin("vendor.future", "enabled")}));
        QVERIFY(plugins->entries().first().canToggle());
        QVERIFY(plugins->entries().first().snapshot.enabled);
        QVERIFY(plugins->entries().first().error.isEmpty());

        // A list sent after a mutation answers first; the older answer loses.
        plugins->setEnabled("vendor.future", false);
        plugins->refresh();
        backend.finish(backend.next<PluginsList>(), listed({plugin()}));
        backend.finish(backend.next<DisablePlugin>(),
                       updated(plugin("vendor.future", "disabling")));
        QCOMPARE(plugins->entries().first().snapshot.state, "disabled");
        QVERIFY(plugins->entries().first().canToggle());

        // A list sent before a mutation answers after it: the answer wins.
        plugins->refresh();
        const auto before = backend.next<PluginsList>();
        plugins->setEnabled("vendor.future", true);
        backend.finish(backend.next<EnablePlugin>(), updated(plugin("vendor.future", "enabled")));
        backend.finish(before, listed({plugin()}));
        QVERIFY(plugins->entries().first().snapshot.enabled);

        // ...and when it answers before the mutation's reply, the reply still applies.
        plugins->refresh();
        const auto earlier = backend.next<PluginsList>();
        plugins->setEnabled("vendor.future", false);
        backend.finish(earlier, listed({plugin("vendor.future", "enabled")}));
        backend.finish(backend.next<DisablePlugin>(), updated(plugin()));
        QVERIFY(!plugins->entries().first().snapshot.enabled);

        // Live events during a list beat it, including for IDs it omits;
        // rows only the older list knew about are dropped.
        plugins->refresh();
        backend.publish(plugin("vendor.future", "enabled"));
        backend.publish(plugin("brand.new"));
        backend.finish(backend.next<PluginsList>(), listed({plugin(), plugin("gone.soon")}));
        QCOMPARE(plugins->entries().size(), 3);
        QVERIFY(plugins->entries().first().snapshot.enabled);
        plugins->refresh();
        backend.finish(backend.next<PluginsList>(),
                       listed({plugin("vendor.future", "enabled"), plugin("brand.new")}));
        QCOMPARE(plugins->entries().size(), 2);
        // An identical repeated event is harmless.
        backend.publish(plugin("brand.new"));
        backend.publish(plugin("brand.new"));
        QCOMPARE(plugins->entries().size(), 2);
    }
    void persistenceOutcomeIsKept()
    {
        PluginBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        auto *plugins = chat.plugins();
        backend.finish(backend.next<PluginsList>(), listed({plugin()}));
        QVERIFY(plugins->entries().first().persisted.isEmpty());
        plugins->setEnabled("vendor.future", true);
        backend.finish(backend.next<EnablePlugin>(),
                       updated(plugin("vendor.future", "enabled"), {}, "memory"));
        QCOMPARE(plugins->entries().first().persisted, "memory");
        plugins->refresh();
        backend.finish(backend.next<PluginsList>(), listed({plugin("vendor.future", "enabled")}));
        QCOMPARE(plugins->entries().first().persisted,
                 "memory"); // still true until the next request
        plugins->setEnabled("vendor.future", false);
        QVERIFY(plugins->entries().first().persisted.isEmpty());
        backend.finish(backend.next<DisablePlugin>(), updated(plugin(), {}, "ambiguous"));
        QCOMPARE(plugins->entries().first().persisted, "ambiguous");
    }
    void reconnectDropsStaleWorkAndRefreshes()
    {
        PluginBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        backend.finish(backend.next<PluginsList>(), listed({plugin()}));
        chat.plugins()->setEnabled("vendor.future", true);
        const auto oldToggle = backend.next<EnablePlugin>();
        chat.plugins()->refresh();
        const auto oldList = backend.next<PluginsList>();
        backend.disconnectBackend();
        QVERIFY(!chat.plugins()->supported());
        QVERIFY(chat.plugins()->entries().isEmpty());
        backend.publish(plugin("late-event"));
        QVERIFY(chat.plugins()->entries().isEmpty());
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        backend.finish(oldToggle, updated(plugin("vendor.future", "enabled")));
        backend.finish(oldList, listed({plugin("stale")}));
        QVERIFY(chat.plugins()->entries().isEmpty());
        QVERIFY(chat.plugins()->loading());
        backend.finish(backend.next<PluginsList>(), listed({plugin("fresh")}));
        QCOMPARE(chat.plugins()->entries().first().snapshot.id, "fresh");
        QCOMPARE(backend.count<PluginsList>(), 3);
        backend.disconnectBackend();
        backend.runtimePlugins = false;
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QVERIFY(!chat.plugins()->supported());
        QVERIFY(chat.plugins()->entries().isEmpty());
        QCOMPARE(backend.count<PluginsList>(), 3);
    }
};
QTEST_GUILESS_MAIN(PluginsTest)
#include "plugins.moc"
