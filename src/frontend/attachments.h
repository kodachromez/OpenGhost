#pragma once
#include "backend/types.h"
#include <QHash>
#include <QUrl>

namespace openghost
{
// Bounded native text preparation. Opaque tokens own full payloads; display
// metadata is never sufficient to resend an attachment. No model-selected paths.
class AttachmentStore
{
  public:
    struct Prepared {
        QString token, name;
        qint64 size = 0;
    };
    QString prepare(const QList<QUrl> &urls, int remaining, QVector<Prepared> &out);
    std::optional<QVector<Attachment>> resolve(const QStringList &tokens) const;
    void release(const QStringList &tokens);

  private:
    QHash<QString, Attachment> m_payloads;
};
} // namespace openghost
