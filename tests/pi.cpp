// PiBackend's turn lifecycle against tests/pi/pi, a scripted stand-in for
// `pi --mode rpc` (no model, network or credentials): real acceptance, final
// errors, Stop, steering receipts, failed-turn Retry and usage; one Pi session
// per chat (isolation, side-by-side runs, restart recovery, deletion), per-chat
// models, standing instructions and pinned files, and message attachments.
#include "backend/pi_backend.h"
#include "frontend/chat_service.h"
#include "frontend/library.h"
#include "frontend/preferences.h"
#include "frontend/store.h"
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace openghost;
namespace
{
struct Harness {
    // `dir` set: a profile that outlives this harness (Pi's sessions and the
    // library), as across an app restart. Empty: nothing outlives it.
    PiBackend backend;
    PreferencesStore prefs{QString()};
    MemoryKeyStore memory;
    std::unique_ptr<FileKeyStore> files;
    Library library;
    ChatService chat;
    QVector<Usage> usage;
    explicit Harness(const QString &dir = {}, const QString &instructions = {})
        : backend(dir.isEmpty() ? QString() : dir + QStringLiteral("/pi")),
          files(dir.isEmpty() ? nullptr
                              : std::make_unique<FileKeyStore>(dir + QStringLiteral("/library"))),
          library(files ? static_cast<KeyStore *>(files.get()) : &memory),
          chat(&backend, &prefs, &library)
    {
        QObject::connect(&chat, &ChatService::usageRecorded,
                         [this](const Usage &spent) { usage.append(spent); });
        if (!instructions.isEmpty())
            instruct(instructions);
        chat.initialize();
    }
    void instruct(const QString &instructions)
    {
        auto value = prefs.value();
        value.userContext.instructions = instructions;
        prefs.save(value);
    }
    const DisplayRow *last(DisplayRow::Role role) const
    {
        const auto &rows = chat.current().rows;
        for (auto it = rows.crbegin(); it != rows.crend(); ++it)
            if (it->role == role && !it->hidden)
                return &*it;
        return nullptr;
    }
    QString text(DisplayRow::Role role) const
    {
        const auto *row = last(role);
        return row ? row->text : QString();
    }
    QString state(const QString &userText) const
    {
        for (const auto &row : chat.current().rows)
            if (row.role == DisplayRow::Role::User && row.text == userText)
                return row.state;
        return {};
    }
    int count(DisplayRow::Role role) const
    {
        return std::count_if(chat.current().rows.cbegin(), chat.current().rows.cend(),
                             [role](const DisplayRow &row) { return row.role == role; });
    }
    bool settled() const { return chat.ready() && !chat.busy() && !chat.pending(); }
};
Result ask(Backend &backend, const Command &command)
{
    static RequestId next = 1000;
    const auto id = ++next;
    std::optional<Result> answer;
    QEventLoop loop;
    const auto connection = QObject::connect(&backend, &Backend::replied, &loop,
                                             [&](RequestId got, const Result &result) {
                                                 if (got == id) {
                                                     answer = result;
                                                     loop.quit();
                                                 }
                                             });
    backend.request(id, command);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection);
    return answer.value_or(Result{Error{QStringLiteral("test_timeout"), {}, {}, {}, {}, {}}});
}
Attachment textFile(const QString &name, const QString &text)
{
    Attachment file;
    file.id = name;
    file.kind = Attachment::Kind::Text;
    file.mime = QStringLiteral("text/plain");
    file.name = name;
    file.text = text;
    file.size = text.toUtf8().size();
    return file;
}
// A 1×1 PNG, as the composer prepares one.
Attachment picture(const QString &name)
{
    Attachment file;
    file.id = name;
    file.kind = Attachment::Kind::Image;
    file.mime = QStringLiteral("image/png");
    file.name = name;
    file.dataUrl = QStringLiteral("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAf"
                                  "FcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==");
    file.size = 70;
    file.width = file.height = 1;
    return file;
}
QString code(const Result &result)
{
    const auto *error = std::get_if<Error>(&result);
    return error ? error->code : QString();
}
// A Pi session file's entries (after its header), as Pi wrote them.
QJsonArray piEntries(const QString &path)
{
    QFile file(path);
    QJsonArray entries;
    if (!file.open(QIODevice::ReadOnly))
        return entries;
    for (const auto &line : file.readAll().split('\n'))
        if (!line.isEmpty())
            entries.append(QJsonDocument::fromJson(line).object());
    if (!entries.isEmpty())
        entries.removeFirst();
    return entries;
}
QStringList userTexts(const QJsonArray &entries)
{
    QStringList texts;
    for (const auto &entry : entries) {
        const auto message = entry.toObject().value("message").toObject();
        if (message.value("role").toString() == QStringLiteral("user"))
            texts.append(
                message.value("content").toArray().first().toObject().value("text").toString());
    }
    return texts;
}
} // namespace

class PiTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    int m_run = 0;
    QString m_log;
    QVector<QJsonObject> m_records;
    // What every fake Pi child received (and each run's instructions), in order.
    QVector<QJsonObject> records() const
    {
        QFile file(m_log);
        QVector<QJsonObject> list;
        if (!file.open(QIODevice::ReadOnly))
            return list;
        for (const auto &line : file.readAll().split('\n'))
            if (!line.isEmpty())
                list.append(QJsonDocument::fromJson(line).object());
        return list;
    }
    QStringList commands(const QString &session = {}) const
    {
        QStringList list;
        for (const auto &command : records())
            if (session.isEmpty() || command.value("session").toString() == session)
                list.append(command.value("type").toString() + ' ' +
                            command.value("message").toString());
        return list;
    }
    // The fake's name for a chat's session (its file name).
    static QString piName(const Harness &h, const QString &session)
    {
        return QFileInfo(h.backend.sessionFile(session)).fileName();
    }

  private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("PATH", QByteArray(OPENGHOST_FAKE_PI_DIR ":") + qgetenv("PATH"));
    }
    void init()
    {
        m_log = m_dir.filePath(QStringLiteral("log-%1").arg(++m_run));
        qputenv("FAKE_PI_LOG", m_log.toUtf8());
        qunsetenv("FAKE_PI_SLOW_EXIT");
        qunsetenv("FAKE_PI_BAD_DEFAULT");
    }

    void answersOnlyAfterPiAccepts()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QSignalSpy accepted(&h.chat, &ChatService::accepted);
        QVERIFY(h.chat.send(QStringLiteral("hello")));
        QCOMPARE(accepted.count(), 0); // Pi has not answered yet.
        QTRY_VERIFY(h.settled());
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Echo: hello"));
        QCOMPARE(h.count(DisplayRow::Role::Note), 0);
        // One increment per Pi message: cached input is part of OpenGhost's input.
        QCOMPARE(h.usage.size(), 1);
        QCOMPARE(h.usage[0].provider, QStringLiteral("p"));
        QCOMPARE(h.usage[0].model, QStringLiteral("m2")); // Pi's own default model
        QCOMPARE(h.usage[0].input, 135.0);
        QCOMPARE(h.usage[0].cached, 30.0);
        QCOMPARE(h.usage[0].written, 5.0);
        QCOMPARE(h.usage[0].output, 20.0);
        const auto *reply = h.last(DisplayRow::Role::Assistant);
        QVERIFY(reply->usage && reply->usage->input == 135.0 && reply->usage->output == 20.0);
        QVERIFY(reply->started > 0 && reply->completed >= reply->started);
    }

    void rejectionKeepsTheDraft()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("hello")));
        QTRY_VERIFY(h.settled());
        QSignalSpy accepted(&h.chat, &ChatService::accepted);
        QSignalSpy failed(&h.chat, &ChatService::submissionFailed);
        QVERIFY(h.chat.send(QStringLiteral("reject")));
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(accepted.count(), 0); // The composer keeps its draft.
        QVERIFY(h.chat.status().contains(QStringLiteral("No API key found for p.")));
        QCOMPARE(h.state(QStringLiteral("reject")), QStringLiteral("unconfirmed"));
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Echo: hello"));
        QVERIFY(!h.chat.busy());
    }

    void finalErrorsAndStopReasonsAreNotSuccess()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("length")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Note),
                 QStringLiteral("The response reached its output limit."));
        QVERIFY(!h.chat.canRetry());
        QVERIFY(h.chat.send(QStringLiteral("tools")));
        QTRY_VERIFY(h.settled());
        // Pi's separate assistant messages stay separate within the reply.
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Let me check.\n\nDone."));
        QCOMPARE(h.usage.size(), 3);
        QCOMPARE(h.usage[2].input, 100.0);
        QCOMPARE(h.usage[2].cached, 40.0);
        QVERIFY(h.chat.send(QStringLiteral("handled")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Note),
                 QStringLiteral("Handled as a command; no reply was started."));
        QVERIFY(h.chat.send(QStringLiteral("fail")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Note), QStringLiteral("boom 500"));
        QCOMPARE(h.last(DisplayRow::Role::Note)->state, QStringLiteral("error"));
        QVERIFY(h.chat.canRetry());
        QCOMPARE(h.usage.size(), 4); // A failed attempt's spend is real too.
    }

    void retryContinuesTheFailedTurnExactly()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("fail")));
        QTRY_VERIFY(h.settled());
        QVERIFY(h.chat.canRetry());
        const int users = h.count(DisplayRow::Role::User);
        h.chat.retry();
        QTRY_COMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Recovered."));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.count(DisplayRow::Role::User), users); // No new or repeated input.
        QVERIFY(!h.chat.canRetry());
        const auto sent = commands();
        QCOMPARE(sent.count(QStringLiteral("prompt fail")), 1);
        QCOMPARE(std::count_if(sent.cbegin(), sent.cend(),
                               [](const QString &c) { return c.contains("\"op\":\"retry\""); }),
                 1);
        QCOMPARE(h.usage.size(), 2);
        QCOMPARE(h.usage[1].input, 12.0);
    }

    void stopAbortsPiAndDropsItsQueue()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("slow")));
        QTRY_COMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("working"));
        QTRY_VERIFY(h.chat.canSteer());
        QVERIFY(h.chat.send(QStringLiteral("later")));
        QTRY_COMPARE(h.state(QStringLiteral("later")), QStringLiteral("queued"));
        QTRY_VERIFY(!h.chat.pending());
        h.chat.stop();
        QVERIFY(!h.chat.busy()); // Frozen at once…
        QTRY_VERIFY(h.settled()); // …and settled only by Pi's abort.
        const auto sent = commands();
        const auto cleared = sent.indexOf(QStringLiteral("clear_queue "));
        QVERIFY(cleared >= 0 && cleared < sent.indexOf(QStringLiteral("abort ")));
        QCOMPARE(h.state(QStringLiteral("later")), QStringLiteral("unconfirmed"));
        QCOMPARE(h.text(DisplayRow::Role::Note), QStringLiteral("Stopped."));
        // The aborted message's spend still counts, after Stop.
        QCOMPARE(h.usage.size(), 1);
        QCOMPARE(h.usage[0].output, 3.0);
        QVERIFY(h.chat.send(QStringLiteral("hello")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Echo: hello"));
    }

    void stopBeforeAcceptanceAbortsTheLateTurn()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("delay")));
        QTRY_VERIFY(commands().contains(QStringLiteral("prompt delay"))); // sent, not yet admitted
        h.chat.stop();
        QVERIFY(!h.chat.busy());
        QTRY_VERIFY(commands().contains(QStringLiteral("abort ")));
        QTRY_VERIFY(!h.chat.pending());
        QVERIFY(!h.chat.ready()); // Uncertain until reconciled, never resent.
        h.chat.retry();
        QTRY_VERIFY(h.settled());
        QCOMPARE(commands().count(QStringLiteral("prompt delay")), 1);
        QVERIFY(h.chat.send(QStringLiteral("hello")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Echo: hello"));
    }

    void steeringReceiptsFollowPi()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("slow")));
        QTRY_VERIFY(h.chat.canSteer());
        const QStringList inputs{QStringLiteral("/t first"), QStringLiteral("dup"),
                                 QStringLiteral("dup")};
        for (const auto &input : inputs) {
            QVERIFY(h.chat.send(input));
            QTRY_VERIFY(!h.chat.pending());
        }
        QCOMPARE(h.state(QStringLiteral("dup")), QStringLiteral("queued"));
        // A text file steers with its contents, as a start sends them.
        QVERIFY(h.chat.send({}, {textFile(QStringLiteral("a.txt"), QStringLiteral("x"))}));
        QTRY_VERIFY(!h.chat.pending());
        // A picture is refused, never sent without its picture.
        QVERIFY(h.chat.send(QStringLiteral("look"), {picture(QStringLiteral("p.png"))}));
        QTRY_VERIFY(!h.chat.pending());
        QVERIFY(h.chat.send(QStringLiteral("dup now")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Steered."));
        int applied = 0;
        for (const auto &row : h.chat.current().rows)
            if (row.role == DisplayRow::Role::User && !row.clientInputId.isEmpty()) {
                if (row.attachments.isEmpty() || !row.attachments.first().image.value_or(false))
                    applied += row.state == QStringLiteral("applied");
                else
                    QCOMPARE(row.state, QStringLiteral("notApplied")); // Never dropped.
            }
        // Transformed and identical texts and the file each matched once.
        QCOMPARE(applied, 5);
        QVERIFY(commands().contains(QStringLiteral("steer <file name=\"a.txt\">\nx\n</file>\n")));
        for (const auto &record : records())
            QVERIFY(!record.contains("images"));
    }

    void identitiesAreChecked()
    {
        PiBackend backend;
        QVERIFY(std::holds_alternative<Reply>(ask(backend, Initialize{})));
        bool completed = false;
        QObject::connect(&backend, &Backend::sessionEvent, [&](const SessionEvent &event) {
            completed |= std::holds_alternative<TurnCompleted>(event.payload);
        });
        StartTurn start;
        start.sessionId = QStringLiteral("chat");
        start.clientTurnId = QStringLiteral("client-1");
        start.input.text = QStringLiteral("hello");
        start.params.selection = {QStringLiteral("p"), QStringLiteral("m"), {}};
        const auto first = ask(backend, start);
        const auto accepted = std::get<StartAccepted>(std::get<Reply>(first));
        QTRY_VERIFY(completed);
        // The same start again is the same acceptance, not a second prompt.
        const auto again = std::get<StartAccepted>(std::get<Reply>(ask(backend, start)));
        QCOMPARE(again.turnId, accepted.turnId);
        QCOMPARE(commands().count(QStringLiteral("prompt hello")), 1);
        start.clientTurnId = QStringLiteral("client-2");
        QCOMPARE(code(ask(backend, start)), QStringLiteral("session_conflict")); // create-only
        start.sessionVersion = QStringLiteral("other");
        QCOMPARE(code(ask(backend, start)), QStringLiteral("session_conflict"));
        RetryTurn retry{QStringLiteral("chat"), accepted.sessionVersion,
                        QStringLiteral("client-3"), accepted.turnId, {}};
        QCOMPARE(code(ask(backend, retry)), QStringLiteral("invalid_request")); // it succeeded
        QCOMPARE(code(ask(backend, CancelTurn{QStringLiteral("chat"), QStringLiteral("nope")})),
                 QStringLiteral("stale_turn"));
        QVERIFY(std::holds_alternative<Reply>(
            ask(backend, CancelTurn{QStringLiteral("chat"), accepted.turnId})));
        const auto missing = std::get<SessionRecovery>(
            std::get<Reply>(ask(backend, GetSession{QStringLiteral("other"), {}})));
        QVERIFY(std::holds_alternative<MissingSession>(missing));
        const auto known = std::get<ExistingSession>(std::get<SessionRecovery>(std::get<Reply>(
            ask(backend, GetSession{QStringLiteral("chat"), QStringLiteral("client-1")}))));
        QVERIFY(known.turn && known.turn->turnId == accepted.turnId);
        QVERIFY(known.turn->input && known.turn->input->text == QStringLiteral("hello"));
        const auto never = std::get<ExistingSession>(std::get<SessionRecovery>(std::get<Reply>(
            ask(backend, GetSession{QStringLiteral("chat"), QStringLiteral("client-2")}))));
        QVERIFY(!never.turn); // Never accepted, so never recovered.
    }
    void newChatsHaveTheirOwnPiSessions()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("first")));
        QTRY_VERIFY(h.settled());
        const auto a = h.chat.current().id;
        h.chat.newChat();
        QVERIFY(h.chat.send(QStringLiteral("second")));
        QTRY_VERIFY(h.settled());
        const auto b = h.chat.current().id;
        QVERIFY(a != b);
        // Each chat is its own Pi session: neither Pi saw the other chat's prompt.
        QCOMPARE(userTexts(piEntries(h.backend.sessionFile(a))), QStringList{"first"});
        QCOMPARE(userTexts(piEntries(h.backend.sessionFile(b))), QStringList{"second"});
        QCOMPARE(commands(piName(h, b)).count(QStringLiteral("prompt first")), 0);
        // Back in the first chat, its next turn continues its own session.
        h.chat.open(a);
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Echo: first"));
        QVERIFY(h.chat.send(QStringLiteral("third")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Echo: third"));
        QCOMPARE(userTexts(piEntries(h.backend.sessionFile(a))),
                 (QStringList{QStringLiteral("first"), QStringLiteral("third")}));
        QCOMPARE(userTexts(piEntries(h.backend.sessionFile(b))), QStringList{"second"});
    }

    void chatsRunSideBySide()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("slow")));
        QTRY_VERIFY(h.chat.canSteer()); // Pi took it and is still answering.
        const auto a = h.chat.current().id;
        h.chat.newChat();
        QVERIFY(h.chat.send(QStringLiteral("hello"))); // Not refused as "busy".
        QTRY_VERIFY(h.settled());
        const auto b = h.chat.current().id;
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Echo: hello"));
        h.chat.open(a);
        QTRY_VERIFY(!h.chat.pending());
        QVERIFY(h.chat.ready() && h.chat.busy()); // Still its own reply, still running.
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("working"));
        h.chat.stop();
        QTRY_VERIFY(h.settled());
        QCOMPARE(commands(piName(h, a)).count(QStringLiteral("abort ")), 1);
        QCOMPARE(commands(piName(h, b)).count(QStringLiteral("abort ")), 0);
        h.chat.open(b);
        QTRY_VERIFY(h.settled());
        for (const auto &row : h.chat.current().rows)
            QVERIFY(row.text != QStringLiteral("working") && row.text != QStringLiteral("slow"));
    }

    void restartRecoversTheChatFromPi()
    {
        QTemporaryDir profile;
        QString a;
        {
            Harness h(profile.path());
            QTRY_VERIFY(h.chat.ready());
            QVERIFY(h.chat.send(QStringLiteral("hello")));
            QTRY_VERIFY(h.settled());
            a = h.chat.current().id;
            QVERIFY(h.chat.send(QStringLiteral("slow")));
            QTRY_VERIFY(h.chat.canSteer());
        } // OpenGhost exits while Pi is still answering.
        Harness h(profile.path());
        QTRY_VERIFY(h.chat.ready());
        QCOMPARE(h.chat.chats().size(), 1);
        h.chat.open(a);
        QTRY_VERIFY(!h.chat.pending());
        QVERIFY(h.chat.current().reconciled);
        // The finished reply is kept; the cut-off one is shown as Pi recorded it:
        // taken, then never finished. Nothing is resent.
        QCOMPARE(h.state(QStringLiteral("slow")), QStringLiteral("done"));
        QCOMPARE(h.text(DisplayRow::Role::Note),
                 QStringLiteral("Pi stopped before this reply finished. Nothing was resent."));
        QVERIFY(std::any_of(h.chat.current().rows.cbegin(), h.chat.current().rows.cend(),
                            [](const DisplayRow &row) { return row.text == "Echo: hello"; }));
        QCOMPARE(commands().count(QStringLiteral("prompt slow")), 1);
        // The chat goes on in the same Pi session.
        QVERIFY(h.chat.send(QStringLiteral("again")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Echo: again"));
        QCOMPARE(userTexts(piEntries(h.backend.sessionFile(a))),
                 (QStringList{QStringLiteral("hello"), QStringLiteral("slow"),
                              QStringLiteral("again")}));
    }

    void piRecordsAnswerSessionGetAfterRestart()
    {
        QTemporaryDir profile;
        StartAccepted accepted;
        StartTurn start;
        start.sessionId = QStringLiteral("chat");
        start.clientTurnId = QStringLiteral("client-1");
        start.input.text = QStringLiteral("hello");
        start.params.selection = {QStringLiteral("p"), QStringLiteral("m"), {}};
        {
            PiBackend backend(profile.path());
            QVERIFY(std::holds_alternative<Reply>(ask(backend, Initialize{})));
            bool completed = false;
            QObject::connect(&backend, &Backend::sessionEvent, [&](const SessionEvent &event) {
                completed |= std::holds_alternative<TurnCompleted>(event.payload);
            });
            accepted = std::get<StartAccepted>(std::get<Reply>(ask(backend, start)));
            QTRY_VERIFY(completed);
            StartTurn refused = start;
            refused.clientTurnId = QStringLiteral("client-2");
            refused.sessionVersion = accepted.sessionVersion;
            refused.input.text = QStringLiteral("reject");
            QCOMPARE(code(ask(backend, refused)), QStringLiteral("rejected"));
        } // The frontend never saw the turn end; Pi's session has it.
        PiBackend backend(profile.path());
        QVERIFY(std::holds_alternative<Reply>(ask(backend, Initialize{})));
        const auto known = std::get<ExistingSession>(std::get<SessionRecovery>(std::get<Reply>(
            ask(backend, GetSession{QStringLiteral("chat"), QStringLiteral("client-1")}))));
        QCOMPARE(known.sessionVersion, accepted.sessionVersion);
        QVERIFY(known.turn && known.turn->turnId == accepted.turnId);
        QVERIFY(known.turn->input && known.turn->input->text == QStringLiteral("hello"));
        const auto &events = known.turn->events;
        QVERIFY(events.size() >= 4);
        QVERIFY(std::holds_alternative<TurnStarted>(events.first().payload));
        const auto *done = std::get_if<TurnCompleted>(&events.last().payload);
        QVERIFY(done && done->status == TurnStatus::Done);
        bool text = false, spent = false;
        Sequence previous = -1;
        for (const auto &event : events) {
            QVERIFY(event.identity.seq > previous && event.identity.seq <= known.revision);
            previous = event.identity.seq;
            if (const auto *message = std::get_if<MessageCompleted>(&event.payload))
                text |= message->text == QStringLiteral("Echo: hello");
            spent |= std::holds_alternative<Usage>(event.payload);
        }
        QVERIFY(text && spent);
        // The same start again, after the restart, is that acceptance, never a new prompt.
        const auto again = std::get<StartAccepted>(std::get<Reply>(ask(backend, start)));
        QCOMPARE(again.turnId, accepted.turnId);
        QCOMPARE(again.sessionVersion, accepted.sessionVersion);
        QCOMPARE(commands().count(QStringLiteral("prompt hello")), 1);
        // The start Pi refused is not recovered as taken.
        const auto refused = std::get<ExistingSession>(std::get<SessionRecovery>(std::get<Reply>(
            ask(backend, GetSession{QStringLiteral("chat"), QStringLiteral("client-2")}))));
        QVERIFY(!refused.turn);
        // The chat still exists: never created anew, and it continues.
        start.clientTurnId = QStringLiteral("client-3");
        QCOMPARE(code(ask(backend, start)), QStringLiteral("session_conflict"));
        start.sessionVersion = accepted.sessionVersion;
        start.input.text = QStringLiteral("more");
        QVERIFY(std::holds_alternative<Reply>(ask(backend, start)));
        QTRY_COMPARE(userTexts(piEntries(backend.sessionFile(QStringLiteral("chat")))),
                     (QStringList{QStringLiteral("hello"), QStringLiteral("more")}));
    }

    void recoveryRequiresSettlement()
    {
        QTemporaryDir profile;
        QString session, client;
        {
            Harness h(profile.path());
            QTRY_VERIFY(h.chat.ready());
            QVERIFY(h.chat.send(QStringLiteral("unsettled")));
            QTRY_COMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("An intermediate answer."));
            QVERIFY(h.chat.busy());
            session = h.chat.current().id;
            client = h.chat.current().turn.clientId;
        }
        PiBackend backend(profile.path() + QStringLiteral("/pi"));
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        const auto recovered = std::get<ExistingSession>(std::get<SessionRecovery>(std::get<Reply>(
            ask(backend, GetSession{session, client}))));
        QVERIFY(recovered.turn);
        const auto done = std::get<TurnCompleted>(recovered.turn->events.last().payload);
        QCOMPARE(done.status, TurnStatus::Error);
        QVERIFY(done.error && done.error->code == QStringLiteral("interrupted"));
    }

    void recoveredTurnExcludesMessagesAfterItsEnd()
    {
        QTemporaryDir profile;
        QString session, client, path;
        {
            Harness h(profile.path());
            QTRY_VERIFY(h.chat.ready());
            QVERIFY(h.chat.send(QStringLiteral("hello")));
            QTRY_VERIFY(h.settled());
            session = h.chat.current().id;
            client = h.chat.current().turn.clientId;
            path = h.backend.sessionFile(session);
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        const QJsonObject later{{"type", "message"}, {"id", "extension-message"},
                               {"message", QJsonObject{{"role", "assistant"}, {"content", "unrelated extension output"},
                                                       {"stopReason", "stop"}}}};
        QVERIFY(file.write(QJsonDocument(later).toJson(QJsonDocument::Compact) + '\n') > 0);
        file.close();
        PiBackend backend(profile.path() + QStringLiteral("/pi"));
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        const auto recovered = std::get<ExistingSession>(std::get<SessionRecovery>(std::get<Reply>(
            ask(backend, GetSession{session, client}))));
        QVERIFY(recovered.turn);
        int messages = 0;
        for (const auto &event : recovered.turn->events)
            if (const auto *message = std::get_if<MessageCompleted>(&event.payload)) {
                ++messages;
                QCOMPARE(message->text.value_or(QString()), QStringLiteral("Echo: hello"));
            }
        QCOMPARE(messages, 1);
    }

    void deletionWaitsForAnAlreadyRetiringChild()
    {
        qputenv("FAKE_PI_SLOW_EXIT", "1");
        // All children inherit it before the parent environment is restored.
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QString first;
        for (int i = 0; i < 6; ++i) {
            h.chat.newChat();
            QVERIFY(h.chat.send(QString::number(i)));
            QTRY_VERIFY(h.settled());
            if (i == 0)
                first = h.chat.current().id;
        } // The idle-child bound has begun closing the first child.
        qunsetenv("FAKE_PI_SLOW_EXIT");
        const auto file = h.backend.sessionFile(first);
        QVERIFY(QFile::exists(file));
        QVERIFY(code(ask(h.backend, DeleteSession{first})).isEmpty());
        QVERIFY(!QFile::exists(file));
        QTest::qWait(600);
        QVERIFY2(!QFile::exists(file), "A retiring Pi child recreated the deleted session");
    }

    void deleteStopsPiAndRemovesItsSession()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("hello")));
        QTRY_VERIFY(h.settled());
        const auto a = h.chat.current().id;
        const auto file = h.backend.sessionFile(a);
        QVERIFY(QFile::exists(file));
        QVERIFY(h.chat.send(QStringLiteral("slow")));
        QTRY_VERIFY(h.chat.canSteer());
        QSignalSpy removed(&h.chat, &ChatService::removed);
        h.chat.remove(a);
        QTRY_COMPARE(removed.count(), 1);
        QVERIFY(removed[0][1].toBool());
        // Pi stopped the reply before its session was erased.
        const auto sent = commands(piName(h, a));
        QVERIFY(sent.contains(QStringLiteral("abort ")));
        QVERIFY(!QFile::exists(file));
        QVERIFY(!QFile::exists(h.backend.sessionFile(a + QStringLiteral(":mini"))));
        QVERIFY(std::holds_alternative<MissingSession>(std::get<SessionRecovery>(
            std::get<Reply>(ask(h.backend, GetSession{a, {}})))));
        // Deleting what is already gone succeeds, so a retried delete is safe.
        QVERIFY(std::holds_alternative<Reply>(ask(h.backend, DeleteSession{a})));
        // A new chat inherits nothing from the deleted one.
        QTRY_VERIFY(h.settled());
        QVERIFY(h.chat.send(QStringLiteral("fresh")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(userTexts(piEntries(h.backend.sessionFile(h.chat.current().id))),
                 QStringList{"fresh"});
    }

    void folderChatsRunInTheirFolderAndDeleteWithIt()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QTemporaryDir folder;
        QVERIFY(h.chat.addFolder(folder.path()).isEmpty());
        h.chat.newChat(folder.path());
        QVERIFY(h.chat.send(QStringLiteral("hello")));
        QTRY_VERIFY(h.settled());
        const auto file = h.backend.sessionFile(h.chat.current().id);
        QFile header(file);
        QVERIFY(header.open(QIODevice::ReadOnly));
        QCOMPARE(QFileInfo(QJsonDocument::fromJson(header.readLine()).object().value("cwd").toString())
                     .canonicalFilePath(),
                 QFileInfo(folder.path()).canonicalFilePath());
        header.close();
        QSignalSpy removed(&h.chat, &ChatService::folderRemoved);
        h.chat.removeFolder(folder.path());
        QTRY_COMPARE(removed.count(), 1);
        QVERIFY(removed[0][1].toBool());
        QVERIFY(!QFile::exists(file));
    }

    void folderDeletionFailureKeepsTheRemainder()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QTemporaryDir folder;
        QVERIFY(h.chat.addFolder(folder.path()).isEmpty());
        for (int i = 0; i < 3; ++i) {
            h.chat.newChat(folder.path());
            QVERIFY(h.chat.send(QString::number(i)));
            QTRY_VERIFY(h.settled());
        }
        const auto ids = h.library.inFolder(folder.path());
        QCOMPARE(ids.size(), 3);
        const auto blocked = h.backend.sessionFile(ids[1]);
        QVERIFY(QFile::rename(blocked, blocked + QStringLiteral(".saved")));
        QVERIFY(QDir().mkpath(blocked)); // deterministic remove failure, even as root
        QSignalSpy removed(&h.chat, &ChatService::folderRemoved);
        h.chat.removeFolder(folder.path());
        QTRY_COMPARE(removed.size(), 1);
        QVERIFY(!removed[0][1].toBool());
        QVERIFY(!QFile::exists(h.backend.sessionFile(ids[0])));
        QVERIFY(QFile::exists(h.backend.sessionFile(ids[2])));
        QCOMPARE(h.library.inFolder(folder.path()), (QStringList{ids[1], ids[2]}));
        QVERIFY(h.library.folder(folder.path()));
        QVERIFY(QDir().rmdir(blocked));
        QVERIFY(QFile::rename(blocked + QStringLiteral(".saved"), blocked));
        h.chat.removeFolder(folder.path());
        QTRY_COMPARE(removed.size(), 2);
        QVERIFY(removed[1][1].toBool());
        QVERIFY(!h.library.folder(folder.path()));
    }

    void miniHistoryIsSeparateAndDeletedWithItsParent()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("main history")));
        QTRY_VERIFY(h.settled());
        const auto id = h.chat.current().id;
        QVERIFY(h.chat.openMini().isEmpty());
        QTRY_VERIFY(h.chat.mini() && h.chat.mini()->reconciled);
        QVERIFY(h.chat.sendMini(QStringLiteral("mini history")));
        QTRY_VERIFY(h.chat.mini()->turn.terminal);
        const auto miniFile = h.backend.sessionFile(id + QStringLiteral(":mini"));
        QCOMPARE(userTexts(piEntries(miniFile)), QStringList{"mini history"});
        QCOMPARE(userTexts(piEntries(h.backend.sessionFile(id))), QStringList{"main history"});
        QSignalSpy removed(&h.chat, &ChatService::removed);
        h.chat.remove(id);
        QTRY_COMPARE(removed.size(), 1);
        QVERIFY(removed[0][1].toBool());
        QVERIFY(!QFile::exists(miniFile));
        QVERIFY(!QFile::exists(h.backend.sessionFile(id)));
    }

    void modelsArePerChatAndCheckedAgainstPi()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        // Pi's own default model leads the catalog: a chat with no saved choice uses it.
        QCOMPARE(h.chat.models().first().id, QStringLiteral("m2"));
        QCOMPARE(h.chat.current().selection.model, QStringLiteral("m2"));
        QVERIFY(h.chat.send(QStringLiteral("hello")));
        QTRY_VERIFY(h.settled());
        const auto a = h.chat.current().id;
        const auto setModels = [this](const QString &session, const QString &model) {
            return std::count_if(m_records.cbegin(), m_records.cend(), [&](const QJsonObject &r) {
                return r.value("type").toString() == QStringLiteral("set_model") &&
                       r.value("session").toString() == session &&
                       r.value("modelId").toString() == model;
            });
        };
        // Choosing in a chat with history switches that chat's own Pi; Pi's answer is
        // what the chat shows.
        h.chat.choose({QStringLiteral("p"), QStringLiteral("m"), {}}, false);
        QTRY_VERIFY(!h.chat.pending());
        QCOMPARE(h.chat.current().selection.model, QStringLiteral("m"));
        m_records = records();
        QCOMPARE(setModels(piName(h, a), QStringLiteral("m")), 1);
        // A model Pi refuses leaves the chat's selection as it was.
        h.chat.choose({QStringLiteral("p"), QStringLiteral("gone"), {}}, false);
        QTRY_VERIFY(!h.chat.pending());
        QCOMPARE(h.chat.current().selection.model, QStringLiteral("m"));
        QVERIFY(h.chat.status().contains(QStringLiteral("Model not found")));
        // Another chat keeps its own model in its own Pi.
        h.chat.newChat();
        h.chat.choose({QStringLiteral("p"), QStringLiteral("m2"), {}}, false);
        QVERIFY(h.chat.send(QStringLiteral("other")));
        QTRY_VERIFY(h.settled());
        const auto b = h.chat.current().id;
        QCOMPARE(h.usage.last().model, QStringLiteral("m2"));
        m_records = records();
        QCOMPARE(setModels(piName(h, b), QStringLiteral("m")), 0);
        // Pi's selection is read before each prompt, never cached: when an extension
        // switches Pi's model, the chat's own choice is set again for its next turn.
        h.chat.open(a);
        QTRY_VERIFY(h.settled());
        QVERIFY(h.chat.send(QStringLiteral("switch")));
        QTRY_VERIFY(h.settled());
        QVERIFY(h.chat.send(QStringLiteral("after")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.usage.last().model, QStringLiteral("m"));
        m_records = records();
        QCOMPARE(setModels(piName(h, a), QStringLiteral("m")), 2);
    }

    void failedDefaultReadIsNotAnArbitrarySelection()
    {
        qputenv("FAKE_PI_BAD_DEFAULT", "1");
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        qunsetenv("FAKE_PI_BAD_DEFAULT");
        QCOMPARE(code(ask(backend, ModelsList{})), QStringLiteral("backend_error"));
    }

    void modelRepliesMustBeCanonical()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("hello")));
        QTRY_VERIFY(h.settled());
        h.chat.choose({QStringLiteral("p"), QStringLiteral("alias"), {}}, false);
        QTRY_VERIFY(!h.chat.pending());
        QCOMPARE(h.chat.current().selection.model, QStringLiteral("m"));
        h.chat.choose({QStringLiteral("p"), QStringLiteral("malformed"), {}}, false);
        QTRY_VERIFY(!h.chat.pending());
        QCOMPARE(h.chat.current().selection.model, QStringLiteral("m"));
        QVERIFY(h.chat.status().contains(QStringLiteral("model"), Qt::CaseInsensitive));
        // A draft's choice is canonicalized on its first start, not only ConfigureSession.
        h.chat.newChat();
        h.chat.choose({QStringLiteral("p"), QStringLiteral("alias"), {}}, false);
        QVERIFY(h.chat.send(QStringLiteral("hello")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.chat.current().selection.model, QStringLiteral("m"));
        const auto *saved = h.library.chat(h.chat.current().id);
        QVERIFY(saved);
        QCOMPARE(saved->model.value("model").toString(), QStringLiteral("m"));
        // A later failed reply's Retry also adopts the actual selection.
        QVERIFY(h.chat.send(QStringLiteral("fail")));
        QTRY_VERIFY(h.settled());
        h.chat.retry();
        QTRY_COMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Recovered."));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.chat.current().selection.model, QStringLiteral("m"));
    }

    void instructionsReachEveryRunAsSystemPrompt()
    {
        Harness h({}, QStringLiteral("Be brief."));
        QTRY_VERIFY(h.chat.ready());
        const auto runs = [this](const QString &session) {
            QStringList list;
            for (const auto &record : records())
                if (record.value("type").toString() == QStringLiteral("run") &&
                    record.value("session").toString() == session)
                    list.append(record.value("instructions").toString());
            return list;
        };
        QVERIFY(h.chat.send(QStringLiteral("hello")));
        QTRY_VERIFY(h.settled());
        const auto a = h.chat.current().id;
        QCOMPARE(runs(piName(h, a)), QStringList{"Be brief."});
        // They are the system prompt's, never Pi's history.
        QFile file(h.backend.sessionFile(a));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("Be brief."));
        // An edit applies from the next run; Retry's continuation carries them too.
        h.instruct(QStringLiteral("Be thorough."));
        QVERIFY(h.chat.send(QStringLiteral("fail")));
        QTRY_VERIFY(h.settled());
        h.chat.retry();
        QTRY_COMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Recovered."));
        QTRY_VERIFY(h.settled());
        h.instruct(QString());
        QVERIFY(h.chat.send(QStringLiteral("plain")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(runs(piName(h, a)),
                 (QStringList{QStringLiteral("Be brief."), QStringLiteral("Be thorough."),
                              QStringLiteral("Be thorough."), QString()}));
        // A new chat's Pi has them before its first run.
        h.instruct(QStringLiteral("New."));
        h.chat.newChat();
        QVERIFY(h.chat.send(QStringLiteral("hi")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(runs(piName(h, h.chat.current().id)), QStringList{"New."});
    }

    void attachmentsReachPiAsItsOwnFileInput()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        const auto prompts = [this] {
            QVector<QJsonObject> list;
            for (const auto &record : records())
                if (record.value("type").toString() == QStringLiteral("prompt") &&
                    !record.value("message").toString().startsWith(QStringLiteral("/openghost ")))
                    list.append(record);
            return list;
        };
        // A text file's whole contents precede the message, as `pi @file` puts them;
        // the chat shows the message and the file's card, not the contents.
        QVERIFY(h.chat.send(QStringLiteral("Summarize"),
                            {textFile(QStringLiteral("notes.txt"), QStringLiteral("alpha\nbeta"))}));
        QTRY_VERIFY(h.settled());
        QCOMPARE(prompts().last().value("message").toString(),
                 QStringLiteral("<file name=\"notes.txt\">\nalpha\nbeta\n</file>\nSummarize"));
        QCOMPARE(h.last(DisplayRow::Role::User)->text, QStringLiteral("Summarize"));
        QCOMPARE(h.last(DisplayRow::Role::User)->attachments.first().name,
                 QStringLiteral("notes.txt"));
        QVERIFY(h.text(DisplayRow::Role::Assistant).contains(QStringLiteral("alpha\nbeta")));
        const auto a = h.chat.current().id;
        // Attachment-only: the files are the whole prompt, and Pi answers it.
        QVERIFY(h.chat.send({}, {textFile(QStringLiteral("a\"b<.txt"), QStringLiteral("one")),
                                 textFile(QStringLiteral("two.md"), QStringLiteral("# two"))}));
        QTRY_VERIFY(h.settled());
        QCOMPARE(prompts().last().value("message").toString(),
                 QStringLiteral("<file name=\"a&quot;b&lt;.txt\">\none\n</file>\n"
                                "<file name=\"two.md\">\n# two\n</file>\n"));
        QCOMPARE(h.count(DisplayRow::Role::Note), 0);
        QVERIFY(h.text(DisplayRow::Role::Assistant).startsWith(QStringLiteral("Echo: <file")));
        // The contents are Pi's history (its session file); the display record
        // OpenGhost keeps there has names and sizes only.
        const auto saved = userTexts(piEntries(h.backend.sessionFile(a)));
        QVERIFY(saved.contains(QStringLiteral("<file name=\"notes.txt\">\nalpha\nbeta\n</file>\nSummarize")));

        // A picture goes only to a model Pi says sees images: refused before the
        // prompt, so nothing reaches Pi. (The window refuses it sooner, keeping the
        // draft; this is the backend's own check against Pi's actual model.)
        const auto before = prompts().size();
        const auto start = [](const QString &session, const QString &model,
                              const Attachment &file) {
            StartTurn command;
            command.sessionId = session;
            command.clientTurnId = session + QStringLiteral("-turn");
            command.input = {QStringLiteral("Look"), {file}};
            command.params.selection = {QStringLiteral("p"), model, {}};
            return command;
        };
        auto refused = ask(h.backend, start(QStringLiteral("seeless"), QStringLiteral("m2"),
                                            picture(QStringLiteral("dot.png"))));
        QCOMPARE(code(refused), QStringLiteral("unsupported_input"));
        QVERIFY(std::get<Error>(refused).message.contains(QStringLiteral("can't see pictures")));
        // An attachment Pi cannot take is refused whole, never sent without it.
        Attachment pdf;
        pdf.id = pdf.name = QStringLiteral("r.pdf");
        pdf.kind = Attachment::Kind::Pdf;
        pdf.size = 10;
        refused = ask(h.backend, start(QStringLiteral("pdf"), QStringLiteral("v"), pdf));
        QCOMPARE(code(refused), QStringLiteral("unsupported_input"));
        QCOMPARE(prompts().size(), before);
        h.chat.choose({QStringLiteral("p"), QStringLiteral("v"), {}}, false);
        QTRY_VERIFY(!h.chat.pending());
        QVERIFY(h.chat.send(QStringLiteral("Look"), {picture(QStringLiteral("dot.png"))}));
        QTRY_VERIFY(h.settled());
        const auto sent = prompts().last();
        QCOMPARE(sent.value("message").toString(),
                 QStringLiteral("<file name=\"dot.png\"></file>\nLook"));
        QCOMPARE(sent.value("images").toArray().size(), 1);
        QCOMPARE(sent.value("images").toArray().first().toObject().value("mimeType").toString(),
                 QStringLiteral("image/png"));
        QVERIFY(h.last(DisplayRow::Role::User)->attachments.first().image.value_or(false));
    }

    void pinnedFilesReachEveryRunAsSystemPrompt()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        const auto pin = [&h](const QVector<ContextFile> &files) {
            auto value = h.prefs.value();
            value.userContext.files = files;
            QVERIFY(h.prefs.save(value));
        };
        ContextFile cv;
        cv.id = QStringLiteral("f1");
        cv.name = QStringLiteral("cv.md");
        cv.kind = ContextFile::Kind::Text;
        cv.text = QStringLiteral("Pinned CV text");
        cv.size = 14;
        cv.path = QStringLiteral("/home/user/cv.md");
        pin({cv});
        const auto runs = [this](const QString &session) {
            QVector<QJsonArray> list;
            for (const auto &record : records())
                if (record.value("type").toString() == QStringLiteral("run") &&
                    record.value("session").toString() == session)
                    list.append(record.value("files").toArray());
            return list;
        };
        QVERIFY(h.chat.send(QStringLiteral("hello")));
        QTRY_VERIFY(h.settled());
        const auto a = h.chat.current().id;
        QCOMPARE(runs(piName(h, a)).size(), 1);
        const auto first = runs(piName(h, a)).first();
        QCOMPARE(first.size(), 1);
        // Name and contents only: the local path stays on this machine.
        QCOMPARE(first.first().toObject(),
                 (QJsonObject{{"name", "cv.md"}, {"text", "Pinned CV text"}}));
        // System prompt, never Pi's history.
        QFile file(h.backend.sessionFile(a));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("Pinned CV text"));
        // A new chat starts with them; removing them applies from the next run.
        h.chat.newChat();
        QVERIFY(h.chat.send(QStringLiteral("hi")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(runs(piName(h, h.chat.current().id)).first().size(), 1);
        pin({});
        QVERIFY(h.chat.send(QStringLiteral("again")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(runs(piName(h, h.chat.current().id)).last().size(), 0);
    }

    // Ask / Auto / Full reach Pi's bridge before the run; a call Pi asks about is
    // an approval card, answered exactly once, and an old card's answer never
    // reaches a newer request.
    void approvalsAreAskedAndAnsweredOnce()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("approve")));
        QTRY_COMPARE(h.chat.approvals().size(), 1);
        const auto card = h.chat.approvals().first();
        QCOMPARE(card.data.sessionId, h.chat.current().id);
        QCOMPARE(card.data.approvalId, QStringLiteral("og-approval-1"));
        QCOMPARE(card.data.toolCallId, QStringLiteral("call-1"));
        QCOMPARE(card.data.tool, QStringLiteral("bash"));
        QVERIFY(card.data.presentation);
        QCOMPARE(card.data.presentation->effect, std::optional(QStringLiteral("change")));
        QCOMPARE(card.data.presentation->code, std::optional(QStringLiteral("touch made.txt")));
        QCOMPARE(card.data.presentation->places.size(), 1);
        const auto session = piName(h, h.chat.current().id);
        QVERIFY(commands(session).indexOf(QStringLiteral("mode ")) <
                commands(session).indexOf(QStringLiteral("prompt approve")));
        QVERIFY(records().contains(
            QJsonObject{{"type", "mode"}, {"mode", "ask"}, {"session", session}}));
        h.chat.approve(card.request, false);
        QTRY_VERIFY(h.settled());
        QVERIFY(h.text(DisplayRow::Role::Assistant).endsWith(QStringLiteral("\n\nblocked")));
        QVERIFY(h.chat.approvals().isEmpty());
        h.chat.approve(card.request, true); // Answered already: nothing more reaches Pi.
        QVERIFY(h.chat.send(QStringLiteral("approve")));
        QTRY_COMPARE(h.chat.approvals().size(), 1);
        const auto newer = h.chat.approvals().first().request;
        QVERIFY(newer != card.request);
        h.chat.approve(card.request, true); // Stale: the newer request still waits.
        QCOMPARE(h.chat.approvals().size(), 1);
        QVERIFY(!h.settled());
        h.chat.approve(newer, true);
        QTRY_VERIFY(h.settled());
        QVERIFY(h.text(DisplayRow::Role::Assistant).endsWith(QStringLiteral("\n\nran")));
        int answers = 0;
        for (const auto &record : records())
            answers += record.value("type").toString() == QStringLiteral("ui_response");
        QCOMPARE(answers, 2);
        // The mode the bridge already holds is not sent again.
        int modes = 0;
        for (const auto &record : records())
            modes += record.value("type").toString() == QStringLiteral("mode");
        QCOMPARE(modes, 1);
    }

    // OpenGhost decides nothing with the mode: Full is relayed to Pi, and a call Pi
    // still asks about is a card in Full too. A change reaches the idle and the
    // running Pi at once; what it does to a waiting request is Pi's to say.
    void modesAreRelayedAndPiDecides()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        h.chat.setMode(PermissionMode::Full);
        QVERIFY(h.chat.send(QStringLiteral("approve")));
        QTRY_COMPARE(h.chat.approvals().size(), 1);
        const auto session = piName(h, h.chat.current().id);
        QVERIFY(records().contains(
            QJsonObject{{"type", "mode"}, {"mode", "full"}, {"session", session}}));
        h.chat.approve(h.chat.approvals().first().request, false);
        QTRY_VERIFY(h.settled());
        QVERIFY(h.text(DisplayRow::Role::Assistant).endsWith(QStringLiteral("\n\nblocked")));
        // Back to Ask between turns: the idle chat's Pi takes it at once, so nothing
        // it runs before the next turn (an extension's own turn) keeps Full.
        auto before = records().size();
        h.chat.setMode(PermissionMode::Ask);
        QTRY_VERIFY(!h.chat.pending());
        QCOMPARE(h.chat.current().mode, PermissionMode::Ask);
        QVERIFY(records().mid(before).contains(
            QJsonObject{{"type", "mode"}, {"mode", "ask"}, {"session", session}}));
        QVERIFY(h.chat.send(QStringLiteral("approve")));
        QTRY_COMPARE(h.chat.approvals().size(), 1);
        // Auto while the card waits reaches the running Pi; the card stays, since Pi
        // has not taken it back.
        before = records().size();
        h.chat.setMode(PermissionMode::Auto);
        QTRY_VERIFY(records().mid(before).contains(
            QJsonObject{{"type", "mode"}, {"mode", "auto"}, {"session", session}}));
        QCOMPARE(h.chat.approvals().size(), 1);
        // Full: the stand-in plugin allows the waiting call itself and takes the
        // request back; the card goes without an answer from OpenGhost.
        h.chat.setMode(PermissionMode::Full);
        QTRY_VERIFY(h.settled());
        QVERIFY(h.text(DisplayRow::Role::Assistant).endsWith(QStringLiteral("\n\nran")));
        QVERIFY(h.chat.approvals().isEmpty());
        QCOMPARE(h.chat.current().mode, PermissionMode::Full);
        int answers = 0;
        for (const auto &record : records())
            answers += record.value("type").toString() == QStringLiteral("ui_response");
        QCOMPARE(answers, 1); // Only the first card's Deny.
    }

    void stopWithdrawsTheApproval()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("approve")));
        QTRY_COMPARE(h.chat.approvals().size(), 1);
        h.chat.stop();
        QTRY_VERIFY(h.settled());
        QVERIFY(h.chat.approvals().isEmpty());
        QCOMPARE(h.text(DisplayRow::Role::Note), QStringLiteral("Stopped."));
        QVERIFY(records().contains(QJsonObject{
            {"type", "tool"}, {"ran", false}, {"session", piName(h, h.chat.current().id)}}));
        QVERIFY(h.chat.send(QStringLiteral("hello"))); // The chat goes on.
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Echo: hello"));
    }

    // Deleting the chat ends its Pi: its card goes, and nothing answers Allow.
    void deletingTheChatClosesItsApproval()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("approve")));
        QTRY_COMPARE(h.chat.approvals().size(), 1);
        const auto a = h.chat.current().id;
        const auto session = piName(h, a);
        QSignalSpy removed(&h.chat, &ChatService::removed);
        h.chat.remove(a);
        QTRY_COMPARE(removed.count(), 1);
        QVERIFY(h.chat.approvals().isEmpty());
        for (const auto &record : records())
            QVERIFY(record.value("type").toString() != QStringLiteral("ui_response") ||
                    !record.value("confirmed").toBool());
        QVERIFY(records().contains(
            QJsonObject{{"type", "tool"}, {"ran", false}, {"session", session}}));
    }

    // Another extension's dialog would block Pi forever: declined at once, and said.
    void otherExtensionDialogsAndErrorsAreDeclinedAndSaid()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QStringList logs;
        QObject::connect(&h.backend, &Backend::globalEvent, [&](const GlobalEvent &event) {
            if (const auto *log = std::get_if<Log>(&event))
                logs.append(log->level + ' ' + log->message);
        });
        QVERIFY(h.chat.send(QStringLiteral("foreign")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("foreign: cancelled"));
        QVERIFY(h.chat.status().contains(QStringLiteral("“Pick one”")));
        QVERIFY(records().contains(QJsonObject{{"type", "ui_response"},
                                               {"cancelled", true},
                                               {"session", piName(h, h.chat.current().id)}}));
        QVERIFY(h.chat.send(QStringLiteral("ext-error")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Echo: ext-error"));
        QVERIFY(
            logs.contains(QStringLiteral("error A Pi extension (helper.ts) failed: helper broke")));
        QVERIFY(logs.contains(QStringLiteral("warning helper is unhappy")));
        QCOMPARE(h.chat.status(), QStringLiteral("helper is unhappy"));
    }

    // Each sign-in is its own flow: a cancelled one's late step and end never reach
    // the next, and a prompt Pi takes back leaves the form waiting.
    void signInFlowsNeverCross()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QVERIFY(h.chat.send(QStringLiteral("hello"))); // The chat's Pi, before sign-in
        QTRY_VERIFY(h.settled());
        QSignalSpy steps(&h.chat, &ChatService::loginStep);
        QSignalSpy finished(&h.chat, &ChatService::authFinished);
        h.chat.authenticate(Login{QStringLiteral("p")}, QStringLiteral("A"));
        QTRY_COMPARE(steps.count(), 1);
        QVERIFY(steps[0][0].value<LoginStep>().promptId->startsWith(QStringLiteral("p-")));
        // Cancel and sign in again at once: A's stale step arrives after B began.
        h.chat.authenticate(CancelLogin{QStringLiteral("p")}, QStringLiteral("A"));
        h.chat.authenticate(Login{QStringLiteral("p")}, QStringLiteral("B"));
        QTRY_COMPARE(steps.count(), 2);
        QTRY_COMPARE(finished.count(), 2); // A's login (cancelled) and its cancel
        QTest::qWait(200);
        QCOMPARE(steps.count(), 2);
        const auto b = steps[1][0].value<LoginStep>();
        QVERIFY(b.promptId && *b.promptId != steps[0][0].value<LoginStep>().promptId &&
                *b.promptId != QStringLiteral("late"));
        QCOMPARE(finished[0][0].toString(), QStringLiteral("A"));
        QCOMPARE(finished[1][0].toString(), QStringLiteral("A"));
        QVERIFY(!h.chat.status().contains(QStringLiteral("cancelled"))); // not an error
        h.chat.answerLogin(AnswerLogin{QStringLiteral("p"), *b.promptId, QStringLiteral("123")});
        QTRY_COMPARE(finished.count(), 3);
        QCOMPARE(finished[2][0].toString(), QStringLiteral("B"));
        // New credentials: the chat's Pi rereads them before its next run.
        QVERIFY(!records().contains(
            QJsonObject{{"type", "refresh"}, {"session", piName(h, h.chat.current().id)}}));
        QVERIFY(h.chat.send(QStringLiteral("again")));
        QTRY_VERIFY(h.settled());
        QVERIFY(records().contains(
            QJsonObject{{"type", "refresh"}, {"session", piName(h, h.chat.current().id)}}));
        // Settings' catalog makes the control child reread models.json and credentials.
        QVERIFY(records().contains(QJsonObject{{"type", "refresh"}, {"session", ""}}));
        // A prompt Pi takes back: the form waits, asking nothing.
        h.chat.authenticate(Login{QStringLiteral("w")}, QStringLiteral("W"));
        QTRY_COMPARE(steps.count(), 4);
        QVERIFY(steps[2][0].value<LoginStep>().promptId);
        const auto waiting = steps[3][0].value<LoginStep>();
        QCOMPARE(waiting.type, QStringLiteral("waiting"));
        QVERIFY(!waiting.promptId);
    }

    // M12/F3: Pi's progress and info notifications never take an unanswered prompt
    // away: text, select and secret prompts all stay answerable until answered,
    // withdrawn, replaced, cancelled or ended.
    void notificationsKeepTheOpenPrompt()
    {
        Harness h;
        QTRY_VERIFY(h.chat.ready());
        QSignalSpy steps(&h.chat, &ChatService::loginStep);
        QSignalSpy finished(&h.chat, &ChatService::authFinished);
        const struct {
            const char *provider, *type;
            bool secret;
            int options;
        } cases[] = {{"n", "prompt", false, 0}, {"sel", "select", false, 2}, {"sec", "prompt", true, 0}};
        for (const auto &c : cases) {
            const auto provider = QString::fromLatin1(c.provider);
            const auto before = steps.count();
            h.chat.authenticate(Login{provider}, provider);
            QTRY_COMPARE(steps.count(), before + 3); // prompt, progress, info
            const auto asked = steps[before][0].value<LoginStep>();
            QVERIFY(asked.promptId);
            for (int i = before + 1; i < before + 3; ++i) {
                const auto step = steps[i][0].value<LoginStep>();
                QCOMPARE(step.provider, provider);
                QCOMPARE(step.promptId, asked.promptId); // still answerable
                QCOMPARE(step.type, QString::fromLatin1(c.type));
                QCOMPARE(step.secret, c.secret);
                QCOMPARE(step.options.size(), c.options);
                QCOMPARE(step.placeholder, asked.placeholder);
                QVERIFY(step.message.startsWith(QStringLiteral("Code?")));
            }
            QVERIFY(steps[before + 1][0].value<LoginStep>().message.contains(
                QStringLiteral("Still waiting for your code")));
            QCOMPARE(steps[before + 2][0].value<LoginStep>().links.size(), 1);
            const auto ends = finished.count();
            h.chat.answerLogin(AnswerLogin{provider, *asked.promptId, QStringLiteral("a")});
            QTRY_COMPARE(finished.count(), ends + 1); // The prompt was still Pi's: it ends the flow.
            if (provider == QStringLiteral("n")) { // Answered: what follows asks nothing.
                QCOMPARE(steps.count(), before + 4);
                const auto after = steps.last()[0].value<LoginStep>();
                QVERIFY(!after.promptId);
                QCOMPARE(after.type, QStringLiteral("waiting"));
                QCOMPARE(after.message, QStringLiteral("Checking your code"));
            }
            QCOMPARE(finished.last()[0].toString(), provider);
            QVERIFY(h.chat.status().isEmpty());
        }
    }
};

QTEST_GUILESS_MAIN(PiTest)
#include "pi.moc"
