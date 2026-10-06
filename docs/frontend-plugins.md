# Frontend plugins

A deliberately small, native (C++/Qt, QtCore-only) SDK for in-process additions
to the window's presentation: RichChat, Browser and Thinking are the planned
users. It is not a backend plugin system: Pi stays the backend and its own
plugin ecosystem, and the backend's runtime plugins (`src/frontend/plugins.*`,
Settings → Plugins rows from `plugin.list`) are unrelated to this.

## Files

- `src/frontend/frontend_plugin.h`: the public SDK (`FrontendPlugin`,
  `FrontendPluginInfo`, `FrontendPluginContext`, `FrontendEvent`, `ChatRowView`,
  `events::*`).
- `src/frontend/frontend_plugins.{h,cpp}`: `FrontendPlugins`, the registry the
  window owns, and `registerBuiltinPlugins()`.
- `src/frontend/example_plugin.{h,cpp}`: the example plugin.
- `src/frontend/permissions_plugin.{h,cpp}`: the Permissions switch (below).
- `plugins/tool_calls/`: the Tool Calls plugin (below).

## Lifecycle

`main.cpp` calls `registerBuiltinPlugins()` once the window controller exists;
there is no dynamic loading. Each plugin starts from the saved choice
(`preferences.json` → `"plugins": {"<id>": true|false}`, written only once a
choice exists), else `FrontendPluginInfo::enabledByDefault`.

Turning a plugin on or off from Settings saves the choice first; if saving
fails, nothing changes and the window shows the store's error. Then it applies
at once: `enable(context)` registers hooks, and `disable()` runs before the
manager drops the context, removing every hook the plugin registered through it.
`FrontendPlugins::remove()` does the same and keeps the saved choice.

## Hooks (all through `FrontendPluginContext`)

| Hook | Effect |
| --- | --- |
| `subscribe(event, handler)` / `unsubscribe(event)` | `chat.changed`, `session.opened`, `message.accepted`, `reply.delta` |
| `decorateRows(fn)` / `redecorate()` | Per transcript row, a map shown to QML as the row's `decorations[pluginId]` |
| `setVisible(target, bool)` / `clearVisible(target)` | `frontendPlugins.visibility[target]`; a hide wins over a show |
| `update()` | Republishes the plugin's `status()` in its Settings entry |
| `renderRows(kind, RowRenderer)` | Draws the transcript rows of `kind` (`"tool"` today; `"thinking"` later): a packaged QML `delegate` made with `row` (its `ChatEntry`), a `selection` function giving the row's selectable texts from its roles, the `trimmedText` path the host trims while output streams, and `startExpanded`. One plugin per kind (the first registered); an empty delegate removes it |

Row renderers are the one hook that changes what the transcript draws. The
host keeps the rows' data whether or not a renderer is registered, so turning
a plugin on draws earlier rows too, and turning it off redraws the open chat
at once (`renderersChanged`): rows are not reset or resent, rows shown again
do not replay their entry motion, the reader's opened rows stay opened (by
key, until another chat opens), and the selection is cleared. Without a
`"tool"` renderer the window draws no tool rows and joins a reply's parts
into one message, exactly as before tool rows existed.

Every registered plugin gets one Settings entry (name, description, `status()`
and an On/Off toggle) on the Plugins page. The page appears for the backend's
runtime plugins or when a frontend plugin is registered. Plugins may be
registered and unregistered while Settings is open: the Plugins tab is created
with the others and only hidden, because Qt 6.11's software Shape node reads its
item from the render thread and a tab icon deleted mid-frame crashes it.

With no plugin enabled, rows carry empty `decorations`, events reach no one and
`visibility` is empty: the window behaves as without the SDK.

## Permissions (`openghost.plugin-permissions`)

Always built in and registered by `main.cpp`; **on by default**, shown in
Settings → Plugins as *Permissions*. It is the switch for Pi's permission plugin
([plugin-permissions](plugin-permissions.md)) and its UI, and registers no
hooks: the window reads whether it is on (`WindowController::permissions`, true
when it is not registered) and tells the backend (`Backend::setPermissionsEnabled`).
Off, the Ask / Auto / Full picker and its dock, the approval card's decisions
and their `y`/`s`/`b`/`n`/`r` shortcuts are gone, a mode pick or card decision
is ignored, a waiting request is declined and withdrawn, and chats' Pi start
without plugin-permissions. On again, all of it returns.

Its `FrontendPluginInfo::restart` is set, so Settings shows *Requires restart*
beside its toggle and changes it only through a confirmed restart
(`WindowController::restartWithPlugin`, then `restartRequested`, on which
`main.cpp` quits and starts OpenGhost again). Any plugin can set it.


Built in (`OPENGHOST_TOOL_CALLS`, default ON) and registered by `main.cpp`;
**on by default**, shown in Settings → Plugins as *Tool Calls* with an On/Off
toggle saved in `preferences.json` like any frontend plugin. Its only hook is
`renderRows("tool", …)`: `plugins/tool_calls/qml/ToolEntry.qml` (Ghosty's
500 ms tool entrance, reduced motion honoured) around `ToolCard.qml` (Ghosty's
ordinary card: head, argument wells and diff, output well with its own scroll
bar and follow-the-end, ending well, disclosure), with `toolcard::units` as its
selection. Formatting (`toolcard.*`) is QtCore; `ToolText` exposes it to QML.

Its data comes from the host, not from the plugin: `PiBackend` maps Pi's
finished assistant message's `toolCall` blocks (in message order; never partial
arguments), `tool_execution_start/update/end` (updates are Pi's snapshots of
the whole output so far) and the saved `toolResult` messages to
`ToolStarted`/`ToolProgress`/`ToolCompleted`; `ChatService` keeps a
`DisplayRow::Role::Tool` row per call after the message that made it (a reply
continues below in a new part), bounds live output to its newest 32 Ki units
and results to their first 32 Ki, and saves the cards in the display cache.
States are `running`, `done`, `error`, `missing` (rebuilt from Pi's session
without a saved result), `unconfirmed` (the turn ended or stopped, or the app
exited, before a result) and `cancelled` (not produced by Pi today). An
outcome Pi did not report is never shown as done. A call waiting on an
approval shows *Waiting for approval* (the approval's `toolCallId`).

## Example plugin

`OPENGHOST_EXAMPLE_PLUGIN=1` registers `openghost.example` (off by default). It
counts `chat.changed` events in its Settings status, decorates every row with
`{"seen": true}` and shows the target `example`. Nothing reads the last two, so
it changes nothing in the chat.

## Left for later

- No QML component in the transcript reads `decorations` yet, and nothing
  reads `visibility` yet (Browser will).
- Thinking is not drawn yet: `ReasoningDelta` stays invisible. A Thinking
  plugin would register `renderRows("thinking", …)` (with `startExpanded` for
  a "Start collapsed" choice) once the host keeps thinking rows; plugin-owned
  options beyond On/Off do not exist yet.
- One handler per event per plugin; no ordering or priority between plugins.
- Built-in, compiled-in plugins only; no out-of-tree discovery or ABI.
