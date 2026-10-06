// PiBackend's turn lifecycle against tests/pi/pi, a scripted stand-in for
// `pi --mode rpc` (no model, network or credentials): real acceptance, final
// errors, Stop, steering receipts, failed-turn Retry and usage; one Pi session
// per chat (isolation, side-by-side runs, restart recovery, deletion), per-chat
// models and standing instructions.
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
        Attachment file;
        file.kind = Attachment::Kind::Text;
        file.name = QStringLiteral("a.txt");
        file.text = QStringLiteral("x");
        file.size = 1;
        QVERIFY(h.chat.send({}, {file}));
        QTRY_VERIFY(!h.chat.pending());
        QVERIFY(h.chat.send(QStringLiteral("dup now")));
        QTRY_VERIFY(h.settled());
        QCOMPARE(h.text(DisplayRow::Role::Assistant), QStringLiteral("Steered."));
        int applied = 0;
        for (const auto &row : h.chat.current().rows)
            if (row.role == DisplayRow::Role::User && !row.clientInputId.isEmpty()) {
                if (row.attachments.isEmpty())
                    applied += row.state == QStringLiteral("applied");
                else
                    QCOMPARE(row.state, QStringLiteral("notApplied")); // Never dropped.
            }
        QCOMPARE(applied, 4); // Transformed and identical texts each matched once.
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
};

QTEST_GUILESS_MAIN(PiTest)
#include "pi.moc"
