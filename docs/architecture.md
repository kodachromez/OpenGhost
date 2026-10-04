# OpenGhost-Frontend architecture

This document describes the current frontend implementation, not a proposed redesign.
OpenGhost is an Electron frontend connected to a separately configured, trusted local backend process. The
frontend implements the client side of Agent Backend Protocol (ABP), protocol version `0.1`. It does not implement
model/provider execution or an agent loop.

**The frontend's saved conversation is a display cache, not the backend's authoritative session history.**
Backend durability, execution and credential storage are external responsibilities. Where this document describes
what a backend must do, that is a compatibility requirement of this frontend, not an implementation supplied here.
See [Backend interface](backend-interface.md) for method shapes and [Browser host-tool lifecycle](browser-host-tools.md)
for the detailed browser contract.

## 1. High-level architecture

```text
Electron app
  Renderer: index.html + script.js + UI modules
    | window.Backend                         ^ events / reverse requests
    v                                        |
  backend-client.js: renderer-side JSON-RPC peer
    | window.openghost.backend
  desktop/preload.js: context-isolated bridge
    | Electron IPC
  desktop/main.js: IPC routing and application lifecycle
    | BackendHost
  desktop/backend-host.js: child process and JSON Lines framing
    | stdin                                  ^ stdout
    +-------------------+--------------------+
                        |
               External backend process
               sessions, agent execution, providers, credentials
```

There is one application window and one configured backend child per app instance. Several main/mini-chat sessions
can have work in progress; the shared renderer client routes by session identity. Browser guests and PDF helper
windows are separate Electron web contents, not backend processes.

| Responsibility | Owner in the current architecture |
| --- | --- |
| Chat list, folders, pins, user renames, selected chat, composer and display | Frontend |
| Rendered replies, attachment previews, diagrams, stats cards, local usage ledger | Frontend projections/annotations |
| Accepted input, model-facing transcript, active turns, compaction and recovery journal | Backend-authoritative |
| Provider/model catalog, connection status, account limits, saved credentials | Backend-authoritative; frontend presents/caches selected fields |
| Model, thinking and permission-mode preferences | Frontend selections sent to backend; `session.configure` responses can canonicalize them |
| Agent tool execution and approval policy | Backend; browser operations are delegated to the frontend host |
| Browser tabs, cookies/site storage, user control, page observations and downloads | Frontend/Electron |
| Standing user instructions and attached context files | Frontend storage; backend decides how to use the supplied context |
| Backend executable selection and process management | Electron main/host; not renderer-controlled |

## 2. Renderer

[`index.html`](../index.html) loads local JavaScript files in order; there is no application bundler or UI framework
in the runtime entry path. Modules mostly publish classes/services on `window`, alongside custom elements.
[`script.js`](../script.js) constructs settings, `Library`, `Chat`, the chat list, composer, attachment UI and,
when the desktop bridge exists, the browser panel and permission-mode picker.

Key renderer responsibilities:

- [`chat.js`](../chat.js): `Chat`, `Conversation` and `SideChat`; local turn/display state, event correlation,
  recovery, steering, approvals and browser reverse-request dispatch. Registered chat instances are searched
  newest-first for the conversation owning a `sessionId`.
- [`library.js`](../library.js) and [`chat-list.js`](../chat-list.js): local index, display persistence,
  folder organization, rename/pin/lock/delete UI. Creating a frontend chat assigns its session ID.
- [`stream-view.js`](../stream-view.js), [`markdown.js`](../markdown.js), diagrams and media components: stream
  presentation and rich output. The renderer groups backend messages into reply parts; it is not constructing
  a model transcript. Reasoning deltas and tool-progress payloads have no dedicated rendered transcript/cards.
- Settings and model controls: backend catalog presentation and user preferences, not provider integrations.
- [`render-guide.js`](../render-guide.js): describes the renderer's supported output formats to the backend.
  It is sent during initialization; the backend decides how to incorporate it.

Opening the UI without the desktop backend bridge does not provide an alternate agent transport. `Backend` is
unavailable and requests fail visibly. `ChatStore` has a localStorage fallback, and some UI remains usable,
but there is no HTTP/WebSocket backend connection path.

## 3. Preload bridge and Electron main process

[`desktop/preload.js`](../desktop/preload.js) exposes a fixed `window.openghost` API through `contextBridge`.
The renderer does not receive `ipcRenderer`, Node filesystem/process APIs, or a command-spawning API.

The bridge includes:

- `backend.send`, `onMessage`, `onStatus`, `status`;
- JSON store read/write/remove; folder picker, reveal, chat-folder location and empty-folder release;
- theme/title-bar updates and platform information;
- `pathOf(File)` and PDF text extraction;
- browser run/cancel/events/visibility;
- a narrow YouTube video-metadata lookup.

Backend message/status subscriptions return unsubscribe functions. The browser event subscription currently does
not expose an unsubscribe function; its normal lifetime is the page.

[`desktop/main.js`](../desktop/main.js) creates the sandboxed, context-isolated app window, installs IPC handlers,
loads the local HTML, owns storage writes and native dialogs, and connects `BackendHost` callbacks to the current
window's web contents. It also hosts browser automation through [`desktop/browser.js`](../desktop/browser.js) and
PDF reading through [`desktop/pdf.js`](../desktop/pdf.js).

The main process routes backend envelopes; it does not dispatch ABP methods or retain session/turn history. Backend
IPC uses `backend:send`, `backend:message` and `backend:status`. `backend:send` is fire-and-forget: no IPC response
reports whether the host accepted the write.

## 4. Backend client and JSON-RPC / JSON Lines transport

[`backend-client.js`](../backend-client.js) is the bidirectional JSON-RPC peer. It owns:

- a fresh `connectionId`, outgoing request counter and pending-request map;
- initialization state, negotiated backend information and capabilities;
- method/event listeners and reverse-request handlers;
- RPC error normalization, request timeouts and cancellation controllers.

When process status becomes `running`, the client waits for page scripts to load and sends `initialize` with
`protocolVersion: '0.1'`, `connectionId`, client metadata, browser tool schemas, the render guide and
`host.attachments.localPaths`. Only an initialize result with that exact version makes it ready. Ordinary
requests fail before readiness; ordinary notifications are dispatched only while ready.

The actively checked capabilities are `auth.providers`, `sessions.recovery`, `sessions.delete`,
`compaction.manual` and `usage.limits`. Stored capability fields are not all enforced as UI gates; in particular,
there is no general capability-driven hiding of every turn, approval, attachment or thinking control.

[`backend-protocol.js`](../backend-protocol.js) validates envelopes on the renderer receive path and host send path:

- JSON-RPC `2.0`, one object, no batches; named object parameters when present;
- requests have string/safe-integer IDs, notifications omit IDs;
- responses have an ID and exactly one of `result` or `error`; ID type is significant;
- a malformed correlated response rejects its pending call as `protocol_error`;
- malformed notifications and unknown/late responses are ignored; identifiable malformed reverse requests
  receive RPC errors without running handlers;
- duplicate reverse RPC IDs still being answered are rejected rather than executed twice.

Outgoing renderer IDs are `<connectionId>:<counter>`. Requests time out after 60 seconds by default, 15 minutes
for `auth.login`, and 10 minutes for `session.compact`. Abort or timeout removes the pending call, sends
`$/cancelRequest { id }`, and ignores a late answer. Receiving that notification aborts the matching reverse
handler's signal. A cancelled reverse request can still be answered; cancellation is not an acknowledgement of
rollback. `BackendClient.dispose()` detaches subscriptions, fails outstanding calls and aborts incoming work
without stopping the backend child.

[`desktop/backend-host.js`](../desktop/backend-host.js) handles process launch and transport, not RPC semantics:

```text
renderer object -> IPC -> validate/JSON.stringify -> child stdin: JSON + LF
renderer peer   <- IPC <- parse complete JSON line <- child stdout
                                      stderr -> app log, outside protocol
```

Input chunks are assembled as bytes before UTF-8 decoding, so chunk boundaries need not match lines or characters.
Blank lines are skipped; invalid JSON is logged/dropped. Parsed values, including malformed envelopes, reach the
renderer for validation. Lines over 64 MiB (excluding LF, including CR/whitespace) are discarded through the next
newline. Outbound lines have the same bound; Node's pending stdin queue is capped at 64 MiB + 1 byte including
framing. Rejected sends are not queued for retry. Accepted writes drain in order, but the renderer cannot observe
host rejection directly and may only see its request time out. There is no durable transport queue or event replay
in the host.

## 5. Sessions, display-cache ownership and persistence

The frontend assigns a main chat's ID and uses it as `sessionId`; its mini chat uses `<chat id>:mini`.
`Library` owns the index and display records. The backend owns whether those IDs actually exist as sessions,
their opaque `sessionVersion` incarnations, accepted turn identities and execution state.

Display records are allowlisted by the internal `displayMessages` function in `library.js`: visible user/assistant
content, attachment previews, compaction markers, stats/moved markers, usage, and recovery identifiers such as
`backendTurn`, `pendingTurn` and `clientInputId`. They are never sent back as model history. Local diagram edits and stats cards are frontend-only. User renames override later backend title
suggestions locally; there is no `session.rename` call.

The runtime keeps the current session version and sequence high-water mark in memory; they are obtained again
from `session.get` after reload. A cached chat's existence is not proof that its backend session exists.
There is no `session.list` import, automatic display-cache import, or `session.create` call.

### Persistence locations

| Data | Storage/authority |
| --- | --- |
| Chat/folder index | Frontend `store/index.json` under Electron `userData` |
| Main/mini display cache and pending checkpoints | Frontend `store/chats/<id>.json`, `store/mini/<id>.json` |
| Standing instructions/file metadata and prepared payloads | Frontend `store/context.json`, `store/context/<id>.json` |
| Daily per-provider/model observed usage | Frontend `store/usage.json` |
| Model/catalog/effort/mode preferences, appearance and browser UI state, approval-detail preference | Renderer localStorage |
| Native theme choice and backend command configuration | `theme.json` and `backend.json` under `userData` |
| Browser cookies and site storage | Electron's persistent `persist:browser` session partition |
| Backend sessions, recovery journals, credentials and provider state | External backend; paths/formats are not selected by this frontend |
| Project/output files | User-selected working folder, or a per-chat path under `~/OpenGhost/Chats/` |

[`chat-store.js`](../chat-store.js) uses the desktop store when present; its browser fallback uses localStorage keys
prefixed `openghost:`. Desktop store keys are restricted to one or two lowercase alphanumeric/hyphen path segments.
Writes are serialized per file, written to a temporary file and renamed. This is not a multi-file transaction or an
explicit `fsync` guarantee. Ordinary display saves are best-effort; pre-dispatch checkpoints propagate failures.
`pagehide` flushes pending index, user-context and usage writes; app quit waits for writes already registered in main.

Chat passwords derive an AES-GCM key using PBKDF2 in [`chat-lock.js`](../chat-lock.js). Protected titles and main/mini
display bodies are sealed; passwords are not stored and open keys/titles live in renderer memory. This does **not**
encrypt backend sessions, user-context files, the global usage ledger, browser storage or project files. Index
metadata remains outside the sealed title/body. Protection changes span several writes, not an all-or-nothing
transaction; mini-cache resealing is best-effort. Locking a view is not a backend access-control or cancellation API.

A turn's `cwd` is protocol data, not the backend process's launch directory. The frontend chooses the project or
per-chat path; a per-chat directory may not exist yet, so the backend must create it if needed. Removing a chat only
releases its own directory when empty; generated/user files are not recursively erased.

Full chat deletion waits for `session.delete` acknowledgements for both main and mini IDs before removing local
records. Offline/unsupported deletion fails visibly. Folder deletion commits children one at a time and keeps
failed/unattempted records. **Mini-chat Clear is different:** it clears local display state and resets the version
to create-only immediately; backend deletion is attempted only when advertised, without awaiting success.

## 6. Starting turns, reload and recovery

```text
Send -> create/select frontend chat -> prepare full input
     -> await index + display checkpoint
     -> turn.start(sessionId, sessionVersion, clientTurnId, input, settings/context)
     <- accepted turnId (+ sessionVersion for start)
     <- ordered display events ... turn.completed
     -> save frontend display

Renderer reload (backend process stays alive)
     -> fresh connectionId + initialize
Open saved chat -> restore display -> session.get(sessionId, pending clientTurnId?)
     <- sessionVersion + revision + recoverable turn/display journal
     -> rebuild that turn -> apply buffered events newer than revision
     -> allow continuation only after reconciliation
```

Before start/retry/steer dispatch, the renderer awaits index and display checkpoints. Storage failure prevents the
request. New main chats and unused mini chats use `sessionVersion: null` as an explicit create-only precondition.
Existing-session operations use the recovered version; the backend must reject a missing/conflicting incarnation
rather than silently start empty history. `sessions.recovery` is required for turns.

Opening a saved conversation, including reopening after unlock when its data must be loaded, calls `session.get`.
If a pending checkpoint exists, its client turn ID identifies the exact uncertain turn, including one completed
while the page was absent. Otherwise recovery asks for the active turn, if any. A successful snapshot contains
`sessionVersion`, atomic high-water `revision`, and optionally the turn's original display input and ordered events.
The renderer buffers live notifications during the request, rebuilds the identified turn, and then accepts only
buffered events newer than the snapshot. Other cached turns and frontend annotations are left in place.

This requires backend behavior the host cannot enforce:

- accepted input and display journals survive before acceptance/events are exposed;
- `session.get` atomically subscribes the new connection and establishes the replay/live boundary;
- accepted starts/retries are deduplicated by session/client turn ID; steering by session/turn/client input ID;
- completed journals remain available for pending checkpoints, and session state survives process restart;
- a new `initialize` replaces the renderer connection without discarding sessions; old reverse RPCs/replies are
  invalidated, renderer-dependent work waits for session subscription, and approvals are reissued with fresh RPC IDs;
- a browser action with an unknown old outcome is reconciled/cancelled, not automatically executed again.

Missing sessions, unsupported recovery, invalid snapshots or a pending turn not known to the backend leave the chat
preserved but unable to continue. Recovery Retry runs `session.get`; it does not import the cache or resend input.
A rejected Send leaves the composer intact.

Main-chat Retry distinguishes three cases: a never-dispatched start can reuse the full input/preparation context
still in memory; an uncertain dispatch must reconcile; an accepted terminal failed turn uses `turn.retry` with
`failedTurnId`. A slim display attachment is never reconstructed as model input. Errors can suppress Retry with
`retryable: false` or `action: 'none'`.

Recovery is not process reconnection. A crashed/misconfigured backend requires fixing configuration and relaunching
the app. Neither Retry nor a page reload starts another child.

## 7. Event ordering and correlation

There are several independent identities; RPC response correlation alone does not establish event ownership.

| Identity | Purpose |
| --- | --- |
| `connectionId` + RPC ID | Renderer request/reply lifetime; protects against old responses |
| `sessionId`, `sessionVersion` | Frontend-selected name and backend-authoritative incarnation guard |
| Session `seq`, recovery `revision` | Ordered event admission and snapshot/live boundary |
| `clientTurnId`, backend `turnId` | Pre-dispatch checkpoint and accepted turn identity |
| `messageId`, `toolCallId`, `approvalId`, `clientInputId` | Message buffers, tool/reverse-request identity, approval and steering correlation |

`chat.js` admits each nonnegative safe-integer session sequence once; missing/invalid, duplicate and backwards
sequences are ignored. Gaps are allowed, with **no general gap detection/repair**. Global auth/catalog/log events
are not session-sequenced. Recovery replay has its own sequence gate, separate from the live snapshot boundary.

`turn.started` can establish an early backend turn identity only for the exact current `clientTurnId`; the start
response must agree. Up to 256 early events can wait for identity. Message deltas/completion and usage may identify
a known message without `turnId`; unknown/conflicting identities are not assigned to whichever turn is current.
Standalone compaction/model-switch events correlate through their `clientTurnId`, not a fabricated backend turn.

Each assistant message has its own buffer and original reply part, preserving start order under interleaved output.
Deltas before a known message start are ignored. Completion optionally replaces the text and seals that message;
duplicate starts/completions cannot reopen it. `turn.completed` is terminal immediately: later text/tool events do
not revive output, even if a message never finalized. The backend must send authoritative final text first.

Usage is an exception to terminal output rejection: up to 16 recent local turns retain accounting references so
correlated late usage can update their original entries without reviving output or changing the current context
meter. Replay rebuilds per-entry totals without charging [`usage.js`](../usage.js)'s global ledger again. That ledger
counts observed live usage locally; it is not a complete backend/provider billing history and does not backfill
missed usage from recovery. Session titles are separately session-scoped.

## 8. Cancellation, Stop and steering

Stop in the main chat is Escape when not consumed by a menu, dialog, field or focused browser guest. Send remains
Send; there is no turn Stop button. A mini chat handles dialog Escape itself: stop if busy, otherwise close.

```text
Stop -> local turn AbortController -> reject further output
                                  -> abort steering / deny approvals / cancel browser work
     -> turn.cancel(sessionId, turnId) when a remote identity is known
Outstanding aborted RPC -> $/cancelRequest(RPC id)
```

`turn.cancel` and `$/cancelRequest` have different scopes: one cancels accepted turn execution, the other a specific
RPC. The UI does not wait for `turn.cancel` to freeze output and ignores cancellation-request errors. Stop before an
acceptance response can therefore leave an uncertain backend outcome; it must be reconciled, not treated as proof
that nothing ran. Aborted preparation cannot dispatch a start once it reaches the signal check. Attachment/PDF
preparation itself is not universally cancellable by Stop.

Sending during a running turn prepares and dispatches `turn.steer` serially in input order. Each input has a saved
`clientInputId`. The RPC acknowledges acceptance, but `input.accepted` places the bubble into the reply and begins
its next part. Rejected/unconfirmed input remains visible with an error, never silently resent. Turn completion,
error or Stop aborts steering RPCs and their interruptible preparation/start waiters. Messages queued during manual
compaction/model switching are instead combined into a new start after that operation.

Sending also denies pending approvals with `reason: 'superseded'` and releases browser hand-back waits with reason
`message`. Stopping/ending a turn cancels its running browser steps and outstanding hand-back waits. Already-issued
browser input/JavaScript cannot be rolled back; cancellation prevents later dispatches, not all partial effects.

## 9. Provider, model and authentication flow

[`settings.js`](../settings.js) obtains providers via `auth.providers` and models via `models.list`. There are no
built-in provider/model catalogs used as authority. Provider methods are validated and rendered as at most one
`apiKey` row and one `oauth` row per provider; unsupported kinds are ignored.

- Typed keys are transient renderer input, sent after a debounce via `auth.setKey`; an empty field sends `null`.
  A successfully saved key is cleared from the field. Saved secrets are not requested back, only `keySaved` status.
- Sign-in, cancel and logout call `auth.login`, `auth.cancel` and `auth.logout`. Credential verification, OAuth
  mechanics and durable credential storage belong to the backend, not the browser-panel login hints.
- `auth.changed` is unversioned: the renderer treats it as invalidation and rereads `auth.providers`, including
  again after a concurrent local mutation settles. Local freshness tokens prevent stale UI updates; they do not
  order backend credential side effects. `models.changed` refreshes providers and models.
- The last model catalog is cached locally for presentation. An unavailable selection is retained rather than
  silently switching providers/models. Thinking levels/defaults, vision support and context windows come from
  backend metadata; absent capabilities are not inferred.
- Model changes with history use `session.configure` and apply returned canonical fields; errors restore the old
  selection. Draft/no-history selections are local until sent. A permission-mode change during an active main-chat
  turn configures the active sessions handled by that `Chat` instance. Ask/Auto/Full policy is backend-defined.
- [`settings-usage.js`](../settings-usage.js) requests `account.limits` for connected providers when supported,
  separately from the frontend's usage ledger.

## 10. Approval flow

```text
backend approval.request(sessionId, turnId, approvalId, tool/args, presentation)
   -> BackendClient reverse handler -> Chat claims the running turn
   -> ApprovalCard -> user allow/deny (or cancellation/supersession)
   -> JSON-RPC result to backend -> backend decides/executes the next step
```

The renderer checks session/turn ownership and repeated approval/tool-call identities before showing a card or
running a host action. Requests arriving before start acknowledgement wait for identity; unknown sessions and
stale identities fail. A known conversation with no running turn, or a stopped turn, yields a cancellation result.

[`approval-card.js`](../approval-card.js) validates presentation fields and displays text/code/diffs, falling back to
the tool name and JSON arguments. It does not infer a command's effects or decide whether execution requires approval.
`approval.resolved` can settle a card from the backend. Changing permission mode does not locally auto-approve it.

Browser host requests are not locally linked to an approval ledger: the backend is responsible for requesting any
required approval **before** `host.tool`. Approval UI is a protocol interaction, not an OS sandbox.

## 11. Attachment flow

[`attachments.js`](../attachments.js) handles file picking/drop/paste, notes and previews (up to 20 items).
[`attachment-reader.js`](../attachment-reader.js) prepares payloads using browser APIs and, for PDFs, the desktop
bridge. `chat.js` waits for readiness and converts these to ABP `Input { text, attachments }`.

- Images become data URLs; large images are downscaled/re-encoded for transport/presentation. The current reader
  reports the original width/height even when the encoded image is resized.
- Text and supported Office files are extracted locally, with a 20-million-byte source guard for that path and
  a 400,000-character text truncation limit. These are frontend payload limits, not model context limits.
- PDFs are read through a hidden sandboxed Electron PDF-viewer helper, serialized one at a time. The main service
  accepts an absolute path or bytes, bounds files to 256 MiB, and deletes temporary files it creates. It extracts
  selectable text, not OCR; helper readings are cancelled on app-window closure, not through a per-turn RPC.
- Videos send a local path when available and duration/dimensions; their poster is for display, not video upload.
- Unreadable files still provide metadata and a path when known. `pathOf` does not upload/copy the file or grant a
  scoped capability. Current image payloads are self-contained and do not carry a path from the reader.

The backend decides how attachment text/images/paths enter a model request or tool workflow. The chat display cache
keeps names/notes, image data, pasted-text summaries and video duration/poster, not the full extracted text, file
paths or original files needed to resend. [`user-context.js`](../user-context.js) is distinct: it persists prepared
standing context and sends it with turns/retries. Its limits are local storage/payload guardrails, not prompt policy.

## 12. Browser and other frontend-owned services

The built-in browser is a shared frontend service, not a backend-controlled browser process. Its logins remain in
Electron's `persist:browser` partition. [`host-tools.js`](../host-tools.js) advertises eleven schemas only when the
desktop browser bridge exists: navigate, snapshot, click, type, select, press, scroll, screenshot, read, wait and tabs.

```text
backend host.tool -> Chat turn/step check -> BrowserPanel shared queue/control
                 -> preload browser.run -> main browser job -> owned webview
                 <- text/image/result metadata <- observation or input result
```

[`browser-panel.js`](../browser-panel.js) owns tab UI/lifecycle, renderer-local stable `tabId` handles, visibility,
Take Control/hand-back and cross-chat serialization. Calls pin targets at receipt. Main owns guest identity,
page identities, DOM observations and CDP input. Input tools require a current `pageId`; stale/closed/changed targets
fail rather than silently retarget. `browser_tabs` switch/close require `tabId`, not a display index. Restored tabs
receive new handles; these are not durable session identifiers.

When the user takes control, active steps are cancelled and new work waits. Hand-back returns a fresh snapshot with
`status: 'handed-back'` instead of repeating the interrupted/requested step. A message or turn cancellation releases
that wait. Waiting for the user is intentionally unbounded; normal readiness and execution have deadlines (15-second
initial readiness, 90-second renderer budget, 75-second main-operation budget).

The actual `host.browser` snapshot includes availability, lifecycle status, control, open state, tab IDs/states/
revisions, and `signedIn` observations with `signedInVerified: false`. It accompanies turns/steering and is also sent
in `host.browser.changed` notifications. A submitted password field only records a site-name hint; it neither
proves login success nor supplies provider authentication.

Snapshots and reads are bounded observations, not transactions or complete page exports. Reads use frozen HTML
and continuation `readId`/UTF-16 offsets; screenshots are bounded, including a four-viewport full-page cap.
Downloads go to the OS Downloads directory and are attributed to the guest/operation that initiated them.
See [Browser host-tool lifecycle](browser-host-tools.md) for coverage, deadlines and structured errors.

Other frontend-owned services include native folder UI, local display encryption, clipboard/presentation controls,
PDF extraction and YouTube card metadata. The metadata service in main calls only YouTube oEmbed for a validated
video ID; it is not a general fetch proxy or a provider client.

## 13. Trust boundaries and process lifecycle

The app renderer is sandboxed/context-isolated; local scripts run under a CSP with `script-src 'self'` and
`connect-src 'none'`. Markdown escapes text and restricts links; approval/provider/browser chrome treats supplied
labels as text or escapes them. These are presentation defenses, not reasons to trust backend/model/site content.
HTTP(S) images remain allowed by CSP, browser guests have their own networking, and main performs explicit desktop
services. App-window navigation/new-window requests are prevented and allowed HTTP(S)/mailto links are handed to
the OS. `connect-src 'none'` is not a whole-application network prohibition.

Browser guests do not receive the app bridge. Main replaces their preload, disables Node integration, enables
sandbox/context isolation/web security, checks the expected partition, and allows `http`, `https`, `file`, `about`
and `data` initial URLs. Browser permissions are limited to sanitized clipboard write, fullscreen and pointer lock.
The small guest preload reports only a site name when it observes password submission; it does not transmit the
password. Automation still observes/acts on signed-in pages, so backend host-tool access is privileged.

Main applies its `fromApp` check (sender type `window` and a `file:` sender-frame URL) to backend, browser, PDF,
media, theme-changing and folder-release paths. It does not match the exact app web contents or HTML URL.
Not every IPC handler uses that check: store and folder pick/reveal/location handlers rely on the exposed preload
surface and their own argument handling. Store keys are constrained;
folder reveal and PDF reading are not restricted to the selected project root. The bridge is narrow, but is not a
per-project filesystem permission system.

The backend is **trusted local executable code**, not a sandboxed plugin. Nonblank `OPENGHOST_BACKEND` overrides
`userData/backend.json`; blank/unset environment falls back to the file. A missing file means unconfigured; invalid
configuration is an error, not fallback. Commands are an executable string or an executable/argument string array
(JSON-encoded for the environment). Plain strings are not shell command lines. Main launches with `shell: false`,
working directory `os.homedir()`, the app user's privileges and the complete inherited environment. No executable
allowlist, ownership vetting, environment filtering or stderr secret redaction is provided. The renderer cannot
change this command through its bridge.

Lifecycle in [`desktop/main.js`](../desktop/main.js) and [`desktop/backend-host.js`](../desktop/backend-host.js):

1. A single-instance lock prevents a second normal app instance; a second launch focuses the existing window.
2. Once Electron is ready, browser services initialize, the configured backend starts, then the app window loads.
   The host reports process states such as `none`, `running`, `error` and `exited`; RPC readiness is a separate
   renderer handshake. There is no host event backlog for a page that has not subscribed yet.
3. Reload recreates renderer state/bridge subscriptions and initializes on the same child. There is no automatic
   backend restart/reconnect command. A backend exit fails pending RPCs, aborts reverse handlers, ends live chat
   replies with errors and marks conversations unreconciled.
4. Closing the app window cancels browser/PDF work. All windows closed causes app quit, including on macOS.
5. Quit waits for registered store writes, sends a `shutdown` request, closes backend stdin and allows two seconds
   before forced termination. It waits for process exit, not a shutdown RPC result.
6. On POSIX the backend starts detached as its own session/process-group leader. Exit cleanup kills its process
   group; Linux additionally enumerates `/proc` session members. Descendants that start their own session escape
   that cleanup. Windows uses `taskkill /T /F` for forced shutdown; do not assume Linux session-reaping behavior
   on other platforms or after an already-exited Windows parent.

## 14. Current limits and architectural ambiguities

- **External durability:** this repository implements recovery checks but cannot establish a backend's actual
  storage format, retention, atomicity or credential protection. Those must be verified in the connected backend.
- **Recovery configuration:** `session.get` restores identity and display events, not canonical model/thinking/mode
  configuration. Recovered turns are initialized from frontend settings/cached selection; no separate authoritative
  configuration resynchronization is performed there.
- **Delivery and side effects:** unacknowledged IPC writes, Stop before acceptance, reload during a reverse request
  and already-issued browser actions can have uncertain outcomes. Recovery/identity guards are not exactly-once
  transport or side-effect rollback.
- **Mini-chat asymmetry:** Clear does not await backend deletion. Also, `SideChat.resume(conv, config)` currently
  forwards only those two arguments, while `Chat.resume` requires a retry intent; the main-chat Retry path cannot
  be assumed to work identically in mini chats. These are current implementation limitations, not promised behavior.
- **Browser context:** [the ABP contract](backend-interface.md#host-tools) documents the full `BrowserState` emitted by
  `BrowserPanel.snapshot()`. Tab positions and submission hints are not stable targets or authentication authority;
  use stable tab/page identities as described in [Browser host-tool lifecycle](browser-host-tools.md).
