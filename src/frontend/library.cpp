#include "library.h"
#include <QDateTime>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace openghost
{
namespace
{
constexpr auto Index = "index";
constexpr int SpaceMax = 32;
const QString chatKey(const QString &id) { return QStringLiteral("chats/") + id; }
const QString sideKey(const QString &id) { return QStringLiteral("mini/") + id; }
QString uid() { return QUuid::createUuid().toString(QUuid::Id128).left(16); }
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
QString baseName(const QString &path)
{
    const auto parts =
        path.split(QRegularExpression(QStringLiteral("[\\\\/]")), Qt::SkipEmptyParts);
    return parts.isEmpty() ? path : parts.last();
}
// Linux file systems are case sensitive; Windows and macOS ones normally are not.
QString pathKey(const QString &path)
{
#ifdef Q_OS_LINUX
    return path;
#else
    return path.toLower();
#endif
}
bool count(const QJsonValue &value)
{
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0;
}
QJsonObject fields(const QJsonObject &value, std::initializer_list<const char *> strings,
                   std::initializer_list<const char *> numbers = {})
{
    QJsonObject out;
    for (const auto *key : strings)
        if (value.value(key).isString())
            out.insert(key, value.value(key));
    for (const auto *key : numbers)
        if (count(value.value(key)))
            out.insert(key, value.value(key));
    return out;
}
const std::initializer_list<const char *> UsageKeys = {"input", "cached", "written", "output",
                                                       "requests"};
QJsonObject usageOf(const QJsonObject &value) { return fields(value, {}, UsageKeys); }
QJsonObject zeroed(const QJsonObject &value, std::initializer_list<const char *> keys)
{
    QJsonObject out = value;
    for (const auto *key : keys)
        if (!out.contains(key))
            out.insert(key, 0);
    return out;
}
QJsonObject displayStats(const QJsonObject &value)
{
    auto out = fields(value, {}, {"version", "uncounted"});
    if (value.value("models").isArray()) {
        QJsonArray models;
        for (const auto &model : value.value("models").toArray())
            if (model.isObject())
                models.append(zeroed(
                    [&] {
                        auto o = fields(model.toObject(), {"id", "name"}, UsageKeys);
                        return o;
                    }(),
                    UsageKeys));
        out.insert("models", models);
    }
    if (value.value("turns").isArray()) {
        QJsonArray turns;
        for (const auto &turn : value.value("turns").toArray())
            if (turn.isObject())
                turns.append(zeroed(fields(turn.toObject(), {}, {"m", "t", "c"}), {"m", "t", "c"}));
        out.insert("turns", turns);
    }
    if (value.value("mini").isObject())
        out.insert("mini", zeroed(usageOf(value.value("mini").toObject()), UsageKeys));
    if (value.value("context").isObject())
        out.insert("context",
                   zeroed(fields(value.value("context").toObject(), {}, {"used", "window"}),
                          {"used", "window"}));
    return out;
}
QJsonArray displayAttachments(const QJsonObject &entry)
{
    // Keep invalid image slots empty rather than shifting later images.
    QStringList urls;
    for (const auto &part : entry.value("content").toArray())
        if (part.toObject().value("type") == QStringLiteral("image_url"))
            urls.append(part.toObject().value("image_url").toObject().value("url").toString());
    int image = 0;
    QJsonArray out;
    for (const auto &value : entry.value("attachments").toArray()) {
        const auto item = value.toObject();
        if (!value.isObject() || !item.value("name").isString())
            continue;
        auto o = fields(item, {"name", "note"}, {"size", "width", "height"});
        const bool isImage = item.value("image").toBool() || item.value("kind") == "image";
        o.insert("image", isImage);
        if (isImage) {
            const auto legacy = image < urls.size() ? urls.at(image) : QString();
            ++image;
            for (const auto &url :
                 {item.value("url").toString(), item.value("dataUrl").toString(), legacy})
                if (!url.isEmpty()) {
                    o.insert("url", url);
                    break;
                }
        }
        if (item.value("pasted").isObject())
            o.insert("pasted", fields(item.value("pasted").toObject(), {"preview"}, {"lines"}));
        if (item.value("video").isObject())
            o.insert("video", fields(item.value("video").toObject(), {"poster"}, {"duration"}));
        out.append(o);
    }
    return out;
}
} // namespace

QJsonArray Library::displayMessages(const QJsonArray &messages)
{
    static const QStringList roles{"user",  "assistant", "compact", "stats",
                                   "moved", "tool",      "thinking"};
    static const QStringList toolStates{"running", "done", "error", "cancelled", "missing",
                                        "unconfirmed"};
    static const QStringList receipts{"queued", "applied", "notApplied", "unconfirmed"};
    QJsonArray result;
    for (const auto &value : messages) {
        const auto entry = value.toObject();
        const auto role = entry.value("role").toString();
        if (!value.isObject() || !roles.contains(role))
            continue;
        QJsonObject out{{"role", role}};
        if (role == "stats") {
            out.insert("stats", displayStats(entry.value("stats").toObject()));
            result.append(out);
            continue;
        }
        if (role == "moved") {
            result.append(out);
            continue;
        }
        // Frontend recovery checkpoints, never model requests or provider history.
        for (const auto *key : {"backendTurn", "clientInputId"})
            if (entry.value(key).isString())
                out.insert(key, entry.value(key));
        if (entry.value("pendingTurn") == QJsonValue(true))
            out.insert("pendingTurn", true);
        if (role == "tool") {
            // A tool call's card, as shown: bounded strings and a known state.
            const auto callId = entry.value("callId").toString(),
                       name = entry.value("name").toString();
            if (callId.isEmpty() || callId.size() > 256 || name.isEmpty() || name.size() > 256 ||
                !toolStates.contains(entry.value("state").toString()))
                continue;
            out.insert("callId", callId);
            out.insert("name", name);
            out.insert("state", entry.value("state"));
            out.insert("output", entry.value("output").toString().left(33 * 1024));
            if (entry.value("arguments").isString())
                out.insert("arguments", entry.value("arguments").toString().left(4097));
            if (entry.value("ending").isString())
                out.insert("ending", entry.value("ending").toString().left(4096));
            for (const auto *key : {"omittedLines", "omittedCharacters"})
                if (count(entry.value(key)))
                    out.insert(key, entry.value(key));
            if (entry.value("turn").isString())
                out.insert("turn", entry.value("turn"));
            if (entry.value("joined") == QJsonValue(true))
                out.insert("joined", true);
            result.append(out);
            continue;
        }
        if (role == "thinking") {
            // A message's thinking, as shown (bounded as the chat keeps it).
            const auto text = entry.value("content").toString().left(64 * 1024);
            if (text.trimmed().isEmpty())
                continue;
            out.insert("content", text);
            if (entry.value("turn").isString())
                out.insert("turn", entry.value("turn"));
            result.append(out);
            continue;
        }
        if (role == "user") {
            const auto text = entry.value("text").isString() ? entry.value("text").toString()
                              : entry.value("content").isString()
                                  ? entry.value("content").toString()
                                  : QString();
            out.insert("text", text);
            if (entry.value("content").isString())
                out.insert("content", text);
            if (entry.value("inputError").isString())
                out.insert("inputError", entry.value("inputError"));
            if (receipts.contains(entry.value("receipt").toString()))
                out.insert("receipt", entry.value("receipt"));
            if (entry.value("attachments").isArray())
                out.insert("attachments", displayAttachments(entry));
        } else {
            if (entry.value("model").isString())
                out.insert("model", entry.value("model"));
            if (entry.value("usage").isObject())
                out.insert("usage", usageOf(entry.value("usage").toObject()));
            if (role == "assistant") {
                out.insert("content", entry.value("content").toString());
                if (entry.value("continued") == QJsonValue(true))
                    out.insert("continued", true);
                if (entry.value("turn").isString() || count(entry.value("turn")))
                    out.insert("turn", entry.value("turn"));
                if (!out.contains("usage") && (entry.value("uncounted") == QJsonValue(true) ||
                                               !entry.value("steps").toArray().isEmpty()))
                    out.insert("uncounted", true);
            }
        }
        result.append(out);
    }
    return result;
}

Library::Library(KeyStore *store, ChatSealer *sealer, QString homePath, QObject *parent)
    : QObject(parent), m_store(store), m_sealer(sealer), m_homePath(std::move(homePath))
{
    const auto read = m_store->read(Index);
    if (read.status == KeyStore::Status::Absent)
        return;
    const auto &index = read.value;
    if (read.status == KeyStore::Status::Unreadable || index.value("version").toInt() != 1) {
        m_error = QStringLiteral("The chat list could not be read; it was left unchanged.");
        return;
    }
    m_homeCollapsed = index.value("home").toObject().value("collapsed").toBool();
    for (const auto &value : index.value("folders").toArray()) {
        const auto o = value.toObject();
        if (!o.value("path").isString())
            continue;
        m_folders.append({o.value("path").toString(),
                          o.value("name").toString(baseName(o.value("path").toString())),
                          o.value("collapsed").toBool(), qint64(o.value("added").toDouble())});
    }
    QSet<QString> seen;
    for (const auto &value : index.value("chats").toArray()) {
        auto o = value.toObject();
        const auto id = o.value("id").toString();
        if (id.isEmpty() || !o.value("folder").isString() || seen.contains(id) ||
            !KeyStore::validKey(chatKey(id)))
            continue;
        seen.insert(id);
        Chat chat;
        chat.id = id;
        chat.title = o.value("title").toString();
        chat.folder = o.value("folder").toString();
        if (o.value("space").isString())
            chat.space = o.value("space").toString();
        chat.created = qint64(o.value("created").toDouble());
        chat.updated = qint64(o.value("updated").toDouble());
        chat.pinned = o.value("pinned").toBool();
        chat.named = o.value("named").toBool();
        chat.model = o.value("model").toObject();
        chat.lock = o.value("lock").toObject();
        for (const auto *key : {"id", "title", "folder", "space", "created", "updated", "pinned",
                                "named", "model", "lock", "archived"})
            o.remove(key);
        chat.extra = o;
        m_chats.append(chat);
        if (!isHome(chat))
            addFolder(chat.folder);
    }
}
const Library::Chat *Library::chat(const QString &id) const
{
    const auto it =
        std::find_if(m_chats.cbegin(), m_chats.cend(), [&](const auto &c) { return c.id == id; });
    return it == m_chats.cend() ? nullptr : &*it;
}
Library::Chat *Library::find(const QString &id) { return const_cast<Chat *>(chat(id)); }
bool Library::persist()
{
    if (!writable())
        return false;
    QJsonArray folders, chats;
    for (const auto &f : m_folders)
        folders.append(QJsonObject{{"path", f.path},
                                   {"name", f.name},
                                   {"collapsed", f.collapsed},
                                   {"added", double(f.added)}});
    for (const auto &c : m_chats) {
        QJsonObject o = c.extra;
        o.insert("id", c.id);
        o.insert("title", c.title);
        o.insert("folder", c.folder);
        if (c.space)
            o.insert("space", *c.space);
        o.insert("created", double(c.created));
        o.insert("updated", double(c.updated));
        o.insert("pinned", c.pinned);
        o.insert("named", c.named);
        if (!c.model.isEmpty())
            o.insert("model", c.model);
        if (!c.lock.isEmpty())
            o.insert("lock", c.lock);
        chats.append(o);
    }
    return m_store->write(Index, {{"version", 1},
                                  {"folders", folders},
                                  {"chats", chats},
                                  {"home", QJsonObject{{"collapsed", m_homeCollapsed}}}});
}
const Library::Folder *Library::folder(const QString &path) const
{
    const auto it = std::find_if(m_folders.cbegin(), m_folders.cend(),
                                 [&](const auto &f) { return samePath(f.path, path); });
    return it == m_folders.cend() ? nullptr : &*it;
}
const Library::Folder *Library::addFolder(const QString &path, const QString &name)
{
    if (path.isEmpty())
        return nullptr;
    if (const auto *known = folder(path))
        return known;
    m_folders.append({path, name.isEmpty() ? baseName(path) : name, false, now()});
    return &m_folders.last();
}
bool Library::within(const Chat &chat, const QString &path) const
{
    return !isHome(chat) && samePath(chat.folder, path);
}
QString Library::cwdOf(const Chat &chat) const
{
    if (!isHome(chat))
        return chat.folder;
    if (m_homePath.isEmpty())
        return {};
    return m_homePath +
           (m_homePath.contains(QLatin1Char('\\')) ? QStringLiteral("\\") : QStringLiteral("/")) +
           *chat.space;
}
bool Library::samePath(const QString &a, const QString &b) { return pathKey(a) == pathKey(b); }
QString Library::spaceName(const QString &title)
{
    QString name = title;
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x{0000}-\\x{001f}\\x{2026}]")),
                 QStringLiteral(" "));
    name = name.simplified();
    if (name.size() > SpaceMax) {
        const auto cut = name.left(SpaceMax + 1);
        const auto space = cut.lastIndexOf(QLatin1Char(' '));
        name = space > SpaceMax / 2 ? cut.left(space) : name.left(SpaceMax);
    }
    name.remove(QRegularExpression(QStringLiteral("[\\s.,;:!?-]+$")));
    // Windows reserves a few device names.
    if (name.isEmpty() || QRegularExpression(QStringLiteral("^(con|prn|aux|nul|com\\d|lpt\\d)$"),
                                             QRegularExpression::CaseInsensitiveOption)
                              .match(name)
                              .hasMatch())
        name = name.isEmpty() ? QStringLiteral("New chat") : QStringLiteral("New chat ") + name;
    return name;
}
QString Library::space(const QString &title) const
{
    QSet<QString> taken;
    for (const auto &c : m_chats)
        if (c.space)
            taken.insert(pathKey(*c.space));
    const auto base = spaceName(title);
    auto name = base;
    for (int n = 2; taken.contains(pathKey(name)); ++n)
        name = QStringLiteral("%1 %2").arg(base).arg(n);
    return name;
}
const Library::Chat *Library::create(const QString &folder, const QString &title)
{
    if (!writable())
        return nullptr;
    Chat chat;
    chat.id = uid();
    chat.title = title;
    chat.created = chat.updated = now();
    if (folder.isEmpty()) {
        chat.folder = m_homePath;
        chat.space = space(title);
    } else {
        chat.folder = addFolder(folder)->path;
    }
    m_chats.append(chat);
    persist();
    emit changed();
    return &m_chats.last();
}
bool Library::update(const QString &id, const std::function<void(Chat &)> &change)
{
    auto *c = find(id);
    if (!c || !writable())
        return false;
    change(*c);
    const bool ok = persist();
    emit changed();
    return ok;
}
bool Library::retitle(const QString &id, const QString &title, bool named)
{
    auto *c = find(id);
    if (!c || !writable())
        return false;
    if (!c->lock.isEmpty()) {
        // A protected chat's title is sealed; it changes only while open.
        const auto key = m_keys.constFind(id);
        if (key == m_keys.cend() || !m_sealer)
            return false;
        const auto sealed = m_sealer->seal(*key, {{"title", title}});
        if (!sealed)
            return false;
        m_titles.insert(id, title);
        c->lock.insert("title", *sealed);
    } else {
        c->title = title;
    }
    c->named = c->named || named;
    const bool ok = persist();
    emit changed();
    return ok;
}
bool Library::setPinned(const QString &id, bool pinned)
{
    return update(id, [pinned](Chat &c) { c.pinned = pinned; });
}
bool Library::toggleFolder(const std::optional<QString> &path)
{
    if (!writable())
        return false;
    if (!path)
        m_homeCollapsed = !m_homeCollapsed;
    else {
        auto it = std::find_if(m_folders.begin(), m_folders.end(),
                               [&](const auto &f) { return samePath(f.path, *path); });
        if (it == m_folders.end())
            return false;
        it->collapsed = !it->collapsed;
    }
    const bool ok = persist();
    emit changed();
    return ok;
}
qint64 Library::activity(const Folder &folder) const
{
    qint64 last = folder.added;
    for (const auto &c : m_chats)
        if (within(c, folder.path))
            last = std::max(last, c.updated);
    return last;
}
QStringList Library::inFolder(const QString &path) const
{
    QStringList ids;
    for (const auto &c : m_chats)
        if (within(c, path))
            ids.append(c.id);
    return ids;
}
bool Library::remove(const QString &id)
{
    const auto it =
        std::find_if(m_chats.begin(), m_chats.end(), [&](const auto &c) { return c.id == id; });
    if (it == m_chats.end() || !writable())
        return false;
    m_chats.erase(it);
    m_keys.remove(id);
    m_titles.remove(id);
    const bool ok = persist();
    m_store->remove(chatKey(id));
    m_store->remove(sideKey(id));
    emit changed();
    return ok;
}
QStringList Library::removeFolder(const QString &path)
{
    if (!writable())
        return {};
    const auto gone = inFolder(path);
    m_chats.erase(std::remove_if(m_chats.begin(), m_chats.end(),
                                 [&](const auto &c) { return within(c, path); }),
                  m_chats.end());
    m_folders.erase(std::remove_if(m_folders.begin(), m_folders.end(),
                                   [&](const auto &f) { return samePath(f.path, path); }),
                    m_folders.end());
    persist();
    for (const auto &id : gone) {
        m_keys.remove(id);
        m_titles.remove(id);
        m_store->remove(chatKey(id));
        m_store->remove(sideKey(id));
    }
    emit changed();
    return gone;
}

QJsonObject Library::bodyObject(const Body &body, bool side) const
{
    QJsonObject o{{"messages", displayMessages(body.messages)},
                  {"tokens", std::isfinite(body.tokens) && body.tokens >= 0 ? body.tokens : 0}};
    if (side)
        o.insert("seen", double(std::max<qint64>(0, body.seen)));
    return o;
}
std::optional<Library::Body> Library::readBody(const QString &key, const QString &id, bool side,
                                               QString *error) const
{
    const auto fail = [error](const QString &message) -> std::optional<Body> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const auto read = m_store->read(key);
    if (read.status == KeyStore::Status::Unreadable)
        return fail(QStringLiteral("This chat's saved messages could not be read."));
    auto data = read.value;
    if (data.value("sealed").isObject()) {
        // Throw rather than hand back an empty chat that could overwrite it.
        const auto k = m_keys.constFind(id);
        if (k == m_keys.cend() || !m_sealer)
            return fail(QStringLiteral("This chat is locked."));
        const auto opened = m_sealer->open(*k, data.value("sealed").toObject());
        if (!opened)
            return fail(QStringLiteral("This chat's saved messages could not be opened."));
        data = *opened;
    } else if (isProtected(id) && read.status == KeyStore::Status::Ok && !m_keys.contains(id)) {
        return fail(QStringLiteral("This chat is locked."));
    }
    Body body;
    body.messages = displayMessages(data.value("messages").toArray());
    body.tokens = count(data.value("tokens")) ? data.value("tokens").toDouble() : 0;
    if (side)
        body.seen = count(data.value("seen")) ? qint64(data.value("seen").toDouble()) : 0;
    return body;
}
bool Library::writeBody(const QString &key, const QString &id, const Body &body, bool side)
{
    const auto *c = chat(id);
    const bool locked = c && !c->lock.isEmpty();
    const auto k = m_keys.constFind(id);
    if (!c || !writable() || (locked && (k == m_keys.cend() || !m_sealer)))
        return false; // A locked chat without its key is never written over.
    QJsonObject value{{"version", 1}};
    const auto plain = bodyObject(body, side);
    if (locked) {
        const auto sealed = m_sealer->seal(*k, plain);
        if (!sealed)
            return false;
        value.insert("sealed", *sealed);
    } else {
        for (auto it = plain.begin(); it != plain.end(); ++it)
            value.insert(it.key(), it.value());
    }
    return m_store->write(key, value);
}
std::optional<Library::Body> Library::conversation(const QString &id, QString *error) const
{
    return readBody(chatKey(id), id, false, error);
}
bool Library::saveMessages(const QString &id, const QJsonArray &messages, double tokens)
{
    return writeBody(chatKey(id), id, {messages, tokens, 0}, false);
}
std::optional<Library::Body> Library::side(const QString &id, QString *error) const
{
    return readBody(sideKey(id), id, true, error);
}
bool Library::saveSide(const QString &id, const Body &body)
{
    return writeBody(sideKey(id), id, body, true);
}
bool Library::clearSide(const QString &id) { return m_store->remove(sideKey(id)); }

bool Library::isProtected(const QString &id) const
{
    const auto *c = chat(id);
    return c && !c->lock.isEmpty();
}
bool Library::isLocked(const QString &id) const { return isProtected(id) && !m_keys.contains(id); }
std::optional<QString> Library::titleOf(const QString &id) const
{
    const auto *c = chat(id);
    if (!c)
        return std::nullopt;
    if (c->lock.isEmpty())
        return c->title;
    const auto t = m_titles.constFind(id);
    return t == m_titles.cend() ? std::nullopt : std::optional<QString>(*t);
}
bool Library::reseal(const QString &id, const std::optional<QByteArray> &key)
{
    // A mini chat that fails to change never holds the chat's own password back.
    const auto read = m_store->read(sideKey(id));
    if (read.status != KeyStore::Status::Ok)
        return read.status == KeyStore::Status::Absent;
    const bool sealed = read.value.value("sealed").isObject();
    if (sealed == key.has_value())
        return true;
    QJsonObject source = read.value;
    if (sealed) {
        const auto k = m_keys.constFind(id);
        const auto opened = k == m_keys.cend() || !m_sealer
                                ? std::nullopt
                                : m_sealer->open(*k, read.value.value("sealed").toObject());
        if (!opened)
            return false;
        source = *opened;
    }
    Body body{displayMessages(source.value("messages").toArray()),
              count(source.value("tokens")) ? source.value("tokens").toDouble() : 0,
              count(source.value("seen")) ? qint64(source.value("seen").toDouble()) : 0};
    QJsonObject value{{"version", 1}};
    const auto plain = bodyObject(body, true);
    if (key) {
        const auto s = m_sealer->seal(*key, plain);
        if (!s)
            return false;
        value.insert("sealed", *s);
    } else
        for (auto it = plain.begin(); it != plain.end(); ++it)
            value.insert(it.key(), it.value());
    return m_store->write(sideKey(id), value);
}
QString Library::protect(const QString &id, const QString &password,
                         const std::optional<Body> &loaded)
{
    auto *c = find(id);
    if (!m_sealer)
        return QStringLiteral("Chat passwords are not available in this build.");
    if (!c || !c->lock.isEmpty() || !writable() || password.isEmpty())
        return QStringLiteral("This chat cannot be protected.");
    QString error;
    const auto body = loaded ? loaded : conversation(id, &error);
    if (!body)
        return error;
    const auto salt = m_sealer->salt();
    const auto key = m_sealer->derive(password.normalized(QString::NormalizationForm_C), salt,
                                      ChatSealer::Iterations);
    const auto title = key ? m_sealer->seal(*key, {{"title", c->title}}) : std::nullopt;
    if (!key || !title)
        return QStringLiteral("The password could not be applied.");
    const auto plainTitle = c->title;
    // The index takes the lock first; a crash in between leaves a protected chat
    // whose messages are still readable, never sealed ones nothing can open.
    c->lock = {
        {"version", 1}, {"iterations", ChatSealer::Iterations}, {"salt", salt}, {"title", *title}};
    c->title.clear();
    if (!persist()) {
        c->lock = {};
        c->title = plainTitle;
        return QStringLiteral("The chat list could not be saved.");
    }
    m_keys.insert(id, *key);
    const bool sealed = saveMessages(id, body->messages, body->tokens);
    reseal(id, *key);
    // Setting a password locks the chat straight away.
    m_keys.remove(id);
    m_titles.remove(id);
    emit changed();
    return sealed ? QString() : QStringLiteral("The chat's messages could not be sealed.");
}
QString Library::unlock(const QString &id, const QString &password)
{
    const auto *c = chat(id);
    if (!c || c->lock.isEmpty())
        return {};
    if (!m_sealer)
        return QStringLiteral("Chat passwords are not available in this build.");
    const auto key = m_sealer->derive(password.normalized(QString::NormalizationForm_C),
                                      c->lock.value("salt").toString(),
                                      c->lock.value("iterations").toInt(ChatSealer::Iterations));
    // The password is checked against the sealed title, which only the right key opens.
    const auto opened =
        key ? m_sealer->open(*key, c->lock.value("title").toObject()) : std::nullopt;
    if (!opened)
        return QStringLiteral("Wrong password.");
    if (!m_keys.contains(id))
        m_keys.insert(id, *key);
    m_titles.insert(id, opened->value("title").toString());
    emit changed();
    return {};
}
void Library::relock(const QString &id)
{
    if (m_keys.remove(id) + m_titles.remove(id))
        emit changed();
}
QString Library::unprotect(const QString &id, const std::optional<Body> &loaded)
{
    auto *c = find(id);
    if (!c || c->lock.isEmpty() || !m_keys.contains(id) || !writable())
        return QStringLiteral("Open the chat with its password first.");
    QString error;
    const auto body = loaded ? loaded : conversation(id, &error);
    if (!body)
        return error;
    // The messages are written in the clear first, then the index lets go.
    QJsonObject plain = bodyObject(*body, false);
    plain.insert("version", 1);
    if (!m_store->write(chatKey(id), plain))
        return QStringLiteral("The chat's messages could not be saved.");
    reseal(id, std::nullopt);
    c->title = m_titles.value(id);
    c->lock = {};
    m_keys.remove(id);
    m_titles.remove(id);
    const bool ok = persist();
    emit changed();
    return ok ? QString() : QStringLiteral("The chat list could not be saved.");
}
} // namespace openghost
