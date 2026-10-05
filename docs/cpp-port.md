# C++ / Qt frontend extraction

## Target and first-pass result

**Clone the dissected OpenGhost 1.3 frontend.** The existing native Qt port
supplies reusable code, not a new visual specification. OpenGhost's own motion
stays; the source port's custom appearance and extra features do not.

The standalone C++17/Qt 6.11/CMake executable builds and opens without any Rust
backend, backend executable, workspace grant, credentials or Node installation.
The copied QML window, sidebar, composer, settings, splash and native renderers
are used directly. The first extraction supplied disconnected bindings. The
second step now supplies a **QtCore-only semantic backend boundary**, frontend
state service, local preferences and an explicit `--fake-backend` fixture.
Default launch remains disconnected; there is no Rust, process or RPC adapter.

The clean fork was made on branch `cpp-native-extraction` at
`/home/brian/openghost-native-qt`, because the supplied Rust-backend worktree and
existing `og-wt-native-cpp` worktree contained unrelated changes. Neither was
modified. This repository contains no Rust backend sources.

## Reference and provenance

The entire tracked dissected frontend at commit
`177ba954d0ddc7b47dd0526b11f98b517c1735ff` was moved, unchanged, under
[`reference/openghost/`](../reference/openghost/). Its `index.html`, `styles.css`,
`splash.js`, `splash-mist.js`, `settings*.js`, chat/composer/sidebar modules,
assets and boundary documentation remain available. Its old Electron launcher,
package files and tests are **reference only**: no CMake target, resource,
install rule or native runtime reaches them.

Most native files are whole-file imports from Ghosty's `native-ghosty/` at
`3b0e7a51` (the full hash and per-file source hashes are in
[`native-import.json`](native-import.json)). Committed source was used rather
than sweeping up unrelated local edits. Selective historical copies restore
OpenGhost 1.3 work that Ghosty later customized:

- `c4143066^`: original **1.3 spiral/mist splash and app reveal**, procedural
  ghost blink, theme splash palette and native theme implementation. The later
  return to 1.2's splash is not the target.
- `29b295c1^`: 1.3 backdrop, gradients and row colors, before Ghosty's glossy
  black/1.2 palette preferences.
- `31c8e38c^`: appearance page/store and welcome ghost without the owner's
  custom mascot splash selector/handoff.
- `b7caaae6^`: General page without Ghosty's execution-backend choices.
- `3222049f`: whole sidebar and list-model files with the already-implemented
  **1.3 folderless Chats group**. Its search-shell colors are restored to the
  earlier 1.3 parity implementation; none of its service-switching code is used.

The original 1.3 parity work is recorded upstream in Ghosty's
`docs/reviews/2026-10-02-openghost-1-3-visual-parity.md` (commits `608f05e6` and
`81212f65`), and the approvals/mode-picker port in `b7caaae6`. These are provenance,
not a claim that this extraction repeated the full visual qualification.

## What was copied

| Area | Native files retained |
| --- | --- |
| Native window and layout | `qml/Main.qml`, `Backdrop.qml`, sidebar and chat delegates; the useful clipboard/selection/network-denial helpers from `window.cpp` |
| Sidebar and chat | `Sidebar.qml`, `ChatEntry.qml`, `LiveText.qml`, `model.*`, folderless Chats grouping, hover/glide/fade/selection components |
| Composer | Existing composer inside `Main.qml`, model/effort controls and stages, mode picker/dock, file-card presentation, tooltips |
| Settings | `SettingsDialog.qml`, General, Providers, Usage and Appearance; `settings.*` presentation model; `appearance.*` local theme persistence |
| Markdown and code | `markdown.*`, `highlight.*`, `rich.*`, `cssfont.*`, Markdown/Block/InlineText/CodeBlock/TableBlock QML |
| TeX and diagrams | `tex.*`, the full `diagram*.cpp/.h` family, `diagramview.*`, editable diagram view and its native drawing/selection code (1.3's 43-kind engine) |
| OpenGhost artwork and motion | Procedural `ghost.*`, original 1.3 `Splash.qml` and `mist.frag`, welcome/working ghost, `motion.*`, `wave.*`, `reveal.*`, effects, springs, entrance/glide components and shader resources |
| Other presentation | Icons, file kinds, shadows, exposure tracking, approval cards, metrics and selection/copy machinery |
| Assets | Original OpenGhost icon; all applicable source/artwork terms and attribution retained |

`presentation.h` extracts only the display-value declarations needed by the
copied list models and settings from `client.h` and `transcript.h`.
No implementations of those clients, projections or protocol parsers were
imported. `presentation.cpp` retains two display-truncation marker strings used
by the rich renderer.

Most imported files are unchanged apart from the QML namespace/resource and
frontend-binding names. Changes to large QML files remove additions or reconnect
existing components; layout, drawing algorithms and OpenGhost animation curves
were not reimplemented. The wordmark and retained code names are **OpenGhost**.
Historical names remain only in source provenance and attribution records.

### Explicit exclusions

- All Rust/backend/agent/tool code; `client.*`, `rpc.*`, `launch.*`, backend
  transcript/activity projection implementations, service discovery and backend
  process/signal supervision.
- The source port's file-upload/storage bridge, audio/voice implementation, notification
  delivery/policy, installer/updater scripts and RPC tests.
- The owner's custom mascot splash/image, Nunito font, custom splash
  mist/aura shaders, Ripple splash/motion assets and happy-motion variants.
- The source port's splash selector, custom Model and Notifications settings tabs and
  bell/toggle animations, execution-mode settings, voice/transcription controls,
  slash-command helper and added Stop button. OpenGhost stops with Escape.
- Tool/subagent result panels, child activity DTOs, transcript thinking disclosure,
  startup workspace/service picker and Recover/Abandon & Delete banner. These
  unused source-port paths and their model/controller bindings have been removed.
  The reference's `chat.js::applyEvent` tracks tool status and shows the working
  ghost; it does not create tool/subagent/thinking transcript cards. Approval
  cards, model thinking-level selection and the original working ghost remain.
- No Electron, Chromium, WebEngine, Node or React dependency in the native
  project. Qt QML's own JavaScript expressions are not Node or a browser.

## Native boundary and reference inventory (second step)

Source of truth: the frozen [backend contract](../reference/openghost/docs/backend-interface.md),
`backend-client.js`, `backend-protocol.js`, `chat.js`, `settings.js`, `library.js`,
`user-context.js`, `usage.js`, `desktop/preload.js`, and
[browser lifecycle](../reference/openghost/docs/browser-host-tools.md). No source-port
RPC, client, backend projection or process code was copied for this step.

### Commands and reverse calls

Every backend method used by that frontend has a semantic C++ request/reply in
[`src/backend/types.h`](../src/backend/types.h). These are **not JSON serializers**.
IDs are opaque strings; absent values remain optional (including canonical
thinking's absent / null / string distinction). Tool arguments and schemas use
JSON objects because their shapes are backend/host-defined, not because the UI
speaks RPC.

| Original method | Native contract | Fake / UI status |
| --- | --- | --- |
| `initialize`, `shutdown` | `Initialize`, `Initialized`, `Capabilities`, `BackendInfo`, `Shutdown` | Fake handshake/shutdown; fake startup selected only in `main.cpp` |
| `models.list`, `auth.providers` | `ModelsList`, `ProvidersList`, `Model`, `Provider`, `ProviderStatus`, `AuthMethod` | Two explicitly fake models, one provider with no auth methods; existing picker/provider view |
| `auth.setKey`, `auth.login`, `auth.cancel`, `auth.logout` | `SetKey`, `Login`, `CancelLogin`, `Logout` | Types only; fake refuses `unsupported`, no credentials accessed |
| `account.limits` | `GetAccountLimits`, `AccountLimits`, `LimitWindow` | Types only; fake does not invent billing/limits |
| `turn.start` | `StartTurn`, `SessionParams`, `Input`, `StartAccepted` | Text-only fake; asynchronous identity-checked acceptance, not completion |
| `turn.cancel` | `CancelTurn` | Escape freezes locally immediately, then cancels known fake turn |
| `turn.retry`, `turn.steer` | `RetryTurn` (no input), `SteerTurn`, separate acceptance types | Represented, not exercised by UI/fake; explicit unsupported, never automatic resend |
| `session.get` | `GetSession`, `SessionRecovery`, `ExistingSession`, `RecoveredTurn` | In-process fake journal/revision read, missing never creates; UI opens its local cached chat and checks incarnation |
| `session.configure` | `ConfigureSession`, `SessionConfigured` | Existing-chat model/effort changes and Ask/Auto/Full; failure retains previous selection, canonical reply applied |
| `session.compact`, `session.delete` | `CompactSession`, `Compacted`, `DeleteSession` | Compact unsupported; fake deletion implemented/tested, UI deletion still disconnected |
| reverse `approval.request` | `ApprovalRequest`, `ApprovalPresentation`, `ApprovalAnswer` | DTOs only; frontend currently answers unsupported rather than claiming consent |
| reverse `host.tool` | `HostToolSchema`, `HostToolRequest`, `HostToolResult` | DTOs only, including text/image result blocks and tool-level error vs RPC error |
| `host.browser.changed`, `$/cancelRequest` | `Backend::browserChanged`, `cancelRequest`, `reverseCancelled` | Semantic hooks only; no wire notification implementation |

There is deliberately **no** backend `session.create`, list, import, rename,
edit-message, save-settings/defaults, or generic context-update method. The
reference's chat index, display annotations and preferences belong to the
frontend. Creating a chat is a local draft; its first `turn.start` uses a null
incarnation. Opening calls `session.get`, never reconstructs model input from
rendered messages. Rename is a local annotation.

### Events, streaming and identity

`SessionEvent` has session/sequence identity and optional turn/message/client
identities separate from its typed payload. The following reference events all
have payload types:

- `turn.started`, `turn.completed` (done/cancelled/error, finish reason, `Error`).
- `message.started`, `message.delta`, `message.completed`, `reasoning.delta`.
- `tool.started`, `tool.progress`, `tool.completed`, `approval.resolved`.
- `compaction.started`, `compaction.completed`, `input.accepted`, `session.updated`.
- `usage`: incremental input/cache-read/cache-write/output/request counts and
  optional context used/window, not invented token estimates or billing totals.

Global `auth.changed`, `models.changed`, `log` have a separate `GlobalEvent`
variant; connection loss is `Backend::closed(Error)`. Auth/model notifications
are invalidations requiring fresh reads, not ordered authoritative snapshots.

The exercised text projection keeps message-start order, interleaved buffers,
append deltas and first-final sealing; a final text replaces the entire buffer,
including empty text. Duplicate/backwards sequences, unknown-message and
conflicting-turn output are ignored. Turn terminal/Stop cannot reopen output.
Retry's required state is inventoried but not yet exercised: never-dispatched
input retains its full prepared payload and retries with start; uncertain dispatch
requires `session.get` keyed by the original client turn; accepted terminal failure
retries with an exact failed remote turn ID and a new client ID, without new input.
No case may substitute cached attachment previews for the original payload.

Reasoning and tool progress do not become transcript rows; tool start can pulse
the existing working ghost, not a tool/subagent card. The fake intentionally
emits `turn.started` before its acceptance reply to exercise that ordering.

### Data and ownership

| Data/state the original expects | Native representation / owner |
| --- | --- |
| Session incarnation, high-water sequence, accepted client/remote turn IDs, recovery display journal | Boundary types; in-memory `FakeBackend` sessions; frontend `ChatRecord` tracks its own display state |
| Frontend chat identity/title/rename/timestamps/model/mode, active turn/message buffers, local drafts and annotations | `frontend/chat_service.*`, existing QML draft cache/list models; not backend history |
| Full attachment input vs lossy cache previews | Separate `Attachment` and `DisplayAttachment`, `Input` and `DisplayInput`; no conversion of previews back into input |
| Standing instructions and pinned payloads | `UserContext` / `ContextFile`; instructions saved locally and sent on starts, pinned-file preparation not yet implemented |
| Preferred model, preferred effort, permission mode | `Preferences` / `PreferencesStore`, separate from canonical session configuration; implicit/default effort never saved as an explicit preference |
| Backend model capabilities and availability | Exact provider/model pair, optional context/vision/default thinking and advertised levels; no guessed medium/first effort, no fallback for removed models |
| Browser availability, lifecycle/control/open state, tab IDs/revisions, sign-in observations | `BrowserState` / `HostContext`; absent browser is null, observations are not verified auth |
| Errors and permitted actions | Extensible named `Error` code/message/provider/action/retryable/status; no silent conversion of unknown outcome to not-started |
| Usage windows, balances/credits, per-event counts | Boundary types; local ledger and accounting projection not yet implemented |
| Appearance | Existing frontend-local Qt appearance store, unchanged |

`Entry`, `Session`, `Selection`, `Account` and QML roles remain **display-only**
values in `presentation.h`. `WindowController` translates semantic state into
these values; the fake never includes or calls any rendering class.

### Layering and execution

```text
QML -> WindowController -> ChatService / PreferencesStore
                              | semantic commands/events
                              v
                           Backend <- FakeBackend (explicit injection)
                              ^
                       future ABP adapter (absent)
                              |
                       ByteTransport (interface only)
```

`native_contract` links **QtCore only**. `Backend` is an asynchronous,
same-owner-thread interface with local request IDs and typed results/events.
Calls do not reply inline; command cancellation is distinct from turn Stop.
The byte transport interface is in `backend/transport.h`, with no process,
network or platform implementation. The future adapter must implement JSONL,
64-MiB bounds, exact string/integer RPC-ID matching, protocol validation,
request deadlines (60 seconds / login 15 minutes / compact 10 minutes), reverse
request lifetimes and connection replacement. None of this belongs in QML.

The fake uses a Qt timer (or deterministic `advance()` in tests), no worker,
subprocess, provider SDK, credentials, files or network. It checks create-only
and versioned starts, deduplicates identical client IDs, rejects conflicting
reuse and concurrent same-session starts, journals display events before
emitting them, supports cancellation and keeps completed turns addressable.
Its `sessions.recovery` capability describes **this process only**: neither fake
acceptance nor the frontend display cache is durable across application exit.
It is a development fixture, **not a conforming production ABP backend**.

This is a buildable extraction, **not yet a complete functional or pixel-exact
1.3 clone**. The inherited native port still lacks parts of the reference,
including persistent mini-chat, chat locks, browser/media integration, plus-menu
operations, and the visual diagram form editor. They have not been invented in
this first pass. Existing text/antialiasing, shadow and drawing differences also
remain; this task did not rerun the original full pixel-comparison suite.

## Exercising the fake and remaining gaps

```sh
./build/openghost-native --fake-backend
```

Choose Fake Echo or Fake Brief in the existing model picker (a fresh draft uses
the first catalog model). Send through the existing composer; acceptance clears
only its unchanged submitted draft. The assistant streams visibly. Escape stops
it; there is still no Stop button. New Chat and sidebar Open keep separate
in-memory conversations. Switch an idle chat's model, edit General instructions,
and change the existing mode picker. Explicit model/effort/mode preferences and
instructions survive restart in `preferences.json` alongside `appearance.json`
under Qt's app-specific `AppConfigLocation`. No prior Electron/native profile is
read or migrated. Writes use portable `QSaveFile`; invalid/unreadable files are
not overwritten with defaults, and failed writes do not publish saved state.

Without `--fake-backend`, no catalog, accepted input or conversation is fabricated.
General/appearance preferences still work locally. Unimplemented file picking,
auth, deletion and other operations refuse rather than simulate success.

**Boundary vocabulary is not end-to-end implementation. Remaining flows:** production durable chat
index/checkpoints and snapshot replay/live-race buffering; uncertain-start
reconciliation and all three Retry cases; the 256-event early queue; steering
and queued input during model switching; compaction; approval/reverse-call
ownership and cancellation; provider mutations/catalog invalidation refresh;
attachment preparation/previews; usage ledger, late/replayed accounting and
context meter; detailed finish-reason/empty-response/error actions. The fake
refuses retry, steer, compact, auth/limits and attachment/browser inputs. Its
open path validates known cached identity and revision, not a general crash-recovery
engine; uncertain chats remain display-only rather than being blessed by an idle
snapshot.
The UI currently disables Send while streaming rather than claiming steering.

**Original frontend state/services not yet given complete native representations:**

- Persisted library/folder/home-space layout and collapse/pin metadata, chat-lock
  encryption/unlock state, legacy display-cache migration, stats/compact/moved
  display entries, mini-chat lifecycle/clear/move state (only `SideContext` exists).
- Persistent pending-input/retry-preparation checkpoints, reply-part accounting
  tail and local usage ledger schema (event/limits DTOs exist, not those stores).
- Concrete browser tool argument/result schemas beyond the generic schema/JSON
  fields: navigate/snapshot/click/type/select/press/scroll/screenshot/read/wait/tabs,
  page/ref/read identities, serialized execution/hand-back queue and downloads.
  Browser state and generic host-tool envelopes are represented, not a browser engine.
- Desktop host file/path/folder/PDF extraction and release APIs, pinned payload
  storage, media/video-info embedding and browser event bridge. Native window,
  clipboard, links and appearance remain the existing implementations.
- Source-port provider prompt/selector presentation still needs replacing for
  real auth. Attachment UI still has inherited 8-file/picture assumptions rather
  than the reference's 20 prepared composer attachments. No file UI is enabled by
  the fake; no new resource/attachment policy is implied.

The original frontend's local display/cache contracts are not ABP history and
must not become an invented backend API. These are follow-ups, not intended
behavior redesigns. No real backend should be connected to this partial state
service until durability, reconciliation and refusal semantics are completed.

## Platform boundaries and remaining Windows/macOS work

`src/platform/` is selected by CMake:

- `desktop.cpp`: Qt external-link dispatch and the explicit
  `OPENGHOST_REDUCED_MOTION=0/1` override.
- `linux.cpp`: copied Linux render-loop/portal setup, desktop identity and KDE
  reduced-motion preference lookup. These assumptions no longer live inside the
  shared renderer.
- `portable.cpp`: Qt-native window/dialog/clipboard path for Windows/macOS,
  with no Linux environment, DBus or POSIX dependency. Native reduced-motion
  discovery there is still a follow-up, not a claimed implementation.

CMake has MSVC UTF-8/warning options, a Windows GUI target and a macOS bundle;
shaders are compiled through Qt ShaderTools. Still to qualify: platform SDK/Qt
builds, framework/plugin deployment, bundle/EXE icons and signing, native dialogs
and link behavior, accessibility/reduced-motion discovery, DPI/font metrics,
IME/shortcuts, graphics backends, and the appearance store's permissions/ACL
behavior. No Linux-only backend process/descriptor code was copied as a supposed
cross-platform implementation.

## Focused checks

On Linux with Qt 6.11.2 and GCC 16.2.1:

- Release configure and build succeeded, using C++/Qt only.
- One `native_ui_smoke` check: independent offscreen/software launch, 1.3 splash
  reveal, both themes, all four settings pages, no source-port-only controls,
  disconnected-send/attachment refusal, Markdown/TeX/diagram rendering, native
  close signal and no QML load/binding warnings.
- The same smoke launched on the real **Wayland/OpenGL** Qt scene graph and
  exited successfully. The uninstalled development executable produced a
  nonfatal portal app-ID registration warning; the Linux install includes its
  desktop entry.

The cleanup pass repeated the Release build and offscreen smoke, adding checks
for the removed panels/helpers, all three retained transcript delegates and
message-state updates. A case-insensitive search found no former product names
in native source, QML, tests, shaders, resources or CMake. The frozen reference
and original license notices were not changed.

Windows/macOS and full visual/behavior parity were not tested. No Rust, Electron,
Node, provider login or live model tests were run. Commands are in the root
[README](../README.md).

## Second-step focused checks

On Linux, Release CMake build plus only `native_contract_test`,
`native_ui_smoke` (disconnected) and `native_fake_ui_smoke`:

- Contract: async settlement, no disconnected acceptance, initial catalog choice,
  identity/version/dedup refusal, ordered recovery journals, cancellation before
  dispatch and during streaming, unsupported input with no session creation,
  terminal sealing and message interleaving, concurrent chat isolation, open,
  model-switch failure, malformed acceptance, canonical thinking, local settings
  round-trip/oversize/corruption/write failure, and user-context forwarding.
- Fake UI smoke: real QML submit and acknowledgement draft clearing, streamed
  transcript, Escape Stop/frozen text, model switch, instructions/mode, New Chat,
  sidebar history reopening and completion; plus existing rendering/settings
  checks in both modes. Two dormant bindings exposed by populated provider/chat
  rows were corrected (logout visibility and the busy ghost's existing accent).
- Temporary preferences/appearance stores; no user profile, live credential,
  model, Rust, Node or Electron tests. No Windows/macOS runtime qualification.
- No QML load/binding warnings in either smoke. GCC 16 emitted
  `-Wmaybe-uninitialized` diagnostics in Qt-generated metatype copying of the
  nested `Result` variant; these were not suppressed. The Release build succeeded.

## Next smallest migration step

Keep rendering fixed. Finish frontend-local checkpoint/recovery and retry/steer
projection against a deterministic semantic peer, including events-before-ack,
terminal-before-ack and replay/live races. A subsequent read-only ABP adapter can
implement `Backend` over `ByteTransport`, with independent envelope/transport
tests. No Rust or real backend has been connected in this step.
