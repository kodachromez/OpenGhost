#pragma once

#include "presentation.h"
#include "rich.h"

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// What a tool call's card shows, from the transcript's fields alone
// (tool-card.js describe() and its body rows): the card (ToolCard.qml) lays
// these texts out, and the conversation's selection (selection.h) copies
// them, from the same strings, whether or not the card is built.
namespace toolcard
{
struct Lines {
    QStringList shown; // At most 14.
    int more = 0;
};
struct Info {
    QString kind, title, path, code, text, url, removed, added;
    QString summary; // The collapsed head's one line, at most 160 units.
    QString link;    // The URL's host (no "www.") and the rest, as shown.
    Lines removedLines, addedLines;
};
// What the model asked for. `json` is the model's untrusted arguments: only
// strings are shown, nothing is interpreted.
Info describe(const QString &name, const QString &json, bool known);
// How the call ended, as the end well's rows.
QStringList endLines(const QString &state, const QString &ending);
// The quiet note under live or partial output, if any.
QString liveNote(const QString &state, bool hasOutput);
// Lines and characters a live tail dropped from the output's front.
QString omission(double lines, double characters);

// A subagent card's steps as its activity panel shows them, one map each:
// id, kind (thinking, text or call), head (a call's tool and arguments;
// "Thinking"), text (with how much was cut), state (a call's) and revision.
QVariantList steps(const QVector<ActivityItem> &items);

// The card's selectable texts in reading order (all of them in its body:
// OpenGhost's head and diff signs are user-select: none). A shut card has
// none. Paths start "t".
struct Row {
    QString name, arguments, state, body, ending;
    bool known = false, expanded = false;
    double omittedLines = 0, omittedCharacters = 0;
    QVariantList steps; // A subagent's (steps()), shown under its prompt.
    QString note;       // The activity panel's line.
};
QVector<SelectionUnit> units(const Row &row);
// A plain text as its document holds it: one position per line end.
QString plain(QString text);
} // namespace toolcard

// The same for QML (ToolCard.qml, ChatEntry.qml).
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
