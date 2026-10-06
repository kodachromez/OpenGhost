// PiBackend against the real `pi` on PATH (skipped without one): the real bridge
// inside real Pi, with Pi AI's faux provider as the model (tests/pi/real/faux.ts)
// in a throwaway agent directory. No network, no credentials, nothing of the
// user's Pi configuration. The faux model asks for exactly the tool call the
// prompt names. OpenGhost owns no permission policy: a test-only stand-in Pi
// permission plugin (tests/pi/real/permission.ts) asks, OpenGhost shows its
// requests and relays the answers and the mode, and every file effect is
// checked on disk.
#include "backend/pi_backend.h"
#include "backend/pi_process.h"
#include "frontend/chat_service.h"
#include "frontend/library.h"
#include "frontend/preferences.h"
#include "frontend/store.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    explicit Harness(const QString &folder)
    {
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
    // A prompt the faux model turns into this one tool call.
    bool call(const QString &tool, const QJsonObject &args)
    {
        return chat.send(
            QStringLiteral("call %1 %2")
                .arg(tool, QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))));
    }
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
        for (const auto *name : {"faux.ts", "helper.ts", "permission.ts"})
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

    // A request Pi's permission plugin makes is a card; the answer is the plugin's.
    void piAsksAndTheAnswerGoesBack()
    {
        Harness h(m_folder);
        const auto made = m_folder + QStringLiteral("/made.txt");
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch made.txt"}}));
        QTRY_COMPARE_WITH_TIMEOUT(h.chat.approvals().size(), 1, 30000);
        const auto card = h.chat.approvals().first().data;
        QCOMPARE(card.tool, QStringLiteral("bash"));
        QCOMPARE(card.args.value("command").toString(), QStringLiteral("touch made.txt"));
        QVERIFY(card.approvalId.startsWith(QStringLiteral("stand-in-")));
        QVERIFY(card.presentation);
        QCOMPARE(card.presentation->title, QStringLiteral("Stand-in asks"));
        QCOMPARE(card.presentation->code, std::optional(QStringLiteral("touch made.txt")));
        QVERIFY(!QFile::exists(made)); // Nothing ran while it waits.
        h.chat.approve(h.chat.approvals().first().request, false);
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.reply().contains(QStringLiteral("done: error: The stand-in was not allowed.")));
        QVERIFY(!QFile::exists(made));

        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch made.txt"}}));
        QTRY_COMPARE_WITH_TIMEOUT(h.chat.approvals().size(), 1, 30000);
        h.chat.approve(h.chat.approvals().first().request, true);
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.reply().contains(QStringLiteral("done: ok")));
        QVERIFY(QFile::exists(made));
    }

    // OpenGhost decides nothing: in Ask, a call no plugin asks about runs (even a
    // write outside the folder), and in Full a call the plugin asks about is
    // still a card.
    void openGhostNeverDecidesItself()
    {
        Harness h(m_folder);
        QCOMPARE(h.chat.current().mode, PermissionMode::Ask);
        const auto outside = m_dir.filePath(QStringLiteral("outside.txt"));
        QVERIFY(h.call(QStringLiteral("write"), {{"path", outside}, {"content", "x"}}));
        QVERIFY(!h.asks());
        QVERIFY(h.reply().contains(QStringLiteral("done: ok")));
        QVERIFY(QFile::exists(outside));
        QFile::remove(outside);

        h.chat.setMode(PermissionMode::Full);
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch full.txt"}}));
        QVERIFY(h.asks());
        h.deny();
        QVERIFY(!QFile::exists(m_folder + QStringLiteral("/full.txt")));
    }

    // Ask / Auto / Full reach Pi's extensions as the chat's mode, before a run and
    // as soon as it changes.
    void modesReachPiExtensions()
    {
        Harness h(m_folder);
        const auto said = [&h](const char *mode) {
            QVERIFY(h.chat.send(QStringLiteral("/og-mode")));
            QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
            QTRY_VERIFY_WITH_TIMEOUT(h.logs.contains(QStringLiteral("warning og-mode got ") +
                                                     QString::fromUtf8(mode)),
                                     10000);
            h.logs.clear();
        };
        said("ask");
        h.chat.setMode(PermissionMode::Auto);
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        said("auto");
        h.chat.setMode(PermissionMode::Full);
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        said("full");
    }

    // A request Pi takes back, allowed after all by its own choice: the card goes.
    void aRequestPiTakesBackLeavesNoCard()
    {
        Harness h(m_folder);
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch m.txt"}}));
        QTRY_COMPARE_WITH_TIMEOUT(h.chat.approvals().size(), 1, 30000);
        h.chat.setMode(PermissionMode::Full); // The stand-in then allows it itself.
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.chat.approvals().isEmpty());
        QCOMPARE(h.chat.current().mode, PermissionMode::Full);
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/m.txt")));
        QVERIFY(h.reply().contains(QStringLiteral("done: ok")));
    }

    void stopWithdrawsTheWaitingCall()
    {
        Harness h(m_folder);
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch s.txt"}}));
        QTRY_COMPARE_WITH_TIMEOUT(h.chat.approvals().size(), 1, 30000);
        h.chat.stop();
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.chat.approvals().isEmpty());
        QVERIFY(!QFile::exists(m_folder + QStringLiteral("/s.txt")));
        QVERIFY(h.chat.send(QStringLiteral("still here?"))); // The chat goes on.
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QCOMPARE(h.reply(), QStringLiteral("echo: still here?"));
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

    // Access mode updates are numbered, and the bridge in real Pi never lets an
    // older one land after a newer one, whatever order Pi runs them in.
    void anOlderModeUpdateNeverWins()
    {
        QTemporaryDir dir;
        const auto bridge = dir.filePath(QStringLiteral("openghost-bridge.js"));
        QVERIFY(QFile::copy(QStringLiteral(":/pi/openghost-bridge.js"), bridge));
        PiProcess pi(bridge);
        QString error;
        QVERIFY2(pi.start({QStringLiteral("--mode"), QStringLiteral("rpc"),
                           QStringLiteral("--no-session"), QStringLiteral("-e"), bridge},
                          m_folder, &error),
                 qPrintable(error));
        const auto mode = [&pi](const char *name, int seq) {
            std::optional<QJsonObject> reply;
            pi.bridge({{"op", "mode"}, {"mode", name}, {"seq", seq}},
                      [&reply](const QJsonObject &r) { reply = r; }, 30000);
            [&reply] { QTRY_VERIFY_WITH_TIMEOUT(reply.has_value(), 30000); }();
            return reply.value_or(QJsonObject{});
        };
        QCOMPARE(mode("full", 5).value("mode").toString(), QStringLiteral("full"));
        const auto stale = mode("ask", 3); // sent earlier, run later
        QVERIFY(stale.value("ok").toBool());
        QVERIFY(stale.value("stale").toBool());
        QCOMPARE(stale.value("mode").toString(), QStringLiteral("full"));
        const auto newer = mode("ask", 6);
        QCOMPARE(newer.value("mode").toString(), QStringLiteral("ask"));
        QVERIFY(!newer.value("stale").toBool());
        bool closed = false;
        pi.close([&closed] { closed = true; });
        QTRY_VERIFY_WITH_TIMEOUT(closed, 10000);
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
