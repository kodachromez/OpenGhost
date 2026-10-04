# OpenGhost-Frontend: agent guide

## Purpose and scope

This repository contains the OpenGhost desktop frontend: an Electron application with a plain JavaScript renderer. It presents conversations, prepares attachments, manages local UI state, and provides the built-in browser. It connects to a separately configured agent backend through the Agent Backend Protocol (ABP).

The frontend is **not** the agent runtime. Model requests, provider integrations, credential storage, model-facing history, prompt assembly, compaction, general tool execution, and approval policy belong to the backend. There is no bundled agent or fallback model. Without a backend, the UI still opens and reports backend unavailability instead of answering.

Use the implementation as the source of truth. Read [the ABP contract](docs/backend-interface.md) when changing the boundary and [the browser host-tool contract](docs/browser-host-tools.md) when changing browser behavior; verify details against the corresponding code and tests.

## Repository layout and runtime architecture

There is no `src/` tree, frontend framework, bundler, or transpilation step. Renderer sources live at the repository root. Most are classic-script IIFEs exposing `window.*` globals; small controls use custom elements. Electron files use CommonJS. `index.html` explicitly orders the renderer scripts.

```text
index.html + script.js + renderer modules
  -> window.Backend                       backend-client.js
  -> window.openghost.backend             desktop/preload.js
  -> backend:* IPC                        desktop/main.js
  -> BackendHost                          desktop/backend-host.js
  -> external executable                  newline-delimited JSON-RPC over stdio

backend approval.request / host.tool
  -> BackendClient reverse handlers
  -> Chat owner selected by sessionId
  -> ApprovalCard / BrowserPanel
  -> browser IPC -> desktop/browser.js -> Electron webview
```

| Location | Responsibility |
| --- | --- |
| `index.html` | App structure, CSP, script dependency order. |
| `script.js` | Composition root: constructs settings, library, chat, sidebar, composer, attachments, browser panel, and lock UI; wires keyboard and page lifecycle events. |
| `desktop.js` | Renderer platform classes and splash setup; **not** the Electron entry point. |
| `styles.css` | Shared layout, themes, component states, and animations. |
| `desktop/main.js` | Electron entry point, window lifecycle, IPC handlers, disk store, folder dialogs, theme integration, PDF/media services, and backend process startup/shutdown. |
| `desktop/preload.js` | Narrow `window.openghost` bridge. Keep Electron/Node APIs out of renderer modules. Backend subscriptions strip the IPC event and return unsubscribe functions. |
| `backend-protocol.js` | Shared JSON-RPC envelope validator, used in renderer and host. |
| `backend-client.js` | Handshake, request correlation, deadlines, cancellation, capabilities, event dispatch, reverse requests, and error presentation. |
| `desktop/backend-host.js` | Trusted executable configuration, child-process lifecycle, JSON-line framing, byte/queue limits, and process-tree cleanup. |
| `chat.js`, `library.js`, `chat-store.js` | Chat UI orchestration, local display projection/index, and storage adapter respectively. These are not backend history stores. |
| `desktop/browser.js`, `desktop/browser-preload.js`, `browser-panel.js`, `host-tools.js` | Browser host service, guest preload, panel UI/lifecycle, and advertised tool schemas/result formatting. |
| `test/` | Node regression tests, fake transports/DOMs, scripted subprocess fixtures, and real-Electron smoke tests. |
| `package.json`, `package-lock.json`, `.github/workflows/build.yml` | Launch/test/package commands, pinned dependencies, and build automation. `dist/` is output, not source. |

`desktop/main.js` loads the local `index.html` in a sandboxed, context-isolated window. Webviews have a separate guarded configuration and preload. Do not give web content the app preload, Node integration, or unrestricted IPC access. Preserve sender checks around backend, browser, PDF, and media IPC.

### Ownership at a glance

| Frontend owns | Backend owns |
| --- | --- |
| Chat index, folders, titles/pins, display messages, scroll/unread state, local annotations | Durable agent sessions, session incarnations, model-facing history, recovery journals |
| Composer input, attachment extraction/previews, standing-instruction/file settings | Provider-specific message conversion, prompt assembly, interpretation of user context |
| Model/provider/auth controls and cached catalog presentation | Model catalog/capabilities, auth operations, stored keys/tokens, provider networking |
| Approval cards and sending the user's decision | Whether approval is needed and what Ask/Auto/Full mean |
| Browser webviews, cookies, tab/control lifecycle, browser host-tool execution | Choosing browser actions and approving them before requesting execution |
| Local usage aggregation, charts, context meter presentation | Incremental usage/context reports and account limits/balances |
| Local display-cache password protection | Any protection or deletion of backend history and credentials |

## Backend boundary and process lifecycle

### Renderer client

`backend-client.js` creates `window.Backend` from the preload transport. It waits for DOM readiness before `initialize`, so `HostTools.schemas` and `RenderGuide` are available. The handshake advertises protocol version `0.1`, a fresh `connectionId`, client metadata, host tools, renderer syntax, and desktop local-path support.

- Use `Backend.request(method, params, { signal })`, `notify`, `on`, `handle`, and `can`; do not bypass the client for agent operations.
- `Backend.ready` settles with a boolean, including on failure. Await it and check `Backend.available`; resolution alone does not mean connected.
- Requests have connection-scoped IDs. Default timeout is 60 seconds; `auth.login` gets 15 minutes and `session.compact` 10 minutes. Abort/timeout sends `$/cancelRequest`; late replies cannot settle a different request.
- Notifications and reverse requests are dispatched only after successful initialization. Reverse requests have their own abort controllers and duplicate-ID protection.
- `BackendError` and `Backend.explain` preserve useful configuration/spawn/exit diagnostics and backend error/action metadata. Do not replace these with an undifferentiated connection error.
- Dispose the old client before replacing it. `dispose()` removes subscriptions and aborts/rejects outstanding work; it does not stop the process.

`backend-protocol.js` requires JSON-RPC 2.0 objects, named object parameters, and string or safe-integer IDs. Batches are unsupported; numeric and string IDs are distinct. Invalid matching responses reject their call with `protocol_error`, rather than dispatching mixed request/response envelopes.

### Desktop host

`desktop/backend-host.js` is a process/transport host, **not an RPC method dispatcher**. It must not learn provider APIs, agent tools, history, or model logic. It frames incoming JSON and lets the client validate envelopes; even a malformed envelope must reach the client when needed to reject a correlated request.

Configuration is read by `configured()`:

- A nonblank `OPENGHOST_BACKEND` overrides `<Electron userData>/backend.json`.
- A plain string is one whole executable path/name, **not a shell command line**. Arguments require an array, such as `OPENGHOST_BACKEND='["/absolute/path/backend","--flag"]'`.
- `backend.json` uses a `command` field containing an executable string or argv array. Blank/unset environment falls back to this file. Invalid nonblank configuration is an error, not permission to fall back silently.
- `spawn` uses `shell: false`, inherits the app's environment and user privileges, and starts in the user's home directory. A session's `cwd` is separate, sent in ABP. Relative executable paths are not resolved relative to the config file; prefer absolute paths.

The selected executable is trusted local code, not a sandboxed plugin. Stdout is protocol-only; stderr is logged. Lines are capped at 64 MiB of UTF-8 bytes, with a bounded outbound stdin queue and no retry queue for rejected sends.

**There is no automatic backend restart.** Configuration changes or a stopped/crashed backend require relaunching OpenGhost. Renderer reload can reconnect to the still-running process; it is not a process restart. On app quit the host sends `shutdown`, closes stdin, allows a two-second grace period, then terminates remaining work. Keep platform-specific descendant cleanup intact.

## Sessions, display caches, and chat lifecycle

### Identity and ownership

`Chat` manages `Conversation` objects, DOM nodes, live turns, and event routing. `Library` manages the local index and display cache. The backend owns actual session history.

- Main session ID: the local chat record's `id`.
- Mini session ID: `<chat id>:mini`. `SideChat` in `chat.js` shares chat mechanics but uses separate display storage and passes `side.parent`, `parentBusy`, and `moved`; it does not assemble the parent's model history.
- `sessionVersion` is the backend's opaque session incarnation. `null` means create-only for a new session; an existing string means require that exact incarnation, never upsert an empty replacement.
- `clientTurnId`, accepted `turnId`, `messageId`, `clientInputId`, and event `seq` have different jobs. Do not substitute one for another or assign ownership from the currently visible chat.

`displayMessages` in `library.js` is the allowlist for reading and writing display data. It retains user/assistant text, attachment previews, compact/stats/moved markers, usage, and required recovery IDs/flags. It discards provider-native messages, tool histories, arbitrary nested fields, and full attachment input payloads. Older display records can be opened without becoming backend history.

### Sending and streaming

1. `script.js` reads the composer and calls `Chat.send`. It clears composer/attachments only when the send is accepted locally. Locked, deleting, or recovering conversations reject sending; an unreconciled saved chat starts recovery first.
2. A draft gets a `Library` record only on its first message. New sessions start with `sessionVersion: null`. A configured backend with no usable selected model opens settings rather than silently choosing another model.
3. `run`/`startTurn` create a local turn and pending display marker, await attachment preparation and user context, and durably checkpoint before dispatch. `turn.start` carries only the new input plus configuration, workspace, user context, and browser snapshot—not cached history.
4. `turn.started` or the start acknowledgement binds the accepted remote turn. Early events wait for that identity in a bounded buffer. Reverse requests also wait for the correct turn identity before showing/running anything.
5. `message.started` allocates an assistant message buffer. Deltas append; completion text, when supplied, replaces that message's buffer. Interleaved messages retain their start order and original reply part. Accepted steering or compaction can split the reply into more parts.
6. `turn.completed` is terminal immediately. `end` cleans up approvals, browser jobs, pending steering, streaming views, display saves, and unread state. It does not wait indefinitely for missing message-final events.

Rendering goes through `StreamView` -> `Markdown`, then local `Diagram`, `Tex`, `Highlight`, `MediaEmbed`, and `MediaSlider` support. `render-guide.js` describes supported syntax, not agent behavior or a system prompt. Diagram edits and posted stats are frontend display annotations; do not send them back as reconstructed history.

**Normal tool calls are not rendered as tool cards.** `tool.started` can show the working indicator; `tool.completed` tracks completion; `tool.progress` has no rendered view. Approval cards are a separate interactive reverse-request path. **Reasoning is not rendered**; `reasoning.delta` is ignored.

### Stop, steering, retry, and compaction

- Main-chat Escape calls `Chat.stop`, unless a menu/dialog or another handler consumes it. Send remains Send; there is no Stop button. Mini-chat key/close handling is in `mini-chat.js`.
- Stop aborts locally at once, denies pending approvals, cancels browser steps/waits, and requests `turn.cancel` when the remote ID is known. Late text/tools/completion must not revive the turn.
- Inputs sent during a normal turn use serialized `turn.steer` calls in user input order, even if attachments finish in another order. RPC acceptance is not the placement event: `input.accepted` moves the bubble into the reply. Rejected/unconfirmed inputs stay visible with an error and are not automatically resent.
- Inputs during manual compaction or a model switch wait, then are combined in order into a new `turn.start` input. This is not ordinary steering.
- Retry before dispatch can reuse the original input/preparation context held in memory. After an uncertain dispatch it must reconcile first. An accepted failed turn is retried with `turn.retry` naming the exact `failedTurnId` and matching session incarnation. Respect `retryable: false`, `action: 'none'`, and invalid-request errors.
- Manual compaction requires `compaction.manual` and a recovered, idle chat with history. `session.compact` and model-switch `session.configure` carry a client operation identity for their compaction/usage events. The backend decides what to summarize and whether switching requires compaction.

## Persistence, recovery, locking, and deletion

### Storage map

`ChatStore` uses `window.openghost.store` on desktop and `localStorage` with the `openghost:` prefix otherwise. On desktop, keys map to `<Electron userData>/store/<key>.json`:

| Key/location | Contents and owner |
| --- | --- |
| `index` | `Library` folders, chat records, and home-group collapse state. |
| `chats/<id>` | Main-chat display messages and token count, optionally sealed. |
| `mini/<id>` | Mini-chat display messages, token count, and parent `seen` timestamp, optionally sealed. |
| `context`, `context/<file id>` | `UserContext` standing instructions/file metadata and extracted file payloads. |
| `usage` | `Usage` daily per-provider/model counters and model names. |
| Renderer `localStorage` | Model/effort/mode preferences, versioned catalog, theme, browser panel/account hints, approval-details preference, media metadata cache. See owning modules for exact keys. |
| `<Electron userData>/theme.json` | Main-process theme choice used before the page paints. |
| Electron `persist:browser` partition | Browser cookies/site state; separate from ABP provider authentication. |
| `~/OpenGhost/Chats/<space>` | Workspace path assigned to a chat without a selected project folder; **not** its display-cache location. |

Main-process store writes are serialized per file and use temporary-file rename; deletion waits for pending writes. `Library.queue` serializes per-chat operations, including mini-cache/lock operations. Ordinary display saves are best-effort; required recovery checkpoints must propagate failure and block backend dispatch. Page exit flushes library/context/usage; main-process quit waits for pending disk writes.

### Recovery rules

Read `Chat.reconcile`, `recoverTurn`, `checkpoint`, `onEvent`, and `end` together before changing recovery.

- Saved main/mini chats load their display cache, then call `session.get` before continuation. They require the `sessions.recovery` capability; new sends also require it.
- A durable `pendingTurn` marker and `backendTurn` client identity let recovery ask about an uncertain start, including a page dying before the start response.
- `session.get` returns `sessionVersion`, a revision high-water mark, and an optional turn display replay. Notifications racing the request are buffered; events at/below the snapshot boundary are not applied again.
- Replay rebuilds the affected turn, preserves other cached messages/annotations, and resumes the original accepted turn. It must not charge the global usage ledger a second time or replay browser actions.
- Session events require increasing nonnegative safe-integer `seq` values. Duplicates/backwards events are ignored; gaps are allowed, not repaired automatically. Replay uses its own sequence gate.
- Missing sessions, unsupported recovery, invalid snapshots, or a pending turn the backend never accepted leave the cache display-only with a recovery error. Never import the cache, silently create replacement history, or resend uncertain input.
- Correlated late usage may update retained accounting entries for the last 16 local turns. This exception does not allow old output to resume or roll back the current context meter.

### Local password protection

`chat-lock.js` supplies PBKDF2/AES-GCM; `Library` handles sealing titles and main/mini display caches; `lock-ui.js` supplies the controls/screens. Passwords are not stored; unlocked keys/titles live in memory. Leaving a protected chat relocks its view; an in-flight reply can finish saving before its cleartext state is dropped.

This protects the **local view/display cache only**. It does not encrypt backend history, credentials, workspace files, browser state, standing instructions, or usage. Preserve the crash-safe ordering in `protect`/`unprotect` and use the display projection on sealed as well as plain saves. Do not promise a backend-history wipe or a password-recovery mechanism.

### Deletion

`Chat.remove` requires backend availability and `sessions.delete`, aborts live work, and waits for deletion acknowledgements for both main and mini session IDs before removing local records. Preserve mini-ID collision checks and the guard against new work during deletion.

Folder deletion commits each acknowledged child separately; failed/unattempted children and the folder remain retryable. `ChatList.removeItem` serializes confirmations and restores UI on error. Removing a project grouping does not recursively delete the user's project. A home workspace is released only when empty and underneath the designated Chats directory.

Mini-chat Clear is a different path: `SideChat.clear` clears its local view/cache and requests backend deletion best-effort when supported. Do not assume it has the parent-chat deletion acknowledgement semantics.

## Model, provider, and authentication UI

Start with `settings.js`, then `model-stage.js`/`model-button.js`, `effort-slider.js`/`effort-button.js` and the `effort-*` visual helpers, and `mode-picker.js`.

- `auth.providers` supplies provider names, grouping, supported auth methods, status, and limit metadata. `models.list` supplies model IDs, names, context windows, vision, thinking levels, and defaults. Do not hardcode provider catalogs, key prefixes, auth URLs, model limits, or capability guesses.
- Model UI IDs are `provider:model`; ABP sends provider and model separately. An unavailable saved selection stays unavailable, rather than silently falling back. An unselected chat may use the first catalog model.
- Missing/empty thinking levels mean no supported effort choices. Missing context window stays unknown; missing vision is not advertised as supported. Backend defaults are display state, not automatically saved user preferences.
- Apply canonical model/provider/thinking/permission fields returned by `session.configure`, including cleared thinking and a model not currently in the catalog. Do not turn a canonical value into an advertised capability.
- Auth UI supports at most one `apiKey` and one `oauth` method per provider. Unsupported/duplicate methods are not additional executable login flows.
- API keys are transient input, sent via `auth.setKey`; an empty field sends `null` to remove the key. Successful saves clear the typed key. Saved secrets are not read back or stored in frontend preferences. Account actions use `auth.login`, `auth.cancel`, and `auth.logout`; the backend implements the actual flow.
- Preserve per-provider mutation tokens and last-request-wins catalog/provider refreshes. `auth.changed` invalidates and triggers authoritative rereads; a late event or request must not resurrect a logged-out account, restore an old key status, or erase a newer error.
- Provider IDs are opaque/untrusted strings. Use `Map` or null-prototype dictionaries and safe DOM assignment/escaping, not raw IDs in HTML/selectors or ordinary prototype-bearing lookup objects.

## Approvals

`Chat.onApproval` answers `approval.request`; `ApprovalCard.present` validates optional presentation fields and otherwise displays the tool name and JSON arguments. `approval-card.js` owns presentation, details expansion, Allow/Deny, and dismissal—not command analysis or permission policy.

- Match session, accepted turn, and approval identity before showing a card. Duplicate/stale requests must not run twice or attach to a new turn.
- A new user message supersedes pending approvals with `deny` / `reason: 'superseded'`. Stop/request cancellation denies them with cancellation semantics.
- Changing Ask/Auto/Full sends `session.configure` for running conversations. The backend re-evaluates pending approvals and sends `approval.resolved`; the UI must not independently auto-allow them.
- Render titles, commands, diffs, places, arguments, and backend errors as untrusted text. The expanded-details preference is local UI state, not a remembered authorization.
- A browser `host.tool` request does not trigger an independent frontend approval policy. The backend must ask first if its policy requires approval.

## Attachments and standing context

`attachments.js` owns picker/drop/paste behavior, up to 20 composer attachments, notes, previews, and long-paste cards (50 lines or 3,000 characters). `file-kinds.js` classifies files. `attachment-reader.js` prepares frontend payloads:

- Images are decoded and, when needed, resized/re-encoded to data URLs; the image-size policy lives in that module.
- Text extraction includes supported text encodings and Office/OpenDocument formats. General text/Office reading is bounded at 20 MB input and 400,000 extracted characters; spreadsheets have a row cap.
- `desktop/pdf.js`/`desktop/pdf.html` use a hidden Electron PDF viewer to extract selectable text. Reads are serialized and bounded; byte-only sources use temporary files that are cleaned up. This is not OCR, and password-protected/unreadable PDFs do not become readable text.
- Videos get metadata and a poster when decodable, plus a desktop local path when available. The backend receives path/metadata, not a frontend video-understanding pipeline. Unsupported files can still supply metadata/path.

`chat.js` `inputOf`/`attachmentOf` await each item's `ready` promise and map prepared data to ABP `Input`/`Attachment`. Paths come from `webUtils.getPathForFile` through preload, never an invented path. Prepared input and display attachments are different: `slim` in `chat.js` and `displayAttachments` in `library.js` retain previews/notes, not full text files or general file/video paths for later resend.

`settings-general.js` and `user-context.js` manage standing instructions and pinned files, reusing `AttachmentReader`. Current local limits are 8,000 instruction characters, 20 pinned files, and 200,000 extracted text characters across those files. These are storage/payload limits, **not token estimates or model context limits**. Re-adding a file replaces its saved copy; it is not continuously watched. `UserContext.forBackend()` supplies structured context each turn; the backend decides how to incorporate it into a prompt.

## Browser and other frontend-owned services

### Browser host tools

The built-in browser intentionally lives here because it operates on the user's Electron webviews and browser session.

- `browser-panel.js`: tabs, stable handles, lazy guest readiness/recreation, URL bar, layout/persistence, keyboard focus, operation queue, Take Control/Hand Back, and browser context reports.
- `desktop/browser.js`: guarded guest adoption, permissions, navigation, isolated-world DOM observations, CDP input/screenshots, deadlines, and operation-scoped download metadata.
- `desktop/browser-preload.js`: reports only a site hostname when a filled password form appears to be submitted. This is an unverified hint, not credentials or proof of login.
- `host-tools.js`: the 11 `browser_*` schemas advertised when a browser bridge exists, HTML-to-readable-text conversion, and ABP result formatting. `Chat.onHostTool` binds calls to their turn and coordinates cancellation/handoff.

Preserve these browser invariants:

1. Calls are serialized across chats. Pin the target tab at receipt, not after waiting in the queue. Use stable `tabId`, not its displayed number; do not silently retarget a closed/switched tab.
2. Input tools require the observed `pageId`. Stale refs/pages, covered/moved elements, navigation during delays, guest crashes, cancellation, and timeout must fail before dispatching further input. A timed-out CDP call completing later cannot resume the action sequence.
3. Take Control stops running/queued actions. After Hand Back, the interrupted request returns a fresh snapshot with `status: 'handed-back'`, not a replay of the old action. Turn completion/Stop releases every waiter and cancels remaining jobs.
4. `browser_read` continuation uses a frozen read identified by `readId`; offsets are UTF-16 code units, not bytes. Preserve `tabId`, `pageId`, errors, refs, coverage/truncation, screenshot, and download metadata in results.
5. `host.browser.changed` reports browser context changes. Empty/closed and unavailable are different states. `signedInVerified` remains false for submission hints.

### Intentional networking

**Do not describe this repository as having no networking.** Provider/model networking belongs to the external backend, but frontend-owned networking is intentional:

| Surface | Implementation |
| --- | --- |
| App renderer fetch/XHR/WebSocket-style connections | Blocked by `connect-src 'none'` in `index.html`; no provider client belongs here. |
| Images, galleries, video thumbnails | HTTP(S) images are allowed by CSP. `media-embed.js` auto-loads its trusted preview sources; other gallery images wait for a click. YouTube thumbnails use image requests. |
| Link icons | `link-chip.js` requests favicon images from DuckDuckGo's icon service. |
| YouTube card metadata | `desktop/main.js` `videoInfo` uses `net.fetch` against YouTube oEmbed after validating an 11-character video ID. It is not a general URL-fetch IPC service. |
| Built-in webviews | Browse sites, search, use cookies, load local pages, and download files through their separate browser session. |
| External links | Main-process navigation/window-open handling delegates allowed HTTP(S)/mailto links to the OS browser/application. |

Preserve the distinction between the app renderer's CSP and browser guests' networking. Do not remove browser, media, or icon services as if they were provider adapters, and do not weaken CSP to add a provider client.

## Settings, usage, and presentation

- `settings.js` owns the settings dialog, provider/auth UI, catalog, and model/effort/mode preferences.
- `settings-general.js`/`user-context.js` own standing instructions and pinned files.
- `settings-appearance.js`/`theme.js` own light/dark/system presentation, coordinated with `nativeTheme` and title-bar colors in `desktop/main.js`.
- `usage.js` records backend usage increments into daily provider/model buckets: input, cached input, cache written, output, and request count. It is a local ledger, not a provider billing source. Replay skips `Usage.record` while rebuilding per-message usage.
- `settings-usage.js` draws totals/charts with a generic provider palette and obtains connected account limits/balances via `account.limits` only when `usage.limits` is advertised. Polling runs while the Usage page is visible.
- `Chat.stats` and `stats-card.js` build local conversation/mini-chat accounting cards. Old uncounted replies remain uncounted; do not estimate missing backend token data. Context fullness uses backend context reports and known catalog windows, not character-based guesses.
- `i18n.js` is the UI string catalog; keep named keys valid. Preserve focus behavior, keyboard access, and reduced-motion paths when editing controls/animations.

## Files to inspect for common tasks

| Task | Start here |
| --- | --- |
| Add/change an ABP method or error | `backend-client.js`, `backend-protocol.js`, caller in `chat.js`/`settings.js`, `docs/backend-interface.md`, scripted backend fixture. Do not dispatch methods in the host. |
| Backend startup, command config, crash, or shutdown | `desktop/backend-host.js`, `desktop/main.js`, `desktop/preload.js`, `backend-client.js`. |
| Send, Stop, Retry, steering, stream ordering | `chat.js`: `send`, `begin`, `startTurn`, `resume`, `steer`, `onEvent`, `applyEvent`, `end`; composer wiring in `script.js`. |
| Reload/resume, storage, cache compatibility | `chat.js`: `reconcile`/`recoverTurn`/`checkpoint`; `library.js` display projection/queues; `chat-store.js`; store handlers in `desktop/main.js`. |
| Sidebar, folders, renaming, deleting | `chat-list.js`, `library.js`, `Chat.remove`/`removeFolder`, `folder-pill.js`. |
| Mini-chat or selected-text question | `mini-chat.js`, `SideChat` in `chat.js`, `selection-menu.js`, `selection-focus.js`. |
| Markdown/stream rendering | `stream-view.js`, `markdown.js`, `highlight.js`, `tex.js`, `diagram.js`, `render-guide.js`, `styles.css`. |
| Media, links, attachments | `media-embed.js`, `media-slider.js`, `link-chip.js`, `attachments.js`, `attachment-reader.js`, `file-kinds.js`, `desktop/pdf.js`. |
| Model picker or effort | `settings.js`, `model-stage.js`, `model-button.js`, `effort-slider.js`, `effort-*`, `Chat.configure`/`switchModel`. |
| Provider/auth or account usage | `settings.js`, `settings-usage.js`, `usage.js`, `backend-client.js`. |
| Approval UX or permission mode | `approval-card.js`, `Chat.claim`/`onApproval`/`onModeChange`, `mode-picker.js`. |
| Browser behavior/host tools | `browser-panel.js`, `host-tools.js`, `Chat.onHostTool`, `desktop/browser.js`, preload/main IPC. |
| Password protection | `chat-lock.js`, `library.js`, `lock-ui.js`, `Chat.seal`/`unlock`. |
| Layout, composer, small controls | `index.html`, `styles.css`, `script.js`, `composer-text.js`, `dock.js`, `add-menu.js`, the relevant `*-button.js`; scrolling helpers for resize/follow issues. |
| Packaging | `package.json`, `package-lock.json`, `.github/workflows/build.yml`, `desktop/icon.*`; never hand-edit built output. |

## Testing guidance

### Commands

Run from the repository root. Node 22 is used by build automation; the standalone E2E scripts use built-in `fetch` and `WebSocket`. `npm ci` installs the locked Electron/build dependencies when setup is needed. There are no runtime npm dependencies.

```sh
npm test                                  # node --test test/*.test.js
node --test test/session-recovery.test.js  # example focused regression
npm run test:e2e                           # real Electron smoke, scripted backend + no-backend cases
node test/e2e/browser-lifecycle.mjs         # separate real-webview regression; not included above
npm start                                 # interactive Electron app; uses your actual profile/config
```

The Node suite does not require a provider account or an installed agent backend. `test/helpers.js` loads classic renderer scripts in VM contexts and supplies fake transports; many tests use minimal DOM/Electron stubs. `test/fixtures/scripted-backend.js` is a deterministic ABP peer, not a production backend. `test/fixtures/tree-backend.js` exercises child-process cleanup. Linux process-tree tests inspect `/proc` and skip elsewhere; a passing mocked test is not a substitute for platform-specific validation.

The E2E scripts launch Electron headlessly with isolated HOME/config directories and drive it over CDP. The smoke test covers startup without a backend, catalog/auth presentation, streaming, approvals, browser tools, usage, Stop, compaction, display saves, and crash reporting. The browser script uses local data/file pages for real guest/input lifecycle coverage. `OPENGHOST_E2E_APP=/absolute/path/to/executable npm run test:e2e` targets an already packaged app. Do not use real credentials or the normal user profile as test fixtures.

### Regression map

For code changes, run the relevant focused tests while iterating and `npm test` before handing off. Add real-Electron checks when changing DOM/IPC/webview interactions that stubs cannot validate.

| Change area | Relevant tests in `test/` |
| --- | --- |
| Boundary/script loading/CSP/dependencies | `boundary.test.js` |
| RPC envelopes, errors, request/reverse cancellation | `backend-client.test.js`, `backend-envelope.test.js`, `backend-lifecycle.test.js` |
| Process configuration/framing/backpressure/shutdown | `backend-config.test.js`, `backend-host.test.js`, `backend-transport.test.js` |
| Session recovery/checkpoints/retry/steering | `session-recovery.test.js`, `retry-steering.test.js` |
| Correlation/sequence/terminal behavior | `event-ordering.test.js`, `turn-identity.test.js`, `cancellation.test.js` |
| Display allowlist, saved records, local locks | `legacy-display.test.js`, `lock-ui.test.js`, `session-recovery.test.js` |
| Chat/folder deletion | `deletion.test.js` |
| Model metadata/canonical configuration | `model-capabilities.test.js` |
| Dynamic providers/auth races/opaque IDs | `provider-auth.test.js` |
| Browser targeting/readiness/input/read continuation | `browser-lifecycle.test.js`, `cancellation.test.js`, `turn-identity.test.js`, plus `e2e/browser-lifecycle.mjs` |
| Untrusted text in UI | `untrusted-text.test.js`, `provider-auth.test.js` |
| Usage/pinned-file metadata and local limits | `usage.test.js`, `settings-presentation.test.js` |

For visual changes, also inspect streaming and restored messages, long content, main and mini chats, keyboard focus/Escape, scrolling, light/dark/system themes, and reduced motion. For attachment changes, exercise image/text/PDF/video/unreadable inputs, delayed preparation, and reload previews. For lifecycle changes, explicitly test unavailable/crashed backend, late events after Stop, pending approvals, reload during a turn, and failed storage/deletion acknowledgements.

For documentation-only work, local path/link review and `git diff --check` are sufficient; do not run the app, E2E suite, or packaging just to change prose. No lint/typecheck script is currently defined. Packaging commands are `npm run dist` (Windows), `dist:mac`, and `dist:linux`; packaging is not a test requirement for ordinary documentation/UI changes. Docs and tests are excluded from packaged app files.

## Important invariants and common mistakes to avoid

- **Do not move agent behavior into the UI.** No provider SDKs/endpoints, shell/general tool runner, prompt policy, or model-history assembly here. Browser host tools and renderer syntax metadata are legitimate frontend services.
- **Do not replay display caches as input.** They deliberately omit full attachments and agent history. Keep required checkpoints ahead of dispatch; never swallow their failure as an ordinary display-save error.
- **Do not guess identity or success.** Preserve session incarnations, turn/message/input IDs, sequence gates, bounded early buffering, and reverse-request deduplication. A timeout is not proof that a start or tool action never ran.
- **Do not repair uncertainty by resending.** Recovery precedes continuation/retry; unconfirmed steering remains unconfirmed. Stop and terminal events freeze output even if late callbacks arrive.
- **Do not add tool cards or reasoning output implicitly.** Working status and approval cards are the implemented UX, not a general tool transcript.
- **Do not infer provider capabilities or credentials.** Keep absent model metadata unknown and unavailable selections intact. Backend auth state is authoritative; frontend/browser login hints are not interchangeable.
- **Do not treat external text as markup.** Preserve escaping/text assignment for errors, provider metadata, URLs, approvals, file names, and site titles, plus Markdown URL restrictions and CSP.
- **Do not equate local locks with backend encryption/deletion.** Keep the security scope explicit in labels and behavior.
- **Do not drop a chat locally before backend deletion succeeds.** Parent deletion, folder deletion, and mini Clear have distinct implementations; inspect the exact path.
- **Do not break classic script ordering or globals.** Update `index.html` deliberately when adding a module. There is no module bundler to resolve dependencies for you.
- **Do not collapse browser and renderer trust boundaries.** Keep sandbox/context isolation, guest guards, stable target identities, cancellation checks after awaits, and intentional non-provider networking.
- **Do not add restart/retry loops around process failure.** Show diagnostics and require relaunch for process recovery; renderer session reconciliation is a separate operation.

## Current intentional limits

Treat these as current behavior, not missing features to invent while doing unrelated work:

- One configured local stdio backend process; no built-in backend, remote HTTP/WebSocket transport, automatic process restart, or offline agent fallback. Browser-only loading is a UI/storage fallback, not a functional agent connection.
- ABP version `0.1`, object parameters, no JSON-RPC batches. Session continuation depends on backend recovery support and durable identity/journals. The frontend does not automatically import old display caches or repair arbitrary live-stream sequence gaps.
- No reasoning view, normal tool cards, or persisted full tool transcript. Approval cards are transient decision UI. Local token history is limited to usage the frontend actually recorded, not a billing reconciliation service.
- Browser automation is bounded and heuristic, not a general browser-testing engine: 12 tabs without eviction; snapshots have partial DOM/visibility coverage; cross-origin iframe content is not readable. `browser_read` excludes form controls, iframe content, and shadow roots, may include hidden text, caps source HTML at 4 Mi UTF-16 code units, and returns 40,000-character text slices. Full-page screenshots are capped at four screens.
- Attachment extraction is best-effort and size-bounded. No PDF OCR, remote file upload service, frontend video interpretation, or guarantee a backend can access a supplied local path. Pinned files are saved snapshots, not live file synchronization.
- Local password protection covers selected display caches/titles only. It is not a sandbox for the configured executable or protection for the backend's data.
