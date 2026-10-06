// PiBackend against the real `pi` on PATH (skipped without one): the real bridge
// and the shipped plugin-permissions inside real Pi, with Pi AI's faux provider
// as the model (tests/pi/real/faux.ts) in a throwaway agent directory. No
// network, no credentials, nothing of the user's Pi configuration. The faux
// model asks for exactly the tool call the prompt names. OpenGhost owns no
// permission policy: plugin-permissions decides, OpenGhost shows its requests
// and relays the answers and the mode, and every file effect is checked on disk.
#include "backend/pi_backend.h"
#include "backend/pi_process.h"
#include "frontend/chat_service.h"
#include "frontend/library.h"
#include "frontend/preferences.h"
#include "frontend/store.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace openghost;
namespace
{
struct Harness {
    PiBackend backend;
    PreferencesStore prefs{QString()};
    MemoryKeyStore memory;
    Library library{&memory};
    ChatService chat{&backend, &prefs, &library};
    QStringList logs;
    explicit Harness(const QString &folder, bool permissions = true)
    {
        backend.setPermissionsEnabled(permissions);
        QObject::connect(&backend, &Backend::globalEvent, [this](const GlobalEvent &event) {
            if (const auto *log = std::get_if<Log>(&event))
                logs.append(log->level + ' ' + log->message);
        });
        chat.initialize();
        QTRY_VERIFY_WITH_TIMEOUT(chat.ready(), 30000);
        QVERIFY(chat.addFolder(folder).isEmpty());
        chat.newChat(folder);
    }
    QString reply() const
    {
        const auto &rows = chat.current().rows;
        for (auto it = rows.crbegin(); it != rows.crend(); ++it)
            if (it->role == DisplayRow::Role::Assistant && !it->hidden)
                return it->text;
        return {};
    }
    bool settled() const { return chat.ready() && !chat.busy() && !chat.pending(); }
    // Whether the turn just sent stops at an approval card (rather than settling).
    bool asks()
    {
        [this] {
            QTRY_VERIFY_WITH_TIMEOUT(chat.approvals().size() == 1 || settled(), 30000);
        }();
        return chat.approvals().size() == 1;
    }
    // Denies the card the turn waits on, and waits for the turn to end.
    void deny()
    {
        chat.approve(chat.approvals().first().request, false);
        [this] { QTRY_VERIFY_WITH_TIMEOUT(settled(), 30000); }();
    }
    void allow()
    {
        chat.approve(chat.approvals().first().request, true);
        [this] { QTRY_VERIFY_WITH_TIMEOUT(settled(), 30000); }();
    }
    // A card action plugin-permissions offered (allow for the session, …).
    void decide(const QString &action, const QString &note = {})
    {
        chat.decide(chat.approvals().first().request, action, note, {});
        [this] { QTRY_VERIFY_WITH_TIMEOUT(settled(), 30000); }();
    }
    // The access mode plugin-permissions itself holds ("none": no plugin).
    void held(const char *mode)
    {
        logs.clear();
        const auto said = QStringLiteral("warning og-mode got ") + QString::fromUtf8(mode);
        [&] {
            QVERIFY(chat.send(QStringLiteral("/og-mode")));
            QTRY_VERIFY_WITH_TIMEOUT(settled(), 30000);
            QTRY_VERIFY_WITH_TIMEOUT(std::any_of(logs.cbegin(), logs.cend(),
                                                 [&](const QString &log) {
                                                     return log == said ||
                                                            log.startsWith(said + ' ');
                                                 }),
                                     10000);
        }();
    }
    void mode(PermissionMode to, const char *name)
    {
        chat.setMode(to);
        [this] { QTRY_VERIFY_WITH_TIMEOUT(settled(), 30000); }();
        held(name);
    }
    // A prompt the faux model turns into this one tool call.
    bool call(const QString &tool, const QJsonObject &args)
    {
        return chat.send(
            QStringLiteral("call %1 %2")
                .arg(tool, QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))));
    }
};
// The operator's plugin-permissions config (global scope) while it lives.
struct Policy {
    QString path = QString::fromLocal8Bit(qgetenv("PI_CODING_AGENT_DIR")) +
                   QStringLiteral("/extensions/plugin-permissions/config.json");
    explicit Policy(const char *json)
    {
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
            file.write(json);
    }
    ~Policy() { QFile::remove(path); }
};
} // namespace

class PiRealTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_folder;

    void fresh()
    {
        QDir(m_folder).removeRecursively();
        QVERIFY(QDir().mkpath(m_folder));
    }

  private slots:
    void initTestCase()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("pi")).isEmpty())
            QSKIP("No `pi` on PATH: the real-Pi checks need Pi installed.");
        QVERIFY(m_dir.isValid());
        const auto agent = m_dir.filePath(QStringLiteral("agent"));
        QVERIFY(QDir().mkpath(agent + QStringLiteral("/extensions")));
        for (const auto *name : {"faux.ts", "helper.ts"})
            QVERIFY(QFile::copy(QStringLiteral(OPENGHOST_REAL_PI_DIR "/") + name,
                                agent + QStringLiteral("/extensions/") + name));
        QFile settings(agent + QStringLiteral("/settings.json"));
        QVERIFY(settings.open(QIODevice::WriteOnly));
        settings.write(R"({"defaultProvider":"og-faux","defaultModel":"faux"})");
        settings.close();
        qputenv("PI_CODING_AGENT_DIR", agent.toUtf8());
        qputenv("PI_OFFLINE", "1");
        qputenv("PI_SKIP_VERSION_CHECK", "1");
        m_folder = m_dir.filePath(QStringLiteral("project"));
    }
    void init() { fresh(); }

    // Ask: plugin-permissions holds `ask`; a command is a card from the plugin
    // (its decisions and shortcuts with it), Deny keeps it from running and Allow
    // runs it. Looking inside the folder asks nothing; changing a file asks.
    void askAsksAndTheAnswerGoesBack()
    {
        Harness h(m_folder);
        h.held("ask");
        const auto made = m_folder + QStringLiteral("/made.txt");
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch made.txt"}}));
        QVERIFY(h.asks());
        const auto card = h.chat.approvals().first().data;
        QCOMPARE(card.tool, QStringLiteral("bash"));
        QCOMPARE(card.args.value("command").toString(), QStringLiteral("touch made.txt"));
        QVERIFY(card.approvalId.startsWith(QStringLiteral("pp-")));
        QVERIFY(card.presentation);
        QCOMPARE(card.presentation->title, QStringLiteral("Run a command"));
        QCOMPARE(card.presentation->code, std::optional(QStringLiteral("touch made.txt")));
        QStringList ids, keys;
        for (const auto &action : card.actions) {
            ids << action.id;
            keys << action.key;
        }
        QCOMPARE(ids, (QStringList{"approve", "approveSession", "deny", "denyWithReason"}));
        QCOMPARE(keys, (QStringList{"y", "s", "n", "r"}));
        QVERIFY(card.actions[1].detail.contains(QStringLiteral("touch")));
        QVERIFY(card.doublePressToConfirm);
        QVERIFY(!QFile::exists(made)); // Nothing ran while it waits.
        h.deny();
        QVERIFY(h.reply().contains(QStringLiteral("done: error:")));
        QVERIFY(!QFile::exists(made));

        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch made.txt"}}));
        QVERIFY(h.asks());
        h.allow();
        QVERIFY(h.reply().contains(QStringLiteral("done: ok")));
        QVERIFY(QFile::exists(made));

        QVERIFY(h.call(QStringLiteral("read"), {{"path", "made.txt"}}));
        QVERIFY(!h.asks());
        QVERIFY(h.reply().contains(QStringLiteral("done: ok")));
        QVERIFY(h.call(QStringLiteral("write"), {{"path", "new.txt"}, {"content", "x"}}));
        QVERIFY(h.asks());
        QCOMPARE(h.chat.approvals().first().data.presentation->title, QStringLiteral("Write a file"));
        h.deny();
        QVERIFY(!QFile::exists(m_folder + QStringLiteral("/new.txt")));
    }

    // Auto: the plugin's Auto rules: in the folder it works on its own, a risky
    // command or a file outside the folder is still a card, and an operator deny
    // rule still denies, without a card.
    void autoFollowsThePluginsPolicy()
    {
        Policy policy(R"({"permission":{"path":{"*.secret":"deny"}}})");
        Harness h(m_folder);
        h.mode(PermissionMode::Auto, "auto");
        QVERIFY(h.call(QStringLiteral("write"), {{"path", "a.txt"}, {"content", "x"}}));
        QVERIFY(!h.asks());
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/a.txt")));
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch b.txt"}}));
        QVERIFY(!h.asks());
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/b.txt")));
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "rm a.txt"}}));
        QVERIFY(h.asks());
        h.deny();
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/a.txt")));
        const auto outside = m_dir.filePath(QStringLiteral("outside.txt"));
        QVERIFY(h.call(QStringLiteral("write"), {{"path", outside}, {"content", "x"}}));
        QVERIFY(h.asks());
        QVERIFY(h.chat.approvals().first().data.presentation->title.contains(
            QStringLiteral("outside the project")));
        h.deny();
        QVERIFY(!QFile::exists(outside));
        QVERIFY(h.call(QStringLiteral("write"), {{"path", "k.secret"}, {"content", "x"}}));
        QVERIFY(!h.asks());
        QVERIFY(h.reply().contains(QStringLiteral("done: error:")));
        QVERIFY(!QFile::exists(m_folder + QStringLiteral("/k.secret")));
    }

    // Pi validates (and coerces) a call's arguments before any extension sees
    // them, and the plugin judges exactly the arguments that run: in Auto, where
    // the plugin would allow a write in the folder, a malformed one never runs.
    void malformedArgumentsNeverRun()
    {
        Harness h(m_folder);
        h.mode(PermissionMode::Auto, "auto");
        QVERIFY(h.call(QStringLiteral("write"),
                       {{"path", QJsonObject{{"not", "a path"}}}, {"content", "x"}}));
        QVERIFY(!h.asks());
        QVERIFY(h.reply().contains(QStringLiteral("done: error:")));
        QCOMPARE(QDir(m_folder).entryList(QDir::Files), QStringList());
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", QJsonObject{{"not", "a string"}}}}));
        QVERIFY(!h.asks());
        QVERIFY(h.reply().contains(QStringLiteral("done: error:")));
    }

    // Full is the plugin's: it allows what would ask (a risky command, a file
    // outside the folder) and keeps an operator's hard deny.
    void fullIsThePluginsAndKeepsHardDenies()
    {
        Policy policy(R"({"permission":{"bash":{"*":"ask","rm kept*":"deny"}}})");
        Harness h(m_folder);
        h.mode(PermissionMode::Full, "full");
        QVERIFY(QFile(m_folder + QStringLiteral("/gone.txt")).open(QIODevice::WriteOnly));
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "rm gone.txt"}}));
        QVERIFY(!h.asks());
        QVERIFY(!QFile::exists(m_folder + QStringLiteral("/gone.txt")));
        const auto outside = m_dir.filePath(QStringLiteral("outside.txt"));
        QVERIFY(h.call(QStringLiteral("write"), {{"path", outside}, {"content", "x"}}));
        QVERIFY(!h.asks());
        QVERIFY(QFile::exists(outside));
        QFile::remove(outside);
        QVERIFY(QFile(m_folder + QStringLiteral("/kept.txt")).open(QIODevice::WriteOnly));
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "rm kept.txt"}}));
        QVERIFY(!h.asks());
        QVERIFY(h.reply().contains(QStringLiteral("done: error:")));
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/kept.txt")));
    }

    // Ask / Auto / Full reach the plugin as the chat's mode, before a run and as
    // soon as it changes; the plugin holds what OpenGhost said last, no other.
    void modesReachThePlugin()
    {
        Harness h(m_folder);
        h.held("ask");
        h.mode(PermissionMode::Auto, "auto");
        h.mode(PermissionMode::Full, "full");
        h.mode(PermissionMode::Ask, "ask");
        // An operator rule is policy, never a mode: an allow in the plugin's own
        // config is what stops Ask asking about it, not OpenGhost.
        Policy policy(R"({"permission":{"bash":{"touch *":"allow"}}})");
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch ruled.txt"}}));
        QVERIFY(!h.asks());
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/ruled.txt")));
    }

    // Mode updates are numbered: in real Pi with the plugin, an older update
    // never lands after a newer one, at the bridge or in the plugin.
    void anOlderModeUpdateNeverWins()
    {
        QTemporaryDir dir;
        const auto bridge = dir.filePath(QStringLiteral("openghost-bridge.js"));
        QVERIFY(QFile::copy(QStringLiteral(":/pi/openghost-bridge.js"), bridge));
        for (const auto *file : {"plugin-permissions.js", "web-tree-sitter.wasm", "tree-sitter-bash.wasm"})
            QVERIFY(QFile::copy(QStringLiteral(":/pi/plugin-permissions/") + file,
                                dir.filePath(QString::fromLatin1(file))));
        PiProcess pi(bridge);
        QString error;
        QVERIFY2(pi.start({QStringLiteral("--mode"), QStringLiteral("rpc"), QStringLiteral("--no-session"),
                           QStringLiteral("-e"), bridge, QStringLiteral("-e"),
                           dir.filePath(QStringLiteral("plugin-permissions.js"))},
                          m_folder, &error),
                 qPrintable(error));
        const auto mode = [&pi](const char *name, int seq) {
            std::optional<QJsonObject> reply;
            pi.bridge({{"op", "mode"}, {"mode", name}, {"seq", seq}},
                      [&reply](const QJsonObject &r) { reply = r; }, 30000);
            [&reply] { QTRY_VERIFY_WITH_TIMEOUT(reply.has_value(), 30000); }();
            return reply.value_or(QJsonObject{});
        };
        const auto full = mode("full", 5);
        QCOMPARE(full.value("mode").toString(), QStringLiteral("full"));
        QCOMPARE(full.value("enforcer").toObject().value("name").toString(),
                 QStringLiteral("plugin-permissions"));
        QCOMPARE(full.value("enforcer").toObject().value("mode").toString(), QStringLiteral("full"));
        QCOMPARE(full.value("enforcer").toObject().value("seq").toInt(), 5);
        const auto stale = mode("ask", 3); // sent earlier, run later
        QVERIFY(stale.value("ok").toBool());
        QVERIFY(stale.value("stale").toBool());
        QCOMPARE(stale.value("mode").toString(), QStringLiteral("full"));
        const auto newer = mode("ask", 6);
        QCOMPARE(newer.value("mode").toString(), QStringLiteral("ask"));
        QVERIFY(!newer.value("stale").toBool());
        QCOMPARE(newer.value("enforcer").toObject().value("mode").toString(), QStringLiteral("ask"));
        QCOMPARE(newer.value("enforcer").toObject().value("seq").toInt(), 6);
        bool closed = false;
        pi.close([&closed] { closed = true; });
        QTRY_VERIFY_WITH_TIMEOUT(closed, 10000);
    }

    // Full while a card waits: the plugin allows it and takes the request back;
    // the card goes without an answer from OpenGhost.
    void fullReleasesTheWaitingCard()
    {
        Harness h(m_folder);
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch m.txt"}}));
        QVERIFY(h.asks());
        h.chat.setMode(PermissionMode::Full);
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.chat.approvals().isEmpty());
        QCOMPARE(h.chat.current().mode, PermissionMode::Full);
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/m.txt")));
        QVERIFY(h.reply().contains(QStringLiteral("done: ok")));
    }

    // Allow for the session: the plugin's own session rule (its suggested pattern)
    // covers the next such command in this chat, and nothing in a new chat.
    void allowForTheSessionIsThePlugins()
    {
        Harness h(m_folder);
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch one.txt"}}));
        QVERIFY(h.asks());
        h.decide(QStringLiteral("approveSession"));
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/one.txt")));
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch two.txt"}}));
        QVERIFY(!h.asks());
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/two.txt")));
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "mkdir three"}})); // another command
        QVERIFY(h.asks());
        h.deny();
        h.chat.newChat(m_folder); // A new session: its own plugin, no grant.
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch four.txt"}}));
        QVERIFY(h.asks());
        h.deny();
        QVERIFY(!QFile::exists(m_folder + QStringLiteral("/four.txt")));
    }

    // An explicit deny always wins: Allow for session suppresses later asks but
    // never overrides an operator's deny its pattern covers, in Ask, Auto or Full.
    void aSessionApprovalNeverOverridesADeny()
    {
        Policy policy(R"({"permission":{"bash":{"touch secret*":"deny"}}})");
        Harness h(m_folder);
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch g1.txt"}}));
        QVERIFY(h.asks());
        h.decide(QStringLiteral("approveSession")); // "touch *" for this session
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/g1.txt")));
        const std::pair<PermissionMode, const char *> modes[] = {
            {PermissionMode::Ask, "ask"}, {PermissionMode::Auto, "auto"},
            {PermissionMode::Full, "full"}};
        for (const auto &[mode, name] : modes) {
            h.mode(mode, name);
            const auto file = QStringLiteral("g-%1.txt").arg(QLatin1String(name));
            QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch " + file}}));
            QVERIFY(!h.asks());
            QVERIFY(QFile::exists(m_folder + QLatin1Char('/') + file));
            QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch secret.txt"}}));
            QVERIFY(!h.asks());
            QVERIFY(h.reply().contains(QStringLiteral("done: error:")));
            QVERIFY(!QFile::exists(m_folder + QStringLiteral("/secret.txt")));
        }
    }

    // Deny with a reason: the plugin tells the agent the user's reason.
    void denyWithAReasonTellsTheAgent()
    {
        Harness h(m_folder);
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch r.txt"}}));
        QVERIFY(h.asks());
        h.decide(QStringLiteral("denyWithReason"), QStringLiteral("use the build script"));
        QVERIFY(h.reply().contains(QStringLiteral("use the build script")));
        QVERIFY(!QFile::exists(m_folder + QStringLiteral("/r.txt")));
    }

    // Stop takes the card back; an answer to it afterwards runs nothing.
    void stopWithdrawsTheWaitingCall()
    {
        Harness h(m_folder);
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch s.txt"}}));
        QVERIFY(h.asks());
        const auto stale = h.chat.approvals().first().request;
        h.chat.stop();
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.chat.approvals().isEmpty());
        h.chat.approve(stale, true);
        h.chat.decide(stale, QStringLiteral("approve"), {}, {});
        QVERIFY(h.chat.send(QStringLiteral("still here?"))); // The chat goes on.
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QCOMPARE(h.reply(), QStringLiteral("echo: still here?"));
        QVERIFY(!QFile::exists(m_folder + QStringLiteral("/s.txt")));
    }

    // An answer counts once: a second Allow runs nothing a second time.
    void aDuplicateAnswerRunsNothingTwice()
    {
        Harness h(m_folder);
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "echo x >> log.txt"}}));
        QVERIFY(h.asks());
        const auto id = h.chat.approvals().first().request;
        h.chat.approve(id, true);
        h.chat.approve(id, true);
        h.chat.decide(id, QStringLiteral("approve"), {}, {});
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QFile log(m_folder + QStringLiteral("/log.txt"));
        QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(log.readAll(), QByteArray("x\n"));
    }

    // Deleting the chat, or its Pi exiting, ends the waiting call: the card goes
    // and the call never runs.
    void deletionAndExitCloseTheCard()
    {
        {
            Harness h(m_folder);
            QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch d.txt"}}));
            QVERIFY(h.asks());
            QSignalSpy removed(&h.chat, &ChatService::removed);
            h.chat.remove(h.chat.current().id);
            QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 30000);
            QVERIFY(h.chat.approvals().isEmpty());
        }
        QVERIFY(!QFile::exists(m_folder + QStringLiteral("/d.txt")));
        // The helper makes this Pi exit a moment after the call: its card shows,
        // then Pi is gone.
        Harness h(m_folder);
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch e.txt # og-exit"}}));
        QVERIFY(h.asks());
        QTRY_VERIFY_WITH_TIMEOUT(h.chat.approvals().isEmpty(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!h.chat.busy(), 30000);
        QVERIFY(!QFile::exists(m_folder + QStringLiteral("/e.txt")));
    }

    // The enforcement boundary: without plugin-permissions nothing enforces
    // permissions. OpenGhost substitutes no policy of its own in any mode: the
    // call runs unasked, and OpenGhost says permissions are not enforced.
    void withoutThePluginOpenGhostEnforcesNothing()
    {
        Harness h(m_folder, false);
        h.held("none");
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch free.txt"}}));
        QVERIFY(!h.asks());
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/free.txt")));
        QVERIFY(std::any_of(h.logs.cbegin(), h.logs.cend(), [](const QString &log) {
            return log.startsWith(QStringLiteral("warning Permissions are not enforced"));
        }));
        const auto outside = m_dir.filePath(QStringLiteral("o.txt"));
        QVERIFY(h.call(QStringLiteral("write"), {{"path", outside}, {"content", "x"}}));
        QVERIFY(!h.asks());
        QVERIFY(QFile::exists(outside));
        QFile::remove(outside);
    }

    // M12/F4: a change to Pi's models.json made outside OpenGhost reaches an
    // already-open chat once Settings rereads: its registry, its session's model and
    // the next model request all use it, though no provider's status changed.
    void outsideConfigChangesReachOpenChats()
    {
        const auto models = QFileInfo(QString::fromLocal8Bit(qgetenv("PI_CODING_AGENT_DIR")) +
                                      QStringLiteral("/models.json"))
                                .filePath();
        struct Remove {
            QString path;
            ~Remove() { QFile::remove(path); }
        } cleanup{models};
        Harness h(m_folder);
        QVERIFY(h.chat.send(QStringLiteral("which model")));
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QCOMPARE(h.reply(), QStringLiteral("model: Faux"));
        QFile file(models);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"providers":{"og-faux":{"modelOverrides":{"faux":{"name":"Changed outside OpenGhost"}}}}})");
        file.close();
        h.chat.refresh(); // Settings → Providers opening
        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(h.chat.models().cbegin(), h.chat.models().cend(),
                        [](const Model &m) { return m.name == QStringLiteral("Changed outside OpenGhost"); }),
            30000);
        QVERIFY(h.chat.send(QStringLiteral("/og-model")));
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(
            h.logs.contains(QStringLiteral("warning og-model registry=Changed outside OpenGhost "
                                           "session=Changed outside OpenGhost")),
            10000);
        QVERIFY(h.chat.send(QStringLiteral("which model")));
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QCOMPARE(h.reply(), QStringLiteral("model: Changed outside OpenGhost"));
    }

    // Effort levels are Pi's own per model (getSupportedThinkingLevels on its
    // metadata), for an extension's provider and a local models.json one alike,
    // and the chosen level is what the chat's Pi records.
    void effortLevelsArePisPerModel()
    {
        const auto models = QFileInfo(QString::fromLocal8Bit(qgetenv("PI_CODING_AGENT_DIR")) +
                                      QStringLiteral("/models.json"))
                                .filePath();
        struct Remove {
            QString path;
            ~Remove() { QFile::remove(path); }
        } cleanup{models};
        QFile file(models);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"providers":{"og-local":{"baseUrl":"http://127.0.0.1:9/v1",)"
                   R"("api":"openai-completions","apiKey":"local","models":[)"
                   R"({"id":"local-think","reasoning":true,)"
                   R"("thinkingLevelMap":{"off":null,"minimal":null}},{"id":"local-plain"}]}}})");
        file.close();
        Harness h(m_folder);
        const auto model = [&](const QString &id) {
            for (const auto &m : h.chat.models())
                if (m.id == id)
                    return m;
            return Model{};
        };
        QVERIFY(model("faux").thinkingLevels.isEmpty() && !model("faux").defaultThinking);
        QCOMPARE(model("faux-think").thinkingLevels,
                 (QStringList{"off", "minimal", "low", "medium", "high"}));
        QCOMPARE(model("faux-think").defaultThinking, std::optional<QString>("medium"));
        QCOMPARE(model("faux-wide").thinkingLevels,
                 (QStringList{"off", "low", "medium", "high", "xhigh", "max"}));
        QCOMPARE(model("local-think").provider, QStringLiteral("og-local"));
        QCOMPARE(model("local-think").thinkingLevels, (QStringList{"low", "medium", "high"}));
        QCOMPARE(model("local-think").defaultThinking, std::optional<QString>("medium"));
        QCOMPARE(model("local-plain").provider, QStringLiteral("og-local"));
        QVERIFY(model("local-plain").thinkingLevels.isEmpty());

        h.chat.choose({QStringLiteral("og-faux"), QStringLiteral("faux-wide"), QStringLiteral("max")},
                      true);
        QVERIFY(h.chat.send(QStringLiteral("which model")));
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QCOMPARE(h.reply(), QStringLiteral("model: Faux Wide"));
        QCOMPARE(h.chat.current().selection.thinking, std::optional<QString>("max"));
        // At once in a chat with history: Pi records the change before any run.
        h.chat.choose({QStringLiteral("og-faux"), QStringLiteral("faux-wide"), QStringLiteral("low")},
                      true);
        QTRY_VERIFY_WITH_TIMEOUT(!h.chat.pending(), 30000);
        QCOMPARE(h.chat.current().selection.thinking, std::optional<QString>("low"));
        const auto changes = [&] {
            QStringList levels;
            QFile session(h.backend.sessionFile(h.chat.current().id));
            if (session.open(QIODevice::ReadOnly))
                for (const auto &line : session.readAll().split('\n')) {
                    const auto entry = QJsonDocument::fromJson(line).object();
                    if (entry.value("type").toString() == QStringLiteral("thinking_level_change"))
                        levels.append(entry.value("thinkingLevel").toString());
                }
            return levels;
        };
        QTRY_VERIFY_WITH_TIMEOUT(changes().endsWith(QStringLiteral("low")), 10000);
        QVERIFY(changes().contains(QStringLiteral("max")));
        // To a model without reasoning: Pi holds "off".
        h.chat.choose({QStringLiteral("og-faux"), QStringLiteral("faux"), {}}, false);
        QTRY_VERIFY_WITH_TIMEOUT(!h.chat.pending(), 30000);
        QCOMPARE(h.chat.current().selection.thinking, std::optional<QString>("off"));
    }

    // M15: every kind of blocking extension dialog is cancelled at once and said.
    void everyBlockingDialogIsCancelled()
    {
        Harness h(m_folder);
        QVERIFY(h.chat.send(QStringLiteral("/og-dialogs")));
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(
            h.logs.contains(QStringLiteral("warning og-dialogs got [null,false,null,null]")), 10000);
        for (const auto *title : {"Pick a letter", "Really?", "Your name", "Edit this"})
            QVERIFY2(h.logs.contains(QStringLiteral("warning A Pi extension asked “%1”, which "
                                                    "OpenGhost cannot show, so it was declined.")
                                         .arg(QString::fromUtf8(title))),
                     title);
    }

    void otherExtensionsCannotHangOpenGhost()
    {
        Harness h(m_folder);
        QVERIFY(h.chat.send(QStringLiteral("/og-ask")));
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000); // Handled: not waiting on the dialog.
        QTRY_VERIFY_WITH_TIMEOUT(h.logs.contains(QStringLiteral("warning og-ask got nothing")),
                                 10000);
        QVERIFY(h.logs.contains(QStringLiteral("warning A Pi extension asked “Pick one”, which "
                                               "OpenGhost cannot show, so it was declined.")));
        QVERIFY(h.chat.send(QStringLiteral("/og-fail")));
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(h.logs.cbegin(), h.logs.cend(),
                        [](const QString &log) {
                            return log.startsWith(QStringLiteral("error A Pi extension")) &&
                                   log.contains(QStringLiteral("helper broke"));
                        }),
            10000);
        QVERIFY(h.chat.status().contains(QStringLiteral("helper broke")));
    }
};

QTEST_GUILESS_MAIN(PiRealTest)
#include "pi_real.moc"
