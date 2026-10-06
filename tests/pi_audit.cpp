// Adversarial regressions for the M02/M03/M13/M14/M16 audit. Scripted RPC,
// not a live-provider qualification. See docs/pi-lifecycle-audit.md.
#include "backend/pi_backend.h"
#include "frontend/chat_service.h"
#include "frontend/library.h"
#include "frontend/preferences.h"
#include "frontend/store.h"
#include "frontend/usage.h"
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace openghost;
namespace {
Result ask(Backend &backend, const Command &command)
{
    static RequestId next = 10000;
    const auto id = ++next;
    std::optional<Result> result;
    QEventLoop loop;
    auto connection = QObject::connect(&backend, &Backend::replied, &loop,
        [&](RequestId got, const Result &reply) {
            if (got == id) { result = reply; loop.quit(); }
        });
    backend.request(id, command);
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection);
    return result.value_or(Result{Error{"test_timeout", {}, {}, {}, {}, {}}});
}
QString code(const Result &result)
{
    const auto *error = std::get_if<Error>(&result);
    return error ? error->code : QString();
}
StartTurn start(const QString &text = QStringLiteral("hold"))
{
    StartTurn command;
    command.sessionId = QStringLiteral("chat");
    command.clientTurnId = QStringLiteral("client");
    command.input.text = text;
    return command;
}
}

class PiAuditTest : public QObject
{
    Q_OBJECT
    QTemporaryDir dir;
    QString log;
    int serial = 0;
    QVector<QJsonObject> commands() const
    {
        QFile file(log);
        QVector<QJsonObject> records;
        if (file.open(QIODevice::ReadOnly))
            for (const auto &line : file.readAll().split('\n'))
                if (!line.isEmpty()) records.append(QJsonDocument::fromJson(line).object());
        return records;
    }
    int count(const QString &type) const
    {
        const auto records = commands();
        return std::count_if(records.cbegin(), records.cend(), [&](const auto &r) {
            return r.value("type").toString() == type;
        });
    }
    void mode(const char *name) { qputenv("PI_AUDIT_CASE", name); }
  private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        qputenv("PATH", QByteArray(OPENGHOST_AUDIT_PI_DIR ":") + qgetenv("PATH"));
    }
    void init()
    {
        mode("");
        log = dir.filePath(QString::number(++serial));
        qputenv("PI_AUDIT_LOG", log.toUtf8());
    }
    void unknownDispositionIsNotAcceptance()
    {
        mode("unknown");
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        QSignalSpy events(&backend, &Backend::sessionEvent);
        QCOMPARE(code(ask(backend, start())), QStringLiteral("protocol_error"));
        QCOMPARE(events.size(), 0);
    }
    void processLossIsNotDefiniteRejection()
    {
        mode("lost-process");
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        QCOMPARE(code(ask(backend, start())), QStringLiteral("backend_unavailable"));
    }
    void unresolvedAdmissionIsNotMissing()
    {
        mode("pending");
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        backend.request(1, start());
        QTRY_VERIFY(count(QStringLiteral("prompt")) >= 2); // mark, then real prompt
        QCOMPARE(code(ask(backend, GetSession{"chat", QStringLiteral("client")})),
                 QStringLiteral("acceptance_pending"));
    }
    void cancellationBeforeDispatchWithdrawsPrompt()
    {
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        QSignalSpy replies(&backend, &Backend::replied);
        backend.request(1, start());
        backend.cancelRequest(1);
        QTRY_COMPARE(replies.size(), 1);
        QCOMPARE(code(qvariant_cast<Result>(replies[0][1])), QStringLiteral("cancelled"));
        QCOMPARE(count(QStringLiteral("prompt")), 0);
    }
    void failedQueueClearIsNotSuccessfulStop()
    {
        mode("clear-fails");
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        const auto result = ask(backend, start());
        QVERIFY(std::holds_alternative<Reply>(result));
        const auto accepted = std::get<StartAccepted>(std::get<Reply>(result));
        QVERIFY(code(ask(backend, SteerTurn{"chat", accepted.turnId, "input", {"later", {}}, {}})).isEmpty());
        QCOMPARE(code(ask(backend, CancelTurn{"chat", accepted.turnId})), QStringLiteral("cancel_failed"));
        QCOMPARE(count(QStringLiteral("abort")), 0); // failed clear must not run queued input
    }
    void stopWaitsForInflightSteering()
    {
        mode("late-steer");
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        const auto accepted = std::get<StartAccepted>(std::get<Reply>(ask(backend, start())));
        backend.request(1, SteerTurn{"chat", accepted.turnId, "input", {"later", {}}, {}});
        QTRY_COMPARE(count(QStringLiteral("steer")), 1);
        bool queued = false, clearedTooSoon = false;
        QObject::connect(&backend, &Backend::replied, [&](RequestId id, const Result &) {
            if (id == 1) { queued = true; clearedTooSoon = count(QStringLiteral("clear_queue")) > 0; }
        });
        QVERIFY(code(ask(backend, CancelTurn{"chat", accepted.turnId})).isEmpty());
        QVERIFY(queued);
        QVERIFY(!clearedTooSoon);
        QCOMPARE(count(QStringLiteral("clear_queue")), 1);
        QCOMPARE(count(QStringLiteral("abort")), 1);
    }
    void abortAcknowledgementFencesNextTurn()
    {
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        const auto accepted = std::get<StartAccepted>(std::get<Reply>(ask(backend, start())));
        bool ended = false, acknowledged = false;
        QObject::connect(&backend, &Backend::sessionEvent, [&](const SessionEvent &e) {
            ended |= std::holds_alternative<TurnCompleted>(e.payload);
        });
        QObject::connect(&backend, &Backend::replied, [&](RequestId id, const Result &) {
            acknowledged |= id == 1;
        });
        backend.request(1, CancelTurn{"chat", accepted.turnId});
        QTRY_VERIFY(ended);
        QVERIFY(!acknowledged);
        auto next = start(QStringLiteral("next"));
        next.clientTurnId = QStringLiteral("next");
        next.sessionVersion = accepted.sessionVersion;
        QCOMPARE(code(ask(backend, next)), QStringLiteral("busy"));
        QTRY_VERIFY(acknowledged);
        QVERIFY(code(ask(backend, next)).isEmpty());
    }
    void lateQueuedReceiptIsNotNotApplied()
    {
        mode("late-settle");
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        const auto accepted = std::get<StartAccepted>(std::get<Reply>(ask(backend, start())));
        QSignalSpy events(&backend, &Backend::sessionEvent);
        const auto reply = ask(backend, SteerTurn{"chat", accepted.turnId, "input", {"later", {}}, {}});
        QVERIFY(std::holds_alternative<Reply>(reply));
        QVERIFY(std::get<SteerAccepted>(std::get<Reply>(reply)).accepted);
        QTRY_COMPARE(count(QStringLiteral("clear_queue")), 1);
        QTRY_VERIFY(std::any_of(events.cbegin(), events.cend(), [](const auto &row) {
            return std::holds_alternative<TurnCompleted>(qvariant_cast<SessionEvent>(row[0]).payload);
        }));
    }
    void emptyTransformedSteeringCanBeApplied()
    {
        mode("empty-steer");
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        const auto accepted = std::get<StartAccepted>(std::get<Reply>(ask(backend, start())));
        bool applied = false;
        QObject::connect(&backend, &Backend::sessionEvent, [&](const SessionEvent &e) {
            if (const auto *input = std::get_if<InputAccepted>(&e.payload))
                applied = input->clientInputId == QStringLiteral("input");
        });
        QVERIFY(code(ask(backend, SteerTurn{"chat", accepted.turnId, "input", {"original", {}}, {}})).isEmpty());
        QTRY_VERIFY(applied);
    }
    void noFinalResponseIsNotSuccess()
    {
        mode("empty-end");
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        std::optional<TurnCompleted> completed;
        QObject::connect(&backend, &Backend::sessionEvent, [&](const SessionEvent &e) {
            if (const auto *done = std::get_if<TurnCompleted>(&e.payload)) completed = *done;
        });
        QVERIFY(code(ask(backend, start(QStringLiteral("end")))).isEmpty());
        QTRY_VERIFY(completed.has_value());
        QCOMPARE(completed->status, TurnStatus::Error);
    }
    void dedupeAndRetrySurviveJournalEviction()
    {
        PiBackend backend;
        QVERIFY(code(ask(backend, Initialize{})).isEmpty());
        int completed = 0;
        QObject::connect(&backend, &Backend::sessionEvent, [&](const SessionEvent &event) {
            completed += std::holds_alternative<TurnCompleted>(event.payload);
        });
        StartAccepted first, last;
        auto command = start(QStringLiteral("hello"));
        for (int i = 0; i < 66; ++i) {
            command.clientTurnId = QString::number(i);
            command.input.text = i == 65 ? QStringLiteral("fail") : QStringLiteral("hello");
            const auto result = ask(backend, command);
            QVERIFY(std::holds_alternative<Reply>(result));
            last = std::get<StartAccepted>(std::get<Reply>(result));
            if (i == 0) first = last;
            command.sessionVersion = last.sessionVersion;
            QTRY_COMPARE(completed, i + 1);
        }
        command.clientTurnId = QStringLiteral("0");
        const auto duplicate = ask(backend, command);
        QVERIFY(std::holds_alternative<Reply>(duplicate));
        QCOMPARE(std::get<StartAccepted>(std::get<Reply>(duplicate)).turnId, first.turnId);
        QCOMPARE(completed, 66);
        // Reading evicted old journals rotates the failed latest turn out of the
        // display cache too. Retry must recover it, not confuse eviction with absence.
        for (int i = 0; i < 65; ++i)
            QVERIFY(code(ask(backend, GetSession{"chat", QString::number(i)})).isEmpty());
        const auto retried = ask(backend, RetryTurn{"chat", last.sessionVersion, "retry", last.turnId, {}});
        QVERIFY2(std::holds_alternative<Reply>(retried), qPrintable(code(retried)));
        QVERIFY(std::holds_alternative<RetryAccepted>(std::get<Reply>(retried)));
    }
    void expiredBridgeRequestIsNeverSentAfterReadiness()
    {
        mode("bridge-delay");
        PiProcess process(QStringLiteral("fixture-bridge"));
        QString error;
        QVERIFY(process.start({QStringLiteral("-e"), QStringLiteral("fixture-bridge")}, {}, &error));
        bool timedOut = false;
        process.bridge({{"op", "retry"}}, [&](const QJsonObject &reply) {
            timedOut = reply.value("timeout").toBool();
        }, 20);
        QTRY_VERIFY(timedOut);
        QTest::qWait(300); // let get_commands arrive after the operation expired
        QCOMPARE(count(QStringLiteral("prompt")), 0);
    }
    void usageAfterStopPersistsWithActualAttribution()
    {
        mode("late-usage");
        FileKeyStore store(dir.filePath(QStringLiteral("ledger")));
        {
            PiBackend backend;
            PreferencesStore prefs{QString()};
            MemoryKeyStore memory;
            Library library(&memory);
            ChatService chat(&backend, &prefs, &library);
            UsageStore ledger(&store);
            QObject::connect(&chat, &ChatService::usageRecorded, &ledger, &UsageStore::record);
            chat.initialize();
            QTRY_VERIFY(chat.ready());
            QVERIFY(chat.send(QStringLiteral("hold")));
            QTRY_COMPARE(ledger.totals(0).value("actual").toMap().value("requests").toInt(), 1);
            chat.stop();
            QTRY_VERIFY(!chat.pending());
            QTRY_COMPARE(ledger.totals(0).value("late-provider").toMap().value("requests").toInt(), 1);
            for (const auto &row : chat.current().rows)
                QVERIFY(!row.text.contains(QStringLiteral("must stay invisible")));
            // Replay the exact turn: metrics may rebuild, the durable ledger never recharges.
            const auto &current = chat.current();
            QVERIFY(code(ask(backend, GetSession{current.id, current.turn.clientId})).isEmpty());
            QVERIFY(ledger.flush());
        }
        UsageStore reopened(&store);
        const auto totals = reopened.totals(0);
        QVERIFY(!totals.contains(QStringLiteral("virtual")));
        for (const auto &provider : {QStringLiteral("actual"), QStringLiteral("late-provider")}) {
            const auto row = totals.value(provider).toMap();
            QCOMPARE(row.value("input").toDouble(), 16.0);
            QCOMPARE(row.value("cached").toDouble(), 3.0);
            QCOMPARE(row.value("written").toDouble(), 2.0);
            QCOMPARE(row.value("output").toDouble(), 7.0); // reasoning already included
            QCOMPARE(row.value("requests").toDouble(), 1.0);
        }
    }
};
QTEST_GUILESS_MAIN(PiAuditTest)
#include "pi_audit.moc"
