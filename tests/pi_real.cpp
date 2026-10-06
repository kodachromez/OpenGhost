// PiBackend against the real `pi` on PATH (skipped without one): the real bridge
// and access policy inside real Pi, with Pi AI's faux provider as the model
// (tests/pi/real/faux.ts) in a throwaway agent directory. No network, no
// credentials, nothing of the user's Pi configuration. The faux model asks for
// exactly the tool call the prompt names, so every approval decision here is
// Pi's own tool_call path, and every file effect is checked on disk.
#include "backend/pi_backend.h"
#include "frontend/chat_service.h"
#include "frontend/library.h"
#include "frontend/preferences.h"
#include "frontend/store.h"
#include <QDir>
#include <QFile>
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

    void askAsksBeforeCommandsAndHonoursTheAnswer()
    {
        Harness h(m_folder);
        const auto made = m_folder + QStringLiteral("/made.txt");
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch made.txt"}}));
        QTRY_COMPARE_WITH_TIMEOUT(h.chat.approvals().size(), 1, 30000);
        const auto card = h.chat.approvals().first().data;
        QCOMPARE(card.tool, QStringLiteral("bash"));
        QCOMPARE(card.args.value("command").toString(), QStringLiteral("touch made.txt"));
        QVERIFY(card.presentation);
        QCOMPARE(card.presentation->kind, QStringLiteral("command"));
        QCOMPARE(card.presentation->title, QStringLiteral("Run a command"));
        QCOMPARE(card.presentation->effect, std::optional(QStringLiteral("change")));
        QCOMPARE(card.presentation->code, std::optional(QStringLiteral("touch made.txt")));
        QVERIFY(!QFile::exists(made)); // Nothing ran while it waits.
        h.chat.approve(h.chat.approvals().first().request, false);
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(
            h.reply().contains(QStringLiteral("done: error: The user did not allow this step.")));
        QVERIFY(!QFile::exists(made));

        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "touch made.txt"}}));
        QTRY_COMPARE_WITH_TIMEOUT(h.chat.approvals().size(), 1, 30000);
        h.chat.approve(h.chat.approvals().first().request, true);
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.reply().contains(QStringLiteral("done: ok")));
        QVERIFY(QFile::exists(made));

        // Reading inside the folder needs nothing; outside it, Ask asks.
        QVERIFY(h.call(QStringLiteral("read"), {{"path", "made.txt"}}));
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.reply().contains(QStringLiteral("done: ok")));
        QVERIFY(h.call(QStringLiteral("read"), {{"path", "@/etc/hostname"}}));
        QTRY_COMPARE_WITH_TIMEOUT(h.chat.approvals().size(), 1, 30000);
        QCOMPARE(h.chat.approvals().first().data.presentation->title,
                 QStringLiteral("Read a file outside the project"));
        h.chat.approve(h.chat.approvals().first().request, false);
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
    }

    void autoWorksInTheFolderAndAsksBeforeRiskOrOutside()
    {
        Harness h(m_folder);
        h.chat.setMode(PermissionMode::Auto);
        QVERIFY(h.call(QStringLiteral("write"), {{"path", "auto.txt"}, {"content", "x"}}));
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.chat.approvals().isEmpty());
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/auto.txt")));
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "ls"}}));
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.reply().contains(QStringLiteral("done: ok: auto.txt")));
        // Deleting, and writing through a link that leads outside, are asked about.
        QVERIFY(h.call(QStringLiteral("bash"), {{"command", "rm auto.txt"}}));
        QTRY_COMPARE_WITH_TIMEOUT(h.chat.approvals().size(), 1, 30000);
        QCOMPARE(h.chat.approvals().first().data.presentation->effect,
                 std::optional(QStringLiteral("delete")));
        h.chat.approve(h.chat.approvals().first().request, false);
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/auto.txt")));
        const auto outside = m_dir.filePath(QStringLiteral("outside"));
        QVERIFY(QDir().mkpath(outside));
        QVERIFY(QFile::link(outside, m_folder + QStringLiteral("/link")));
        QVERIFY(h.call(QStringLiteral("write"), {{"path", "link/escaped.txt"}, {"content", "x"}}));
        QTRY_COMPARE_WITH_TIMEOUT(h.chat.approvals().size(), 1, 30000);
        h.chat.approve(h.chat.approvals().first().request, false);
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(!QFile::exists(outside + QStringLiteral("/escaped.txt")));
    }

    void fullNeverAsks()
    {
        Harness h(m_folder);
        h.chat.setMode(PermissionMode::Full);
        QVERIFY(h.call(QStringLiteral("bash"),
                       {{"command", "touch full.txt && rm full.txt && echo gone"}}));
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.chat.approvals().isEmpty());
        QVERIFY(h.reply().contains(QStringLiteral("done: ok: gone")));
    }

    void aModeThatNoLongerAsksReleasesTheWaitingCall()
    {
        Harness h(m_folder);
        QVERIFY(h.call(QStringLiteral("write"), {{"path", "m.txt"}, {"content", "x"}}));
        QTRY_COMPARE_WITH_TIMEOUT(h.chat.approvals().size(), 1, 30000);
        h.chat.setMode(PermissionMode::Auto); // Auto writes in the folder on its own.
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.chat.approvals().isEmpty());
        QCOMPARE(h.chat.current().mode, PermissionMode::Auto);
        QVERIFY(QFile::exists(m_folder + QStringLiteral("/m.txt")));
        QVERIFY(h.reply().contains(QStringLiteral("done: ok")));
    }

    void stopWithdrawsTheWaitingCall()
    {
        Harness h(m_folder);
        QVERIFY(h.call(QStringLiteral("write"), {{"path", "s.txt"}, {"content", "x"}}));
        QTRY_COMPARE_WITH_TIMEOUT(h.chat.approvals().size(), 1, 30000);
        h.chat.stop();
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QVERIFY(h.chat.approvals().isEmpty());
        QVERIFY(!QFile::exists(m_folder + QStringLiteral("/s.txt")));
        QVERIFY(h.chat.send(QStringLiteral("still here?"))); // The chat goes on.
        QTRY_VERIFY_WITH_TIMEOUT(h.settled(), 30000);
        QCOMPARE(h.reply(), QStringLiteral("echo: still here?"));
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
