#pragma once

// OpenGhost display-only values; no protocol, storage or agent logic.
#include <QMetaType>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

// One file card: OpenGhost's attachment metadata, or a chosen file's
// name and size while its message is admitted. Never a local path.
struct Attachment {
    QString name;
    QString mime;     // Empty until OpenGhost has classified the file.
    qint64 size = -1; // -1: not a file (the count of cards not shown).
    bool operator==(const Attachment &other) const
    {
        return name == other.name && mime == other.mime && size == other.size;
    }
};

// One bounded display row. Text is implicitly shared, so publishing a copy to
// the GUI thread does not duplicate unchanged rows.
struct Entry {
    enum Kind { User, Assistant, Note };
    Kind kind = Note;
    QString key;       // Same for a live row and its canonical replacement.
    QString text;      // Message or notice.
    QString preview;   // A steering receipt or reply-metrics tooltip.
    QString state;     // Assistant: live while its exchange streams;
                       // steering: sending/queued/applied/notApplied/unconfirmed/refused.
    QVector<Attachment> attachments; // User rows: file cards, in message order.
    bool copyable = false;           // Copy takes the run's full reply text.
    // A run's last reply row: its reply metrics ("%1" where the elapsed time
    // goes; preview holds their tooltip) and the run's saved start and end
    // (ms since the epoch; -1 unknown).
    QString metrics;
    qint64 started = -1, completed = -1;
    // How the row follows the one before it: separate messages or parts of
    // one reply. The view chooses the spacing.
    enum Join { Apart, Joined };
    Join join = Apart;
    quint64 revision = 0; // Changes exactly when displayed content changes.
};


struct Selection {
    QString provider, model, thinking;
    bool invalid = false;
};
Q_DECLARE_METATYPE(Selection)

// One saved session in OpenGhost's list; times are Unix milliseconds.
struct Session {
    QString id, title, folder;
    qint64 updated = 0;
    qint64 created = 0;
};

// One catalog model, levels in OpenGhost's advertised order.
struct ModelInfo {
    QString provider, id, name;
    QStringList levels;
    bool available = false;
    bool imageInput = false;
    QString defaultThinking; // Only used when advertised among levels.
};

// Settings and provider presentation. The backend owns credentials/catalog;
// model/effort preferences belong to the frontend, not a defaults RPC. A login
// answer is never held here.
struct Account {
    QVector<ModelInfo> models;
    QString catalogError;
    Selection defaults; // Frontend model/effort preference; empty fields when unset.
    // Rows for the provider page: id, name, hint, connected, logout,
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
