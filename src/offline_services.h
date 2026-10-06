#pragma once

#include "frontend/preferences.h"
#include "frontend/usage.h"
#include <QDate>
#include <QImage>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

// Small QML settings projections. Instructions use the frontend preference
// store; pinned-file preparation remains explicitly absent. Usage has a separate
// frontend-owned ledger, backed by the selected file or memory store.
inline QString backendUnavailable()
{
    return QStringLiteral("This operation is not connected in OpenGhost C++ yet.");
}

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
    QVariantList files() const { return {}; }
    int maxInstructions() const { return 8000; }
    int maxFiles() const { return 20; }
    int pictures() const { return 0; }
    int maxPictures() const { return 0; }
    Q_INVOKABLE QString add(const QList<QUrl> &)
    {
        return QStringLiteral("Pinned file preparation is not implemented yet.");
    }
    Q_INVOKABLE void remove(const QString &) { emit saveFailed(backendUnavailable()); }
    Q_INVOKABLE QString tip(const QString &) const { return {}; }
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
