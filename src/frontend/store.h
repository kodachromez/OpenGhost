#pragma once
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <optional>

namespace openghost
{
// Frontend-local key/value storage (chat-store.js). It holds display caches,
// the chat index, mini chats, recovery markers and the usage ledger, never
// backend history or credentials. Keys are short relative names such as
// "index", "chats/<id>" or "mini/<id>".
class KeyStore
{
  public:
    enum class Status { Absent, Ok, Unreadable };
    struct Read {
        Status status = Status::Absent;
        QJsonObject value;
    };
    virtual ~KeyStore() = default;
    virtual Read read(const QString &key) const = 0;
    // A write is reported done only when the whole value is published.
    virtual bool write(const QString &key, const QJsonObject &value) = 0;
    virtual bool remove(const QString &key) = 0;
    static bool validKey(const QString &key);
};

// Explicit in-memory store: tests and ephemeral launches. Nothing survives exit.
class MemoryKeyStore final : public KeyStore
{
  public:
    Read read(const QString &key) const override;
    bool write(const QString &key, const QJsonObject &value) override;
    bool remove(const QString &key) override;
    // Test fault injection: refuse writes/removals whose key starts with this.
    QString failWrites;
    QHash<QString, QJsonObject> values;
    QHash<QString, QByteArray> raw; // Unparseable bytes, for corruption tests.
};

// One JSON file per key under an explicit directory, published with QSaveFile.
// Unreadable/oversized/invalid files are reported, never silently replaced.
class FileKeyStore final : public KeyStore
{
  public:
    explicit FileKeyStore(QString directory) : m_directory(std::move(directory)) {}
    Read read(const QString &key) const override;
    bool write(const QString &key, const QJsonObject &value) override;
    bool remove(const QString &key) override;
    static constexpr qint64 MaxBytes = 64 * 1024 * 1024;

  private:
    QString path(const QString &key) const;
    QString m_directory;
};
} // namespace openghost
