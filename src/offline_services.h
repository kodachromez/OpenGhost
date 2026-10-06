#pragma once

#include "frontend/attachments.h"
#include "frontend/preferences.h"
#include "frontend/usage.h"
#include <QFileInfo>
#include <QUuid>
#include <QDate>
#include <QImage>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

// Small QML settings projections. Instructions and pinned files use the frontend
// preference store. Usage has a separate frontend-owned ledger, backed by the
// selected file or memory store.
inline QString backendUnavailable()
{
    return QStringLiteral("This operation is not connected in OpenGhost C++ yet.");
}

// Settings → General: standing instructions and the files every chat keeps at
// hand (user-context.js). A pinned file is a copy of a local text file's contents,
// read when added; adding the same file again replaces that copy. Pictures and
// other files are refused by name: nothing could deliver them as context.
class GeneralPreview final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(
        QString instructions READ instructions WRITE setInstructions NOTIFY instructionsChanged)
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool adding READ adding CONSTANT)
    Q_PROPERTY(int maxInstructions READ maxInstructions CONSTANT)
    Q_PROPERTY(int maxFiles READ maxFiles CONSTANT)
    Q_PROPERTY(int pictures READ pictures CONSTANT)
    Q_PROPERTY(int maxPictures READ maxPictures CONSTANT)
  public:
    explicit GeneralPreview(openghost::PreferencesStore *store, QObject *parent = nullptr)
        : QObject(parent), m_store(store)
    {
        connect(store, &openghost::PreferencesStore::changed, this,
                &GeneralPreview::instructionsChanged);
        connect(store, &openghost::PreferencesStore::changed, this, &GeneralPreview::filesChanged);
        connect(store, &openghost::PreferencesStore::saveFailed, this, &GeneralPreview::saveFailed);
    }
    bool available() const { return true; }
    bool adding() const { return false; }
    QString instructions() const { return m_store->value().userContext.instructions; }
    void setInstructions(const QString &text)
    {
        auto preferences = m_store->value();
        preferences.userContext.instructions = text;
        m_store->save(preferences);
    }
    QVariantList files() const
    {
        QVariantList list;
        for (const auto &file : m_store->value().userContext.files)
            list.append(QVariantMap{{"id", file.id},
                                    {"name", file.name},
                                    {"size", double(file.size)},
                                    {"kind", QStringLiteral("text")},
                                    {"chars", file.text ? int(file.text->size()) : 0}});
        return list;
    }
    int maxInstructions() const { return 8000; }
    int maxFiles() const { return 20; }
    int pictures() const { return 0; }
    int maxPictures() const { return 0; }
    // The whole selection, or nothing: the refusal names why.
    Q_INVOKABLE QString add(const QList<QUrl> &urls)
    {
        auto preferences = m_store->value();
        auto &files = preferences.userContext.files;
        for (const auto &url : urls) {
            const auto read = openghost::readLocalFile(url, false);
            if (!read.error.isEmpty())
                return read.error;
            openghost::ContextFile file;
            file.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            file.name = read.attachment.name;
            file.size = read.attachment.size;
            file.kind = openghost::ContextFile::Kind::Text;
            file.text = read.attachment.text;
            file.path = QFileInfo(url.toLocalFile()).absoluteFilePath();
            const auto same = std::find_if(files.begin(), files.end(), [&](const auto &kept) {
                return kept.path == file.path;
            });
            if (same != files.end())
                *same = file;
            else
                files.append(file);
        }
        qint64 chars = 0;
        for (const auto &file : files)
            chars += file.text ? file.text->size() : 0;
        if (files.size() > maxFiles())
            return QStringLiteral("Keep at most 20 files for every chat. Nothing was added.");
        if (chars > 200000)
            return QStringLiteral("Files kept for every chat are limited to 200,000 characters "
                                  "together. Nothing was added.");
        // A failed save is said through saveFailed and changes nothing.
        m_store->save(preferences);
        return {};
    }
    Q_INVOKABLE void remove(const QString &id)
    {
        auto preferences = m_store->value();
        if (preferences.userContext.files.removeIf([&](const auto &f) { return f.id == id; }))
            m_store->save(preferences);
    }
    // A pinned file's tooltip: where its copy was read from (1.2's title).
    Q_INVOKABLE QString tip(const QString &id) const
    {
        for (const auto &file : m_store->value().userContext.files)
            if (file.id == id)
                return file.path.value_or(file.name);
        return {};
    }
    Q_INVOKABLE bool hasThumbnail(const QString &) const { return false; }
    Q_INVOKABLE QImage thumbnail(const QString &) const { return {}; }
  signals:
    void instructionsChanged();
    void filesChanged();
    void thumbnailsChanged();
    void added(QString error);
    void saveFailed(QString error);

  private:
    openghost::PreferencesStore *m_store;
};

using UsagePreview = openghost::UsageStore;
