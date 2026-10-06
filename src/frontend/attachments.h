#pragma once
#include "backend/types.h"
#include <QHash>
#include <QUrl>

namespace openghost
{
// One local file, read for a message or for General's pinned files: UTF-8 text,
// or a picture (sent as the image input Pi takes). Anything else (PDF, office
// documents, media, other binary files) is refused by name: nothing reads it.
struct FileRead {
    QString error; // nonempty: refused, nothing read
    Attachment attachment; // no id; never a path
};
FileRead readLocalFile(const QUrl &url, bool pictures);
QString textOnlyRefusal(const QString &name);

// Bounded native preparation. Opaque tokens own full payloads; display
// metadata is never sufficient to resend an attachment. No model-selected paths.
class AttachmentStore
{
  public:
    struct Prepared {
        QString token, name;
        qint64 size = 0;
        bool picture = false;
    };
    QString prepare(const QList<QUrl> &urls, int remaining, QVector<Prepared> &out);
    std::optional<QVector<Attachment>> resolve(const QStringList &tokens) const;
    void release(const QStringList &tokens);

  private:
    QHash<QString, Attachment> m_payloads;
};
} // namespace openghost
