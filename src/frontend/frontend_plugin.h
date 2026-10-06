#pragma once
#include <QHash>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <QVector>
#include <functional>

// OpenGhost's frontend plugin SDK: native, in-process additions to the window's
// presentation (rich chat rendering, the browser panel, thinking display). Pi
// stays the backend and its plugin ecosystem; nothing here reaches a backend,
// a provider or a tool. QtCore only, like the rest of the frontend state.
namespace openghost
{
class FrontendPlugins;

// Who a plugin is. `id` is stable (it keys the saved on/off choice and the
// plugin's row decorations); name and description are shown in Settings.
struct FrontendPluginInfo {
    QString id, name, description;
    bool enabledByDefault = false; // Until the user turns it on or off.
    // Non-empty: changing this plugin from Settings requires an app restart.
    // The string explains why.
    QString restart = {};
    // Where Settings offers its on/off, in one place only: "" under Plugins;
    // "chat" under Appearance → Chat Settings (Plugins then leaves it out).
    QString placement = {};
    // Its own on/off settings and their defaults, by key. Saved like its
    // on/off and kept while it is off; the plugin reads them through its
    // context and hears of changes in optionChanged().
    QMap<QString, bool> options = {};
};

// Something that happened in the frontend. Plugins subscribe by name.
struct FrontendEvent {
    QString name;
    QString sessionId; // The open chat's session ("" for a new chat's draft).
    QVariantMap data;
};
namespace events
{
inline const QString ChatChanged = QStringLiteral("chat.changed");   // The open chat's rows or turn.
inline const QString SessionOpened = QStringLiteral("session.opened"); // Another chat was opened.
inline const QString MessageAccepted =
    QStringLiteral("message.accepted"); // data.submission: the composer's submission number.
inline const QString ReplyDelta = QStringLiteral("reply.delta"); // A reply's text grew.
} // namespace events

// One displayed transcript row, read-only, as the chat rendering hook sees it.
struct ChatRowView {
    QString sessionId, key;
    QString role; // "user", "assistant", "note", "tool" or "thinking"
    QString text, state;
};

// One text of a row the conversation-wide selection can take, in reading
// order: a path that stays the same while the row shows it, and the text
// exactly as shown (one position per line end).
struct SelectionText {
    QString path, text;
};

// How a plugin draws the transcript rows of one kind the host keeps but does
// not draw itself ("tool": a tool call; "thinking": a message's thinking). The
// host keeps the rows' data whether or not a renderer is registered; without
// one, it shows them as it always has (not at all).
struct RowRenderer {
    // The packaged QML component drawing a row (qrc:, never a backend's URL).
    // It is made with `row`: the row's ChatEntry (its model roles, column,
    // toggle(), dropped(), frontend, ListView view).
    QUrl delegate;
    // The row's selectable texts as the component shows them, from the row's
    // model roles by name, so a selection takes rows that are not built.
    // Called while selecting: no side effects.
    std::function<QVector<SelectionText>(const QVariantMap &row)> selection;
    // The path of the text whose front the host trims as live output streams
    // (its `trimmed` role counts the units), if any.
    QString trimmedText;
    // Rows arrive open rather than shut (the reader's toggles still win, until
    // the renderer is registered again starting them the other way).
    bool startExpanded = false;
};

// Everything a plugin registers goes through its context, so disabling or
// unregistering the plugin removes all of it. A context is valid from
// FrontendPlugin::enable() until FrontendPlugin::disable() returns; a plugin
// keeping its pointer must drop it in disable().
class FrontendPluginContext final
{
  public:
    using EventHandler = std::function<void(const FrontendEvent &)>;
    // Extra presentation data for a row, shown to QML as the row's
    // `decorations[pluginId]`. Return an empty map to leave the row alone.
    // Called during rendering: must not change plugin state or enable plugins.
    using RowDecorator = std::function<QVariantMap(const ChatRowView &)>;

    QString pluginId() const { return m_id; }
    // One handler per event name; subscribing again replaces it.
    void subscribe(const QString &event, EventHandler handler);
    void unsubscribe(const QString &event);
    // The chat rendering hook; an empty function removes it.
    void decorateRows(RowDecorator decorator);
    // Ask for the open chat's rows to be decorated again (the decorator's
    // output depends on state that changed).
    void redecorate();
    // The rows of `kind` are drawn by `renderer` while the plugin is on (one
    // plugin per kind: the first registered wins). An empty delegate removes it.
    void renderRows(const QString &kind, RowRenderer renderer);
    // Named UI targets (an action or a panel, e.g. "browser") this plugin
    // shows or hides. Hidden by any enabled plugin wins over shown.
    void setVisible(const QString &target, bool visible);
    void clearVisible(const QString &target);
    // The plugin's status() changed: republish its Settings entry.
    void update();
    // One of the plugin's options (FrontendPluginInfo::options): the user's
    // saved value, else its default; false for an undeclared key.
    bool option(const QString &key) const;

  private:
    friend class FrontendPlugins;
    FrontendPluginContext(FrontendPlugins *owner, QString id) : m_owner(owner), m_id(std::move(id))
    {
    }
    int hooks() const
    {
        return int(m_events.size() + m_visibility.size() + m_renderers.size()) +
               (m_decorator ? 1 : 0);
    }
    FrontendPlugins *m_owner;
    QString m_id;
    QHash<QString, EventHandler> m_events;
    RowDecorator m_decorator;
    QHash<QString, bool> m_visibility;
    QHash<QString, RowRenderer> m_renderers;
};

class FrontendPlugin
{
  public:
    virtual ~FrontendPlugin() = default;
    virtual FrontendPluginInfo info() const = 0;
    // Turned on: register hooks through `context`.
    virtual void enable(FrontendPluginContext &context) = 0;
    // Turned off or unregistered. Every hook is removed afterwards anyway;
    // release anything else the plugin holds (including the context pointer).
    virtual void disable() {}
    // A line under the plugin's name in Settings; "" for none.
    virtual QString status() const { return {}; }
    // One of its options changed (saved already) while it is on.
    virtual void optionChanged(const QString &key) { Q_UNUSED(key) }
};
} // namespace openghost
