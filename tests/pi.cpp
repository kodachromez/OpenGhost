// PiBackend's turn lifecycle against tests/pi/pi, a scripted stand-in for
// `pi --mode rpc` (no model, network or credentials): real acceptance, final
// errors, Stop, steering receipts, failed-turn Retry and usage.
#include "backend/pi_backend.h"
#include "frontend/chat_service.h"
#include "frontend/preferences.h"
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace openghost;
namespace
{
struct Harness {
    PiBackend backend;
    PreferencesStore prefs{QString()};
    ChatService chat{&backend, &prefs};
    QVector<Usage> usage;
    Harness()
    {
        QObject::connect(&chat, &ChatService::usageRecorded,
                         [this](const Usage &spent) { usage.append(spent); });
        chat.initialize();
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
} // namespace

class PiTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    int m_run = 0;
    QString m_log;
    // The commands the fake Pi received, in order.
    QStringList commands() const
    {
        QFile file(m_log);
        QStringList list;
        if (!file.open(QIODevice::ReadOnly))
            return list;
        for (const auto &line : file.readAll().split('\n'))
            if (!line.isEmpty()) {
                const auto command = QJsonDocument::fromJson(line).object();
                list.append(command.value("type").toString() + ' ' +
                            command.value("message").toString());
            }
        return list;
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
        QCOMPARE(h.usage[0].model, QStringLiteral("m"));
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
};

QTEST_GUILESS_MAIN(PiTest)
#include "pi.moc"
