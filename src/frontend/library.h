#pragma once
#include "store.h"
#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QVector>
#include <functional>

namespace openghost
{
// Chat locks (chat-lock.js): a password is never stored. A protected chat keeps
// a random salt; the stretched password becomes a key sealing its messages and
// title. The cipher/KDF is a host service: QtCore has no AES-GCM/PBKDF2, so
// production supplies none yet and protected chats stay locked (fail closed).
class ChatSealer
{
  public:
    static constexpr int Iterations = 600000;
    virtual ~ChatSealer() = default;
    virtual QString salt() = 0;
    virtual std::optional<QByteArray> derive(const QString &password, const QString &salt,
                                             int iterations) = 0;
    virtual std::optional<QJsonObject> seal(const QByteArray &key, const QJsonObject &value) = 0;
    // Must refuse (nullopt) unless `key` sealed `sealed`, byte for byte.
    virtual std::optional<QJsonObject> open(const QByteArray &key, const QJsonObject &sealed) = 0;
};

// The frontend's chat index and per-chat display caches (library.js): folders,
// home (folderless) chats with their own spaces, pins, collapse, titles, lock
// metadata, display messages with recovery markers, and each mini chat. These
// are display caches and frontend checkpoints, never backend/model history.
//
// An unreadable or unknown index makes the library read-only: nothing on disk
// is overwritten with an empty list. Writes are synchronous and ordered.
class Library final : public QObject
{
    Q_OBJECT
  public:
    struct Folder {
        QString path, name;
        bool collapsed = false;
        qint64 added = 0;
    };
    struct Chat {
        QString id, title, folder;
        std::optional<QString> space; // set: a chat without a project folder
        qint64 created = 0, updated = 0;
        bool pinned = false, named = false;
        QJsonObject model, lock, extra; // extra: unknown index fields, kept as read
    };
    struct Body {
        QJsonArray messages;
        double tokens = 0;
        qint64 seen = 0; // mini chats: the chat's `updated` at the latest question
    };
    Library(KeyStore *store, ChatSealer *sealer = nullptr, QString homePath = {},
            QObject *parent = nullptr);
    QString error() const { return m_error; }
    bool writable() const { return m_error.isEmpty(); }
    const QVector<Folder> &folders() const { return m_folders; }
    const QVector<Chat> &chats() const { return m_chats; }
    const Chat *chat(const QString &id) const;
    bool homeCollapsed() const { return m_homeCollapsed; }
    QString homePath() const { return m_homePath; }

    bool persist(); // The index, now: a step that must be on disk before the next.
    const Chat *create(const QString &folder, const QString &title);
    bool update(const QString &id, const std::function<void(Chat &)> &change);
    bool retitle(const QString &id, const QString &title, bool named);
    bool setPinned(const QString &id, bool pinned);
    bool toggleFolder(const std::optional<QString> &path); // nullopt: the home group
    const Folder *folder(const QString &path) const;
    const Folder *addFolder(const QString &path, const QString &name = {});
    bool remove(const QString &id);
    QStringList removeFolder(const QString &path);
    QStringList inFolder(const QString &path) const;
    qint64 activity(const Folder &folder) const;

    bool isHome(const Chat &chat) const { return chat.space.has_value(); }
    bool within(const Chat &chat, const QString &path) const;
    QString cwdOf(const Chat &chat) const;
    QString space(const QString &title) const;
    static QString spaceName(const QString &title);
    static bool samePath(const QString &a, const QString &b);

    // Display caches. A locked/unreadable cache never reads back as empty.
    std::optional<Body> conversation(const QString &id, QString *error = nullptr) const;
    // Synchronous; the real result is always reported, so a pre-dispatch
    // recovery checkpoint fails closed while ordinary saves stay best effort.
    bool saveMessages(const QString &id, const QJsonArray &messages, double tokens);
    std::optional<Body> side(const QString &id, QString *error = nullptr) const;
    bool saveSide(const QString &id, const Body &body);
    bool clearSide(const QString &id);
    static QJsonArray displayMessages(const QJsonArray &messages);

    bool canSeal() const { return m_sealer != nullptr; }
    bool isProtected(const QString &id) const;
    bool isLocked(const QString &id) const;
    std::optional<QString> titleOf(const QString &id) const;
    QString protect(const QString &id, const QString &password,
                    const std::optional<Body> &loaded = {});
    QString unlock(const QString &id, const QString &password);
    void relock(const QString &id);
    QString unprotect(const QString &id, const std::optional<Body> &loaded = {});

  signals:
    void changed();

  private:
    Chat *find(const QString &id);
    QJsonObject bodyObject(const Body &body, bool side) const;
    std::optional<Body> readBody(const QString &key, const QString &id, bool side,
                                 QString *error) const;
    bool writeBody(const QString &key, const QString &id, const Body &body, bool side);
    bool reseal(const QString &id, const std::optional<QByteArray> &key);
    KeyStore *m_store;
    ChatSealer *m_sealer;
    QString m_homePath, m_error;
    QVector<Folder> m_folders;
    QVector<Chat> m_chats;
    bool m_homeCollapsed = false;
    QHash<QString, QByteArray> m_keys; // open protected chats: memory only
    QHash<QString, QString> m_titles;
};
} // namespace openghost
