#include "model.h"

namespace
{
constexpr int SessionBlocks = 32;

QString kindName(Entry::Kind kind)
{
    static const char *const kinds[] = {"user", "assistant", "note", "tool"};
    return QString::fromLatin1(kinds[kind]);
}

QList<int> changedRoles(const Entry &a, const Entry &b)
{
    QList<int> roles;
    if (a.kind != b.kind)
        roles << TranscriptModel::KindRole;
    if (a.text != b.text)
        roles << TranscriptModel::BodyRole;
    if (a.preview != b.preview)
        roles << TranscriptModel::PreviewRole;
    if (a.state != b.state)
        roles << TranscriptModel::MessageStateRole;
    if (a.copyable != b.copyable)
        roles << TranscriptModel::CopyableRole;
    if (a.attachments != b.attachments)
        roles << TranscriptModel::AttachmentsRole;
    if (a.metrics != b.metrics)
        roles << TranscriptModel::MetricsRole;
    if (a.started != b.started)
        roles << TranscriptModel::StartedRole;
    if (a.completed != b.completed)
        roles << TranscriptModel::CompletedRole;
    if (a.join != b.join)
        roles << TranscriptModel::JoinRole;
    if (a.decorations != b.decorations)
        roles << TranscriptModel::DecorationsRole;
    if (a.tool.id != b.tool.id)
        roles << TranscriptModel::ToolCallRole;
    if (a.tool.name != b.tool.name)
        roles << TranscriptModel::ToolNameRole;
    if (a.tool.arguments != b.tool.arguments)
        roles << TranscriptModel::ArgumentsRole;
    if (a.tool.known != b.tool.known)
        roles << TranscriptModel::ArgumentsKnownRole;
    if (a.tool.ending != b.tool.ending)
        roles << TranscriptModel::EndingRole;
    if (a.tool.omittedLines != b.tool.omittedLines)
        roles << TranscriptModel::OmittedLinesRole;
    if (a.tool.omittedCharacters != b.tool.omittedCharacters)
        roles << TranscriptModel::OmittedCharactersRole;
    if (a.tool.trimmed != b.tool.trimmed)
        roles << TranscriptModel::TrimmedRole;
    if (a.revision != b.revision)
        roles << TranscriptModel::RevisionRole;
    return roles;
}
} // namespace

QVector<bool> KeyedModel::inOrder(const QVector<int> &to, int size)
{
    // Patience sorting: tails[k] ends the best increasing run of length k + 1.
    QVector<int> tails, previous(to.size(), -1);
    for (int i = 0; i < to.size(); ++i) {
        if (to.at(i) < 0)
            continue;
        const auto k = std::lower_bound(tails.cbegin(), tails.cend(), to.at(i),
                                        [&to](int j, int t) { return to.at(j) < t; }) -
                       tails.cbegin();
        if (k > 0)
            previous[i] = tails.at(k - 1);
        if (k == tails.size())
            tails.append(i);
        else
            tails[k] = i;
    }
    QVector<bool> stays(size, false);
    for (int i = tails.isEmpty() ? -1 : tails.last(); i >= 0; i = previous.at(i))
        stays[to.at(i)] = true;
    return stays;
}

int TranscriptModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant TranscriptModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Entry &entry = m_rows.at(index.row());
    switch (role) {
    case KindRole:
        return kindName(entry.kind);
    case ToolCallRole:
        return entry.tool.id;
    case ToolNameRole:
        return entry.tool.name;
    case ArgumentsRole:
        return entry.tool.arguments;
    case ArgumentsKnownRole:
        return entry.tool.known;
    case EndingRole:
        return entry.tool.ending;
    case OmittedLinesRole:
        return entry.tool.omittedLines;
    case OmittedCharactersRole:
        return entry.tool.omittedCharacters;
    case TrimmedRole:
        return entry.tool.trimmed;
    case ExpandedRole:
        return expanded(entry);
    case RevisionRole:
        return double(entry.revision);
    case TrimmedTextRole:
        return m_renderers.value(kindName(entry.kind)).trimmedText;
    case KeyRole:
        return entry.key;
    case BodyRole:
        return entry.text;
    case PreviewRole:
        return entry.preview;
    case MessageStateRole:
        return entry.state;
    case CopyableRole:
        return entry.copyable;
    case MetricsRole:
        return entry.metrics;
    case StartedRole:
        return double(entry.started);
    case CompletedRole:
        return double(entry.completed);
    case JoinRole:
        return int(entry.join);
    case DecorationsRole:
        return entry.decorations;
    case AttachmentsRole: {
        QVariantList cards;
        for (const auto &card : entry.attachments)
            cards << QVariantMap{{"name", card.name}, {"mime", card.mime}, {"size", card.size}};
        return cards;
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> TranscriptModel::roleNames() const
{
    return {{KindRole, "kind"},                 {KeyRole, "key"},
            {BodyRole, "body"},                 {PreviewRole, "preview"},
            {MessageStateRole, "messageState"}, {CopyableRole, "copyable"},
            {AttachmentsRole, "attachments"},   {MetricsRole, "metrics"},
            {StartedRole, "started"},           {CompletedRole, "completed"},
            {JoinRole, "join"},                 {DecorationsRole, "decorations"},
            {ToolCallRole, "toolCallId"},       {ToolNameRole, "toolName"},
            {ArgumentsRole, "arguments"},       {ArgumentsKnownRole, "argumentsKnown"},
            {EndingRole, "ending"},             {OmittedLinesRole, "omittedLines"},
            {OmittedCharactersRole, "omittedCharacters"},
            {TrimmedRole, "trimmed"},           {ExpandedRole, "expanded"},
            {RevisionRole, "revision"},         {TrimmedTextRole, "trimmedText"}};
}

bool TranscriptModel::expanded(const Entry &entry) const
{
    return m_toggled.contains(entry.key) != m_renderers.value(kindName(entry.kind)).startExpanded;
}

void TranscriptModel::toggle(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    const QString &key = m_rows.at(row).key;
    if (!m_toggled.remove(key))
        m_toggled.insert(key);
    emit dataChanged(index(row), index(row), {ExpandedRole});
}

void TranscriptModel::setRenderers(QHash<QString, Renderer> renderers)
{
    m_renderers = std::move(renderers);
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(int(m_rows.size()) - 1),
                         {ExpandedRole, TrimmedTextRole});
}

QVariantMap TranscriptModel::fields(const Entry &entry) const
{
    const auto names = roleNames();
    QVariantMap row;
    const int at = int(&entry - m_rows.constData());
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        row.insert(QString::fromUtf8(it.value()), data(index(at), it.key()));
    return row;
}

QVariantList TranscriptModel::rowTexts(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    const auto &entry = m_rows.at(row);
    const auto renderer = m_renderers.constFind(kindName(entry.kind));
    if (renderer == m_renderers.cend() || !renderer->selection)
        return {};
    QVariantList texts;
    for (const auto &[path, text] : renderer->selection(fields(entry)))
        texts.append(QVariantMap{{QStringLiteral("path"), path}, {QStringLiteral("text"), text}});
    return texts;
}

void TranscriptModel::apply(const QVector<Entry> &rows, bool redrawn)
{
    if (redrawn) {
        QSet<QString> shown;
        for (const auto &entry : std::as_const(m_rows))
            shown.insert(entry.key);
        for (const auto &entry : rows)
            if (!shown.contains(entry.key))
                m_quiet.insert(entry.key);
    }
    diff(
        m_rows, rows, [](const Entry &entry) -> const QString & { return entry.key; },
        [this](int i, Entry &old, const Entry &next) {
            if (old.revision == next.revision)
                return;
            const auto roles = changedRoles(old, next);
            old = next;
            if (!roles.isEmpty())
                emit dataChanged(index(i), index(i), roles);
        });
}

void TranscriptModel::reset(const QVector<Entry> &rows)
{
    beginResetModel();
    m_rows = rows;
    m_edits.clear();
    m_toggled.clear();
    m_quiet.clear();
    endResetModel();
}

QVariant TranscriptModel::edit(const QString &key) const
{
    const auto found = m_edits.constFind(key);
    return found == m_edits.constEnd() ? QVariant() : QVariant(*found);
}

void TranscriptModel::setEdit(const QString &key, const QVariant &source)
{
    if (source.typeId() == QMetaType::QString)
        m_edits.insert(key, source.toString());
    else
        m_edits.remove(key);
}

int TranscriptModel::indexOf(const QString &key) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).key == key)
            return i;
    }
    return -1;
}

int SessionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant SessionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Session &session = m_rows.at(index.row());
    switch (role) {
    case IdRole:
        return session.id;
    case TitleRole:
        return session.title;
    case FolderRole:
        return session.folder;
    case FolderNameRole: {
        if (session.folder.isEmpty())
            return QStringLiteral("Chats");
        const auto name =
            session.folder.section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty);
        return name.isEmpty() ? session.folder : name;
    }
    case UpdatedRole:
        return double(session.updated);
    case PinnedRole:
        return m_pinned.contains(session.id);
    case GroupRole:
        return m_pinned.contains(session.id) ? QStringLiteral("pinned:")
               : session.folder.isEmpty()     ? QStringLiteral("home:")
                                              : session.folder;
    default:
        return {};
    }
}

QHash<int, QByteArray> SessionModel::roleNames() const
{
    return {{IdRole, "sessionId"},    {TitleRole, "title"},
            {FolderRole, "folder"},   {FolderNameRole, "folderName"},
            {UpdatedRole, "updated"}, {PinnedRole, "pinned"},
            {GroupRole, "group"}};
}

QString SessionModel::folderAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).folder : QString();
}

QString SessionModel::folderOf(const QString &id) const
{
    for (const Session &session : m_source) {
        if (session.id == id)
            return session.folder.isNull() ? QStringLiteral("") : session.folder;
    }
    return {};
}

void SessionModel::setEmptyFolders(const QStringList &folders, const QStringList &order)
{
    if (folders == m_emptyFolders && order == m_order)
        return;
    m_emptyFolders = folders;
    m_order = order;
    arrange();
    emit groupsChanged();
}

// Places each empty folder before the next folder shown after it in m_order.
void SessionModel::arrange()
{
    const QSet<QString> empty(m_emptyFolders.cbegin(), m_emptyFolders.cend());
    const QSet<QString> shown(m_folders.cbegin(), m_folders.cend());
    m_emptyBefore.clear();
    m_arranged.clear();
    QStringList waiting;
    for (const QString &folder : std::as_const(m_order)) {
        if (empty.contains(folder)) {
            waiting << folder;
        } else if (shown.contains(folder)) {
            if (!waiting.isEmpty())
                m_emptyBefore.insert(folder, std::exchange(waiting, {}));
            m_arranged << m_emptyBefore.value(folder) << folder;
        }
    }
    // Shown folders the order has not met yet keep the list's order.
    for (const QString &folder : std::as_const(m_folders)) {
        if (!m_arranged.contains(folder))
            m_arranged << folder;
    }
    if (!waiting.isEmpty())
        m_emptyBefore.insert(QString(), waiting);
    m_arranged << waiting;
}

void SessionModel::togglePin(const QString &id)
{
    if (!m_pinned.remove(id))
        m_pinned.insert(id);
    place();
    emit pinToggled(id, m_pinned.contains(id));
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).id == id)
            emit dataChanged(index(i), index(i), {PinnedRole, GroupRole});
    }
}

void SessionModel::setQuery(const QString &query)
{
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
}

void SessionModel::apply(const QVector<Session> &sessions)
{
    m_source = sessions;
    for (const auto &session : sessions)
        if (session.pinned)
            m_pinned.insert(session.id);
        else
            m_pinned.remove(session.id);
    place();
}

void SessionModel::place()
{
    QVector<Session> next, rest;
    next.reserve(m_source.size());
    for (const auto &session : std::as_const(m_source))
        (m_pinned.contains(session.id) ? next : rest).append(session);
    std::stable_sort(next.begin(), next.end(),
                     [](const Session &a, const Session &b) { return a.updated > b.updated; });
    const int pinned = int(next.size());
    int home = 0;
    QStringList folders;
    for (const auto &session : std::as_const(rest)) {
        if (session.folder.isEmpty())
            ++home;
        else if (folders.isEmpty() || folders.last() != session.folder)
            folders.append(session.folder);
    }
    next += rest;
    // A search or a long refresh resets rather than changing row by row.
    diff(
        m_rows, next, [](const Session &session) -> const QString & { return session.id; },
        [this](int i, Session &old, const Session &now) {
            QList<int> roles;
            if (old.title != now.title)
                roles << TitleRole;
            if (old.folder != now.folder)
                roles << FolderRole << FolderNameRole << GroupRole;
            if (old.updated != now.updated)
                roles << UpdatedRole;
            old = now;
            if (!roles.isEmpty())
                emit dataChanged(index(i), index(i), roles);
        },
        SessionBlocks);
    if (pinned != m_pinnedCount || home != m_homeCount || folders != m_folders) {
        m_pinnedCount = pinned;
        m_homeCount = home;
        m_folders = folders;
        arrange();
        emit groupsChanged();
    }
}

int PluginModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant PluginModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case IdRole:
        return row.id;
    case NameRole:
        return row.name;
    case DescriptionRole:
        return row.description;
    case StatusRole:
        return row.status;
    case EnabledRole:
        return row.enabled;
    case AvailableRole:
        return row.available;
    case PendingRole:
        return row.pending;
    case CanToggleRole:
        return row.canToggle;
    case NoteRole:
        return row.note;
    case NoteErrorRole:
        return row.noteError;
    }
    return {};
}

QHash<int, QByteArray> PluginModel::roleNames() const
{
    return {
        {IdRole, "pluginId"},        {NameRole, "name"},           {DescriptionRole, "description"},
        {StatusRole, "status"},      {EnabledRole, "enabled"},     {AvailableRole, "available"},
        {PendingRole, "pending"},    {CanToggleRole, "canToggle"}, {NoteRole, "note"},
        {NoteErrorRole, "noteError"}};
}

PluginModel::Row PluginModel::format(const openghost::PluginEntry &entry)
{
    const auto &p = entry.snapshot;
    QString state =
        p.state == "enabled"     ? QStringLiteral("Enabled")
        : p.state == "disabled"  ? QStringLiteral("Disabled")
        : p.state != "disabling" ? p.state // Future states render as reported.
        : p.activeCalls == 1 ? QStringLiteral("Disabling — waiting for 1 running call to finish")
        : p.activeCalls > 1  ? QStringLiteral("Disabling — waiting for %1 running calls to finish")
                                   .arg(p.activeCalls)
                             : QStringLiteral("Disabling — shutting down…");
    if (!p.available)
        state = QStringLiteral("Unavailable / not configured") +
                (state.isEmpty() ? QString() : QStringLiteral(" · ") + state);
    if (!entry.confirmed())
        state += QStringLiteral(" · State not confirmed");
    if (entry.pending)
        state += QStringLiteral(" · Request pending…");
    // Only the last answer's own persistence outcome; "saved" needs no note.
    QStringList notes;
    if (entry.warning.size())
        notes << entry.warning;
    if (entry.persisted == "memory")
        notes << QStringLiteral("Applies until the backend restarts: it has no saved plugin "
                                "state.");
    else if (entry.persisted == "ambiguous")
        notes << QStringLiteral("Applied, but saving was not confirmed: it may not survive a "
                                "restart.");
    return {p.id,
            p.name.isEmpty() ? p.id : p.name,
            p.description,
            state,
            entry.error.isEmpty() ? notes.join(QLatin1Char('\n')) : entry.error,
            p.enabled,
            p.available,
            entry.pending,
            entry.canToggle(),
            !entry.error.isEmpty(),
            entry.revision};
}

void PluginModel::update(int i, Row &old, const Row &now)
{
    if (old.revision == now.revision)
        return;
    QList<int> roles;
    const auto changed = [&](auto field, int role) {
        if (old.*field != now.*field)
            roles << role;
    };
    changed(&Row::name, NameRole);
    changed(&Row::description, DescriptionRole);
    changed(&Row::status, StatusRole);
    changed(&Row::enabled, EnabledRole);
    changed(&Row::available, AvailableRole);
    changed(&Row::pending, PendingRole);
    changed(&Row::canToggle, CanToggleRole);
    changed(&Row::note, NoteRole);
    changed(&Row::noteError, NoteErrorRole);
    old = now;
    if (!roles.isEmpty())
        emit dataChanged(index(i), index(i), roles);
}

void PluginModel::sync(const QVector<openghost::PluginEntry> &entries)
{
    const auto sameKeys = [&] {
        for (int i = 0; i < m_rows.size(); ++i)
            if (m_rows.at(i).id != entries.at(i).snapshot.id)
                return false;
        return true;
    };
    if (entries.size() == m_rows.size() && sameKeys()) {
        // The usual update (one row's pending flip, event or answer): in place.
        for (int i = 0; i < m_rows.size(); ++i)
            if (m_rows.at(i).revision != entries.at(i).revision)
                update(i, m_rows[i], format(entries.at(i)));
        return;
    }
    QHash<QString, int> at;
    at.reserve(m_rows.size());
    for (int i = 0; i < m_rows.size(); ++i)
        at.insert(m_rows.at(i).id, i);
    QVector<Row> next;
    next.reserve(entries.size());
    for (const auto &entry : entries) {
        const int i = at.value(entry.snapshot.id, -1);
        next.append(i >= 0 && m_rows.at(i).revision == entry.revision ? m_rows.at(i)
                                                                      : format(entry));
    }
    diff(
        m_rows, next, [](const Row &row) -> const QString & { return row.id; },
        [this](int i, Row &old, const Row &now) { update(i, old, now); });
}
