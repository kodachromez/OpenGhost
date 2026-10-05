#include "backend/fake_backend.h"
#include "frontend/chat_service.h"
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
    bool badStart = false, refuseConfigure = false;
    QVector<Command> commands;
    InspectBackend()
    {
        connect(&fake, &Backend::sessionEvent, this, &Backend::sessionEvent);
        connect(&fake, &Backend::replied, this, [this](RequestId id, const Result &result) {
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
    void answer(RequestId, const ReverseResult &) override {}
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
        QCOMPARE(std::get<Error>(ask(fake, SteerTurn{})).code, QStringLiteral("unsupported"));
        QCOMPARE(std::get<Error>(ask(fake, RetryTurn{})).code, QStringLiteral("unsupported"));
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
        QCOMPARE(chat.current().rows.last().text, sealed);
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
