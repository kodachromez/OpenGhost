#include "pi_backend.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>
#include <cstdio>
#include <utility>

namespace openghost
{
namespace
{
constexpr int StartDeadline = 60000; // Pi's prompt reply, as the reference's turn.start
constexpr int AbortDeadline = 60000; // abort replies once Pi is idle
constexpr int StepDeadline = 30000;  // context, model and turn records
constexpr qsizetype MaxJournals = 64;
constexpr qint64 IdleChild = 10 * 60 * 1000; // an idle chat's Pi child is then closed
constexpr int MaxIdleChildren = 4;           // beyond these, the oldest idle ones close
const QString Mark = QStringLiteral("openghost-turn"); // the bridge's turn records
const QString RetryTrigger = QStringLiteral("openghost-retry");

QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString modeName(PermissionMode mode)
{
    return mode == PermissionMode::Full   ? QStringLiteral("full")
           : mode == PermissionMode::Auto ? QStringLiteral("auto")
                                          : QStringLiteral("ask");
}
const QString ApprovalTitle = QStringLiteral("openghost:approval"); // a Pi permission request
// An approval card's presentation, as the permission request describes the call.
std::optional<ApprovalPresentation> presentationOf(const QJsonValue &value)
{
    if (!value.isObject())
        return std::nullopt;
    const auto given = value.toObject();
    ApprovalPresentation shown;
    shown.kind = given.value("kind").toString();
    shown.title = given.value("title").toString();
    const auto text = [&](const char *key) -> std::optional<QString> {
        return given.value(key).isString() ? std::optional(given.value(key).toString())
                                           : std::nullopt;
    };
    shown.effect = text("effect");
    if (given.value("badge").isBool())
        shown.badge = given.value("badge").toBool();
    for (const auto &item : given.value("places").toArray()) {
        const auto place = item.toObject();
        shown.places.append({place.value("kind").toString(), place.value("label").toString(),
                             place.value("title").toString()});
    }
    shown.code = text("code");
    shown.removed = text("removed");
    shown.added = text("added");
    shown.quote = text("quote");
    shown.reveal = text("reveal");
    return shown;
}
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
Error failure(const QString &code, const QString &message)
{
    return {code, message, {}, QStringLiteral("none"), false, {}};
}
QString errorOf(const QJsonObject &reply, const QString &fallback)
{
    return reply.value("error").toString(fallback);
}
bool succeeded(const QJsonObject &reply)
{
    return reply.value("success").toBool(true) && reply.value("ok").toBool(true);
}
QString journalKey(const QString &session, const QString &client) { return session + '\n' + client; }
// Pi's text content: a string, or its text blocks joined as Pi joins them.
QString textOf(const QJsonValue &content)
{
    if (content.isString())
        return content.toString();
    QString text;
    for (const auto &block : content.toArray())
        if (block.toObject().value("type").toString() == QStringLiteral("text"))
            text += block.toObject().value("text").toString();
    return text;
}
std::optional<QString> finishOf(const QString &stopReason)
{
    if (stopReason == QStringLiteral("stop") || stopReason == QStringLiteral("length"))
        return stopReason;
    if (stopReason == QStringLiteral("toolUse"))
        return QStringLiteral("tool_calls");
    return std::nullopt;
}
// A Pi assistant message's final usage, once: Pi's per-update usage is cumulative.
// Cached input is part of OpenGhost's input count; Pi's reasoning is in its output.
std::optional<Usage> usageOf(const QJsonObject &message)
{
    const auto spent = message.value("usage").toObject();
    Usage usage;
    usage.provider = message.value("provider").toString();
    usage.model = message.value("model").toString();
    usage.cached = spent.value("cacheRead").toDouble();
    usage.written = spent.value("cacheWrite").toDouble();
    usage.input = spent.value("input").toDouble() + usage.cached + usage.written;
    usage.output = spent.value("output").toDouble();
    usage.requests = 1;
    if (usage.provider.isEmpty() || usage.model.isEmpty() || (usage.input <= 0 && usage.output <= 0))
        return std::nullopt;
    return usage;
}
DisplayInput displayOf(const Input &input)
{
    DisplayInput display{input.text, {}};
    for (const auto &attachment : input.attachments) {
        DisplayAttachment shown;
        shown.name = attachment.name;
        shown.size = attachment.size;
        shown.image = attachment.kind == Attachment::Kind::Image;
        display.attachments.append(shown);
    }
    return display;
}
// The display copy kept in a turn's start record: names and sizes, no contents.
QJsonObject displayJson(const DisplayInput &input)
{
    QJsonArray attachments;
    for (const auto &attachment : input.attachments) {
        QJsonObject shown{{"name", attachment.name}};
        if (attachment.size)
            shown.insert("size", double(*attachment.size));
        if (attachment.image)
            shown.insert("image", *attachment.image);
        attachments.append(shown);
    }
    return {{"text", input.text}, {"attachments", attachments}};
}
DisplayInput displayFrom(const QJsonObject &saved)
{
    DisplayInput input{saved.value("text").toString(), {}};
    for (const auto &item : saved.value("attachments").toArray()) {
        const auto object = item.toObject();
        DisplayAttachment shown;
        shown.name = object.value("name").toString();
        if (object.contains("size"))
            shown.size = qint64(object.value("size").toDouble());
        if (object.contains("image"))
            shown.image = object.value("image").toBool();
        input.attachments.append(shown);
    }
    return input;
}
// An input as Pi's prompt takes it, the way Pi's own `pi @file` builds one: each
// text file's contents in a <file> element, a picture's name in an empty one with
// the picture among the prompt's images, then the message itself. Nothing is
// dropped: an attachment that is neither is refused by the caller.
struct Prompt {
    QString message;
    QJsonArray images;
};
bool deliverable(const Attachment &a)
{
    return a.kind == Attachment::Kind::Image
               ? a.dataUrl && a.dataUrl->startsWith(QStringLiteral("data:")) &&
                     a.dataUrl->contains(QStringLiteral(";base64,")) && !a.mime.isEmpty()
               : a.kind == Attachment::Kind::Text && a.text.has_value();
}
QString fileTag(const QString &name)
{
    return QStringLiteral("<file name=\"%1\">").arg(name.toHtmlEscaped());
}
Prompt promptOf(const Input &input)
{
    Prompt prompt;
    for (const auto &a : input.attachments) {
        if (a.kind == Attachment::Kind::Image) {
            const auto data = a.dataUrl->mid(a.dataUrl->indexOf(QLatin1Char(',')) + 1);
            prompt.images.append(
                QJsonObject{{"type", "image"}, {"data", data}, {"mimeType", a.mime}});
            prompt.message += fileTag(a.name) + QStringLiteral("</file>\n");
        } else {
            prompt.message += fileTag(a.name) + QLatin1Char('\n') + *a.text +
                              QStringLiteral("\n</file>\n");
        }
    }
    prompt.message += input.text;
    return prompt;
}
// Settings → General as the bridge's `context`: instructions and pinned text files.
QJsonObject contextOf(const UserContext &context)
{
    QJsonArray files;
    for (const auto &file : context.files)
        if (file.text)
            files.append(QJsonObject{{"name", file.name}, {"text", *file.text}});
    return {{"instructions", context.instructions}, {"files", files}};
}
QString statusName(TurnStatus status)
{
    return status == TurnStatus::Done        ? QStringLiteral("done")
           : status == TurnStatus::Cancelled ? QStringLiteral("cancelled")
                                             : QStringLiteral("error");
}
// One of OpenGhost's turn records in a Pi session ("start" or "end").
bool isMark(const QJsonObject &entry, const QString &event)
{
    return entry.value("type").toString() == QStringLiteral("custom") &&
           entry.value("customType").toString() == Mark &&
           entry.value("data").toObject().value("event").toString() == event;
}
QJsonObject markOf(const QJsonValue &entry) { return entry.toObject().value("data").toObject(); }
// Whether Pi took the turn whose start record is entries[at]: Pi's session holds
// its prompt (or its retry trigger) before the next turn's start, or its end record.
// A start Pi refused is followed by neither.
bool taken(const QJsonArray &entries, qsizetype at)
{
    const auto mark = markOf(entries[at]);
    const bool retry = mark.value("retry").toBool();
    bool open = true;
    for (qsizetype i = at + 1; i < entries.size(); ++i) {
        const auto entry = entries[i].toObject();
        if (isMark(entry, QStringLiteral("end")) &&
            markOf(entry).value("turn").toString() == mark.value("turn").toString())
            return true;
        if (isMark(entry, QStringLiteral("start")))
            open = false;
        if (!open)
            continue;
        const auto type = entry.value("type").toString();
        if (!retry && type == QStringLiteral("message") &&
            entry.value("message").toObject().value("role").toString() == QStringLiteral("user"))
            return true;
        if (retry && type == QStringLiteral("custom_message") &&
            entry.value("customType").toString() == RetryTrigger)
            return true;
    }
    return false;
}
QVector<Provider> providersOf(const QJsonArray &catalog)
{
    QVector<Provider> list;
    for (const auto &entry : catalog) {
        const auto item = entry.toObject();
        Provider provider;
        provider.id = item.value("id").toString();
        provider.name = item.value("name").toString(provider.id);
        if (item.value("oauth").isString()) // Pi's sign-in flow's own name
            provider.methods.append({AuthMethod::Kind::OAuth, item.value("oauth").toString(),
                                     item.value("oauth").toString(), {}, {}, {}});
        if (item.value("apiKey").isString())
            provider.methods.append({AuthMethod::Kind::ApiKey, item.value("apiKey").toString(),
                                     {}, {}, item.value("apiKey").toString(), {}});
        provider.status.connected = item.value("connected").toBool();
        provider.status.keySaved = item.value("stored").toBool();
        if (!provider.id.isEmpty())
            list.append(provider);
    }
    // Settings → Providers: Pi's whole catalog by display name.
    std::stable_sort(list.begin(), list.end(), [](const Provider &a, const Provider &b) {
        return QString::localeAwareCompare(a.name.toCaseFolded(), b.name.toCaseFolded()) < 0;
    });
    return list;
}
QVector<Model> modelsOf(const QJsonArray &catalog)
{
    QVector<Model> models;
    for (const auto &entry : catalog) {
        const auto model = entry.toObject();
        const auto provider = model.value("provider").toString();
        if (provider.isEmpty())
            continue;
        Model item{model.value("id").toString(), provider, model.value("name").toString(),
                   {}, model.value("input").toArray().contains(QStringLiteral("image")),
                   {}, {}};
        if (model.contains("contextWindow"))
            item.contextWindow = model.value("contextWindow").toDouble();
        models.append(item);
    }
    return models;
}
// The turn whose start record names `client`, rebuilt from Pi's own session
// entries: its prompt, each assistant message with its usage, and its outcome.
// Only a turn Pi took; a run that never ended (OpenGhost or Pi exited) is said so.
std::optional<std::pair<QJsonObject, QVector<EventPayload>>>
rebuilt(const QJsonArray &entries, const QString &client, TurnStatus *status)
{
    qsizetype at = -1;
    for (qsizetype i = 0; i < entries.size(); ++i)
        if (isMark(entries[i].toObject(), QStringLiteral("start")) &&
            markOf(entries[i]).value("client").toString() == client && taken(entries, i))
            at = i;
    if (at < 0)
        return std::nullopt;
    const auto mark = markOf(entries[at]);
    const auto turn = mark.value("turn").toString();
    const bool retry = mark.value("retry").toBool();
    QVector<EventPayload> events{TurnStarted{}};
    QString stopReason, errorMessage;
    std::optional<QJsonObject> ended;
    bool open = true, prompted = false;
    for (qsizetype i = at + 1; i < entries.size(); ++i) {
        const auto entry = entries[i].toObject();
        if (isMark(entry, QStringLiteral("end")) && markOf(entry).value("turn").toString() == turn) {
            ended = markOf(entry);
            break; // Later extension messages do not belong to this settled turn.
        }
        if (isMark(entry, QStringLiteral("start")))
            open = false;
        if (!open || entry.value("type").toString() != QStringLiteral("message"))
            continue;
        const auto message = entry.value("message").toObject();
        const auto role = message.value("role").toString();
        if (role == QStringLiteral("user") && !retry && !prompted) {
            prompted = true; // the prompt itself; later ones are steering
            continue;
        }
        if (role != QStringLiteral("assistant"))
            continue;
        MessageStarted started;
        if (!message.value("model").toString().isEmpty())
            started.model = message.value("model").toString();
        stopReason = message.value("stopReason").toString();
        errorMessage = message.value("errorMessage").toString();
        events.append(started);
        events.append(MessageCompleted{textOf(message.value("content")), finishOf(stopReason)});
        if (const auto usage = usageOf(message))
            events.append(*usage);
    }
    TurnCompleted done;
    if (ended) {
        const auto name = ended->value("status").toString();
        done.status = name == QStringLiteral("done")        ? TurnStatus::Done
                      : name == QStringLiteral("cancelled") ? TurnStatus::Cancelled
                                                            : TurnStatus::Error;
        if (ended->value("finish").isString())
            done.finishReason = ended->value("finish").toString();
        if (done.status == TurnStatus::Error)
            done.error = Error{ended->value("code").toString(QStringLiteral("model_error")),
                               ended->value("error").toString(QStringLiteral("Pi reported an error.")),
                               {}, {}, {}, {}};
    } else if (stopReason == QStringLiteral("aborted")) {
        done.status = TurnStatus::Cancelled;
    } else if (stopReason == QStringLiteral("error")) {
        done.status = TurnStatus::Error;
        done.error = Error{QStringLiteral("model_error"),
                           errorMessage.isEmpty() ? QStringLiteral("Pi reported an error.") : errorMessage,
                           {}, {}, {}, {}};
    } else {
        done.status = TurnStatus::Error;
        done.error = failure(QStringLiteral("interrupted"),
                             QStringLiteral("Pi stopped before this reply finished. Nothing was "
                                            "resent."));
    }
    *status = done.status;
    events.append(done);
    return std::pair{mark, events};
}
} // namespace

PiBackend::PiBackend(QString sessionDir, QObject *parent)
    : Backend(parent),
      m_sessionDir(sessionDir.isEmpty() ? m_ownSessions.path() : std::move(sessionDir))
{
    QDir().mkpath(m_sessionDir);
    connect(this, &Backend::replied, this, [this](RequestId id, const Result &) {
        m_pendingStarts.remove(id);
        m_withdrawn.remove(id);
    });
    m_reaper.setInterval(30000);
    connect(&m_reaper, &QTimer::timeout, this, &PiBackend::reap);
    m_reaper.start();
}
PiBackend::~PiBackend()
{
    for (auto &[session, chat] : m_chats) {
        delete std::exchange(chat.pi, nullptr);
        delete std::exchange(chat.retiring, nullptr);
    }
    delete std::exchange(m_control, nullptr);
}
QString PiBackend::sessionFile(const QString &session) const
{
    // One Pi session file per OpenGhost session ID (`:` and all), named reversibly.
    return m_sessionDir + QLatin1Char('/') + QString::fromLatin1(session.toUtf8().toHex()) +
           QStringLiteral(".jsonl");
}
void PiBackend::request(RequestId id, const Command &command)
{
    if (std::holds_alternative<StartTurn>(command) || std::holds_alternative<RetryTurn>(command))
        m_pendingStarts.insert(id);
    // A sign-in's flow begins (or ends) when it is asked for, not when dispatched:
    // a step of the flow before it that Pi reports meanwhile is already stale.
    if (const auto *login = std::get_if<Login>(&command)) {
        m_flowOf.insert(id,
                        m_flows[login->provider] = QStringLiteral("og-login-%1").arg(++m_tokens));
        m_steps.remove(login->provider);
        m_prompts.remove(login->provider);
    } else if (const auto *key = std::get_if<SetKey>(&command); key && key->key) {
        m_flowOf.insert(id, m_flows[key->provider] = QStringLiteral("og-login-%1").arg(++m_tokens));
        m_steps.remove(key->provider);
        m_prompts.remove(key->provider);
    } else if (const auto *cancel = std::get_if<CancelLogin>(&command)) {
        m_flowOf.insert(id, m_flows.take(cancel->provider));
        m_steps.remove(cancel->provider);
        m_prompts.remove(cancel->provider);
    } else if (const auto *answer = std::get_if<AnswerLogin>(&command)) {
        // Answered: the prompt is closed now, so a notification Pi sends while it
        // works on the answer never offers it again.
        if (const auto open = m_prompts.constFind(answer->provider);
            open != m_prompts.cend() && open->promptId == answer->promptId) {
            m_prompts.erase(open);
            auto &shown = m_steps[answer->provider];
            shown.promptId.reset();
            shown.placeholder.reset();
            shown.secret = false;
            shown.options.clear();
            shown.type = shown.userCode ? QStringLiteral("device_code") : QStringLiteral("waiting");
        }
    }
    QTimer::singleShot(0, this, [this, id, command] { dispatch(id, command); });
}
// Stop before Pi accepted the start: unsent, it is never sent; sent, Pi's acceptance
// is followed at once by an abort. Nothing else here can be withdrawn.
void PiBackend::cancelRequest(RequestId id)
{
    if (m_pendingStarts.contains(id))
        m_withdrawn.insert(id);
    for (auto &[session, chat] : m_chats)
        if (chat.run && chat.run->request == id && !chat.run->accepted)
            chat.run->cancelled = true;
}
void PiBackend::dispatch(RequestId id, const Command &command)
{
    if (m_withdrawn.contains(id)) {
        emit replied(id, failure(QStringLiteral("cancelled"),
                                 QStringLiteral("Stopped before Pi received it.")));
        return;
    }
    const auto settle = [this, id](const QJsonObject &reply, const QString &fallback,
                                   const auto &value) {
        if (succeeded(reply))
            emit replied(id, Reply{value(reply)});
        else
            emit replied(id, failure(QStringLiteral("backend_error"), errorOf(reply, fallback)));
    };
    const bool control = std::holds_alternative<ModelsList>(command) ||
                         std::holds_alternative<ProvidersList>(command) ||
                         std::holds_alternative<SetKey>(command) ||
                         std::holds_alternative<Login>(command) ||
                         std::holds_alternative<Logout>(command) ||
                         std::holds_alternative<CancelLogin>(command) ||
                         std::holds_alternative<AnswerLogin>(command);
    if (control && !m_control) {
        m_flowOf.remove(id);
        emit replied(id, failure(QStringLiteral("backend_unavailable"),
                                 QStringLiteral("Pi is not running.")));
        return;
    }
    if (std::holds_alternative<ModelsList>(command)) {
        // Pi's catalog, Pi's own default model first: a chat with no saved choice
        // starts on the model Pi itself would use.
        // Pi's runtime rereads models.json and credentials first: a change made
        // outside OpenGhost (another `pi /login`) is seen, not an old snapshot.
        m_control->bridge(
            {{"op", "refresh"}},
            [this, id](const QJsonObject &refreshed) {
                if (!m_control)
                    return emit replied(id, failure(QStringLiteral("backend_unavailable"),
                                                    QStringLiteral("Pi is not running.")));
                if (!succeeded(refreshed))
                    report(QStringLiteral("warning"),
                           QStringLiteral("Pi could not reread its models and credentials: ") +
                               errorOf(refreshed, QStringLiteral("no reason given.")));
                else
                    ++m_auth; // Every chat's Pi rereads before its next run.
                m_control->rpc({{"type", "get_available_models"}}, [this,
                                                                    id](const QJsonObject &reply) {
                    if (!succeeded(reply) || !m_control) {
                        emit replied(
                            id,
                            failure(QStringLiteral("backend_error"),
                                    errorOf(reply, QStringLiteral("Pi could not list models."))));
                        return;
                    }
                    auto models =
                        modelsOf(reply.value("data").toObject().value("models").toArray());
                    m_control->rpc({{"type", "get_state"}}, [this, id, models](
                                                                const QJsonObject &state) mutable {
                        if (!state.value("success").toBool()) {
                            emit replied(id, failure(QStringLiteral("backend_error"),
                                                     errorOf(state, QStringLiteral("Pi could not report its default model."))));
                            return;
                        }
                        const auto chosen =
                            state.value("data").toObject().value("model").toObject();
                        const auto it =
                            std::find_if(models.begin(), models.end(), [&](const Model &m) {
                                return m.provider == chosen.value("provider").toString() &&
                                       m.id == chosen.value("id").toString();
                            });
                        if (it != models.end())
                            std::rotate(models.begin(), it, it + 1);
                        emit replied(id, Reply{models});
                    });
                });
            },
            StepDeadline);
    } else if (std::holds_alternative<ProvidersList>(command)) {
        m_control->bridge({{"op", "providers"}}, [this, settle](const QJsonObject &reply) {
            // The control child reread models.json and credentials first. Whatever
            // changed there (here or outside OpenGhost, status flags or not), every
            // chat's Pi rereads before its next run.
            if (succeeded(reply))
                ++m_auth;
            settle(reply, QStringLiteral("Pi could not list providers."),
                   [](const QJsonObject &r) { return providersOf(r.value("providers").toArray()); });
        });
    } else if (const auto *key = std::get_if<SetKey>(&command)) {
        auth(id, key->provider,
             key->key ? QJsonObject{{"op", "setKey"}, {"key", *key->key}}
                      : QJsonObject{{"op", "logout"}});
    } else if (const auto *login = std::get_if<Login>(&command)) {
        auth(id, login->provider, {{"op", "login"}});
    } else if (const auto *logout = std::get_if<Logout>(&command)) {
        auth(id, logout->provider, {{"op", "logout"}});
    } else if (const auto *cancel = std::get_if<CancelLogin>(&command)) {
        auth(id, cancel->provider, {{"op", "cancel"}, {"flow", m_flowOf.take(id)}});
    } else if (const auto *answer = std::get_if<AnswerLogin>(&command)) {
        m_control->bridge({{"op", "answer"}, {"promptId", answer->promptId}, {"value", answer->value}},
                          [settle](const QJsonObject &reply) {
                              settle(reply, QStringLiteral("Pi did not take the answer."),
                                     [](const QJsonObject &) { return Null{}; });
                          });
    } else if (const auto *configuring = std::get_if<ConfigureSession>(&command)) {
        configure(id, *configuring);
    } else if (const auto *start = std::get_if<StartTurn>(&command)) {
        const auto known = m_journal.constFind(journalKey(start->sessionId, start->clientTurnId));
        if (known != m_journal.cend()) { // Accepted once already: its identity, never twice.
            if (!known->input)
                emit replied(id, failure(QStringLiteral("duplicate_request"),
                                         QStringLiteral("That turn ID belongs to a retry.")));
            else
                emit replied(id, Reply{StartAccepted{known->turn,
                                                     m_chats[start->sessionId].version}});
            return;
        }
        load(start->sessionId, [this, id, start = *start](std::optional<Error> error) {
            // Taken before this process, as Pi's session records: the same identity.
            const auto &chat = m_chats[start.sessionId];
            if (const auto it = chat.recorded.constFind(start.clientTurnId);
                !error && it != chat.recorded.cend()) {
                emit replied(id, it->second ? Result{failure(QStringLiteral("duplicate_request"),
                                                             QStringLiteral("That turn ID belongs to a retry."))}
                                            : Result{Reply{StartAccepted{it->first, chat.version}}});
                return;
            }
            if (!error)
                error = admissible(start.sessionId, start.sessionVersion);
            if (error) {
                emit replied(id, *error);
                return;
            }
            if (!std::all_of(start.input.attachments.cbegin(), start.input.attachments.cend(),
                             deliverable)) {
                emit replied(id, failure(QStringLiteral("unsupported_input"),
                                         QStringLiteral("Pi takes text files and pictures only. "
                                                        "Nothing was sent.")));
                return;
            }
            Run run;
            run.session = start.sessionId;
            run.client = start.clientTurnId;
            run.input = displayOf(start.input);
            const auto prompt = promptOf(start.input);
            run.text = prompt.message;
            run.images = prompt.images;
            this->start(id, std::move(run), start.params);
        });
    } else if (const auto *retry = std::get_if<RetryTurn>(&command)) {
        const auto known = m_journal.constFind(journalKey(retry->sessionId, retry->clientTurnId));
        if (known != m_journal.cend()) {
            if (known->input)
                emit replied(id, failure(QStringLiteral("duplicate_request"),
                                         QStringLiteral("That turn ID belongs to a new message.")));
            else
                emit replied(id, Reply{RetryAccepted{known->turn}});
            return;
        }
        load(retry->sessionId, [this, id, retry = *retry](std::optional<Error> error) {
            const auto &chat = m_chats[retry.sessionId];
            if (const auto it = chat.recorded.constFind(retry.clientTurnId);
                !error && it != chat.recorded.cend()) {
                emit replied(id, it->second ? Result{Reply{RetryAccepted{it->first}}}
                                            : Result{failure(QStringLiteral("duplicate_request"),
                                                             QStringLiteral("That turn ID belongs to a new message."))});
                return;
            }
            if (!error)
                error = admissible(retry.sessionId, retry.sessionVersion);
            if (error) {
                emit replied(id, *error);
                return;
            }
            // Exactly the chat's failed turn, and only while it is still Pi's latest:
            // Pi continues its own context from there, with no new or repeated input.
            const auto failed =
                std::find_if(m_journal.cbegin(), m_journal.cend(), [&](const Journal &journal) {
                    return journal.session == retry.sessionId && journal.turn == retry.failedTurnId;
                });
            if (failed == m_journal.cend()) {
                const auto record = std::find_if(chat.recorded.cbegin(), chat.recorded.cend(),
                    [&](const auto &r) { return r.first == retry.failedTurnId; });
                if (record != chat.recorded.cend()) {
                    const auto client = record.key();
                    entries(retry.sessionId, [this, id, retry, client](std::optional<QJsonArray> list, Error error) {
                        if (!list)
                            emit replied(id, error);
                        else if (!recoverJournal(retry.sessionId, client, *list))
                            emit replied(id, failure(QStringLiteral("invalid_request"),
                                                     QStringLiteral("Pi cannot recover that failed turn.")));
                        else
                            dispatch(id, retry); // validate again after the asynchronous read
                    });
                    return;
                }
            }
            if (failed == m_journal.cend() || !failed->terminal ||
                failed->status != TurnStatus::Error) {
                emit replied(id, failure(QStringLiteral("invalid_request"),
                                         QStringLiteral("Pi can retry only a reply that failed.")));
                return;
            }
            if (failed.key() != m_chats[retry.sessionId].last) {
                emit replied(id,
                             failure(QStringLiteral("stale_turn"),
                                     QStringLiteral("This chat has taken newer input since this "
                                                    "reply failed, so Pi cannot retry it exactly.")));
                return;
            }
            Run run;
            run.session = retry.sessionId;
            run.client = retry.clientTurnId;
            run.retry = true;
            run.failedTurn = retry.failedTurnId;
            this->start(id, std::move(run), retry.params);
        });
    } else if (const auto *steering = std::get_if<SteerTurn>(&command)) {
        steer(id, *steering);
    } else if (const auto *stopping = std::get_if<CancelTurn>(&command)) {
        cancelTurn(id, *stopping);
    } else if (const auto *get = std::get_if<GetSession>(&command)) {
        getSession(id, *get);
    } else if (const auto *deleting = std::get_if<DeleteSession>(&command)) {
        remove(id, deleting->sessionId);
    } else {
        emit replied(id, execute(command));
    }
}
Result PiBackend::execute(const Command &command)
{
    if (std::holds_alternative<Initialize>(command)) {
        if (!m_control) {
            QString error;
            m_control = spawn({QStringLiteral("--mode"), QStringLiteral("rpc"),
                               QStringLiteral("--no-session")},
                              {}, &error);
            if (!m_control)
                return failure(QStringLiteral("backend_unavailable"), error);
            m_control->onStep = [this](const QString &token, const QString &kind,
                                       const QJsonObject &value) { step(token, kind, value); };
            // The control child never runs a turn: nothing there can be approved.
            m_control->onDialog = [this](const QJsonObject &request) {
                return dialog({}, m_control, request);
            };
            m_control->onExit = [this] {
                // The connection ends with its control child: every chat's Pi stops too.
                m_control->deleteLater();
                m_control = nullptr;
                const auto gone =
                    failure(QStringLiteral("backend_unavailable"), QStringLiteral("Pi exited."));
                for (auto &[session, chat] : m_chats) {
                    if (chat.run && chat.run->accepted)
                        finish(session, {TurnStatus::Error, {}, gone});
                    else if (chat.run)
                        refuse(session, gone);
                    retire(chat);
                }
                const auto stops = std::exchange(m_stops, {});
                for (const auto &waiting : stops)
                    for (const auto &stopped : waiting)
                        stopped(gone);
                emit closed(gone);
            };
        }
        Initialized result;
        result.backend = BackendInfo{QStringLiteral("Pi"), {}, {}};
        result.capabilities.authProviders = true;
        // Turn records in each chat's Pi session answer session.get, across restarts.
        result.capabilities.sessionRecovery = true;
        result.capabilities.sessionDelete = true;
        return Reply{result};
    }
    if (std::holds_alternative<Shutdown>(command))
        return Reply{Null{}};
    return failure(QStringLiteral("unsupported"), QStringLiteral("Not implemented for Pi."));
}
PiProcess *PiBackend::spawn(const QStringList &args, const QString &cwd, QString *error)
{
    if (m_bridgePath.isEmpty() && m_bridgeDir.isValid()) {
        const auto path = m_bridgeDir.filePath(QStringLiteral("openghost-bridge.js"));
        if (QFile::copy(QStringLiteral(":/pi/openghost-bridge.js"), path))
            m_bridgePath = path;
    }
    auto arguments = args;
    if (!m_bridgePath.isEmpty())
        arguments << QStringLiteral("-e") << m_bridgePath;
    auto *pi = new PiProcess(m_bridgePath, this);
    pi->onExtension = [this](const QString &type, const QJsonObject &record) {
        extensionRecord(type, record);
    };
    if (!pi->start(arguments, cwd, error)) {
        delete pi;
        return nullptr;
    }
    return pi;
}
// The chat's Pi child, started on its session file when needed. A new session is
// created in the chat's folder (made if missing); an existing one keeps its own.
PiProcess *PiBackend::child(const QString &session, const QString &cwd, QString *error)
{
    auto &chat = m_chats[session];
    if (chat.deleting || chat.retiring) {
        *error = QStringLiteral("Pi is still closing this chat. Try again once it has stopped.");
        return nullptr;
    }
    if (chat.pi)
        return chat.pi;
    const auto folder = cwd.isEmpty() ? QDir::currentPath() : cwd;
    if (!QDir().mkpath(folder)) {
        *error = QStringLiteral("Cannot create the chat's folder: ") + folder;
        return nullptr;
    }
    auto *pi = spawn({QStringLiteral("--mode"), QStringLiteral("rpc"), QStringLiteral("--session"),
                      sessionFile(session)},
                     folder, error);
    if (!pi)
        return nullptr;
    pi->onRecord = [this, session](const QString &type, const QJsonObject &object) {
        auto &chat = m_chats[session];
        if (chat.run && chat.run->accepted)
            runEvent(session, type, object);
    };
    pi->onExit = [this, session] { exited(session); };
    pi->onDialog = [this, session, pi](const QJsonObject &request) {
        return dialog(session, pi, request);
    };
    pi->onStep = [this, session](const QString &token, const QString &kind,
                                 const QJsonObject &value) {
        if (kind == QStringLiteral("approval"))
            approvalEnded(session, token, value);
    };
    chat.pi = pi;
    chat.context.reset();
    chat.mode.reset();
    chat.auth = m_auth; // A new Pi reads the credentials as they are now.
    reap();
    return pi;
}
// Closes a chat's child deliberately (idle, or its chat deleted): Pi finishes what
// it read and exits; `gone` runs once it has.
void PiBackend::retire(Chat &chat, std::function<void()> gone)
{
    // A child that is closing still owns its session file. A second retirement
    // (notably DeleteSession after idle reaping) must wait for the same exit.
    if (chat.retiring) {
        chat.retiring->close(std::move(gone));
        return;
    }
    auto *pi = std::exchange(chat.pi, nullptr);
    chat.context.reset();
    chat.mode.reset();
    // Its cards go, declined while the closing child can still hear it.
    QVector<RequestId> cards;
    for (auto it = m_approvals.cbegin(); pi && it != m_approvals.cend(); ++it)
        if (it->pi == pi)
            cards.append(it.key());
    for (const auto id : cards) {
        const auto approval = m_approvals.take(id);
        pi->send({{"type", "extension_ui_response"}, {"id", approval.dialog}, {"confirmed", false}});
        emit reverseCancelled(id);
    }
    if (!pi) {
        if (gone)
            gone();
        return;
    }
    chat.retiring = pi;
    pi->onRecord = {};
    pi->onExit = {};
    pi->onStep = {};
    pi->onDialog = {};
    pi->close([&chat, pi, gone = std::move(gone)] {
        chat.retiring = nullptr;
        pi->deleteLater();
        if (gone)
            gone();
    });
}
// A chat's child ended on its own: its turn ends with it, its outcome unknown.
void PiBackend::exited(const QString &session)
{
    auto &chat = m_chats[session];
    if (auto *pi = std::exchange(chat.pi, nullptr))
        pi->deleteLater();
    chat.context.reset();
    chat.mode.reset();
    withdrawApprovals(session, false);
    const auto gone = failure(QStringLiteral("backend_unavailable"), QStringLiteral("Pi exited."));
    if (chat.run && chat.run->accepted)
        finish(session, {TurnStatus::Error, {}, gone});
    else if (chat.run)
        refuse(session, gone);
}
void PiBackend::reap()
{
    QVector<Chat *> idle;
    for (auto &[session, chat] : m_chats)
        if (chat.pi && !chat.run && !chat.loading && !chat.deleting && chat.pi->idle())
            idle.append(&chat);
    std::sort(idle.begin(), idle.end(),
              [](const Chat *a, const Chat *b) { return a->pi->used < b->pi->used; });
    for (qsizetype i = 0; i < idle.size(); ++i)
        if (now() - idle[i]->pi->used > IdleChild || idle.size() - i > MaxIdleChildren)
            retire(*idle[i]);
}
// Whether Pi has this session, and its incarnation: read once from Pi's own session
// entries (the latest turn record Pi took names it). No session file: none.
void PiBackend::load(const QString &session, Then then)
{
    auto &chat = m_chats[session];
    if (chat.deleting || chat.retiring)
        return then(failure(QStringLiteral("busy"), QStringLiteral("Pi is still closing this chat.")));
    if (chat.loaded)
        return then(std::nullopt);
    chat.waiting.append(std::move(then));
    if (chat.loading)
        return;
    chat.loading = true;
    const auto done = [this, session](std::optional<Error> error) {
        auto &chat = m_chats[session];
        chat.loading = false;
        chat.loaded = !error;
        const auto waiting = std::exchange(chat.waiting, {});
        for (const auto &then : waiting)
            then(error);
    };
    if (!QFile::exists(sessionFile(session)) && !chat.pi) {
        chat.version.clear();
        chat.last.clear();
        return done(std::nullopt);
    }
    entries(session, [this, session, done](std::optional<QJsonArray> list, Error error) {
        if (!list)
            return done(error);
        loaded(session, *list);
        done(std::nullopt);
    });
}
void PiBackend::loaded(const QString &session, const QJsonArray &entries)
{
    auto &chat = m_chats[session];
    chat.version.clear();
    chat.last.clear();
    chat.recorded.clear();
    for (qsizetype i = 0; i < entries.size(); ++i)
        if (isMark(entries[i].toObject(), QStringLiteral("start")) && taken(entries, i)) {
            const auto mark = markOf(entries[i]);
            chat.version = mark.value("version").toString();
            chat.last = journalKey(session, mark.value("client").toString());
            chat.recorded.insert(mark.value("client").toString(),
                                 {mark.value("turn").toString(), mark.value("retry").toBool()});
        }
    // A Pi conversation OpenGhost never took a turn in still exists: never created
    // anew. Pi's own bookkeeping (its model and thinking level) is no conversation.
    for (const auto &entry : entries) {
        const auto role = entry.toObject().value("message").toObject().value("role").toString();
        if (chat.version.isEmpty() && entry.toObject().value("type").toString() == "message" &&
            (role == QStringLiteral("user") || role == QStringLiteral("assistant")))
            chat.version = QStringLiteral("pi:") + entries.first().toObject().value("id").toString();
    }
}
void PiBackend::entries(const QString &session,
                        std::function<void(std::optional<QJsonArray>, Error)> done)
{
    QString error;
    auto *pi = child(session, {}, &error);
    if (!pi)
        return done(std::nullopt, failure(QStringLiteral("backend_unavailable"), error));
    pi->rpc({{"type", "get_entries"}}, [done](const QJsonObject &reply) {
        if (!reply.value("success").toBool())
            done(std::nullopt, failure(QStringLiteral("backend_error"),
                                       errorOf(reply, QStringLiteral("Pi could not read the chat."))));
        else
            done(reply.value("data").toObject().value("entries").toArray(), {});
    });
}
// session.get: the chat's incarnation and revision, and the named turn exactly as
// Pi took it: from this process's journal, else rebuilt from Pi's session entries.
void PiBackend::getSession(RequestId id, const GetSession &get)
{
    load(get.sessionId, [this, id, get](std::optional<Error> error) {
        if (error) {
            emit replied(id, *error);
            return;
        }
        auto &chat = m_chats[get.sessionId];
        // No acknowledgement is not proof of absence: Retry must never resend
        // while the original prompt can still be admitted.
        if (chat.run && chat.run->sent && !chat.run->accepted) {
            emit replied(id, failure(QStringLiteral("acceptance_pending"),
                                     QStringLiteral("Pi has not confirmed the original prompt yet. "
                                                    "Nothing was resent.")));
            return;
        }
        if (chat.version.isEmpty() || chat.deleting) {
            emit replied(id, Reply{SessionRecovery{MissingSession{}}});
            return;
        }
        // The named turn only if Pi took it; without one, the running turn.
        QString key;
        if (get.clientTurnId)
            key = journalKey(get.sessionId, *get.clientTurnId);
        else if (chat.run && chat.run->accepted)
            key = journalKey(get.sessionId, chat.run->client);
        const auto answer = [this, id, key, session = get.sessionId] {
            auto &chat = m_chats[session];
            if (chat.seq == 0)
                chat.seq = now() * 1000;
            std::optional<RecoveredTurn> turn;
            if (const auto it = m_journal.constFind(key); it != m_journal.cend())
                turn = RecoveredTurn{it->client, it->turn, it->input, it->events};
            emit replied(id, Reply{SessionRecovery{ExistingSession{chat.version, chat.seq, turn}}});
        };
        if (key.isEmpty() || m_journal.contains(key))
            return answer();
        entries(get.sessionId, [this, id, get, answer](std::optional<QJsonArray> list, Error error) {
            if (!list) {
                emit replied(id, error);
                return;
            }
            recoverJournal(get.sessionId, *get.clientTurnId, *list);
            answer();
        });
    });
}
bool PiBackend::recoverJournal(const QString &session, const QString &client, const QJsonArray &entries)
{
    TurnStatus status = TurnStatus::Done;
    const auto turn = rebuilt(entries, client, &status);
    if (!turn)
        return false;
    auto &chat = m_chats[session];
    Journal journal{session, client, turn->first.value("turn").toString(), {}, {}, true, status};
    if (!turn->first.value("retry").toBool())
        journal.input = displayFrom(turn->first.value("input").toObject());
    QString message;
    for (const auto &payload : turn->second) {
        if (std::holds_alternative<MessageStarted>(payload))
            message = uuid();
        const bool part = std::holds_alternative<MessageStarted>(payload) ||
                          std::holds_alternative<MessageCompleted>(payload) ||
                          std::holds_alternative<Usage>(payload);
        journal.events.append({{session, next(chat), journal.turn,
                                part ? std::optional(message) : std::nullopt, client}, payload});
    }
    remember(journal, false);
    return true;
}
// session.configure: the chat's own Pi switches model; Pi's reply is canonical.
void PiBackend::configure(RequestId id, const ConfigureSession &configure)
{
    load(configure.sessionId, [this, id, configure](std::optional<Error> error) {
        auto &chat = m_chats[configure.sessionId];
        if (!error && (chat.version.isEmpty() || chat.deleting))
            error = failure(QStringLiteral("session_missing"), QStringLiteral("Pi no longer has this chat."));
        else if (!error && chat.version != configure.sessionVersion)
            error = failure(QStringLiteral("session_conflict"), QStringLiteral("This chat changed in Pi."));
        const bool model = configure.model && configure.provider && !configure.model->isEmpty();
        // A model switch waits for the running turn; an access mode applies at once.
        if (!error && (chat.run || chat.stopping) && model)
            error = failure(QStringLiteral("busy"), QStringLiteral("A turn is already running."));
        if (error) {
            emit replied(id, *error);
            return;
        }
        const auto switchModel = [this, id, configure, model](SessionConfigured configured) {
            if (!model) {
                emit replied(id, Reply{configured});
                return;
            }
            QString problem;
            auto *pi = child(configure.sessionId, {}, &problem);
            if (!pi) {
                emit replied(id, failure(QStringLiteral("backend_unavailable"), problem));
                return;
            }
            setModel(pi, *configure.provider, *configure.model,
                     [this, id, configured](std::optional<Error> error,
                                            const ModelSelection &actual) mutable {
                         if (error) {
                             emit replied(id, *error);
                             return;
                         }
                         configured.model = actual.model;
                         configured.provider = actual.provider;
                         emit replied(id, Reply{configured});
                     });
        };
        if (!configure.permissionMode)
            return switchModel({});
        // The mode reaches a running Pi now (what Pi does with it is Pi's); an idle
        // chat's Pi takes it at once too.
        const auto mode = *configure.permissionMode;
        if (chat.run)
            chat.run->mode = mode;
        if (!chat.pi) { // Set before the next run (StartTurn names its mode).
            SessionConfigured configured;
            configured.permissionMode = mode;
            return switchModel(configured);
        }
        // A child that is idle takes it too: nothing it runs later (an extension's
        // own turn included) keeps a mode the chat has left.
        setMode(configure.sessionId, mode,
                [this, id, mode, switchModel](std::optional<Error> error) {
                    if (error) {
                        emit replied(id, *error);
                        return;
                    }
                    SessionConfigured configured;
                    configured.permissionMode = mode;
                    switchModel(configured);
                });
    });
}
// The chat's child takes `mode`. Updates are numbered: the bridge ignores one older
// than the last it applied, and the mode recorded as held is only ever the latest
// update's, confirmed. Until then it is unknown, so the next run sets it again.
void PiBackend::setMode(const QString &session, PermissionMode mode,
                        std::function<void(std::optional<Error>)> done)
{
    auto &chat = m_chats[session];
    auto *pi = chat.pi;
    const auto seq = ++m_modeSeq;
    chat.modeSent = seq;
    chat.mode.reset();
    pi->bridge(
        {{"op", "mode"}, {"mode", modeName(mode)}, {"seq", double(seq)}},
        [this, session, pi, seq, mode, done = std::move(done)](const QJsonObject &reply) {
            const bool ok = succeeded(reply);
            const auto chat = m_chats.find(session);
            if (ok && chat != m_chats.end() && chat->second.pi == pi && chat->second.modeSent == seq)
                chat->second.mode = mode;
            if (ok)
                done(std::nullopt);
            else
                done(failure(QStringLiteral("backend_error"),
                             errorOf(reply, QStringLiteral("Pi did not take the access mode."))));
        },
        StepDeadline);
}
// session.delete: the chat's running reply is aborted, its Pi child exits, and only
// then is its Pi session file removed. An absent session succeeds (retry is safe).
void PiBackend::remove(RequestId id, const QString &session)
{
    auto &chat = m_chats[session];
    chat.deletes.append(id);
    if (chat.deleting)
        return;
    chat.deleting = true;
    const auto close = [this, session] {
        auto &chat = m_chats[session];
        if (chat.run && chat.run->accepted)
            finish(session, {TurnStatus::Cancelled, {}, {}}); // Pi did not confirm the abort.
        retire(chat, [this, session] { erase(session); });
    };
    if (chat.run && chat.run->accepted && chat.pi) {
        abort(session, [close](const Result &) { close(); });
        return;
    }
    if (chat.run)
        refuse(session, failure(QStringLiteral("cancelled"),
                                QStringLiteral("The chat was deleted before Pi took it.")));
    close();
}
void PiBackend::erase(const QString &session)
{
    auto &chat = m_chats[session];
    QFile file(sessionFile(session));
    const bool removed = !file.exists() || file.remove();
    for (auto it = m_journal.begin(); it != m_journal.end();)
        if (it->session == session) {
            m_order.removeAll(it.key());
            it = m_journal.erase(it);
        } else {
            ++it;
        }
    chat.deleting = false;
    chat.loaded = removed; // A file still there is read again, never assumed gone.
    if (removed) {
        chat.version.clear();
        chat.last.clear();
        chat.recorded.clear();
    }
    const auto ids = std::exchange(chat.deletes, {});
    for (const auto request : ids)
        emit replied(request, removed ? Result{Reply{Null{}}}
                                      : Result{failure(QStringLiteral("delete_failed"),
                                                       QStringLiteral("Pi's session file could not "
                                                                      "be removed: ") +
                                                           file.errorString())});
}
// A start/retry for this chat, now: one turn per chat at a time, and the incarnation
// the frontend names (none = create-only) must be the one Pi's history belongs to.
std::optional<Error> PiBackend::admissible(const QString &session,
                                           const std::optional<QString> &version)
{
    const auto &chat = m_chats[session];
    if (chat.deleting)
        return failure(QStringLiteral("session_missing"), QStringLiteral("This chat is being deleted."));
    if (chat.run || chat.stopping)
        return failure(QStringLiteral("busy"), QStringLiteral("A turn is already running."));
    if (!m_control)
        return failure(QStringLiteral("backend_unavailable"), QStringLiteral("Pi is not running."));
    if (!version && !chat.version.isEmpty())
        return failure(QStringLiteral("session_conflict"),
                       QStringLiteral("This chat already exists in Pi."));
    if (version && chat.version.isEmpty())
        return failure(QStringLiteral("session_missing"),
                       QStringLiteral("Pi no longer has this chat."));
    if (version && chat.version != *version)
        return failure(QStringLiteral("session_conflict"),
                       QStringLiteral("This chat changed in Pi."));
    return std::nullopt;
}
void PiBackend::start(RequestId id, Run run, const SessionParams &params)
{
    const auto session = run.session;
    auto &chat = m_chats[session];
    run.request = id;
    run.cancelled = m_withdrawn.contains(id);
    run.turn = uuid();
    run.version = chat.version.isEmpty() ? uuid() : chat.version; // a new one is created
    run.context = contextOf(params.userContext);
    run.mode = params.permissionMode;
    run.chosen = params.selection;
    QString error;
    if (!child(session, params.cwd, &error)) {
        emit replied(id, failure(QStringLiteral("backend_unavailable"), error));
        return;
    }
    const auto turn = run.turn;
    chat.run = std::move(run);
    advance(session, turn);
}
PiBackend::Run *PiBackend::running(const QString &session, const QString &turn)
{
    const auto it = m_chats.find(session);
    if (it == m_chats.end() || !it->second.run || it->second.run->turn != turn)
        return nullptr;
    return &*it->second.run;
}
// A start Pi never took: refused, and the chat's history is unchanged.
void PiBackend::refuse(const QString &session, const Error &error)
{
    auto &chat = m_chats[session];
    if (!chat.run)
        return;
    const auto request = chat.run->request;
    const bool answered = chat.run->abandoned;
    chat.run.reset();
    if (!answered)
        emit replied(request, error);
}
// What the chat's Pi needs before the prompt, each awaited because Pi runs RPC lines
// concurrently: the instructions and pinned files, the chat's access mode,
// credentials changed since it last read them, the chat's model, that model's sight
// when the input has pictures, then the turn's start record in Pi's session.
void PiBackend::advance(const QString &session, const QString &turn)
{
    auto *run = running(session, turn);
    if (!run || run->sent)
        return;
    auto &chat = m_chats[session];
    if (run->cancelled) {
        refuse(session, failure(QStringLiteral("cancelled"),
                                QStringLiteral("Stopped before Pi received it.")));
        return;
    }
    if (!chat.pi) {
        refuse(session, failure(QStringLiteral("backend_unavailable"), QStringLiteral("Pi exited.")));
        return;
    }
    const auto proceed = [this, session, turn](const QString &fallback) {
        return [this, session, turn, fallback](const QJsonObject &reply) {
            if (!running(session, turn))
                return;
            if (succeeded(reply))
                advance(session, turn);
            else
                refuse(session, failure(QStringLiteral("backend_error"), errorOf(reply, fallback)));
        };
    };
    switch (run->stage++) {
    case 0: { // Instructions and pinned files: every run's system prompt, not history.
        const auto context = run->context;
        if (chat.context.value_or(contextOf({})) == context)
            return advance(session, turn);
        QJsonObject request = context;
        request.insert("op", "context");
        chat.pi->bridge(
            request,
            [this, session, context,
             next = proceed(QStringLiteral("Pi did not take the instructions and files."))](
                const QJsonObject &reply) {
                if (succeeded(reply))
                    m_chats[session].context = context;
                next(reply);
            },
            StepDeadline);
        return;
    }
    case 1: { // Ask / Auto / Full, relayed to Pi: Pi decides what it means.
        const auto mode = run->mode;
        if (chat.mode == mode)
            return advance(session, turn);
        setMode(session, mode, [this, session, turn](std::optional<Error> error) {
            if (!running(session, turn))
                return;
            if (error)
                return refuse(session, *error);
            advance(session, turn);
        });
        return;
    }
    case 2: { // Configuration or credentials reread since this Pi read them: it rereads.
        const auto generation = m_auth;
        if (chat.auth == generation)
            return advance(session, turn);
        chat.pi->bridge(
            {{"op", "refresh"}},
            [this, session, generation,
             next = proceed(QStringLiteral("Pi could not reread its credentials."))](
                const QJsonObject &reply) {
                if (succeeded(reply))
                    m_chats[session].auth = generation;
                next(reply);
            },
            StepDeadline);
        return;
    }
    case 3: { // The chat's model, checked against what Pi has selected, never cached.
        const auto chosen = run->chosen;
        chat.pi->rpc(
            {{"type", "get_state"}},
            [this, session, turn, chosen](const QJsonObject &state) {
                if (!running(session, turn))
                    return;
                const auto model = state.value("data").toObject().value("model").toObject();
                if (!state.value("success").toBool()) {
                    refuse(session, failure(QStringLiteral("backend_error"),
                                            errorOf(state, QStringLiteral("Pi did not answer."))));
                } else if (chosen.model.isEmpty() &&
                           (model.value("provider").toString().isEmpty() ||
                            model.value("id").toString().isEmpty())) {
                    refuse(session, failure(QStringLiteral("model_unavailable"),
                                            QStringLiteral("Pi did not report a selected model.")));
                } else if (chosen.model.isEmpty() ||
                           (model.value("provider").toString() == chosen.provider &&
                            model.value("id").toString() == chosen.model)) {
                    running(session, turn)->chosen = {model.value("provider").toString(),
                                                      model.value("id").toString(), {}};
                    advance(session, turn);
                } else if (auto *pi = m_chats[session].pi) {
                    setModel(pi, chosen.provider, chosen.model,
                             [this, session, turn](std::optional<Error> error, const ModelSelection &actual) {
                                 if (!running(session, turn))
                                     return;
                                 if (error)
                                     refuse(session, *error);
                                 else {
                                     running(session, turn)->chosen = actual;
                                     advance(session, turn);
                                 }
                             });
                }
            },
            StepDeadline);
        return;
    }
    case 4: { // Pictures go only to a model Pi says sees images; Pi would replace
              // them with a placeholder for any other.
        if (run->images.isEmpty())
            return advance(session, turn);
        chat.pi->rpc(
            {{"type", "get_state"}},
            [this, session, turn](const QJsonObject &state) {
                if (!running(session, turn))
                    return;
                const auto model = state.value("data").toObject().value("model").toObject();
                if (!state.value("success").toBool())
                    refuse(session, failure(QStringLiteral("backend_error"),
                                            errorOf(state, QStringLiteral("Pi did not answer."))));
                else if (!model.value("input").toArray().contains(QStringLiteral("image")))
                    refuse(session,
                           failure(QStringLiteral("unsupported_input"),
                                   QStringLiteral("%1 can't see pictures. Choose a model that "
                                                  "sees photos, or remove the pictures.")
                                       .arg(model.value("name").toString(
                                           model.value("id").toString(QStringLiteral("This model"))))));
                else
                    advance(session, turn);
            },
            StepDeadline);
        return;
    }
    case 5: { // Its start, in Pi's session before Pi can take it.
        QJsonObject entry{{"event", "start"}, {"version", run->version}, {"client", run->client},
                          {"turn", run->turn}, {"retry", run->retry}};
        if (run->input)
            entry.insert("input", displayJson(*run->input));
        chat.pi->bridge({{"op", "mark"}, {"entry", entry}},
                        proceed(QStringLiteral("Pi could not record the turn.")), StepDeadline);
        return;
    }
    default:
        submit(session);
    }
}
// The prompt (or bridge retry) itself. Its acceptance is Pi's reply, never assumed.
void PiBackend::submit(const QString &session)
{
    auto &chat = m_chats[session];
    auto &run = *chat.run;
    const auto turn = run.turn;
    run.sent = true;
    QTimer::singleShot(StartDeadline, this, [this, session, turn] {
        auto *run = running(session, turn);
        if (!run || run->accepted || run->abandoned)
            return;
        // Unknown outcome: close the child even if the acknowledgement never
        // arrives. Recovery rereads its journal; absence is never guessed here.
        run->abandoned = true;
        emit replied(run->request,
                     failure(QStringLiteral("timeout"),
                             QStringLiteral("Pi did not answer within 60 seconds. Nothing will "
                                            "be resent; use Retry to check what Pi received.")));
        retire(m_chats[session], [this, session, turn] {
            m_chats[session].loaded = false;
            if (running(session, turn))
                refuse(session, failure(QStringLiteral("timeout"), QStringLiteral("Pi did not answer.")));
        });
    });
    if (run.retry) {
        chat.pi->bridge({{"op", "retry"}, {"failedTurnId", run.failedTurn}},
                        [this, session, turn](const QJsonObject &reply) {
            admitted(session, turn,
                     {{"success", reply.value("ok").toBool()},
                      {"transport", reply.value("transport")},
                      {"timeout", reply.value("timeout")},
                      {"error", reply.value("error")},
                      {"data", QJsonObject{{"disposition", "started"}}}});
        });
        return;
    }
    const auto text = run.text;
    const auto imageCount = run.images.size(); // rpc write failure can synchronously release run
    QJsonObject prompt{{"type", "prompt"}, {"message", text}};
    if (!run.images.isEmpty())
        prompt.insert("images", run.images);
    chat.pi->rpc(
        prompt,
        [this, session, turn](const QJsonObject &reply) { admitted(session, turn, reply); },
        0); // The start deadline above answers; Pi's later reply still settles it.
    fprintf(stderr, "[pi] prompt sent (%lld chars, %lld pictures)\n",
            static_cast<long long>(text.size()), static_cast<long long>(imageCount));
}
void PiBackend::admitted(const QString &session, const QString &turn, const QJsonObject &reply)
{
    if (!running(session, turn) || !m_chats[session].pi)
        return; // A retiring child's late acknowledgement cannot reopen admission.
    if (!reply.value("success").toBool()) {
        const bool uncertain = reply.value("transport").toBool() || reply.value("timeout").toBool();
        if (uncertain)
            m_chats[session].loaded = false; // reread Pi's records on reconciliation
        refuse(session, Error{uncertain ? QStringLiteral("backend_unavailable")
                                       : QStringLiteral("rejected"),
                              errorOf(reply, QStringLiteral("Pi did not accept it.")),
                              {}, {}, {}, {}});
        return;
    }
    const auto disposition = reply.value("data").toObject().value("disposition").toString();
    if (disposition != QStringLiteral("started") && disposition != QStringLiteral("queued") &&
        disposition != QStringLiteral("handled")) {
        // A malformed success is not admission and not a safe rejection either.
        // Close the child before allowing reconciliation of its durable records.
        auto &chat = m_chats[session];
        retire(chat, [this, session] {
            m_chats[session].loaded = false;
            refuse(session, failure(QStringLiteral("protocol_error"),
                                    QStringLiteral("Pi returned an unknown prompt disposition. "
                                                   "Acceptance must be reconciled.")));
        });
        return;
    }
    accept(session, disposition);
}
void PiBackend::accept(const QString &session, const QString &disposition)
{
    auto &chat = m_chats[session];
    auto &run = *chat.run;
    run.accepted = true;
    chat.version = run.version; // Created by its first accepted turn.
    chat.loaded = true;
    chat.recorded.insert(run.client, {run.turn, run.retry}); // dedupe outlives display-journal eviction
    remember({session, run.client, run.turn, run.input, {}, false, TurnStatus::Done}, true);
    if (!run.abandoned) {
        if (run.retry)
            emit replied(run.request, Reply{RetryAccepted{run.turn, run.chosen}});
        else
            emit replied(run.request, Reply{StartAccepted{run.turn, chat.version, run.chosen}});
    }
    publish(session, TurnStarted{});
    if (disposition == QStringLiteral("handled")) {
        // A Pi command consumed it: no run started, so none is awaited.
        TurnCompleted done;
        done.finishReason = QStringLiteral("handled");
        finish(session, done);
        return;
    }
    if (run.cancelled || run.abandoned)
        abort(session); // Late acceptance of a stopped or abandoned start.
}
void PiBackend::remember(Journal journal, bool latest)
{
    const auto key = journalKey(journal.session, journal.client);
    if (latest)
        m_chats[journal.session].last = key;
    m_order.removeAll(key);
    m_order.append(key);
    m_journal.insert(key, std::move(journal));
    for (auto it = m_order.begin(); m_order.size() > MaxJournals && it != m_order.end();) {
        if (*it != key && m_journal.value(*it).terminal) {
            m_journal.remove(*it);
            it = m_order.erase(it);
        } else {
            ++it;
        }
    }
}
// Stop: Pi's queued steering is dropped first (abort alone would run it), then
// abort, which replies once Pi is idle.
void PiBackend::abort(const QString &session, std::function<void(const Result &)> stopped)
{
    auto &chat = m_chats[session];
    auto &run = *chat.run;
    run.cancelled = true;
    if (stopped)
        m_stops[run.turn].append(std::move(stopped));
    chat.stopping = true; // agent_settled may precede the abort acknowledgement
    if (run.aborting || !run.steering.isEmpty())
        return; // An input handler may still enqueue: clear only after its reply.
    run.aborting = true;
    const auto turn = run.turn;
    const auto settle = [this, session, turn](const QJsonObject &reply) {
        const bool ok = reply.value("success").toBool();
        const auto error = failure(reply.value("timeout").toBool() ? QStringLiteral("timeout")
                                                                   : QStringLiteral("cancel_failed"),
                                   errorOf(reply, QStringLiteral("Pi did not stop.")));
        const auto complete = [this, session, turn, ok, error] {
            if (running(session, turn))
                finish(session, {ok ? TurnStatus::Cancelled : TurnStatus::Error, {},
                                 ok ? std::nullopt : std::optional(error)});
            m_chats[session].stopping = false;
            const auto waiting = m_stops.take(turn);
            for (const auto &stopped : waiting)
                stopped(ok ? Result{Reply{Null{}}} : Result{error});
        };
        if (!ok) // Never leave queued work alive after a failed clear/abort.
            retire(m_chats[session], complete);
        else
            complete();
    };
    if (!chat.pi)
        return settle({{"success", false}, {"error", "Pi is not running."}});
    chat.pi->rpc({{"type", "clear_queue"}}, [this, session, settle](const QJsonObject &reply) {
        if (!reply.value("success").toBool())
            return settle(reply);
        auto *pi = m_chats[session].pi;
        if (!pi)
            return settle({{"success", false}, {"error", "Pi is not running."}});
        pi->rpc({{"type", "abort"}}, settle, AbortDeadline);
    });
}
void PiBackend::cancelTurn(RequestId id, const CancelTurn &cancel)
{
    const auto it = m_chats.find(cancel.sessionId);
    if (it != m_chats.end() && it->second.run && it->second.run->accepted &&
        it->second.run->turn == cancel.turnId) {
        abort(cancel.sessionId, [this, id](const Result &result) { emit replied(id, result); });
        return;
    }
    // Not running: an ended turn has nothing left to stop; an unknown one is refused.
    const bool ended = std::any_of(m_journal.cbegin(), m_journal.cend(), [&](const Journal &j) {
        return j.session == cancel.sessionId && j.turn == cancel.turnId && j.terminal;
    });
    emit replied(id, ended ? Result{Reply{Null{}}}
                           : Result{failure(QStringLiteral("stale_turn"),
                                            QStringLiteral("Pi is not running that reply."))});
}
void PiBackend::finish(const QString &session, TurnCompleted done)
{
    auto &chat = m_chats[session];
    auto &run = *chat.run;
    if (!run.steering.isEmpty()) {
        run.settled = done;
        return; // A late queue admission still needs a truthful receipt and clear.
    }
    if (chat.pi && !run.cancelled && !run.clearing &&
        std::any_of(run.steers.cbegin(), run.steers.cend(),
                    [](const Steer &s) { return s.admitted && !s.applied; })) {
        run.clearing = true;
        const auto turn = run.turn;
        chat.pi->rpc({{"type", "clear_queue"}}, [this, session, turn, done](const QJsonObject &reply) {
            if (!running(session, turn))
                return;
            if (reply.value("success").toBool())
                finish(session, done);
            else
                retire(m_chats[session], [this, session, turn, reply] {
                    if (running(session, turn))
                        finish(session, {TurnStatus::Error, {},
                                         failure(QStringLiteral("queue_clear_failed"),
                                                 errorOf(reply, QStringLiteral("Pi did not clear its queue.")))});
                });
        });
        return;
    }
    const auto status = done.status;
    withdrawApprovals(session, true); // Nothing of an ended turn runs later.
    // Its end, in Pi's session beside the turn's own messages.
    if (chat.pi) {
        QJsonObject entry{{"event", "end"}, {"turn", run.turn}, {"client", run.client},
                          {"status", statusName(status)}};
        if (done.finishReason)
            entry.insert("finish", *done.finishReason);
        if (done.error) {
            entry.insert("code", done.error->code);
            entry.insert("error", done.error->message);
        }
        chat.pi->bridge({{"op", "mark"}, {"entry", entry}}, [](const QJsonObject &) {}, StepDeadline);
    }
    publish(session, std::move(done));
    auto &journal = m_journal[journalKey(session, run.client)];
    journal.terminal = true;
    journal.status = status;
    chat.run.reset();
}
// Send while busy: Pi's steer queue. Admission is Pi's "queued" reply; application is
// Pi delivering that exact queued text as the next user message, in queue order.
void PiBackend::steer(RequestId id, const SteerTurn &steer)
{
    const auto found = m_chats.find(steer.sessionId);
    auto *run = found == m_chats.end() ? nullptr : running(steer.sessionId, steer.turnId);
    if (!run || !run->accepted || run->cancelled || run->settled || run->clearing ||
        !found->second.pi) {
        emit replied(id, Reply{SteerAccepted{false}}); // That reply is no longer running.
        return;
    }
    for (const auto &known : std::as_const(run->steers))
        if (known.client == steer.clientInputId) {
            emit replied(id, Reply{SteerAccepted{known.admitted}});
            return;
        }
    // Text and text files enter Pi's steer queue; pictures are refused, never
    // dropped (the reply's model was not checked for them).
    const bool text = std::all_of(steer.input.attachments.cbegin(),
                                  steer.input.attachments.cend(), [](const Attachment &a) {
                                      return a.kind == Attachment::Kind::Text && deliverable(a);
                                  });
    if (!text || (steer.input.text.trimmed().isEmpty() && steer.input.attachments.isEmpty()) ||
        !run->steering.isEmpty()) {
        emit replied(id, Reply{SteerAccepted{false}});
        return;
    }
    const auto message = promptOf(steer.input).message;
    run->steers.append({steer.clientInputId, {}, displayOf(steer.input), false, false});
    run->steering = steer.clientInputId;
    const auto session = steer.sessionId, turn = steer.turnId, client = steer.clientInputId;
    found->second.pi->rpc(
        {{"type", "steer"}, {"message", message}},
        [this, id, session, turn, client](const QJsonObject &reply) {
            const bool queued = reply.value("success").toBool() &&
                                reply.value("data").toObject().value("disposition").toString() ==
                                    QStringLiteral("queued");
            auto *run = running(session, turn);
            if (!run) {
                // The reply ended first: what Pi queued would open the next turn.
                if (auto *pi = m_chats[session].pi; queued && pi)
                    pi->rpc({{"type", "clear_queue"}}, [](const QJsonObject &) {});
                emit replied(id, Reply{SteerAccepted{false}});
                return;
            }
            run->steering.clear();
            if (reply.value("timeout").toBool() || reply.value("transport").toBool()) {
                // The input handler could still enqueue after this timeout. A
                // later clear cannot fence that work; dispose the child instead.
                emit replied(id, failure(QStringLiteral("steering_uncertain"),
                                         errorOf(reply, QStringLiteral("Pi did not confirm steering."))));
                retire(m_chats[session], [this, session, turn] {
                    if (running(session, turn))
                        abort(session); // no live child: reports failure, settles Stop waiters
                });
                return;
            }
            const auto it = std::find_if(run->steers.begin(), run->steers.end(),
                                         [&](const Steer &s) { return s.client == client; });
            if (queued) {
                it->admitted = true;
                emit replied(id, Reply{SteerAccepted{true}});
            } else if (reply.value("success").toBool()) {
                run->steers.erase(it); // Handled by Pi itself: not part of this reply.
                emit replied(id, Reply{SteerAccepted{false}});
            } else {
                run->steers.erase(it);
                emit replied(id, failure(QStringLiteral("steering_rejected"),
                                         errorOf(reply, QStringLiteral("Pi did not take it."))));
            }
            // Stop waits for this admission before clearing; natural settlement
            // waits too, rather than labelling a late queued input notApplied.
            if (run->cancelled)
                abort(session);
            if (auto *current = running(session, turn); current && current->settled)
                finish(session, *current->settled);
        });
}
// Sign-in, API key, cancel and logout run Pi's own login/logout; the reply comes
// when Pi finishes. Status is then reread from Pi, never assumed. A sign-in is the
// flow its token names (see request()): once cancelled or superseded, its steps are
// dropped and its failure is only that it was cancelled.
void PiBackend::auth(RequestId id, const QString &provider, QJsonObject request)
{
    const auto op = request.value("op").toString();
    const bool flow = op == QStringLiteral("login") || op == QStringLiteral("setKey");
    const auto token = flow ? m_flowOf.take(id) : QStringLiteral("og-login-%1").arg(++m_tokens);
    request.insert("provider", provider);
    request.insert("token", token);
    if (flow)
        m_logins.insert(token, provider);
    m_control->bridge(request, [this, id, provider, op, flow, token](const QJsonObject &reply) {
        m_logins.remove(token);
        const bool current = flow && m_flows.value(provider) == token;
        if (current) {
            m_flows.remove(provider);
            m_steps.remove(provider);
            m_prompts.remove(provider);
        }
        const bool ok = reply.value("ok").toBool();
        if (ok) {
            emit replied(id, Reply{Null{}});
        } else if (flow && !current) {
            emit replied(id, Reply{Null{}}); // Ended because it was cancelled or replaced.
        } else {
            emit replied(id, failure(QStringLiteral("auth_failed"),
                                     errorOf(reply, QStringLiteral("Pi could not sign in."))));
        }
        if (op == QStringLiteral("cancel"))
            return;
        if (ok)
            ++m_auth; // Every chat's Pi rereads credentials before its next run.
        emit globalEvent(ModelsChanged{provider});
    });
}
// set_model on one chat's Pi. Pi's reply names the model it actually selected.
void PiBackend::setModel(PiProcess *pi, const QString &provider, const QString &model,
                         std::function<void(std::optional<Error>, ModelSelection)> done)
{
    pi->rpc(
        {{"type", "set_model"}, {"provider", provider}, {"modelId", model}},
        [done = std::move(done)](const QJsonObject &reply) {
            if (!reply.value("success").toBool()) {
                done(failure(QStringLiteral("model_unavailable"),
                             errorOf(reply, QStringLiteral("Pi could not switch models."))),
                     {});
                return;
            }
            const auto chosen = reply.value("data").toObject();
            const auto provider = chosen.value("provider").toString();
            const auto model = chosen.value("id").toString();
            if (provider.isEmpty() || model.isEmpty()) {
                done(failure(QStringLiteral("protocol_error"),
                             QStringLiteral("Pi did not confirm the selected model.")), {});
                return;
            }
            done(std::nullopt, {provider, model, {}});
        },
        StepDeadline);
}
// Pi's auth event or prompt, as the sign-in step Settings shows for that provider.
// The sign-in page and device code stay offered until the sign-in ends.
void PiBackend::step(const QString &token, const QString &kind, const QJsonObject &value)
{
    const auto provider = m_logins.value(token);
    if (provider.isEmpty() || m_flows.value(provider) != token)
        return; // A cancelled or replaced sign-in: never shown in the current one's form.
    const auto prior = m_steps.value(provider);
    if (kind == QStringLiteral("withdrawn")) {
        // Pi took its prompt back (the browser finished first): no answer is wanted.
        const auto open = m_prompts.constFind(provider);
        if (open == m_prompts.cend() || open->promptId != value.value("promptId").toString())
            return;
        m_prompts.erase(open);
        LoginStep waiting;
        waiting.provider = provider;
        waiting.type = prior.userCode ? QStringLiteral("device_code") : QStringLiteral("waiting");
        waiting.url = prior.url;
        waiting.userCode = prior.userCode;
        waiting.message =
            prior.userCode
                ? QStringLiteral("Enter the code %1 on the verification page.").arg(*prior.userCode)
                : QStringLiteral("Finish signing in in the browser…");
        m_steps.insert(provider, waiting);
        emit globalEvent(waiting);
        return;
    }
    LoginStep step;
    step.provider = provider;
    step.type = QStringLiteral("waiting");
    step.url = prior.url;
    step.userCode = prior.userCode;
    const auto type = value.value("type").toString();
    step.message = value.value("message").toString();
    if (kind == QStringLiteral("prompt")) { // A new prompt replaces any still open.
        step.promptId = value.value("promptId").toString();
        step.type = type == QStringLiteral("select") ? QStringLiteral("select")
                                                     : QStringLiteral("prompt");
        step.secret = type == QStringLiteral("secret");
        if (value.value("placeholder").isString())
            step.placeholder = value.value("placeholder").toString();
        for (const auto &option : value.value("options").toArray())
            step.options.append({option.toObject().value("id").toString(),
                                 option.toObject().value("label").toString()});
        m_prompts.insert(provider, step);
    } else if (type == QStringLiteral("auth_url")) {
        step.url = value.value("url").toString();
        step.message = value.value("instructions")
                           .toString(QStringLiteral("Finish signing in in the browser…"));
    } else if (type == QStringLiteral("device_code")) {
        step.type = QStringLiteral("device_code");
        step.url = value.value("verificationUri").toString();
        step.userCode = value.value("userCode").toString();
        step.message = QStringLiteral("Enter the code %1 on the verification page.")
                           .arg(*step.userCode);
    } else if (type == QStringLiteral("info")) {
        for (const auto &link : value.value("links").toArray())
            step.links.append({link.toObject().value("url").toString(),
                               link.toObject().value("label").toString()});
    }
    if (step.userCode && step.type == QStringLiteral("waiting"))
        step.type = QStringLiteral("device_code");
    // A notification while a prompt is open (progress, info, a sign-in page) keeps
    // that prompt answerable: its question, with the notification beneath it.
    if (const auto open = m_prompts.constFind(provider);
        kind != QStringLiteral("prompt") && open != m_prompts.cend()) {
        auto kept = *open;
        kept.url = step.url;
        kept.userCode = step.userCode;
        kept.links = step.links;
        if (!step.message.isEmpty() && step.message != kept.message)
            kept.message += QStringLiteral("\n") + step.message;
        step = kept;
    }
    m_steps.insert(provider, step);
    emit globalEvent(step);
}
// An extension's blocking dialog. A Pi permission request is the running turn's
// approval card; anything else (and a request no turn can show) is
// declined at once and said, so Pi never waits on a question nobody sees.
bool PiBackend::dialog(const QString &session, PiProcess *pi, const QJsonObject &request)
{
    const auto method = request.value("method").toString();
    const auto title = request.value("title").toString();
    if (method == QStringLiteral("confirm") && title == ApprovalTitle) {
        const auto ask =
            QJsonDocument::fromJson(request.value("message").toString().toUtf8()).object();
        const auto chat = session.isEmpty() ? m_chats.end() : m_chats.find(session);
        if (chat == m_chats.end() || chat->second.pi != pi || !chat->second.run ||
            !chat->second.run->accepted || chat->second.run->cancelled ||
            ask.value("approvalId").toString().isEmpty())
            return false; // Cancelled: no card can show it.
        const auto &run = *chat->second.run;
        ApprovalRequest approval;
        approval.sessionId = session;
        approval.turnId = run.turn;
        approval.approvalId = ask.value("approvalId").toString();
        approval.toolCallId = ask.value("toolCallId").toString();
        approval.tool = ask.value("tool").toString();
        approval.args = ask.value("args").toObject();
        approval.presentation = presentationOf(ask.value("presentation"));
        const auto id = ++m_reverse;
        m_approvals.insert(
            id, {session, run.turn, request.value("id").toString(), approval.approvalId, pi});
        emit reverseRequest(id, approval);
        return true;
    }
    report(QStringLiteral("warning"),
           title.isEmpty()
               ? QStringLiteral(
                     "A Pi extension asked a question OpenGhost cannot show, so it was declined.")
               : QStringLiteral("A Pi extension asked “%1”, which OpenGhost cannot show, so it was "
                                "declined.")
                     .arg(title));
    return false;
}
// The user's answer to an approval card: Pi's confirm, answered once.
void PiBackend::answer(RequestId id, const ReverseResult &result)
{
    const auto it = m_approvals.find(id);
    if (it == m_approvals.end())
        return; // Already withdrawn or answered.
    const auto approval = *it;
    m_approvals.erase(it);
    const auto *decided = std::get_if<ApprovalAnswer>(&result);
    // Allowed only by the user's Allow for this very card, while the child that asked
    // and its turn still run: never another child's or a later turn's dialog.
    const auto chat = m_chats.find(approval.session);
    if (chat == m_chats.end() || chat->second.pi != approval.pi || !approval.pi)
        return;
    const bool allow = decided && decided->decision == Decision::Allow &&
                       running(approval.session, approval.turn);
    approval.pi->send(
        {{"type", "extension_ui_response"}, {"id", approval.dialog}, {"confirmed", allow}});
}
// Pi took a permission request back unanswered: Stop (decision null), or Pi's own
// decision to allow it after all (allow). Its card goes; an allowed one says so.
void PiBackend::approvalEnded(const QString &session, const QString &approvalId,
                              const QJsonObject &value)
{
    for (auto it = m_approvals.begin(); it != m_approvals.end(); ++it) {
        if (it->session != session || it->approvalId != approvalId)
            continue;
        const auto id = it.key();
        const auto turn = it->turn;
        m_approvals.erase(it);
        emit reverseCancelled(id);
        if (value.value("decision").toString() == QStringLiteral("allow"))
            if (auto *run = running(session, turn))
                if (run->accepted)
                    publish(session, ApprovalResolved{approvalId, Decision::Allow});
        return;
    }
}
// A chat's approval cards end with its turn (or its Pi): declined in Pi if it still
// waits on them, and withdrawn from the frontend.
void PiBackend::withdrawApprovals(const QString &session, bool answer)
{
    QVector<RequestId> ids;
    for (auto it = m_approvals.cbegin(); it != m_approvals.cend(); ++it)
        if (it->session == session)
            ids.append(it.key());
    for (const auto id : ids) {
        const auto approval = m_approvals.take(id);
        auto *pi = m_chats[session].pi;
        if (answer && pi && pi == approval.pi)
            pi->send(
                {{"type", "extension_ui_response"}, {"id", approval.dialog}, {"confirmed", false}});
        emit reverseCancelled(id);
    }
}
// Extension errors and error/warning notifications: said in the status line, never
// turned into the assistant's reply.
void PiBackend::extensionRecord(const QString &type, const QJsonObject &record)
{
    if (type == QStringLiteral("extension_error")) {
        // The bridge's retry trigger failing is the retry's own error (runEvent).
        if (record.value("extensionPath").toString() == m_bridgePath &&
            record.value("event").toString() == QStringLiteral("send_message"))
            return;
        const auto name = QFileInfo(record.value("extensionPath").toString()).fileName();
        report(QStringLiteral("error"),
               QStringLiteral("A Pi extension%1 failed: %2")
                   .arg(name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(name),
                        record.value("error").toString(QStringLiteral("no reason given."))));
    } else if (type == QStringLiteral("notify")) {
        const auto level = record.value("notifyType").toString();
        if (level == QStringLiteral("error") || level == QStringLiteral("warning"))
            report(level, record.value("message").toString());
    }
}
void PiBackend::report(const QString &level, const QString &message)
{
    fprintf(stderr, "[pi] %s: %s\n", qPrintable(level), qPrintable(message));
    emit globalEvent(Log{level, message});
}
// Pi's events for the chat's accepted turn. Messages are Pi's own: deltas stream, and
// message_end replaces them with the authoritative text and stop reason.
void PiBackend::runEvent(const QString &session, const QString &type, const QJsonObject &object)
{
    auto &run = *m_chats[session].run;
    if (type == QStringLiteral("agent_start")) {
        run.active = true;
    } else if (type == QStringLiteral("queue_update")) {
        QStringList steering;
        for (const auto &text : object.value("steering").toArray())
            steering.append(text.toString());
        // The text Pi queued for the steer awaiting its reply (after Pi's own
        // input handling), which is the text its user message will carry.
        if (!run.steering.isEmpty() && steering.size() > run.queue.size())
            for (auto &steer : run.steers)
                if (steer.client == run.steering && !steer.queued)
                    steer.queued = steering.last();
        run.queue = steering;
    } else if (type == QStringLiteral("message_start") || type == QStringLiteral("message_end")) {
        const auto message = object.value("message").toObject();
        const auto role = message.value("role").toString();
        if (role == QStringLiteral("user") && type == QStringLiteral("message_start")) {
            if (!run.retry && !run.prompted) {
                run.prompted = true; // The prompt itself.
                return;
            }
            // Pi takes steering in queue order; the first matching queued input is it.
            const auto text = textOf(message.value("content"));
            for (auto &steer : run.steers)
                if (!steer.applied && steer.queued && *steer.queued == text) {
                    steer.applied = true;
                    publish(session, InputAccepted{steer.client, steer.input});
                    break;
                }
            return;
        }
        if (role != QStringLiteral("assistant"))
            return;
        if (type == QStringLiteral("message_start")) {
            run.message = uuid();
            MessageStarted started;
            if (!message.value("model").toString().isEmpty())
                started.model = message.value("model").toString();
            publish(session, started, true);
            return;
        }
        if (run.message.isEmpty()) {
            run.message = uuid();
            publish(session, MessageStarted{}, true);
        }
        run.stopReason = message.value("stopReason").toString();
        run.errorMessage = message.value("errorMessage").toString();
        publish(session, MessageCompleted{textOf(message.value("content")), finishOf(run.stopReason)},
                true);
        if (const auto usage = usageOf(message))
            publish(session, *usage, true);
        run.message.clear();
    } else if (type == QStringLiteral("message_update")) {
        const auto event = object.value("assistantMessageEvent").toObject();
        if (event.value("type").toString() != QStringLiteral("text_delta"))
            return;
        if (run.message.isEmpty()) {
            run.message = uuid();
            publish(session, MessageStarted{}, true);
        }
        publish(session, MessageDelta{event.value("delta").toString()}, true);
    } else if (type == QStringLiteral("extension_error")) {
        // The bridge's retry trigger failed before Pi started anything.
        if (run.retry && !run.active &&
            object.value("event").toString() == QStringLiteral("send_message"))
            finish(session, {TurnStatus::Error,
                             {},
                             Error{QStringLiteral("retry_failed"),
                                   object.value("error").toString(QStringLiteral("Pi could not retry.")),
                                   {}, {}, {}, {}}});
    } else if (type == QStringLiteral("agent_settled")) {
        // Pi's last assistant message decides the outcome; Pi's own automatic
        // retries and recovery have already run by now.
        TurnCompleted done;
        if (run.cancelled || run.stopReason == QStringLiteral("aborted")) {
            done.status = TurnStatus::Cancelled;
        } else if (run.stopReason == QStringLiteral("error")) {
            done.status = TurnStatus::Error;
            done.error = Error{QStringLiteral("model_error"),
                               run.errorMessage.isEmpty() ? QStringLiteral("Pi reported an error.")
                                                          : run.errorMessage,
                               {}, {}, {}, {}};
        } else if (run.stopReason == QStringLiteral("length")) {
            done.finishReason = run.stopReason;
        } else if (run.stopReason != QStringLiteral("stop") &&
                   run.stopReason != QStringLiteral("toolUse")) {
            done.status = TurnStatus::Error;
            done.error = failure(QStringLiteral("invalid_lifecycle"),
                                 QStringLiteral("Pi settled without a supported final assistant response."));
        }
        finish(session, done);
    }
}
// Session sequences rise across turns and restarts: each process starts above the
// last one's (milliseconds × 1000), then counts up.
Sequence PiBackend::next(Chat &chat)
{
    if (chat.seq == 0)
        chat.seq = now() * 1000;
    return ++chat.seq;
}
void PiBackend::publish(const QString &session, EventPayload payload, bool message)
{
    auto &chat = m_chats[session];
    auto &run = *chat.run;
    SessionEvent event{{session, next(chat), run.turn,
                        message ? std::optional<QString>(run.message) : std::nullopt, run.client},
                       std::move(payload)};
    if (auto journal = m_journal.find(journalKey(session, run.client)); journal != m_journal.end()) {
        // Recovery needs a message's final text, not every delta that led to it.
        if (const auto *done = std::get_if<MessageCompleted>(&event.payload); done && done->text)
            journal->events.removeIf([&](const SessionEvent &e) {
                return e.identity.messageId == event.identity.messageId &&
                       std::holds_alternative<MessageDelta>(e.payload);
            });
        journal->events.append(event);
    }
    emit sessionEvent(event);
}
} // namespace openghost
