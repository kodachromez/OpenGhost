#pragma once

#include "toolcard.h"

#include <QObject>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// toolcard.h for the card's QML (ToolCard.qml): the same strings the
// selection copies. From Ghosty's ToolText (see NOTICE.md).
class ToolText final : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

  public:
    using QObject::QObject;
    Q_INVOKABLE QVariantMap describe(const QString &name, const QString &json, bool known) const;
    Q_INVOKABLE QStringList endLines(const QString &state, const QString &ending) const
    {
        return toolcard::endLines(state, ending);
    }
    Q_INVOKABLE QString liveNote(const QString &state, bool hasOutput) const
    {
        return toolcard::liveNote(state, hasOutput);
    }
    Q_INVOKABLE QString omission(double lines, double characters) const
    {
        return toolcard::omission(lines, characters);
    }
    Q_INVOKABLE QString plain(const QString &text) const { return toolcard::plain(text); }
};

// Registers OpenGhost.ToolCalls (ToolText) with QML; once, before the window loads.
void registerToolCallsTypes();
