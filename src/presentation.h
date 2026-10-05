#pragma once

// Display-only values extracted from Ghosty; no protocol, storage or agent logic.
#include <QJsonObject>
#include <QMetaType>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

struct ActivityItem {
    enum Kind { Thinking, Text, Call };
    Kind kind = Text;
    qint64 id = 0;        // Unique and increasing within its log: the panel's key.
    quint64 revision = 0; // Changes exactly when this step's content changes.
    qint64 turn = 0;      // The child's exchange; 0 when read from its saved run.
    QString call, name, arguments; // A call's ID, tool and compact JSON arguments.
    QString text;  // Thinking or text; a call's live output, then its result.
    QString state; // A call's: running, done or error.
    qsizetype clipped = 0; // Characters past the display bound, not shown.
};

// One file card: Ghosty's canonical attachment metadata, or a chosen file's
// name and size while its message is admitted. Never a local path.
struct Attachment {
    QString name;
    QString mime;     // Empty until Ghosty has classified the file.
    qint64 size = -1; // -1: not a file (the count of cards not shown).
    bool operator==(const Attachment &other) const
    {
        return name == other.name && mime == other.mime && size == other.size;
    }
};

// One bounded display row. Text is implicitly shared, so publishing a copy to
// the GUI thread does not duplicate unchanged rows.
struct Entry {
    enum Kind { User, Assistant, Thinking, Tool, Note };
    Kind kind = Note;
    QString key;       // Same for a live row and its canonical replacement.
    QString text;      // Message, thinking, note, or tool output/result.
    QString ending;    // Tool: how a Bash result ended ("exited 0"), then each
                       // failure or uncertainty it reports, one per line.
    QString preview;   // One bounded line for collapsed thinking/tool rows; a steering receipt.
    QString name;      // Tool name.
    QString arguments; // Compact JSON; empty when the call was not observed.
    QString state;     // Tool: running/done/error/missing/unconfirmed; thinking: live/done;
                       // assistant: live while its exchange streams;
                       // steering: sending/queued/applied/notApplied/unconfirmed/refused.
    QVector<Attachment> attachments; // User rows: file cards, in message order.
    bool argumentsKnown = true;
    bool copyable = false;           // Copy takes the run's full reply text (Transcript::copyText).
    qsizetype omittedLines = 0;      // Tool: complete live output lines dropped before text.
    qsizetype omittedCharacters = 0; // Tool: code points dropped from text's partial first line.
    qsizetype trimmed = 0;           // UTF-16 units dropped from text's front while live.
    // A run's last reply row: its reply metrics ("%1" where the elapsed time
    // goes; preview holds their tooltip) and the run's saved start and end
    // (ms since the epoch; -1 unknown).
    QString metrics;
    qint64 started = -1, completed = -1;
    // A subagent card's steps (ChildActivity), their log's revision, and a
    // line about what the panel shows or lacks.
    QVector<ActivityItem> activity;
    quint64 activityRevision = 0;
    QString activityNote;
    // How the row follows the one before it. OpenGhost shows one model
    // exchange's thinking, text and tool cards as one message, spaced closer
    // than separate messages; the view chooses the spacing.
    enum Join { Apart, Joined, AfterThinking };
    Join join = Apart;
    quint64 revision = 0; // Changes exactly when displayed content changes.
};


struct Selection {
    QString provider, model, thinking;
    bool invalid = false;
};
Q_DECLARE_METATYPE(Selection)

// One saved session in Ghosty's list; times are Unix milliseconds.
struct Session {
    QString id, title, folder;
    qint64 updated = 0;
    qint64 created = 0;
};

// One catalog model, levels in Ghosty's canonical order.
struct ModelInfo {
    QString provider, id, name;
    QStringList levels;
    bool available = false;
    bool imageInput = false;
};

// Settings and provider presentation. Ghosty owns credentials, the catalog and
// saved defaults; this holds only its latest answers. A login answer is
// never held here.
struct Account {
    QVector<ModelInfo> models;
    QString catalogError;
    Selection defaults; // Ghosty's effective defaults; empty fields when unset.
    // Rows for the provider page: id, name, hint, connected, disconnectable,
    // oauth, apiKey, note, error.
    QVariantList providers;
    bool providersLoaded = false;
    QString providersError;
    // The current login attempt's step (loginStart … prompt/select/info);
    // empty when none. Terminal steps become a provider note instead.
    QVariantMap login;
    QString notice; // Save Defaults outcome.
    bool noticeError = false;
    bool saving = false;
    quint64 revision = 0;
};


const QString &clippedNotice();
const QString &exhaustedNotice();
