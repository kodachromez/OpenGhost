#pragma once

#include <QDate>
#include <QImage>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

// Explicit absent services for the copied settings pages. These are not a
// backend, do not read any existing stores, and never report an operation as saved.
inline QString backendUnavailable()
{
    return QStringLiteral("Backend not connected — this build is a standalone UI shell.");
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
    using QObject::QObject;
    bool available() const { return false; }
    bool adding() const { return false; }
    QString instructions() const { return {}; }
    void setInstructions(const QString &) { emit saveFailed(backendUnavailable()); }
    QVariantList files() const { return {}; }
    int maxInstructions() const { return 8000; }
    int maxFiles() const { return 20; }
    int pictures() const { return 0; }
    int maxPictures() const { return 0; }
    Q_INVOKABLE QString add(const QList<QUrl> &) { return backendUnavailable(); }
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
};

class UsagePreview final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString thisMonth READ thisMonth CONSTANT)
    Q_PROPERTY(double since READ since CONSTANT)
  public:
    using QObject::QObject;
    QString thisMonth() const { return QDate::currentDate().toString(QStringLiteral("yyyy-MM")); }
    double since() const { return 0; }
    Q_INVOKABLE QVariantMap totals(int = 0) const { return {}; }
    Q_INVOKABLE QVariantMap between(const QString &, const QString &) const { return {}; }
    Q_INVOKABLE QVariantList daily(int) const { return {}; }
    Q_INVOKABLE QVariantList month(const QString &) const { return {}; }
    Q_INVOKABLE QStringList months() const { return {}; }
    Q_INVOKABLE QString nameOf(const QString &id) const { return id; }
  signals:
    void changed();
};
