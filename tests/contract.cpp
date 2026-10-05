#include "backend/fake_backend.h"
#include "frontend/attachments.h"
#include "frontend/chat_service.h"
#include "frontend/usage.h"
#include "settings.h"
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace openghost;
namespace
{
Result ask(Backend &backend, const Command &command)
{
    static RequestId next = 0;
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
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection);
    return answer.value_or(Result{Error{QStringLiteral("test_timeout"), {}, {}, {}, {}, {}}});
}
template <class T> T reply(const Result &result) { return std::get<T>(std::get<Reply>(result)); }
StartTurn start(QString session = QStringLiteral("chat"))
{
    StartTurn s;
    s.sessionId = session;
    s.clientTurnId = QStringLiteral("client-1");
    s.input.text = QStringLiteral("Hello native frontend");
    s.params.selection = {QStringLiteral("fake"), QStringLiteral("echo"), {}};
    return s;
}
void initialized(FakeBackend &backend) { ask(backend, Initialize{}); }

// Failure injection at the semantic seam, without any RPC/transport or UI.
class InspectBackend final : public Backend
{
  public:
    FakeBackend fake{nullptr, 0};
    bool badStart = false, refuseConfigure = false, completeBeforeAck = false,
         withoutStarted = false;
    QVector<Command> commands;
    QVector<QPair<RequestId, ReverseResult>> answers;
    bool earlyApproval = false, holdStart = false;
    std::optional<QPair<RequestId, Result>> heldStart;
    InspectBackend()
    {
        connect(&fake, &Backend::sessionEvent, this, [this](const SessionEvent &event) {
            if (earlyApproval && std::holds_alternative<TurnStarted>(event.payload))
                emit reverseRequest(900, ApprovalRequest{event.identity.sessionId,
                                                         *event.identity.turnId,
                                                         "early",
                                                         "tool",
                                                         "fixture",
                                                         {},
                                                         {}});
            if (!withoutStarted || !std::holds_alternative<TurnStarted>(event.payload))
                emit sessionEvent(event);
        });
        connect(&fake, &Backend::replied, this, [this](RequestId id, const Result &result) {
            if (holdStart && std::holds_alternative<Reply>(result) &&
                std::holds_alternative<StartAccepted>(std::get<Reply>(result))) {
                heldStart = QPair<RequestId, Result>{id, result};
                return;
            }
            if (completeBeforeAck && std::holds_alternative<Reply>(result) &&
                std::holds_alternative<StartAccepted>(std::get<Reply>(result)))
                for (int i = 0; i < 100; ++i)
                    fake.advance();
            if (badStart && std::holds_alternative<Reply>(result) &&
                std::holds_alternative<StartAccepted>(std::get<Reply>(result))) {
                auto ack = reply<StartAccepted>(result);
                ack.sessionVersion.clear();
                emit replied(id, Reply{ack});
            } else
                emit replied(id, result);
        });
    }
    void request(RequestId id, const Command &command) override
    {
        commands.append(command);
        if (refuseConfigure && std::holds_alternative<ConfigureSession>(command)) {
            QTimer::singleShot(0, this, [this, id] {
                emit replied(id, Error{QStringLiteral("model_unavailable"),
                                       QStringLiteral("Model removed"),
                                       {},
                                       {},
                                       {},
                                       {}});
            });
        } else
            fake.request(id, command);
    }
    void cancelRequest(RequestId id) override { fake.cancelRequest(id); }
    void answer(RequestId id, const ReverseResult &result) override
    {
        answers.append({id, result});
        fake.answer(id, result);
    }
    void browserChanged(const BrowserState &) override {}
};
} // namespace

class ContractTest : public QObject
{
    Q_OBJECT
  private slots:
    void absenceAndAsync()
    {
        PreferencesStore prefs({});
        ChatService disconnected(nullptr, &prefs);
        disconnected.initialize();
        QVERIFY(!disconnected.ready());
        QCOMPARE(disconnected.send(QStringLiteral("retain draft")), 0ULL);
        QVERIFY(disconnected.chats().isEmpty());
        FakeBackend fake(nullptr, 0);
        const auto unavailable = ask(fake, ModelsList{});
        QCOMPARE(std::get<Error>(unavailable).code, QStringLiteral("backend_unavailable"));
        bool returned = false, settled = false;
        connect(&fake, &Backend::replied, this, [&](RequestId, const Result &) {
            QVERIFY(returned);
            settled = true;
        });
        fake.request(900, Initialize{});
        returned = true;
        QTRY_VERIFY(settled);
    }
    void identitiesJournalsAndRefusals()
    {
        FakeBackend fake(nullptr, 0);
        initialized(fake);
        auto s = start();
        QVector<SessionEvent> events;
        connect(&fake, &Backend::sessionEvent, this,
                [&](const auto &event) { events.append(event); });
        const auto accepted = reply<StartAccepted>(ask(fake, s));
        QVERIFY(!accepted.turnId.isEmpty());
        QVERIFY(!accepted.sessionVersion.isEmpty());
        QCOMPARE(events.size(), 1);
        QCOMPARE(events[0].identity.clientTurnId.value(), s.clientTurnId);
        QCOMPARE(reply<StartAccepted>(ask(fake, s)).turnId, accepted.turnId);
        QCOMPARE(events.size(), 1); // identical retry never runs twice
        auto conflict = s;
        conflict.input.text += QLatin1Char('!');
        QCOMPARE(std::get<Error>(ask(fake, conflict)).code, QStringLiteral("duplicate_request"));
        auto other = s;
        other.clientTurnId = QStringLiteral("client-2");
        QCOMPARE(std::get<Error>(ask(fake, other)).code, QStringLiteral("session_conflict"));
        other.sessionVersion = accepted.sessionVersion;
        QCOMPARE(std::get<Error>(ask(fake, other)).code, QStringLiteral("busy"));
        other.sessionId = QStringLiteral("missing");
        QCOMPARE(std::get<Error>(ask(fake, other)).code, QStringLiteral("session_missing"));
        const auto missing = reply<SessionRecovery>(ask(fake, GetSession{other.sessionId, {}}));
        QVERIFY(std::holds_alternative<MissingSession>(missing));
        fake.advance();
        const auto snapshot = std::get<ExistingSession>(
            reply<SessionRecovery>(ask(fake, GetSession{s.sessionId, s.clientTurnId})));
        QCOMPARE(snapshot.revision, events.last().identity.seq);
        QCOMPARE(snapshot.turn->events.size(), events.size());
        QCOMPARE(snapshot.turn->input->text, s.input.text);
        QVERIFY(std::holds_alternative<MessageDelta>(events.last().payload));
        QVERIFY(std::holds_alternative<Reply>(ask(fake, CancelTurn{s.sessionId, accepted.turnId})));
        const auto count = events.size();
        for (int i = 0; i < 100; ++i)
            fake.advance();
        QCOMPARE(events.size(), count);
        QCOMPARE(std::get<TurnCompleted>(events.last().payload).status, TurnStatus::Cancelled);
        for (int i = 1; i < events.size(); ++i)
            QVERIFY(events[i].identity.seq > events[i - 1].identity.seq);
        // Completed journals stay addressable, but idle get has no active turn.
        QVERIFY(!std::get<ExistingSession>(
                     reply<SessionRecovery>(ask(fake, GetSession{s.sessionId, {}})))
                     .turn);
        QVERIFY(std::get<ExistingSession>(
                    reply<SessionRecovery>(ask(fake, GetSession{s.sessionId, s.clientTurnId})))
                    .turn);
        QVERIFY(std::holds_alternative<Reply>(ask(fake, DeleteSession{s.sessionId})));
        QVERIFY(std::holds_alternative<Reply>(ask(fake, DeleteSession{s.sessionId})));
        auto stale = s;
        stale.sessionVersion = accepted.sessionVersion;
        QCOMPARE(std::get<Error>(ask(fake, stale)).code, QStringLiteral("session_missing"));
        const auto recreated = reply<StartAccepted>(ask(fake, s));
        QVERIFY(recreated.sessionVersion != accepted.sessionVersion);
        QCOMPARE(std::get<Error>(ask(fake, SteerTurn{})).code, QStringLiteral("turn_missing"));
        QCOMPARE(std::get<Error>(ask(fake, RetryTurn{})).code, QStringLiteral("session_conflict"));
        auto withFile = start(QStringLiteral("file"));
        withFile.input.attachments.append(openghost::Attachment{});
        QCOMPARE(std::get<Error>(ask(fake, withFile)).code, QStringLiteral("unsupported"));
        QVERIFY(std::holds_alternative<MissingSession>(
            reply<SessionRecovery>(ask(fake, GetSession{withFile.sessionId, {}}))));
    }
    void frontendFlowsAndProjection()
    {
        FakeBackend fake(nullptr, 0);
        PreferencesStore prefs({});
        ChatService chat(&fake, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QCOMPARE(chat.models().size(), 2);
        chat.choose({QStringLiteral("fake"), QStringLiteral("echo"), {}}, false);
        int accepted = 0;
        connect(&chat, &ChatService::accepted, this, [&](quint64) { ++accepted; });
        QVERIFY(chat.send(QStringLiteral("first input")) > 0);
        QCOMPARE(accepted, 0);
        QTRY_COMPARE(accepted, 1);
        QVERIFY(chat.busy());
        const auto session = chat.current().id;
        const auto turn = chat.current().turn.remoteId;
        const auto client = chat.current().turn.clientId;
        fake.advance();
        QVERIFY(chat.current().rows.last().text.size() > 0);
        // Interleaved message buffers, final replacement (including empty),
        // duplicate/backward sequence, unknown messages and first-final sealing.
        Sequence seq = chat.current().sequence;
        auto publish = [&](EventPayload payload, QString message, std::optional<QString> owner) {
            emit fake.sessionEvent({{session, ++seq, owner, message, client}, std::move(payload)});
        };
        publish(MessageStarted{}, QStringLiteral("second"), turn);
        publish(MessageDelta{QStringLiteral("second message")}, QStringLiteral("second"), {});
        QVERIFY(chat.current().rows.last().text.endsWith(QStringLiteral("second message")));
        publish(MessageCompleted{QString(), {}}, QStringLiteral("second"), turn);
        const auto sealed = chat.current().rows.last().text;
        publish(MessageDelta{QStringLiteral("must not append")}, QStringLiteral("second"), turn);
        QCOMPARE(chat.current().rows.last().text, sealed);
        publish(MessageDelta{QStringLiteral("wrong turn")}, QStringLiteral("second"),
                QStringLiteral("stale"));
        publish(MessageDelta{QStringLiteral("unknown")}, QStringLiteral("unknown"), turn);
        emit fake.sessionEvent({{session, 1, turn, QStringLiteral("second"), client},
                                MessageDelta{QStringLiteral("duplicate")}});
        QCOMPARE(chat.current().rows.last().text, sealed);
        // Busy model switch restores existing choice, not an optimistic new one.
        chat.choose({QStringLiteral("fake"), QStringLiteral("brief"), QStringLiteral("low")},
                    false);
        QCOMPARE(chat.current().selection.model, QStringLiteral("echo"));
        chat.stop();
        QVERIFY(!chat.busy());
        publish(MessageDelta{QStringLiteral("late")}, QStringLiteral("second"), turn);
        QCOMPARE(chat.current().rows[1].text, sealed);
        QCOMPARE(chat.current().rows.last().text, QStringLiteral("Stopped."));
        QTRY_VERIFY(!chat.pending());
        chat.choose({QStringLiteral("fake"), QStringLiteral("brief"), QStringLiteral("low")},
                    false);
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(chat.current().selection.model, QStringLiteral("brief"));
        QVERIFY(!prefs.value().preferredThinking); // canonical default != user preference
        // Use a separate clean chat: synthetic seqs above deliberately exceed fake journal.
        chat.newChat();
        QVERIFY(chat.current().id.isEmpty());
        QCOMPARE(chat.current().selection.model, QStringLiteral("brief"));
        QVERIFY(chat.send(QStringLiteral("second chat")));
        QTRY_COMPARE(accepted, 2);
        const auto second = chat.current().id;
        for (int i = 0; i < 100; ++i)
            fake.advance();
        QVERIFY(!chat.busy());
        const auto text = chat.current().rows.last().text;
        QVERIFY(text.contains(QStringLiteral("Fake Brief")));
        chat.newChat();
        chat.open(second);
        QTRY_VERIFY(!chat.pending());
        QVERIFY(chat.ready());
        QCOMPARE(chat.current().rows.last().text, text);
        QCOMPARE(chat.chats().size(), 2);
        chat.rename(second, QStringLiteral("  Local   name  "));
        QCOMPARE(chat.current().title, QStringLiteral("Local name"));
    }
    void refusalUnknownAcceptanceAndContext()
    {
        InspectBackend backend;
        PreferencesStore prefs({});
        auto p = prefs.value();
        p.userContext.instructions = QStringLiteral("User context, not history");
        QVERIFY(prefs.save(p));
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QCOMPARE(chat.current().selection.model, QStringLiteral("echo")); // initial catalog choice
        QVERIFY(chat.send(QStringLiteral("> quote\n\nActual title")));
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(chat.current().title, QStringLiteral("Actual title"));
        const auto sent = std::get<StartTurn>(backend.commands.last());
        QCOMPARE(sent.params.userContext.instructions, p.userContext.instructions);
        QVERIFY(!sent.sessionVersion); // no invented session.create
        for (int i = 0; i < 100; ++i)
            backend.fake.advance();
        QVERIFY(!chat.busy());
        backend.refuseConfigure = true;
        chat.choose({QStringLiteral("fake"), QStringLiteral("brief"), QStringLiteral("low")},
                    false);
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(chat.current().selection.model, QStringLiteral("echo"));
        QCOMPARE(chat.status(), QStringLiteral("Model removed"));
        backend.badStart = true;
        const auto acceptedBefore = backend.commands.size();
        QSignalSpy accepted(&chat, &ChatService::accepted);
        QVERIFY(chat.send(QStringLiteral("uncertain")));
        QTRY_VERIFY(!chat.pending());
        QVERIFY(!chat.ready());
        QCOMPARE(accepted.count(), 0);
        QCOMPARE(chat.send(QStringLiteral("must not resend")), 0ULL);
        QCOMPARE(backend.commands.size(), acceptedBefore + 1);
        QCOMPARE(chat.current().rows.last().state, QStringLiteral("unconfirmed"));
        const auto uncertain = chat.current().id;
        chat.newChat();
        chat.open(uncertain);
        QVERIFY(!chat.ready()); // an ordinary open cannot bless uncertain acceptance
    }
    void stopBeforeDispatchAndConcurrentChats()
    {
        FakeBackend fake(nullptr, 0);
        PreferencesStore prefs({});
        ChatService chat(&fake, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QSignalSpy accepted(&chat, &ChatService::accepted);
        QVERIFY(chat.send(QStringLiteral("stop before dispatch")));
        const auto cancelled = chat.current().id;
        chat.stop();
        QVERIFY(!chat.busy());
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(accepted.count(), 0);
        QVERIFY(std::holds_alternative<MissingSession>(
            reply<SessionRecovery>(ask(fake, GetSession{cancelled, {}}))));
        chat.newChat();
        QVERIFY(chat.send(QStringLiteral("one")));
        QTRY_VERIFY(!chat.pending());
        const auto first = chat.current().id;
        chat.newChat();
        QVERIFY(chat.send(QStringLiteral("two")));
        QTRY_VERIFY(!chat.pending());
        const auto second = chat.current().id;
        for (int i = 0; i < 100; ++i)
            fake.advance();
        chat.open(first);
        QTRY_VERIFY(!chat.pending());
        QVERIFY(chat.current().rows.last().text.endsWith(QStringLiteral("one")));
        chat.open(second);
        QTRY_VERIFY(!chat.pending());
        QVERIFY(chat.current().rows.last().text.endsWith(QStringLiteral("two")));
    }
    void modelMetadataAndCanonicalThinking()
    {
        Settings settings;
        Account account;
        account.models = {{QStringLiteral("p"),
                           QStringLiteral("no-default"),
                           QStringLiteral("No default"),
                           {QStringLiteral("medium"), QStringLiteral("high")},
                           true,
                           false,
                           {}},
                          {QStringLiteral("p"),
                           QStringLiteral("default"),
                           QStringLiteral("Default"),
                           {QStringLiteral("low"), QStringLiteral("high")},
                           true,
                           false,
                           QStringLiteral("low")}};
        settings.apply(account);
        settings.choose(QStringLiteral("p"), QStringLiteral("no-default"));
        QVERIFY(settings.thinking().isEmpty()); // no inferred medium/first
        settings.choose(QStringLiteral("p"), QStringLiteral("default"));
        QCOMPARE(settings.thinking(), QStringLiteral("low"));
        settings.use(
            {QStringLiteral("p"), QStringLiteral("default"), QStringLiteral("canonical-unlisted")});
        settings.apply(account);
        QCOMPARE(settings.thinking(), QStringLiteral("canonical-unlisted"));
        settings.use({QStringLiteral("p"), QStringLiteral("default"), {}});
        QVERIFY(settings.thinking().isEmpty()); // canonical clear is not default
        settings.use({QStringLiteral("gone"), QStringLiteral("removed"), {}});
        settings.apply(account);
        QCOMPARE(settings.model(), QStringLiteral("removed"));
    }
    void steeringRetryUsageAndDeletion()
    {
        InspectBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        UsageStore ledger;
        connect(&chat, &ChatService::usageRecorded, &ledger, &UsageStore::record);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QVERIFY(chat.send("original"));
        QTRY_VERIFY(!chat.pending());
        backend.fake.advance();
        QVERIFY(chat.canSteer());
        QVERIFY(chat.send("steering"));
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(chat.current().rows.last().state, QStringLiteral("queued"));
        QVERIFY(std::holds_alternative<SteerTurn>(backend.commands.last()));
        backend.fake.advance();
        QCOMPARE(chat.current().rows[2].state, QStringLiteral("applied"));
        QCOMPARE(chat.current().rows.size(), 4);
        for (int i = 0; i < 100; ++i)
            backend.fake.advance();
        QVERIFY(!chat.busy());
        QVERIFY(chat.current().rows.last().text.contains("steering"));
        QVERIFY(!chat.current().rows.last().metrics.isEmpty());
        QVERIFY(chat.current().rows.last().completed >= chat.current().rows.last().started);
        QCOMPARE(ledger.totals()["fake"].toMap()["tokens"].toDouble(), 130.0);
        const auto oldTurn = chat.current().turn;
        QVERIFY(chat.send("/fake error"));
        QTRY_VERIFY(!chat.pending());
        for (int i = 0; i < 100; ++i)
            backend.fake.advance();
        QVERIFY(chat.canRetry());
        QCOMPARE(chat.current().rows.last().state, QStringLiteral("error"));
        const auto failed = chat.current().turn.remoteId;
        const auto rows = chat.current().rows.size();
        chat.retry();
        QTRY_VERIFY(!chat.pending());
        const auto retry = std::get<RetryTurn>(backend.commands.last());
        QCOMPARE(retry.failedTurnId, failed);
        QCOMPARE(reply<RetryAccepted>(ask(backend.fake, retry)).turnId,
                 chat.current().turn.remoteId);
        auto conflicting = retry;
        conflicting.failedTurnId = "another";
        QCOMPARE(std::get<Error>(ask(backend.fake, conflicting)).code,
                 QStringLiteral("duplicate_request"));
        QVERIFY(retry.clientTurnId != chat.current().pastTurns[failed].clientId);
        for (int i = 0; i < 100; ++i)
            backend.fake.advance();
        QVERIFY(!chat.canRetry());
        QCOMPARE(chat.current().rows.size(), rows + 1); // No duplicate user input.
        QVERIFY(chat.current().rows.last().text.contains("Fake retry"));
        const auto liveContext = chat.current().turn.context->used;
        Usage late;
        late.provider = "fake";
        late.model = "echo";
        late.input = 7;
        late.context = Usage::Context{999, 1000};
        const SessionEvent event{{chat.current().id, chat.current().sequence + 1, oldTurn.remoteId,
                                  oldTurn.messages.first().id, oldTurn.clientId},
                                 late};
        emit backend.sessionEvent(event);
        emit backend.sessionEvent(event); // Duplicate seq never charges twice.
        QCOMPARE(ledger.totals()["fake"].toMap()["tokens"].toDouble(), 397.0);
        QCOMPARE(chat.current().turn.context->used, liveContext);
        QVERIFY(ledger.daily(7).size() == 7);
        QVERIFY(!ledger.months().isEmpty());
        const auto id = chat.current().id;
        QSignalSpy removed(&chat, &ChatService::removed);
        chat.remove(id);
        QTRY_COMPARE(removed.count(), 1);
        QVERIFY(removed.first()[1].toBool());
        QVERIFY(chat.current().id.isEmpty());
        QVERIFY(chat.chats().isEmpty());
        QVERIFY(std::holds_alternative<MissingSession>(
            reply<SessionRecovery>(ask(backend.fake, GetSession{id, {}}))));
    }
    void approvalsToolsAndHostRefusal()
    {
        FakeBackend fake(nullptr, 0);
        PreferencesStore prefs({});
        ChatService chat(&fake, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QVERIFY(chat.send("/fake approval"));
        QTRY_VERIFY(!chat.pending());
        fake.advance();
        QCOMPARE(chat.approvals().size(), 1);
        QCOMPARE(chat.current().turn.tools["fixture-tool"].state, QStringLiteral("started"));
        const auto pending = chat.approvals().first();
        emit fake.reverseRequest(999,
                                 pending.data); // Duplicate request must not duplicate the card.
        QCOMPARE(chat.approvals().size(), 1);
        chat.newChat();
        chat.approve(pending.request, true); // Hidden other-chat cards cannot be answered here.
        QCOMPARE(chat.approvals().size(), 1);
        chat.open(pending.data.sessionId);
        QTRY_VERIFY(!chat.pending());
        chat.approve(pending.request, true);
        chat.approve(pending.request, false); // One answer only.
        QVERIFY(chat.approvals().isEmpty());
        fake.advance();
        QCOMPARE(chat.current().turn.tools["fixture-tool"].state, QStringLiteral("completed"));
        QVERIFY(!chat.current().turn.tools["fixture-tool"].progress.isEmpty());
        QVERIFY(!chat.current().turn.tools["fixture-tool"].result.isEmpty());
        for (int i = 0; i < 100; ++i)
            fake.advance();
        QCOMPARE(chat.current().rows.size(), 2); // No invented tool transcript cards.
        QVERIFY(chat.send("/fake approval"));
        QTRY_VERIFY(!chat.pending());
        fake.advance();
        QVERIFY(!chat.approvals().isEmpty());
        chat.stop();
        QVERIFY(chat.approvals().isEmpty());
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(chat.current().rows.last().state, QStringLiteral("cancelled"));
        QVERIFY(chat.send("/fake approval"));
        QTRY_VERIFY(!chat.pending());
        fake.advance();
        const auto next = chat.approvals().first().request;
        emit fake.reverseCancelled(next);
        QVERIFY(chat.approvals().isEmpty());
        chat.stop();
        QTRY_VERIFY(!chat.pending());
    }
    void terminalReceiptsAndEmptyResponse()
    {
        FakeBackend fake(nullptr, 0);
        PreferencesStore prefs({});
        ChatService chat(&fake, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QVERIFY(chat.send("running"));
        QTRY_VERIFY(!chat.pending());
        QVERIFY(chat.send("queued, not yet applied"));
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(chat.current().rows.last().state, QStringLiteral("queued"));
        chat.stop();
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(chat.current().rows[1].state, QStringLiteral("unconfirmed"));
        QVERIFY(chat.send("/fake empty"));
        QTRY_VERIFY(!chat.pending());
        fake.advance();
        QCOMPARE(chat.current().rows.last().text, QStringLiteral("The model returned no text."));
        QVERIFY(chat.send("no retry"));
        QTRY_VERIFY(!chat.pending());
        emit fake.sessionEvent(
            {{chat.current().id,
              chat.current().sequence + 1,
              chat.current().turn.remoteId,
              {},
              chat.current().turn.clientId},
             TurnCompleted{
                 TurnStatus::Error,
                 {},
                 Error{"invalid_request", "Do not retry", {}, QStringLiteral("none"), false, {}}}});
        QVERIFY(!chat.canRetry());
        QCOMPARE(chat.current().rows.last().text, QStringLiteral("Do not retry"));
    }
    void reverseOwnershipAndMockHost()
    {
        InspectBackend backend;
        backend.earlyApproval = backend.withoutStarted = true;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QVERIFY(chat.send("before ack"));
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(chat.approvals().size(), 1); // Queued until canonical remote identity is known.
        QVERIFY(backend.answers.isEmpty());
        emit backend.reverseRequest(
            901,
            HostToolRequest{
                chat.current().id, chat.current().turn.remoteId, "host", "browser.navigate", {}});
        QCOMPARE(std::get<Error>(backend.answers.last().second).code,
                 QStringLiteral("unsupported"));
        auto stale = chat.approvals().first().data;
        stale.turnId = "old";
        emit backend.reverseRequest(902, stale);
        QCOMPARE(std::get<Error>(backend.answers.last().second).code, QStringLiteral("stale_turn"));
        chat.approve(900, false);
        QCOMPARE(std::get<ApprovalAnswer>(backend.answers.last().second).decision, Decision::Deny);
        const auto count = backend.answers.size();
        chat.approve(900, true);
        QCOMPARE(backend.answers.size(), count);
        chat.stop();
        QTRY_VERIFY(!chat.pending());
    }
    void attachmentsAreOwnedPayloads()
    {
        QTemporaryDir temp;
        QFile text(temp.filePath("input.txt"));
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("original café\n");
        text.close();
        AttachmentStore store;
        QVector<AttachmentStore::Prepared> prepared;
        QVERIFY(store.prepare({QUrl::fromLocalFile(text.fileName())}, 20, prepared).isEmpty());
        QCOMPARE(prepared.size(), 1);
        const auto token = prepared.first().token;
        const auto input = *store.resolve({token});
        QCOMPARE(input.first().text.value(), QString::fromUtf8("original café\n"));
        QVERIFY(!input.first().path); // Native host paths never leak into this demo.
        QFile bad(temp.filePath("binary"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("a\0b", 3);
        bad.close();
        QVERIFY(!store
                     .prepare({QUrl::fromLocalFile(text.fileName()),
                               QUrl::fromLocalFile(bad.fileName())},
                              20, prepared)
                     .isEmpty());
        QVERIFY(prepared.isEmpty()); // Whole failed selection publishes nothing.
        QVERIFY(!store.prepare({QUrl("https://example.com/file")}, 20, prepared).isEmpty());
        QVERIFY(!store.resolve({token, token}));
        FakeBackend fake(nullptr, 0);
        PreferencesStore prefs({});
        ChatService chat(&fake, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QVERIFY(chat.send({}, input));
        QTRY_VERIFY(!chat.pending());
        store.release({token});
        QVERIFY(!store.resolve({token}));
        QCOMPARE(chat.current().turn.prepared->input.attachments.first().text, input.first().text);
        QCOMPARE(chat.current().rows.first().attachments.first().name, QStringLiteral("input.txt"));
        for (int i = 0; i < 100; ++i)
            fake.advance();
        QVERIFY(chat.current().rows.last().text.contains("input.txt"));
    }
    void reconciliationAndEarlyCompletion()
    {
        InspectBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        UsageStore ledger;
        connect(&chat, &ChatService::usageRecorded, &ledger, &UsageStore::record);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        backend.badStart = true;
        QVERIFY(chat.send("/fake error"));
        QTRY_VERIFY(!chat.pending());
        QVERIFY(!chat.ready());
        QVERIFY(chat.canRetry());
        for (int i = 0; i < 100; ++i)
            backend.fake.advance();
        const auto count = backend.commands.size();
        chat.retry();
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(backend.commands.size(), count + 1);
        QVERIFY(std::holds_alternative<GetSession>(backend.commands.last()));
        QVERIFY(chat.ready());
        QVERIFY(chat.canRetry());
        QCOMPARE(chat.current().rows.last().state, QStringLiteral("error"));
        QVERIFY(ledger.totals()
                    .isEmpty()); // Unknown message usage refused; replay never charges the ledger.
        chat.retry();
        QTRY_VERIFY(!chat.pending());
        QVERIFY(std::holds_alternative<RetryTurn>(backend.commands.last()));
        for (int i = 0; i < 100; ++i)
            backend.fake.advance();
        QCOMPARE(ledger.totals()["fake"].toMap()["tokens"].toDouble(), 130.0);

        // Stop before dispatch: Retry first proves absence, then uses the exact
        // prepared payload and original client ID (not a display cache).
        chat.newChat();
        backend.badStart = false;
        QVERIFY(chat.send("undispatched"));
        const auto prepared = *chat.current().turn.prepared;
        chat.stop();
        QTRY_VERIFY(!chat.pending());
        chat.retry();
        QTRY_VERIFY(!chat.pending());
        QVERIFY(chat.ready());
        QCOMPARE(chat.current().turn.clientId, prepared.clientTurnId);
        QCOMPARE(std::get<StartTurn>(backend.commands.last()).input.text, prepared.input.text);
    }
    void terminalAndOutputBeforeAcknowledgement()
    {
        InspectBackend backend;
        backend.completeBeforeAck = backend.withoutStarted = true;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        UsageStore ledger;
        connect(&chat, &ChatService::usageRecorded, &ledger, &UsageStore::record);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QSignalSpy accepted(&chat, &ChatService::accepted);
        QVERIFY(chat.send("early"));
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(accepted.count(), 1);
        QVERIFY(!chat.busy());
        QCOMPARE(chat.current().rows.size(), 2);
        QVERIFY(chat.current().rows.last().text.endsWith("early"));
        QVERIFY(chat.current().turn.early.isEmpty());
        QCOMPARE(ledger.totals()["fake"].toMap()["tokens"].toDouble(), 130.0);
    }
    void stopFollowedByLateAcceptance()
    {
        InspectBackend backend;
        backend.holdStart = backend.withoutStarted = true;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QVERIFY(chat.send("accepted but acknowledgement delayed"));
        QTRY_VERIFY(backend.heldStart.has_value());
        chat.stop();
        QVERIFY(!chat.busy());
        QVERIFY(!chat.ready());
        const auto held = *backend.heldStart;
        emit backend.replied(held.first, held.second);
        QVERIFY(chat.pending()); // Cancellation must still settle after the original ack.
        QVERIFY(std::holds_alternative<CancelTurn>(backend.commands.last()));
        QCOMPARE(chat.send("must not race cancellation"), 0ULL);
        QTRY_VERIFY(!chat.pending());
        const auto before = chat.current().rows.size();
        for (int i = 0; i < 100; ++i)
            backend.fake.advance();
        QCOMPARE(chat.current().rows.size(), before);
        chat.retry(); // Reconcile only; the stopped turn must not run again.
        QTRY_VERIFY(!chat.pending());
        QVERIFY(chat.ready());
        QVERIFY(!chat.busy());
        QVERIFY(!chat.canRetry());
        QCOMPARE(chat.current().rows.last().text, QStringLiteral("Stopped."));
    }
    void providerInvalidationsAndCancellation()
    {
        FakeBackend fake(nullptr, 0);
        PreferencesStore prefs({});
        ChatService chat(&fake, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        chat.authenticate(Logout{"fake"});
        QTRY_VERIFY(chat.models().isEmpty());
        QCOMPARE(chat.current().selection.model,
                 QStringLiteral("echo")); // Never silently fall back.
        QVERIFY(!chat.send("unavailable"));
        chat.authenticate(SetKey{"fake", QStringLiteral("not-a-real-key")});
        QTRY_VERIFY(chat.status().contains("only the word fixture"));
        QTRY_VERIFY(chat.providers().first().status.error.has_value());
        QVERIFY(chat.models().isEmpty());
        chat.authenticate(Login{"fake"});
        QTRY_VERIFY(chat.providers().first().status.waiting.value_or(false));
        chat.authenticate(CancelLogin{"fake"});
        QTRY_VERIFY(!chat.providers().first().status.waiting.value_or(true));
        QTest::qWait(300);
        QVERIFY(chat.models().isEmpty());
        chat.authenticate(SetKey{"fake", QStringLiteral("fixture")});
        QTRY_COMPARE(chat.models().size(), 2);
        QTRY_VERIFY(!chat.providers().first().status.error.has_value());
    }
    void metatypeResultCopies()
    {
        const auto copy = [](const Result &source) {
            const auto type = QMetaType::fromType<Result>();
            auto *stored = static_cast<Result *>(type.create(&source));
            Result result = *stored;
            type.destroy(stored);
            return result;
        };
        const Result error{Error{"test", "message", QStringLiteral("p"), {}, true, 429}};
        QCOMPARE(std::get<Error>(copy(error)).status.value(), 429);
        ExistingSession saved{"version", 42,
                              RecoveredTurn{"client", "turn", DisplayInput{"input", {}}, {}}};
        const Result nested{Reply{SessionRecovery{saved}}};
        QCOMPARE(std::get<ExistingSession>(reply<SessionRecovery>(copy(nested))).turn->input->text,
                 QStringLiteral("input"));
        const Result null{Reply{Null{}}};
        QVERIFY(std::holds_alternative<Null>(std::get<Reply>(copy(null))));
    }
    void preferencesRoundTripAndFailures()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath(QStringLiteral("nested/preferences.json"));
        PreferencesStore prefs(path);
        auto value = prefs.value();
        value.model = {QStringLiteral("opaque:provider"), QStringLiteral("opaque|model"), {}};
        value.preferredThinking = QStringLiteral("high");
        value.mode = PermissionMode::Auto;
        value.userContext.instructions = QStringLiteral("Line one\nUnicode: café 👻");
        QVERIFY(prefs.save(value));
        PreferencesStore reopened(path);
        QCOMPARE(reopened.value().userContext.instructions, value.userContext.instructions);
        QCOMPARE(reopened.value().model.model, value.model.model);
        QCOMPARE(reopened.value().preferredThinking, value.preferredThinking);
        QCOMPARE(reopened.value().mode, PermissionMode::Auto);
        FakeBackend fake(nullptr, 0);
        ChatService chat(&fake, &reopened);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QCOMPARE(chat.current().selection.model, value.model.model); // removed preference is kept
        QCOMPARE(chat.send(QStringLiteral("unavailable model")), 0ULL);
        QVERIFY(chat.chats().isEmpty());
        value.userContext.instructions = QString(8001, QLatin1Char('x'));
        QVERIFY(!prefs.save(value));
        QCOMPARE(prefs.value().userContext.instructions, reopened.value().userContext.instructions);
        PreferencesStore unwritable(
            temp.path()); // directory, not a file; deterministic on all platforms
        QVERIFY(!unwritable.save(Preferences{}));
        QFile corrupt(temp.filePath(QStringLiteral("bad.json")));
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("{bad");
        corrupt.close();
        PreferencesStore invalid(corrupt.fileName());
        QVERIFY(!invalid.error().isEmpty());
        QVERIFY(!invalid.save(Preferences{}));
        QVERIFY(corrupt.open(QIODevice::ReadOnly));
        QCOMPARE(corrupt.readAll(), QByteArray("{bad"));
    }
};
QTEST_GUILESS_MAIN(ContractTest)
#include "contract.moc"
