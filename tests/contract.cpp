#include "backend/fake_backend.h"
#include "browser-tools/fixtures.h"
#include "frontend/attachments.h"
#include "frontend/chat_service.h"
#include "frontend/host.h"
#include "frontend/library.h"
#include "frontend/store.h"
#include "frontend/usage.h"
#include "settings.h"
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
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
    s.input.text = QStringLiteral("Hello OpenGhost C++");
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

// Test-only sealer: an HMAC-authenticated XOR stream. It checks the lock STATE
// machine (sealed title, wrong key refusal, ordering); it is NOT a cipher claim.
class TestSealer final : public ChatSealer
{
  public:
    int salts = 0;
    QString salt() override { return QStringLiteral("salt-%1").arg(++salts); }
    std::optional<QByteArray> derive(const QString &password, const QString &salt, int) override
    {
        return QMessageAuthenticationCode::hash(password.toUtf8(), salt.toUtf8(),
                                                QCryptographicHash::Sha256);
    }
    static QByteArray stream(const QByteArray &key, qsizetype size)
    {
        QByteArray out;
        for (int block = 0; out.size() < size; ++block)
            out += QCryptographicHash::hash(key + QByteArray::number(block),
                                            QCryptographicHash::Sha256);
        return out.left(size);
    }
    std::optional<QJsonObject> seal(const QByteArray &key, const QJsonObject &value) override
    {
        auto data = QJsonDocument(value).toJson(QJsonDocument::Compact);
        const auto mask = stream(key, data.size());
        for (qsizetype i = 0; i < data.size(); ++i)
            data[i] = char(data[i] ^ mask[i]);
        const auto tag = QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
        return QJsonObject{{"iv", QString::fromLatin1(tag.toBase64())},
                           {"data", QString::fromLatin1(data.toBase64())}};
    }
    std::optional<QJsonObject> open(const QByteArray &key, const QJsonObject &sealed) override
    {
        auto data = QByteArray::fromBase64(sealed.value("data").toString().toLatin1());
        if (QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256).toBase64() !=
            sealed.value("iv").toString().toLatin1())
            return std::nullopt;
        const auto mask = stream(key, data.size());
        for (qsizetype i = 0; i < data.size(); ++i)
            data[i] = char(data[i] ^ mask[i]);
        return QJsonDocument::fromJson(data).object();
    }
};
// A scripted host-service mock: one published tool, explicit settlement.
class ScriptedHost final : public HostServices
{
  public:
    QVector<RequestId> ran, cancelled;
    QHash<RequestId, HostToolRequest> received;
    QVector<HostToolSchema> definitions{
        {QStringLiteral("browser_snapshot"), QStringLiteral("fixture"), {}}};
    std::optional<BrowserState> context;
    std::optional<BrowserState> browser() const override { return context; }
    QVector<HostToolSchema> tools() const override { return definitions; }
    void run(RequestId id, const HostToolRequest &request) override
    {
        ran.append(id);
        received.insert(id, request);
    }
    void cancel(RequestId id) override { cancelled.append(id); }
};
void settle(FakeBackend &fake)
{
    for (int i = 0; i < 200; ++i)
        fake.advance();
}
template <class T> int countOf(const QVector<Command> &commands)
{
    return int(std::count_if(commands.cbegin(), commands.cend(),
                             [](const auto &c) { return std::holds_alternative<T>(c); }));
}
template <class F> int countIf(const QVector<Command> &commands, F predicate)
{
    return int(std::count_if(commands.cbegin(), commands.cend(), predicate));
}
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
    void fakeEffortCapabilitiesAndSessionSelection()
    {
        InspectBackend backend;
        PreferencesStore prefs({});
        ChatService chat(&backend, &prefs);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        const QStringList levels{"none", "low", "medium", "high", "xhigh", "max", "ultra"};
        QCOMPARE(chat.models().first().thinkingLevels, levels);
        QCOMPARE(chat.models().first().defaultThinking, std::optional<QString>("medium"));
        QCOMPARE(chat.models().last().thinkingLevels, (QStringList{"low", "high"}));
        QCOMPARE(chat.current().selection.model, QStringLiteral("echo"));
        QCOMPARE(chat.current().selection.thinking, std::optional<QString>("medium"));
        QVERIFY(!prefs.value().preferredThinking); // advertised default isn't a user choice

        chat.choose({"fake", "echo", QStringLiteral("ultra")}, true);
        QCOMPARE(countOf<ConfigureSession>(backend.commands), 0); // draft only
        QVERIFY(chat.send("Effort fixture"));
        QTRY_VERIFY(!chat.pending());
        const auto sent = std::get<StartTurn>(backend.commands.last());
        QCOMPARE(sent.params.selection.thinking, std::optional<QString>("ultra"));
        const int commands = backend.commands.size();
        chat.choose({"fake", "echo", QStringLiteral("none")}, true);
        QCOMPARE(backend.commands.size(), commands); // locked while running
        QCOMPARE(chat.current().selection.thinking, std::optional<QString>("ultra"));
        settle(backend.fake);

        // Configure replies, not optimistic UI state, establish the fake session's choice.
        ConfigureSession inspect;
        inspect.sessionId = chat.current().id;
        inspect.sessionVersion = *chat.current().version;
        const auto canonical = [&] {
            return reply<SessionConfigured>(ask(backend.fake, inspect)).thinking.value();
        };
        for (const auto &level : levels) {
            chat.choose({"fake", "echo", level}, true);
            QVERIFY(chat.pending());
            const auto config = std::get<ConfigureSession>(backend.commands.last());
            QCOMPARE(config.sessionId, inspect.sessionId);
            QCOMPARE(config.sessionVersion, inspect.sessionVersion);
            QCOMPARE(config.thinking, std::optional<QString>(level));
            QTRY_VERIFY(!chat.pending());
            QCOMPARE(chat.current().selection.thinking, std::optional<QString>(level));
            QCOMPARE(canonical(), std::optional<QString>(level));
            QCOMPARE(prefs.value().preferredThinking, std::optional<QString>(level));
        }
        auto invalid = inspect;
        invalid.thinking = "off"; // reference Instant is `none`, not `off`
        QCOMPARE(std::get<Error>(ask(backend.fake, invalid)).code,
                 QStringLiteral("model_unavailable"));
        QCOMPARE(canonical(), std::optional<QString>("ultra"));
        backend.refuseConfigure = true;
        chat.choose({"fake", "echo", QStringLiteral("low")}, true);
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(chat.current().selection.thinking, std::optional<QString>("ultra"));
        QCOMPARE(prefs.value().preferredThinking, std::optional<QString>("ultra"));
        backend.refuseConfigure = false;
        chat.choose({"fake", "brief", QStringLiteral("low")}, false);
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(canonical(), std::optional<QString>("low"));
        QCOMPARE(prefs.value().preferredThinking, std::optional<QString>("ultra"));
        invalid.thinking = "ultra"; // full set belongs to Echo, not every model
        QCOMPARE(std::get<Error>(ask(backend.fake, invalid)).code,
                 QStringLiteral("model_unavailable"));
        QCOMPARE(canonical(), std::optional<QString>("low"));
        chat.newChat();
        QCOMPARE(chat.current().selection.model, QStringLiteral("brief"));
        QCOMPARE(chat.current().selection.thinking, std::optional<QString>("low"));
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
    void storeAndLibraryPersistence()
    {
        QTemporaryDir dir;
        FileKeyStore files(dir.path());
        QVERIFY(!KeyStore::validKey("../escape") && !KeyStore::validKey("a//b") &&
                !KeyStore::validKey(""));
        QVERIFY(!files.write("../escape", {}));
        QCOMPARE(files.read("index").status, KeyStore::Status::Absent);
        QVERIFY(files.write("mini/a:b", {{"x", 1}}));
        QVERIFY(QFile::exists(dir.path() + "/mini/a%3Ab.json")); // ':' never in a file name
        QCOMPARE(files.read("mini/a:b").value.value("x").toInt(), 1);
        {
            QFile corrupt(dir.path() + "/broken.json");
            QVERIFY(corrupt.open(QIODevice::WriteOnly));
            corrupt.write("{not json");
        }
        QCOMPARE(files.read("broken").status, KeyStore::Status::Unreadable);

        QString id;
        {
            Library library(&files);
            QVERIFY(library.writable());
            const auto *chat = library.create({}, QStringLiteral("Plan: a/b <trip>?"));
            QVERIFY(chat && chat->space);
            id = chat->id;
            QCOMPARE(*chat->space, QStringLiteral("Plan a b trip"));
            QCOMPARE(*library.create({}, QStringLiteral("Plan: a/b <trip>?"))->space,
                     QStringLiteral("Plan a b trip 2"));
            QCOMPARE(Library::spaceName("con"), QStringLiteral("New chat con"));
            QVERIFY(library.create(QStringLiteral("/work/project"), QStringLiteral("Folder chat")));
            QVERIFY(library.setPinned(id, true));
            QVERIFY(library.toggleFolder(QStringLiteral("/work/project")));
            QVERIFY(library.toggleFolder(std::nullopt));
            QVERIFY(library.retitle(id, QStringLiteral("Renamed"), true));
            QJsonArray messages{
                QJsonObject{{"role", "user"},
                            {"text", "hi"},
                            {"backendTurn", "c1"},
                            {"pendingTurn", true},
                            {"secret", "dropped"}},
                QJsonObject{{"role", "system"}, {"content", "never kept"}},
                QJsonObject{{"role", "assistant"}, {"content", "yo"}, {"steps", QJsonArray{1}}},
                QJsonObject{{"role", "stats"}, {"stats", QJsonObject{{"uncounted", 2}}}}};
            QVERIFY(library.saveMessages(id, messages, 42));
        }
        Library reread(&files);
        QCOMPARE(reread.chats().size(), 3);
        const auto *chat = reread.chat(id);
        QVERIFY(chat->pinned && chat->named);
        QCOMPARE(chat->title, QStringLiteral("Renamed"));
        QVERIFY(reread.homeCollapsed());
        QVERIFY(reread.folder("/work/project")->collapsed);
        QCOMPARE(reread.inFolder("/work/project").size(), 1);
        const auto body = reread.conversation(id);
        QVERIFY(body);
        QCOMPARE(body->tokens, 42.0);
        QCOMPARE(body->messages.size(), 3); // unknown roles dropped
        const auto user = body->messages.at(0).toObject();
        QVERIFY(user.value("pendingTurn").toBool() && !user.contains("secret"));
        QVERIFY(body->messages.at(1).toObject().value("uncounted").toBool());
        // Folder removal takes its chats and their caches; home chats stay.
        QCOMPARE(reread.removeFolder("/work/project").size(), 1);
        QCOMPARE(reread.chats().size(), 2);
        QVERIFY(reread.remove(id));
        QCOMPARE(files.read("chats/" + id).status, KeyStore::Status::Absent);

        // An unreadable index is never replaced by an empty list.
        MemoryKeyStore memory;
        memory.raw.insert("index", "garbage");
        Library unreadable(&memory);
        QVERIFY(!unreadable.writable() && !unreadable.error().isEmpty());
        QVERIFY(!unreadable.create({}, "x") && !unreadable.persist());
        QVERIFY(memory.raw.contains("index"));
        memory.raw.clear();
        memory.values.insert("index", {{"version", 9}});
        QVERIFY(!Library(&memory).writable()); // unknown version: read-only
    }
    void restartRestoresAndReconciles()
    {
        FakeBackend fake(nullptr, 0);
        MemoryKeyStore store;
        PreferencesStore prefs({});
        QString id;
        {
            Library library(&store);
            UsageStore ledger(&store);
            ChatService chat(&fake, &prefs, &library);
            connect(&chat, &ChatService::usageRecorded, &ledger, &UsageStore::record);
            chat.initialize();
            QTRY_VERIFY(chat.ready());
            QVERIFY(chat.send("remember me"));
            QTRY_VERIFY(!chat.pending());
            settle(fake);
            QTRY_VERIFY(!chat.busy());
            id = chat.current().id;
            QVERIFY(chat.setPinned(id, true));
            QVERIFY(ledger.flush());
        }
        // A new frontend process over the same local store and live backend.
        Library library(&store);
        UsageStore ledger(&store);
        QCOMPARE(ledger.totals()["fake"].toMap()["tokens"].toDouble(), 130.0);
        ChatService chat(&fake, &prefs, &library);
        chat.initialize();
        QTRY_VERIFY(chat.connected());
        QCOMPARE(chat.chats().size(), 1);
        QVERIFY(chat.chats().first().pinned);
        QCOMPARE(chat.chats().first().title, QStringLiteral("remember me"));
        QVERIFY(!chat.chats().first().reconciled); // display only until session.get
        chat.open(id);
        QTRY_VERIFY(chat.ready());
        const auto &rows = chat.current().rows;
        QCOMPARE(rows.first().text, QStringLiteral("remember me"));
        QVERIFY(std::any_of(rows.cbegin(), rows.cend(), [](const auto &r) {
            return r.role == DisplayRow::Role::Assistant && r.text.contains("remember me") &&
                   r.usage && r.usage->input == 100;
        }));
        QVERIFY(
            std::none_of(rows.cbegin(), rows.cend(), [](const auto &r) { return r.pendingTurn; }));
        QVERIFY(chat.current().version.has_value()); // adopted from session.get
        QVERIFY(chat.send("and continue"));
        QTRY_VERIFY(!chat.pending());
        settle(fake);
        QTRY_VERIFY(!chat.busy());
        QVERIFY(chat.current().rows.last().text.contains("and continue"));

        // The backend's session is gone: the cache stays display-only, nothing is sent.
        FakeBackend fresh(nullptr, 0);
        Library again(&store);
        ChatService orphan(&fresh, &prefs, &again);
        orphan.initialize();
        QTRY_VERIFY(orphan.connected());
        orphan.open(id);
        QTRY_VERIFY(!orphan.pending());
        QVERIFY(!orphan.ready());
        QVERIFY(orphan.status().contains("missing"));
        QVERIFY(!orphan.current().rows.isEmpty());
        QCOMPARE(orphan.send("lost"), 0u);
    }
    void interruptedStartRecoversWithoutResend()
    {
        InspectBackend backend;
        MemoryKeyStore store;
        PreferencesStore prefs({});
        QString id, client;
        {
            Library library(&store);
            ChatService chat(&backend, &prefs, &library);
            chat.initialize();
            QTRY_VERIFY(chat.ready());
            backend.holdStart = true;
            QVERIFY(chat.send("uncertain start"));
            QTRY_VERIFY(backend.heldStart.has_value());
            id = chat.current().id;
            client = chat.current().turn.clientId;
            // The required checkpoint reached storage before the start went out.
            const auto saved = store.values.value("chats/" + id).value("messages").toArray();
            QCOMPARE(saved.first().toObject().value("backendTurn").toString(), client);
            QVERIFY(saved.first().toObject().value("pendingTurn").toBool());
        } // "crash": the acknowledgement never reached this frontend
        backend.holdStart = false;
        backend.heldStart.reset();
        settle(backend.fake);
        const auto starts = countOf<StartTurn>(backend.commands);
        Library library(&store);
        ChatService chat(&backend, &prefs, &library);
        chat.initialize();
        QTRY_VERIFY(chat.connected());
        chat.open(id);
        QTRY_VERIFY(chat.ready());
        QCOMPARE(countOf<StartTurn>(backend.commands), starts); // never resent
        const auto &get = std::get<GetSession>(backend.commands.last());
        QCOMPARE(get.clientTurnId.value_or(QString()), client);
        QCOMPARE(chat.current().rows.first().text, QStringLiteral("uncertain start"));
        QVERIFY(chat.current().rows.last().text.contains("uncertain start"));
        QVERIFY(std::none_of(chat.current().rows.cbegin(), chat.current().rows.cend(),
                             [](const auto &r) { return r.pendingTurn; }));
        QVERIFY(!store.values.value("chats/" + id)
                     .value("messages")
                     .toArray()
                     .first()
                     .toObject()
                     .value("pendingTurn")
                     .toBool());

        // A saved pending marker the backend never accepted: turn_missing, kept, not sent.
        auto messages = store.values.value("chats/" + id).value("messages").toArray();
        messages.append(QJsonObject{{"role", "user"},
                                    {"text", "never accepted"},
                                    {"backendTurn", "ghost-turn"},
                                    {"pendingTurn", true}});
        QVERIFY(store.write("chats/" + id, {{"version", 1}, {"messages", messages}}));
        Library third(&store);
        ChatService lost(&backend, &prefs, &third);
        lost.initialize();
        QTRY_VERIFY(lost.connected());
        lost.open(id);
        QTRY_VERIFY(!lost.pending());
        QVERIFY(!lost.ready());
        QVERIFY(lost.status().contains("did not accept the saved turn"));
        QCOMPARE(countOf<StartTurn>(backend.commands), starts);
        QCOMPARE(lost.current().rows.last().text, QStringLiteral("never accepted"));
    }
    void checkpointFailureNeverDispatches()
    {
        InspectBackend backend;
        MemoryKeyStore store;
        PreferencesStore prefs({});
        Library library(&store);
        ChatService chat(&backend, &prefs, &library);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        store.failWrites = "chats/";
        QVERIFY(chat.send("must not leave"));
        QCOMPARE(countOf<StartTurn>(backend.commands), 0);
        QVERIFY(chat.status().contains("checkpoint"));
        QVERIFY(chat.canRetry());
        const auto client = chat.current().turn.clientId;
        chat.retry(); // still failing: still nothing dispatched
        QCOMPARE(countOf<StartTurn>(backend.commands), 0);
        store.failWrites.clear();
        chat.retry();
        QTRY_VERIFY(!chat.pending());
        QCOMPARE(countOf<StartTurn>(backend.commands), 1);
        QCOMPARE(std::get<StartTurn>(backend.commands.last()).clientTurnId, client);
        settle(backend.fake);
        QTRY_VERIFY(!chat.busy());
        // Retry of a failed turn also checkpoints first; a steer failing it is not sent.
        QVERIFY(chat.send("/fake error"));
        QTRY_VERIFY(!chat.pending());
        settle(backend.fake);
        QTRY_VERIFY(chat.canRetry());
        store.failWrites = "chats/";
        const auto before = countOf<RetryTurn>(backend.commands);
        chat.retry();
        QCOMPARE(countOf<RetryTurn>(backend.commands), before);
        QVERIFY(chat.canRetry()); // the failed turn is intact
        store.failWrites.clear();
    }
    void miniChatLifecycle()
    {
        FakeBackend fake(nullptr, 0);
        MemoryKeyStore store;
        PreferencesStore prefs({});
        Library library(&store);
        ChatService chat(&fake, &prefs, &library);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QVERIFY(!chat.openMini().isEmpty()); // a draft has no mini chat
        QVERIFY(chat.send("main question"));
        QTRY_VERIFY(!chat.pending());
        settle(fake);
        QTRY_VERIFY(!chat.busy());
        const auto id = chat.current().id;
        const auto mainRows = chat.current().rows.size();
        QVERIFY(chat.openMini().isEmpty());
        QTRY_VERIFY(chat.mini() && chat.mini()->reconciled); // new mini session: missing + empty
        QVERIFY(chat.sendMini("side question"));
        QTRY_VERIFY(!chat.pending());
        settle(fake);
        QTRY_VERIFY(chat.mini()->turn.terminal);
        QCOMPARE(chat.mini()->id, id + ":mini");
        QVERIFY(chat.mini()->rows.last().text.contains("side question"));
        QCOMPARE(chat.current().id, id); // main view untouched
        QCOMPARE(chat.current().rows.size(), mainRows);
        QCOMPARE(chat.chats().size(), 1); // the mini session is not a listed chat
        QVERIFY(store.values.contains("mini/" + id));
        QVERIFY(!chat.miniBehind());
        chat.closeMini();
        QVERIFY(!chat.mini());

        QTest::qWait(5);
        QVERIFY(chat.send("main moves on"));
        QTRY_VERIFY(!chat.pending());
        settle(fake);
        QTRY_VERIFY(!chat.busy());
        QVERIFY(chat.openMini().isEmpty());
        QVERIFY(chat.miniBehind());
        QVERIFY(chat.sendMini("after the move"));
        QTRY_VERIFY(!chat.pending());
        settle(fake);
        QTRY_VERIFY(chat.mini()->turn.terminal);
        QVERIFY(std::any_of(chat.mini()->rows.cbegin(), chat.mini()->rows.cend(),
                            [](const auto &r) { return r.role == DisplayRow::Role::Moved; }));
        QVERIFY(!chat.miniBehind());
        const auto saved = store.values.value("mini/" + id);
        QVERIFY(saved.value("seen").toDouble() > 0);
        QVERIFY(std::any_of(saved.value("messages").toArray().cbegin(),
                            saved.value("messages").toArray().cend(),
                            [](const auto &m) { return m.toObject().value("role") == "moved"; }));

        // Restart: the mini chat reopens from its own cache and reconciles.
        {
            Library reread(&store);
            ChatService again(&fake, &prefs, &reread);
            again.initialize();
            QTRY_VERIFY(again.connected());
            again.open(id);
            QTRY_VERIFY(again.ready());
            QVERIFY(again.openMini().isEmpty());
            QTRY_VERIFY(again.mini()->reconciled);
            QVERIFY(again.mini()->rows.last().text.contains("after the move"));
        }
        QSignalSpy cleared(&chat, &ChatService::miniCleared);
        chat.clearMini();
        QTRY_COMPARE(cleared.size(), 1);
        QVERIFY(cleared.first().first().toBool());
        QVERIFY(chat.mini()->rows.isEmpty() && !chat.mini()->version);
        QVERIFY(!store.values.contains("mini/" + id));
        QVERIFY(!chat.current().rows.isEmpty()); // only the mini session was deleted
        QSignalSpy removed(&chat, &ChatService::removed);
        chat.remove(id);
        QTRY_COMPARE(removed.size(), 1);
        QVERIFY(removed.first().at(1).toBool());
        QVERIFY(!store.values.contains("chats/" + id));
        QCOMPARE(ask(fake, GetSession{id + ":mini", {}}).index(), 0u);
    }
    void locksFailClosed()
    {
        FakeBackend fake(nullptr, 0);
        MemoryKeyStore store;
        PreferencesStore prefs({});
        TestSealer sealer;
        QString id;
        {
            Library library(&store, &sealer);
            ChatService chat(&fake, &prefs, &library);
            chat.initialize();
            QTRY_VERIFY(chat.ready());
            QVERIFY(chat.send("private words"));
            QTRY_VERIFY(!chat.pending());
            settle(fake);
            QTRY_VERIFY(!chat.busy());
            id = chat.current().id;
            QVERIFY(chat.protect(id, "pw").isEmpty());
            QVERIFY(chat.current().locked && chat.current().rows.isEmpty());
            QVERIFY(store.values.value("chats/" + id).contains("sealed"));
            // The title is sealed (its home `space` folder name stays plain, as in library.js).
            QVERIFY(store.values.value("index")
                        .value("chats")
                        .toArray()
                        .first()
                        .toObject()
                        .value("title")
                        .toString()
                        .isEmpty());
            QCOMPARE(chat.send("refused"), 0u);
            QCOMPARE(chat.unlock(id, "wrong"), QStringLiteral("Wrong password."));
            QVERIFY(chat.unlock(id, "pw").isEmpty());
            QCOMPARE(chat.current().rows.first().text, QStringLiteral("private words"));
            QCOMPARE(chat.current().title, QStringLiteral("private words"));
        }
        // Without a sealer (this build's production state) a protected chat stays
        // locked and is never overwritten as empty.
        Library plain(&store);
        QVERIFY(plain.isLocked(id));
        QVERIFY(!plain.conversation(id));
        QVERIFY(!plain.saveMessages(id, {}, 0));
        QVERIFY(!plain.protect(plain.chats().first().id, "pw").isEmpty());
        ChatService locked(&fake, &prefs, &plain);
        locked.initialize();
        QTRY_VERIFY(locked.connected());
        QCOMPARE(locked.chats().first().title, QStringLiteral("Locked local view"));
        locked.open(id);
        QVERIFY(locked.current().locked && locked.status().contains("locked"));
        QVERIFY(!locked.unlock(id, "pw").isEmpty());
        QVERIFY(store.values.value("chats/" + id).contains("sealed"));
        // With the key, removing the password writes the clear copy first.
        Library keyed(&store, &sealer);
        QVERIFY(keyed.unlock(id, "pw").isEmpty());
        QVERIFY(keyed.unprotect(id).isEmpty());
        QVERIFY(!store.values.value("chats/" + id).contains("sealed"));
        QCOMPARE(*keyed.titleOf(id), QStringLiteral("private words"));
    }
    void usageLedgerTimerPersists()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FileKeyStore store(dir.path());
        UsageStore ledger(&store);
        Usage usage;
        usage.provider = "p|x";
        usage.model = "m";
        usage.modelName = "Model";
        usage.input = 10;
        usage.cached = 20; // clamped to input
        usage.written = 3;
        usage.output = 5;
        usage.requests = 2;
        const auto today = QDate::currentDate().toString(Qt::ISODate);
        ledger.record(usage);
        QCOMPARE(store.read("usage").status, KeyStore::Status::Absent); // still debounced
        QJsonObject expected{
            {"version", 2},
            {"since", ledger.since()},
            {"days", QJsonObject{{today, QJsonObject{{R"(["p|x","m"])",
                                                    QJsonArray{10, 10, 3, 5, 2}}}}}},
            {"names", QJsonObject{{R"(["p|x","m"])", "Model"}}}};
        // No explicit flush or destruction: only the real timer can publish it.
        QTRY_COMPARE_WITH_TIMEOUT(store.read("usage").value, expected, UsageStore::SaveDelay * 3);
        usage.input = 4;
        usage.cached = 1;
        usage.written = 0;
        usage.output = 1;
        usage.requests = 1;
        const auto nextDay = QDate::currentDate().toString(Qt::ISODate);
        ledger.record(usage);
        auto days = expected.value("days").toObject();
        days.insert(nextDay, QJsonObject{{R"(["p|x","m"])", nextDay == today
                                                                ? QJsonArray{14, 11, 3, 6, 3}
                                                                : QJsonArray{4, 1, 0, 1, 1}}});
        expected.insert("days", days);
        QTRY_COMPARE_WITH_TIMEOUT(store.read("usage").value, expected, UsageStore::SaveDelay * 3);
        UsageStore reread(&store);
        QCOMPARE(reread.totals()["p|x"].toMap()["tokens"].toDouble(), 20.0);
        QCOMPARE(reread.nameOf(R"(["p|x","m"])"), QStringLiteral("Model"));
        QCOMPARE(reread.since(), ledger.since());
    }
    void usageLedgerSaveFailure_data()
    {
        QTest::addColumn<QString>("retry");
        QTest::newRow("explicit-flush") << QStringLiteral("flush");
        QTest::newRow("next-record") << QStringLiteral("record");
        QTest::newRow("destructor") << QStringLiteral("destructor");
    }
    void usageLedgerSaveFailure()
    {
        QFETCH(QString, retry);
        MemoryKeyStore store;
        double expectedTokens = 30;
        {
            UsageStore ledger(&store);
            QSignalSpy failed(&ledger, &UsageStore::saveFailed);
            Usage usage;
            usage.provider = "p";
            usage.model = "m";
            usage.input = 10;
            usage.output = 5;
            ledger.record(usage);
            QVERIFY(ledger.flush());
            const auto saved = store.values.value("usage");
            store.failWrites = "usage";
            ledger.record(usage);
            // The timer's return value has no caller: failure must also signal.
            QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, UsageStore::SaveDelay * 3);
            QCOMPARE(failed.first().first().toString(), QStringLiteral("Usage could not be saved."));
            QCOMPARE(store.values.value("usage"), saved);
            QCOMPARE(ledger.totals()["p"].toMap()["tokens"].toDouble(), expectedTokens);
            // The failed timeout left the write pending, even with no active timer.
            QVERIFY(!ledger.flush());
            QCOMPARE(failed.size(), 2);
            QCOMPARE(store.values.value("usage"), saved);
            store.failWrites.clear();
            if (retry == "flush") {
                QVERIFY(ledger.flush());
            } else if (retry == "record") {
                ledger.record(usage); // write failure must not block further usage
                expectedTokens += 15;
                QTRY_VERIFY_WITH_TIMEOUT(store.values.value("usage") != saved,
                                         UsageStore::SaveDelay * 3);
            } else {
                QCOMPARE(store.values.value("usage"), saved); // destructor must retry
            }
            if (retry != "destructor") {
                UsageStore reread(&store);
                QCOMPARE(reread.totals()["p"].toMap()["tokens"].toDouble(), expectedTokens);
                store.failWrites = "usage";
                QVERIFY(ledger.flush()); // clean: do not attempt another write
                QCOMPARE(failed.size(), 2);
            }
        }
        UsageStore reread(&store);
        QCOMPARE(reread.totals()["p"].toMap()["tokens"].toDouble(), expectedTokens);
        QCOMPARE(store.values.value("usage").value("version").toInt(), 2);
    }
    void usageLedgerPersists()
    {
        MemoryKeyStore store;
        {
            UsageStore ledger(&store);
            Usage usage;
            usage.provider = "p|x";
            usage.model = "m";
            usage.modelName = "Model";
            usage.input = 10;
            usage.cached = 20; // clamped to input
            usage.output = 5;
            ledger.record(usage);
        } // destructor flushes the debounced save
        UsageStore reread(&store);
        const auto total = reread.totals()["p|x"].toMap();
        QCOMPARE(total["tokens"].toDouble(), 15.0);
        QCOMPARE(total["cached"].toDouble(), 10.0);
        QCOMPARE(reread.nameOf(R"(["p|x","m"])"), QStringLiteral("Model"));
        QVERIFY(reread.since() > 0);
        // Version 1 keys upgrade on read.
        const auto today = QDate::currentDate().toString(Qt::ISODate);
        store.values.insert(
            "usage",
            {{"version", 1},
             {"since", 1},
             {"days", QJsonObject{{today, QJsonObject{{"prov|mod", QJsonArray{1, 0, 0, 2, 1}}}}}},
             {"names", QJsonObject{}}});
        UsageStore old(&store);
        QCOMPARE(old.totals()["prov"].toMap()["tokens"].toDouble(), 3.0);
        QCOMPARE(old.nameOf(R"(["prov","mod"])"), QStringLiteral("mod"));
        Usage increment;
        increment.provider = "prov";
        increment.model = "mod";
        increment.input = 2;
        increment.output = 1;
        old.record(increment);
        QVERIFY(old.flush());
        const auto upgraded = store.values.value("usage");
        QCOMPARE(upgraded.value("version").toInt(), 2);
        QCOMPARE(upgraded.value("since").toDouble(), 1.0);
        QCOMPARE(upgraded.value("days").toObject().value(today).toObject(),
                 QJsonObject({{R"(["prov","mod"])", QJsonArray{3, 0, 0, 3, 2}}}));
        // An unreadable ledger is neither extended nor overwritten.
        store.raw.insert("usage", "broken");
        UsageStore broken(&store);
        QVERIFY(!broken.error().isEmpty());
        Usage usage;
        usage.provider = usage.model = "x";
        usage.input = 1;
        broken.record(usage);
        QVERIFY(broken.flush());
        QVERIFY(store.raw.contains("usage") && broken.totals().isEmpty());
        store.raw.clear();
        const QJsonObject future{{"version", 3}, {"unknown", "keep"}};
        store.values.insert("usage", future);
        UsageStore unknown(&store);
        QVERIFY(!unknown.error().isEmpty());
        unknown.record(usage);
        QVERIFY(unknown.flush());
        QVERIFY(unknown.totals().isEmpty());
        QCOMPARE(store.values.value("usage"), future);
    }
    void hostToolsRoutedAndReleased()
    {
        InspectBackend backend;
        PreferencesStore prefs({});
        ScriptedHost host;
        MemoryKeyStore store;
        Library library(&store);
        ChatService chat(&backend, &prefs, &library, &host);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QCOMPARE(std::get<Initialize>(backend.commands.first()).tools.size(), 1);
        QVERIFY(chat.send("/fake approval"));
        QTRY_VERIFY(!chat.current().turn.remoteId.isEmpty());
        const auto session = chat.current().id, turn = chat.current().turn.remoteId;
        const auto answered = [&](RequestId id) {
            return std::count_if(backend.answers.cbegin(), backend.answers.cend(),
                                 [id](const auto &a) { return a.first == id; });
        };
        emit backend.reverseRequest(501, HostToolRequest{session, turn, "t1", "browser_open", {}});
        QCOMPARE(answered(501), 1); // unpublished tool: unsupported
        emit backend.reverseRequest(502,
                                    HostToolRequest{session, "old", "t2", "browser_snapshot", {}});
        QCOMPARE(answered(502), 1); // stale turn
        emit backend.reverseRequest(503,
                                    HostToolRequest{session, turn, "t3", "browser_snapshot", {}});
        QCOMPARE(host.ran, QVector<RequestId>{503});
        QCOMPARE(answered(503), 0);
        emit host.finished(503, HostToolResult{{HostToolResult::Text{"page"}}, false, {}, {}, {}});
        emit host.finished(503, HostToolResult{});
        QCOMPARE(answered(503), 1); // exactly once
        emit backend.reverseRequest(506,
                                    HostToolRequest{session, turn, "t3", "browser_snapshot", {}});
        QCOMPARE(host.ran, QVector<RequestId>{503}); // spent even after completion
        QCOMPARE(answered(506), 1);
        emit backend.reverseRequest(504,
                                    HostToolRequest{session, turn, "t4", "browser_snapshot", {}});
        emit backend.reverseCancelled(504);
        QCOMPARE(host.cancelled, QVector<RequestId>{504});
        emit host.finished(504, HostToolResult{});
        QCOMPARE(answered(504), 1); // cancellation settles, late callback is dropped
        emit backend.reverseRequest(505,
                                    HostToolRequest{session, turn, "t5", "browser_snapshot", {}});
        chat.stop(); // the turn ends: its running host step is released
        QVERIFY(host.cancelled.contains(505));
        QCOMPARE(answered(505), 1);
        const auto &last = std::get<HostToolResult>(
            std::find_if(backend.answers.cbegin(), backend.answers.cend(), [](const auto &a) {
                return a.first == 505;
            })->second);
        QCOMPARE(last.status, HostToolResult::Status::Cancelled);
        QVERIFY(last.content.isEmpty());
        QVERIFY(!last.isError);
    }
    void hostHandBackRejectsAlreadyQueuedMessage()
    {
        InspectBackend backend;
        PreferencesStore prefs({});
        ScriptedHost host;
        MemoryKeyStore store;
        Library library(&store);
        ChatService chat(&backend, &prefs, &library, &host);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QVERIFY(chat.send("/fake approval"));
        QTRY_VERIFY(!chat.current().turn.remoteId.isEmpty());
        const auto session = chat.current().id, turn = chat.current().turn.remoteId;
        host.context = BrowserState{};
        host.context->available = true;
        host.context->control = BrowserState::Control::User;
        QVERIFY(chat.send("New instruction while the user owns the page"));
        emit backend.reverseRequest(601, HostToolRequest{session, turn, "held", "browser_snapshot", {}});
        QVERIFY(host.ran.isEmpty());
        const auto &answer = std::get<HostToolResult>(backend.answers.last().second);
        QCOMPARE(answer.status, HostToolResult::Status::Cancelled);
        QCOMPARE(answer.reason.value(), "message");
        QVERIFY(answer.content.isEmpty());
    }
    void browserToolFixtures_data()
    {
        QTest::addColumn<QJsonObject>("fixture");
        const auto cases = browser_tools_test::fixture("cases.json");
        QCOMPARE(cases.size(), 11);
        for (const auto &value : cases) {
            const auto row = value.toObject();
            QTest::newRow(qPrintable(row.value("name").toString())) << row;
        }
    }
    void browserToolFixtures()
    {
        QFETCH(QJsonObject, fixture);
        InspectBackend backend;
        PreferencesStore prefs({});
        ScriptedHost host;
        host.definitions = browser_tools_test::schemas();
        QCOMPARE(host.definitions.size(), 11);
        MemoryKeyStore store;
        Library library(&store);
        ChatService chat(&backend, &prefs, &library, &host);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        const auto published = std::get<Initialize>(backend.commands.first()).tools;
        QCOMPARE(published.size(), 11);
        for (qsizetype i = 0; i < published.size(); ++i) {
            QCOMPARE(published[i].name, host.definitions[i].name);
            QCOMPARE(published[i].description, host.definitions[i].description);
            QCOMPARE(published[i].parameters, host.definitions[i].parameters);
        }
        QVERIFY(chat.send("/fake approval"));
        QTRY_VERIFY(!chat.current().turn.remoteId.isEmpty());
        const HostToolRequest request{chat.current().id, chat.current().turn.remoteId,
                                      "fixture-call", fixture.value("name").toString(),
                                      fixture.value("args").toObject()};
        emit backend.reverseRequest(801, request);
        QCOMPARE(host.ran, QVector<RequestId>{801});
        QCOMPARE(host.received.value(801).args, request.args);
        QCOMPARE(host.received.value(801).name, request.name);
        QCOMPARE(host.received.value(801).sessionId, request.sessionId);
        QCOMPARE(host.received.value(801).turnId, request.turnId);
        QCOMPARE(host.received.value(801).toolCallId, request.toolCallId);
        QVERIFY(backend.answers.isEmpty()); // passive host has performed no operation
        const auto expected = browser_tools_test::result(fixture.value("result").toString());
        QVERIFY(!expected.isEmpty());
        const auto result = browser_tools_test::body(expected);
        emit host.finished(801, result);
        QCOMPARE(backend.answers.size(), 1);
        QCOMPARE(backend.answers.first().first, RequestId(801));
        QVERIFY(std::holds_alternative<HostToolResult>(backend.answers.first().second));
        browser_tools_test::compare(std::get<HostToolResult>(backend.answers.first().second), result);
        emit host.finished(801, result);
        QCOMPARE(backend.answers.size(), 1); // late duplicate settlement is dropped
    }
    void browserResultFixtures_data()
    {
        QTest::addColumn<QJsonObject>("fixture");
        const auto results = browser_tools_test::fixture("results.json");
        QCOMPARE(results.size(), 10);
        for (const auto &value : results) {
            const auto row = value.toObject();
            QTest::newRow(qPrintable(row.value("id").toString())) << row.value("expected").toObject();
        }
    }
    void browserResultFixtures()
    {
        QFETCH(QJsonObject, fixture);
        InspectBackend backend;
        PreferencesStore prefs({});
        ScriptedHost host;
        MemoryKeyStore store;
        Library library(&store);
        ChatService chat(&backend, &prefs, &library, &host);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        QVERIFY(chat.send("/fake approval"));
        QTRY_VERIFY(!chat.current().turn.remoteId.isEmpty());
        auto result = browser_tools_test::body(fixture);
        // All status values must be forwarded, not inferred from text/data.code.
        const QVector<HostToolResult::Status> statuses{
            result.status, HostToolResult::Status::HandedBack, HostToolResult::Status::Cancelled};
        RequestId id = 900;
        for (const auto status : statuses) {
            ++id;
            result.status = status;
            result.reason = status == HostToolResult::Status::Cancelled
                                ? std::optional<QString>("message")
                                : std::nullopt;
            emit backend.reverseRequest(
                id, HostToolRequest{chat.current().id, chat.current().turn.remoteId,
                                    QString::number(id), "browser_snapshot", {}});
            QVERIFY(!host.ran.isEmpty());
            QCOMPARE(host.ran.last(), id);
            emit host.finished(id, result);
            QVERIFY(!backend.answers.isEmpty());
            QCOMPARE(backend.answers.last().first, id);
            QVERIFY(std::holds_alternative<HostToolResult>(backend.answers.last().second));
            browser_tools_test::compare(std::get<HostToolResult>(backend.answers.last().second),
                                        result);
        }
        // This only qualifies envelope transport inside C++, not cancellation
        // orchestration or whether a real host can produce any of these results.
    }
    void foldersAndPinsThroughService()
    {
        FakeBackend fake(nullptr, 0);
        MemoryKeyStore store;
        PreferencesStore prefs({});
        Library library(&store);
        ChatService chat(&fake, &prefs, &library);
        chat.initialize();
        QTRY_VERIFY(chat.ready());
        chat.newChat(QStringLiteral("/unknown"));
        QVERIFY(chat.current().folder.isEmpty()); // unknown folders are not invented
        QVERIFY(chat.addFolder(QStringLiteral("/work/a")).isEmpty());
        chat.newChat(QStringLiteral("/work/a"));
        QCOMPARE(chat.current().folder, QStringLiteral("/work/a"));
        for (const auto *text : {"first in folder", "second in folder"}) {
            chat.newChat(QStringLiteral("/work/a"));
            QVERIFY(chat.send(QString::fromLatin1(text)));
            QTRY_VERIFY(!chat.pending());
            settle(fake);
            QTRY_VERIFY(!chat.busy());
        }
        const auto ids = library.inFolder("/work/a");
        QCOMPARE(ids.size(), 2);
        QVERIFY(!library.isHome(*library.chat(ids.first())));
        QVERIFY(chat.setPinned(ids.first(), true));
        QVERIFY(Library(&store).chat(ids.first())->pinned);
        QSignalSpy folders(&chat, &ChatService::folderRemoved);
        chat.removeFolder(QStringLiteral("/work/a"));
        QTRY_COMPARE(folders.size(), 1);
        QVERIFY(folders.first().at(1).toBool());
        QVERIFY(library.chats().isEmpty() && !library.folder("/work/a"));
        QVERIFY(chat.chats().isEmpty());
        for (const auto &id : ids) // both backend sessions were deleted
            QCOMPARE(
                std::get<SessionRecovery>(std::get<Reply>(ask(fake, GetSession{id, {}}))).index(),
                0u);
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
