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

Every registered plugin gets one Settings entry (name, description, `status()`
and an On/Off toggle) on the Plugins page. The page appears for the backend's
runtime plugins or when a frontend plugin is registered.

With no plugin enabled, rows carry empty `decorations`, events reach no one and
`visibility` is empty: the window behaves as without the SDK.

## Example plugin

`OPENGHOST_EXAMPLE_PLUGIN=1` registers `openghost.example` (off by default). It
counts `chat.changed` events in its Settings status, decorates every row with
`{"seen": true}` and shows the target `example`. Nothing reads the last two, so
it changes nothing in the chat.

## Left for later

- No QML component in the transcript reads `decorations` yet (RichChat will),
  and nothing reads `visibility` yet (Browser will).
- One handler per event per plugin; no ordering or priority between plugins.
- Built-in, compiled-in plugins only; no out-of-tree discovery or ABI.
- Under Qt's software renderer, a Settings tab icon created while Settings is
  open crashes the render thread (seen when the Plugins tab is added to an open
  sheet); plugins register at startup, so the smoke test registers before
  opening Settings.
