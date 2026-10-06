#pragma once

#include "frontend/frontend_plugin.h"

#include <QStringList>
#include <QVector>

// What a tool call's card shows, from the transcript row's fields alone
// (tool-card.js describe() and its body rows): the card (ToolCard.qml) lays
// these texts out, and the conversation's selection copies them, from the
// same strings, whether or not the card is built. Adapted from Ghosty's
// native-ghosty/src/toolcard.{h,cpp} (see NOTICE.md); QtCore only.
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

// The card's selectable texts in reading order (all of them in its body:
// OpenGhost's head and diff signs are user-select: none). A shut card has
// none. Paths start "t".
struct Row {
    QString name, arguments, state, body, ending;
    bool known = false, expanded = false;
    double omittedLines = 0, omittedCharacters = 0;
};
QVector<openghost::SelectionText> units(const Row &row);
// A plain text as its document holds it: one position per line end.
QString plain(QString text);
} // namespace toolcard
