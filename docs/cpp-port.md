# Native C++ / Qt port notes

This is the engineering inventory for the standalone native OpenGhost frontend.
Start with the [README](../README.md) for build/run/test commands, feature status
and storage locations. [Repository readiness](repository-readiness.md) records
current audit findings and qualification limits.

## Scope and current state

The target is the **dissected OpenGhost 1.3 frontend**, not a redesign. The native
C++17/Qt 6.11+ implementation supplies the window, sidebar, composer, settings,
renderers, original splash/motion and frontend-owned state. It builds without an
agent backend, credentials, workspace grants or Node. The built-in browser panel's
pages need Qt WebEngine; `-DOPENGHOST_BROWSER=OFF` builds without it.

Normal startup is disconnected. `--fake-backend` injects a QtCore-only in-memory
fixture. There is **no real backend, wire adapter, process transport, RPC or FFI**.
Frontend lifecycle/recovery tests must not be described as real backend recovery
qualification. This is neither a complete functional clone nor a newly qualified
pixel-exact port.

The extraction progressed from disconnected bindings, to a typed contract/fake,
to existing UI flow wiring, then local library/checkpoint/mini-chat/lock/usage
state. State implemented in those passes is not necessarily exposed in QML:
mini chat, password controls and adding folders still have no UI entry point.

## Reference and provenance

The entire tracked dissected frontend at commit
`177ba954d0ddc7b47dd0526b11f98b517c1735ff` was moved, unchanged, under
[`reference/openghost/`](../reference/openghost/). Its `index.html`, `styles.css`,
`splash.js`, `splash-mist.js`, settings/chat/composer/sidebar modules, assets and
boundary documentation remain the visual/behavior authority. Its Electron
launcher, package files and tests are **reference only**: no CMake target,
resource, install rule or native runtime reaches them. Do not edit that tree.

Most native files are whole-file imports from Ghosty's `native-ghosty/` at
`3b0e7a51` (full hash and per-file source hashes are in
[`native-import.json`](native-import.json)). Committed source was used, not
unrelated local edits. Selective historical copies restore OpenGhost 1.3 work
that the source port later customized:

- `c4143066^`: original **1.3 spiral/mist splash and app reveal**, procedural
  ghost blink, theme splash palette and native theme implementation. The later
  return to 1.2's splash is not the target.
- `29b295c1^`: 1.3 backdrop, gradients and row colors, before the source port's
  glossy black/1.2 palette preferences.
- `31c8e38c^`: appearance page/store and welcome ghost without the owner's
  custom mascot splash selector/handoff.
- `b7caaae6^`: General page without execution-backend choices.
- `3222049f`: whole sidebar and list-model files with the already-implemented
  **1.3 folderless Chats group**. Search-shell colors were restored to the earlier
  1.3 parity implementation; none of its service-switching code is used.

The original 1.3 parity work is recorded upstream in Ghosty's
`docs/reviews/2026-10-02-openghost-1-3-visual-parity.md` (commits `608f05e6` and
`81212f65`), and the approvals/mode-picker port in `b7caaae6`. Those are historical
source-repository paths/revisions, not local documentation or a claim that this
extraction repeated the full visual qualification. The import manifest records
source snapshots, not current-file checksums.

The branch is `cpp-native-extraction`; it was created as a separate worktree to
avoid modifying unrelated source/backend work. There are no Rust backend sources
in this tree. Historical names remain in provenance and attribution, not native
product bindings. Preserve [NOTICE.md](../NOTICE.md), [LICENSE](../LICENSE) and
[`licenses/`](../licenses/); retaining them does not grant redistribution rights
for the excluded name, artwork, animations or design.

## Retained native presentation

| Area | Main files |
| --- | --- |
| Window/composition | `src/main.cpp`, `window.*`, `qml/Main.qml`, `Backdrop.qml`; clipboard/selection and QML network-denial helpers |
| Sidebar/chat | `qml/Sidebar.qml`, `ChatEntry.qml`, `LiveText.qml`, `src/model.*`; folderless Chats, hover/glide/fade/selection |
| Composer | Existing composer in `Main.qml`, model/effort controls/stages, mode picker/dock, file cards and tooltips |
| Settings | `SettingsDialog.qml`, General, Providers, capability-gated Plugins, Usage, Appearance; `settings.*`, `appearance.*`, `offline_services.h`, `window.*` projections |
| Markdown/code | `markdown.*`, `highlight.*`, `rich.*`, `cssfont.*`, Markdown/Block/InlineText/CodeBlock/TableBlock QML |
| TeX/diagrams | `tex.*`, `diagram*.cpp/.h`, `diagramview.*`; native painting/selection, the inherited 1.3 43-kind diagram engine |
| Artwork/motion | Procedural `ghost.*`, 1.3 `Splash.qml`/`mist.frag`, welcome/working ghost, `motion.*`, `wave.*`, `reveal.*`, effects/springs and shaders |
| Other helpers/assets | Icons/file kinds, shadows, exposure, approval cards, metrics/copy machinery, original OpenGhost icon |

`presentation.h` retains only display-value declarations needed by the copied
list models/settings from the source `client.h` and `transcript.h`.
`presentation.cpp` retains two rich-renderer display-truncation markers. No
implementation of those clients, projections or protocol parsers was imported.
QML resource namespaces are `OpenGhost.Ui` and `OpenGhost.Native`.

### Explicit exclusions and unexposed UI

- Backend/agent/tool implementations; source `client.*`, `rpc.*`, `launch.*`,
  backend transcript/activity projections, discovery and process supervision.
- Source-port upload/storage bridge, audio/voice, notifications, updater/installer
  scripts and RPC tests. The native CMake install rules are not a Qt deployment
  system or a replacement for those installers.
- Custom mascot splash/images/font, alternate/ripple/happy-motion assets,
  splash selector, extra Model/Notifications settings tabs, execution-backend
  settings, voice controls, slash helper and an added Stop button. Stop is Escape.
- Tool/subagent/thinking transcript panels, startup service/workspace picker and
  Recover/Abandon & Delete banner. The reference's `chat.js::applyEvent` tracks
  tool status/working ghost, not tool-result cards. Approval cards and model
  thinking-level selection remain; reasoning events are not drawn.
- No mini-chat dialog, password lock screen, add-folder picker, broader
  plus-menu operations or visual diagram form editor. Mini/lock/folder
  **state** exists; that does not imply those views are ported.
- No Electron, Node or React in the native target. Qt WebEngine (Chromium) is
  used only for the browser panel's guest pages (`OPENGHOST_BROWSER`), mirroring
  the reference's Chromium webviews; it never renders the app's own UI.

## Typed boundary and future adapters

Source contract: the frozen [backend interface](../reference/openghost/docs/backend-interface.md),
`backend-client.js`, `backend-protocol.js`, `chat.js`, `settings.js`, `library.js`,
`user-context.js`, `usage.js`, `desktop/preload.js` and
[browser host lifecycle](../reference/openghost/docs/browser-host-tools.md).
Implementation: [`types.h`](../src/backend/types.h), [`backend.h`](../src/backend/backend.h),
[`transport.h`](../src/backend/transport.h).

`Backend` accepts a typed `Command`, signals `Result = variant<Reply, Error>`
replies and emits typed session/global/reverse events. It and its consumer share an owner
thread; requests never reply inline and settle once. Events may precede replies.
Local `RequestId` is not an RPC, session, turn or message ID. IDs stay opaque;
canonical thinking preserves absent / null / string through nested optionals.
Unknown error codes/actions remain named data rather than guessed success.

`native_contract` links **QtCore only**. `WindowController` adapts `ChatService`
into display models; no QML renderer is called by the fake. Full `Input` /
`Attachment` and `UserContext` remain separate from lossy `DisplayInput` /
`DisplayAttachment`. Tool schemas/arguments/details are JSON objects because
those payloads are extensible, not because the UI speaks RPC.

| Reference method/event family | C++ contract and current reachability |
| --- | --- |
| `initialize`, `shutdown` | `Initialize`, `Initialized`, `Capabilities`, `BackendInfo`, `Shutdown`; fake implements them. Service startup currently requires protocol `0.1` and session recovery, then reads models/providers. |
| `models.list`, `auth.providers` | `ModelsList`, `ProvidersList`, `Model`, `Provider`; existing picker/provider UI, fake catalog only. |
| `plugin.list/enable/disable`, `plugin.changed` | `PluginsList`, `EnablePlugin`, `DisablePlugin`, `PluginsListed`, `PluginUpdated`, `PluginChanged`; conditional Settings page, tested at the semantic seam only. `Capabilities::runtimePlugins` maps to `capabilities.plugins.runtime`; no adapter is implemented here. |
| `auth.setKey/login/cancel/logout` | `SetKey`, `Login`, `CancelLogin`, `Logout`; key prompt and sign-in/wait/cancel/logout UI, fixture status only. |
| `account.limits` | `GetAccountLimits`, `AccountLimits`, `LimitWindow`; types only, fake refuses, no invented balances. |
| `turn.start` | `StartTurn`, `SessionParams`, `Input`, `StartAccepted`; identity-checked asynchronous acceptance, not completion. |
| `turn.cancel` | `CancelTurn`; Escape freezes locally first, then cancels the known turn (including a late acknowledgement). |
| `turn.retry`, `turn.steer` | `RetryTurn` has no input; `SteerTurn` has its own input ID and receipt. Wired in existing UI. |
| `session.get` | `GetSession`, `SessionRecovery`, `ExistingSession`, `RecoveredTurn`; read/reconciliation only, not resending cached text. |
| `session.configure` | `ConfigureSession`, `SessionConfigured`; canonical model/effort/mode replies, previous selection kept on failure. |
| `session.compact`, `session.delete` | `CompactSession`/`Compacted` are unsupported by fake; `DeleteSession` is wired and acknowledgement-gated. |
| reverse `approval.request` | `ApprovalRequest`, `ApprovalPresentation`, `ApprovalAnswer`; existing cards and correlated lifetime. |
| reverse `host.tool` | `HostToolRequest`/`HostToolResult`, text/image blocks, tool-level error vs command error; service routing exists, shipped host exposes no tools. |
| `host.browser.changed`, `$/cancelRequest` | `browserChanged`, `cancelRequest`, `reverseCancelled` semantic hooks only; no wire implementation. |

There is deliberately no backend session-create/list/import/rename/edit-message/
save-settings/defaults API invented here. New Chat is a local draft; first start
uses an absent version (create-only). Rename/index/preferences are frontend-owned.

`SessionEvent` separates session/sequence and optional turn/message/client IDs
from its payload: turn/message start/delta/completion, reasoning, tool
start/progress/completion, approval resolution, compaction notices, input
acceptance, session updates and usage. `GlobalEvent` holds auth/model
invalidations, plugin snapshots and logs; `closed(Error)` signals connection loss. Auth/model
changes trigger rereads, not blind adoption of unordered snapshots.

The frontend is backend-agnostic **through these semantics**, not through an
already-universal wire protocol. A future adapter could target the Rust harness,
Pi or another compatible backend, translating its vocabulary and refusing
unsupported capabilities. None is linked or tested here, and no drop-in
compatibility is claimed. Required recovery/identity/consent semantics cannot be
fabricated to make a candidate backend fit.

`ByteTransport` declares bounded ordered writes, received bytes, status and write
failure; `write(true)` means queued, not accepted. It has **no implementation**.
For an adapter targeting the reference ABP, framing/parsing/validation belong in
that adapter: JSON Lines, 64 MiB bounds, exact string/integer RPC IDs, sequence
range validation, request deadlines (reference: 60 seconds, login 15 minutes,
compact 10 minutes), reverse-call lifetimes and connection replacement. Another
backend may need another wire mapping. Neither belongs in QML or requires
changing native rendering. No adapter work was done in this pass.

### Plugin state

[`frontend/plugins.*`](../src/frontend/plugins.h) is owned by `ChatService`, uses
its normal request correlation and has its own row/state signals and errors.
Plugin operations do not set chat pending/error flags or refresh the model
catalog. Initialization explicitly gates discovery on `runtimePlugins`;
disconnect clears rows/capability and invalidates pending plugin callbacks. A
subsequent successful initialization reads a fresh list. This does not add a
reconnect supervisor.

Each initialization is a generation bound to its `Initialize.connectionId`.
Completions dispatched under an earlier generation are dropped, and the port
stamps every `PluginChanged` with the `connectionId` of the connection that
delivered it (a typed field, not a wire field). `Plugins::observe` accepts only
the current connection's events; an event from a replaced connection, or with
no connection, can neither add a row nor regress one, even while the new
connection's first list is in flight. Within one connection the port must
deliver events and replies in backend order; the frontend does not reorder
them.

Rows retain backend snapshots and per-ID pending/error bookkeeping. Toggle
requests never optimistically alter enabled state; returned snapshots reconcile
it. Every observation carries a local tick (not a backend version), relying on
ABP's order: events and answers arrive in backend order, and an answer is never
older than a `plugin.changed` before it. An answer is adopted only if nothing
about its plugin was observed after it was sent; a list replaces only rows not
observed after it was sent (so live events racing it win, including newly
observed IDs) and clears only doubt that predates it. A failed operation marks
the row unconfirmed and rereads state with a list sent afterwards, without
retrying the mutation; an older in-flight list cannot reauthorize it, and the
row error clears once a later observation confirms the state. Failed refreshes
leave unconfirmed rows read-only. A `disabling` plugin can be re-enabled (the
backend keeps its settling calls). List load errors, persistence warnings and a
`persisted` of `memory`/`ambiguous` are displayed, not interpreted as frontend
activation/persistence policy. Plugin requests are not chat work: Stop before
acceptance cancels only the turn's own start/retry request.

`SettingsDialog.qml` reuses its existing rows, pills, status text, plug icon and
navigation/entrance animations. Rows come from `PluginModel`, a keyed list model
updated in place, and the page list depends only on `runtimePlugins`' own
signal, so updates neither recreate row/tab delegates nor drop keyboard focus.
`Plugins` keeps an ID index, moves (not copies) entries when a list reorders
them, and signals only real changes: an identical event or list emits nothing.
Each entry carries a display revision, so `PluginModel::sync` formats status
text only for rows whose revision moved and updates same-order rows in place.
A pending or unconfirmed toggle stays focusable but does not act. Unavailable
and unknown states are not toggleable. All names, IDs and descriptions come from the backend and
render as plain text. No host-tool registration, plugin code loading, local
preference or restart is involved. The backend API is still typed, **not a wire
implementation**; `tests/plugins.cpp` and `tests/plugin_smoke.cpp` use the scripted
`tests/plugin_fixture.h`, not the Rust backend or live RPC.

## Frontend state and recovery

Read [`chat_service.*`](../src/frontend/chat_service.h),
[`library.*`](../src/frontend/library.h), [`store.*`](../src/frontend/store.h),
[`preferences.*`](../src/frontend/preferences.h) and [`usage.*`](../src/frontend/usage.h).
The reference equivalents are `library.js`, `chat-store.js`, `chat.js`,
`mini-chat.js`, `chat-lock.js` and `usage.js`.

### Ownership and persistence

| Owner | State |
| --- | --- |
| Backend | Canonical session/history, incarnation, accepted remote turn identity, journal/revision and actual execution/credentials. Fake substitutes only process-local values. |
| `Library` / `KeyStore` | Version-1 index; home/folder chats, collision-free home `space` names, rename/pin/timestamps/model/lock metadata; `chats/<id>` and `mini/<id>` display caches. Unknown per-chat index fields are retained, not arbitrary unknown top-level fields. |
| `ChatService` | Current/draft/mini records, live output buffers, local turn state, approvals, host-call ownership, recovery checkpoints. Backend versions/sequence/remote IDs are runtime state reacquired on reopen; client IDs/pending markers are saved in display entries. |
| QML / `AttachmentStore` | In-memory composer drafts (32 inactive maximum) and opaque attachment tokens/full prepared input; never recovered from saved previews. |
| `PreferencesStore` | Instructions (8,000-character bound), explicit model/effort and global permission preference. Per-chat mode is not an independently persisted canonical backend configuration. Pinned payloads unsupported. |
| `UsageStore` | Version-2 local ledger, JSON-pair provider/model keys, v1 `provider\|model` upgrade; per-reply metrics also in display caches. Save/failure behavior below. |
| Appearance store | Theme choice only, with a separate read/write policy. |

File store keys are validated relative names; `:` is escaped as `%3A` in file
names. Each key is a JSON object up to 64 MiB, replaced via `QSaveFile`. There is
no cross-file transaction, interprocess lock or hostile-filesystem containment.
An unreadable/unknown-version index makes the library read-only; unreadable cache
loads fail rather than supplying empty messages. This does not mean the raw
`KeyStore::write` prevents every overwrite, nor that every cache schema field or
version is strictly rejected. Display entries are normalized through an allowlist.

Display entries retain user/assistant/compact/stats/moved data, selected legacy
attachment metadata, `backendTurn` (the **client** turn ID), `clientInputId`,
`pendingTurn` and steering receipts. Error/Stopped notes remain view-only.
Compact/stats rows are kept but not drawn. Full attachment payloads, backend
history, signed model state and credentials are not stored here.

### Admission, projection and reconciliation

- Required index + display checkpoints precede each start, steer and failed-turn
  Retry. A failed start checkpoint keeps exact prepared input/client ID for an
  explicit in-process Retry; failed steering is not applied; failed Retry
  checkpoint leaves the old failed turn intact. Ordinary subsequent saves are
  best effort and do not establish backend durability.
- The pending marker lives on the prompt (or a hidden open reply part for Retry).
  Only backend completion of an acknowledged start or acknowledged cancellation
  settles it, not local Stop or invalid/unacknowledged acceptance.
- Restored chats start display-only/unreconciled. Opening reads `session.get`
  with the saved pending client ID, adopts its version/revision and reconstructs
  the matching turn. Missing session means retained display-only data; missing
  pending turn means `turn_missing`, not a resend. A missing **empty new mini**
  session is the separate create-only case.
- Recovery reuses cached prompts/steering display, never prepares input from
  previews. Unaccepted queued input stays visible. Raced events are bounded to
  256 and applied beyond the snapshot revision; unreconciled restored chats do
  not infer history from live deltas. The recovered checkpoint must save.
- In-process uncertain-start Retry first queries the original client ID. Only a
  positively missing new session with no known remote turn and exact retained
  prepared input can restart that request. Matching recovery projects its journal
  without resend; a recovered terminal failure requires another explicit Retry.
  Accepted failure Retry names the exact remote failure plus a new client ID and
  sends **no input**. Uncertain retry-of-retry is not guessed safe.
- Streaming preserves message-start order and interleaved buffers; first final
  text replaces the whole buffer, even when empty, then seals it. Backward/
  duplicate sequence, foreign turn or unknown-message output is ignored.
  Terminal/Stop cannot reopen output. Early output before acceptance is tested.
- Send while busy steers; `queued`, `applied`, `notApplied`, `unconfirmed` are
  distinct. Approvals superseded by steering are denied. Escape freezes local
  presentation, but cancellation can remain pending; it is not proof of no effects.
- Chat deletion acknowledges both main and `<id>:mini` sessions before local
  removal, refusing mini-ID collisions. Folder deletion is sequential; partial
  backend success is not rolled back. Local removal/persistence failures are not
  fully surfaced (see readiness findings).

There is no automatic connection-replacement supervisor. Restart tests recreate
frontend objects over retained stores and a surviving fake/scripted backend;
they do not kill/restart a production backend or prove exactly-once effects.

### Mini-chat and folder state without new UI

`openMini/sendMini/stopMini/retryMini/approveMini/closeMini/clearMini` maintain
session `<id>:mini`, cache `mini/<id>`, `seen`, and `side.parent/parentBusy/moved`.
After the parent advances, the next mini send includes the caught-up display
marker. Closing stops and keeps it; Clear deletes only mini state; deleting the
parent targets both sessions. Mini records do not appear as independent sidebar
chats. The fake accepts side parameters but does not perform real model-context
catch-up.

Pins and folder/home collapse persist and drive the existing sidebar. Existing
folders, including empty ones, are listed by activity; New Chat in a known folder
uses it. `addFolder` is a service method with no UI picker. There is no desktop
`chatsFolder` host, so home chats have an empty `cwd`, not an invented workspace.
`spaceName` sanitizes reserved characters/device names; it is not a filesystem
grant or a promise to create directories.

### Password/encryption limitation

`Library` has a tested lock state machine and an injected `ChatSealer` seam.
The shipped application constructs it **without a sealer**. Protect and unlock
refuse; pre-existing protected records remain locked and are not saved as empty.
There is no password UI and no production chat encryption in this build.

The reference's `chat-lock.js` uses NFC-normalized passwords, a random salt,
PBKDF2-SHA256 with 600,000 iterations and AES-256-GCM. QtCore supplies no such
complete implementation here. A vetted platform/library sealer and format,
authentication, failure and compatibility tests are required. `TestSealer` is an
HMAC-authenticated XOR **test fixture**, not secure crypto or reference-format
compatibility proof. Never ship it as encryption.

With an injected sealer, protection writes index lock metadata before sealing
messages; unprotect writes clear messages before dropping the index lock. These
ordered multi-file steps are **not atomic**; an interruption can leave plaintext,
and mini-cache reseal failures need qualification/reporting. Unlock verifies the
sealed title; relock drops the runtime key/title. Even a future sealer would not
protect backend history, every index field or all in-memory copies: folder/space
names remain visible. Do not promise secure deletion or a secure vault.

### Usage and other host-owned state

Only correlated live usage increments the ledger; replay updates reply metrics
without charging again. Late old-turn usage cannot overwrite current context
state. Counts are not locally guessed; absent timing remains unknown. The ledger
uses an 800 ms debounce and destructor flush. Pending writes are tracked
independently of the single-shot timer, which Qt stops before emitting timeout.
Only a successful write clears that pending state. A failed write returns false
from `flush()` and emits `saveFailed`, following the preference store's error
signal pattern; counts remain in memory for an explicit flush, a later usage
update or destruction. There is no automatic retry loop or dedicated usage-error
UI. Unreadable/unknown-version ledgers still block recording and remain untouched.
Tests cover real timer-driven file writes, failure signalling and all three retry
paths, unchanged v2 serialization and v1 upgrade. Unsaved counts can still be lost
on crash or persistent write failure; see [repository readiness](repository-readiness.md).

Preferences preserve invalid/unreadable files and publish changed values only on
successful save. Appearance instead ignores invalid input, can replace it after
a theme choice and requests owner-only permissions. Library/preferences storage
has no comparable explicit permissions/ACL policy.

Text attachments are a real frontend-local, synchronous file operation: 20 per
message, 256 KiB each, 64 retained draft tokens/8 MiB; an unsuccessful selection
publishes no tokens. UTF-8/NUL/PDF/nonlocal/symlink refusals are tested. Byte bounds
are not I/O deadlines or descriptor-contained file authority. The fake itself
performs no file access. Images/PDF/media/office extraction, pinned payloads and
a production asynchronous file host are absent.

Approvals are correlated once, including bounded early requests and stale/
duplicate refusal, and removed on resolution, Stop, reverse cancellation or
steering. Auth/model invalidations reread catalogs; status and pending login are
not saved credentials. The fake accepts only `fixture` and simulates sign-in via
a local timer. Account limits/billing remain unsupported.

[`HostServices`](../src/frontend/host.h) supplies browser observations, initialized
tool schemas and asynchronous completion/cancellation. `ChatService` checks
published names, live session/turn and duplicate **in-flight** call identities;
finished callbacks answer once, reverse cancellation releases without answering,
and turn end cancels/releases with a cancelled result and tells the host
(`turnEnded`) so the browser drops that chat's hold. The desktop build's
[`Browser`](../src/frontend/browser.h) reports the panel's snapshot and changes but
publishes no tools (`available: false`); an OFF build's `NoHost` reports a null
browser. Either way the service refuses host calls as unsupported. Tests inject a
scripted host; browser operations, page/ref identities, the agent-side hand-back
wait, downloads and media handling do not exist yet. Observed browser sign-in is
never verified auth. The fake accepts a browser snapshot as ignored context.

## Fake-backend limits

The fake uses timers or manual `advance()` only: no worker, process, network,
provider SDK or credential storage. It models create-only/versioned starts,
supported-input client-ID deduplication, per-session busy refusal, cancellation,
ordered in-memory display journals and completed-turn lookup. It deliberately
emits `turn.started` before acceptance to exercise that ordering.

It is **not a conforming durable ABP backend**, real agent or security policy.
Side parameters do not confer real side-chat context semantics. Auth is fixture
state and token/context counts are fixed simulation data. `sessions.recovery`
means only recovery within that fake instance's lifetime. Default fake launches
therefore use a memory library/ledger; preferences and appearance still use the
normal profile. Smokes select temporary file-backed stores separately.

## Known GCC metatype warnings

On the exercised **GCC 16.2.1 / Qt 6.11.2 Release** build there are two unsuppressed
`-Wmaybe-uninitialized` diagnostics. Both originate in
`native_contract_autogen/mocs_compilation.cpp` → generated `moc_backend.cpp` →
`QtPrivate::QMetaTypeForType<Result>::getCopyCtr()` (`qmetatype.h:2499` on this host):

1. Nested `std::variant` discriminator `_M_index` (`variant:524`).
2. `std::optional<QString>` engagement flag `_M_engaged` (`optional:360`).

They concern optimized copy-construction exception cleanup for
`Result = variant<Reply, Error>`. No uninitialized application member has been
identified; that is **not proof that the diagnostics are harmless**. The build
succeeds because warnings are not errors. Neither warning is suppressed.

An earlier trial of an out-of-line defaulted copy constructor on a variant-derived
result moved one warning but did not remove it, and triggered Qt's generic
comparison registration for non-comparable payloads. That trial was reverted
rather than inventing equality, private Qt specializations or diagnostic bypasses.
`metatypeResultCopies` covers Qt create/copy/destroy for Error, Null and nested
session recovery; it passes but does not prove compiler correctness. A separately
qualified compiler/Qt fix or value-wrapper change remains necessary before a
warning-free GCC Release claim or `-Werror` build policy.

## Platform boundaries and qualification

CMake selects `src/platform/linux.cpp` on Linux and `portable.cpp` elsewhere;
`desktop.cpp` is shared Qt external-link dispatch and the explicit
`OPENGHOST_REDUCED_MOTION=0/1` override.

- **Linux:** threaded render-loop/portal defaults, desktop identity, KDE
  reduced-motion lookup. Release and offscreen/software tests are exercised;
  real Wayland/OpenGL smokes have also passed. An uninstalled executable can
  produce a nonfatal portal app-ID warning. This does not qualify every desktop,
  X11 environment or GPU.
- **Windows/macOS:** CMake has MSVC `/W4 /utf-8`, a Windows GUI target, macOS
  bundle identifier and Qt ShaderTools resources. The portable implementation
  relies on Qt for window/dialog/clipboard/URL handling, with no Linux/DBus/POSIX
  dependency in that platform path. No native system reduced-motion discovery.
  Neither platform is build/runtime-qualified yet.
- **Remaining validation:** compiler/SDK/Qt availability; frameworks/DLLs and QML/
  platform plugins; icons/signing/notarization; file dialogs and links; clipboard,
  IME/shortcuts/accessibility; DPI/fonts/shadows and graphics backends; appearance
  permissions and sandbox containers.
- **Storage/path work:** case-sensitive identity on Linux, lowercase elsewhere.
  Case-sensitive macOS volumes, Windows Unicode folding/8.3 aliases/long paths
  need review; `:` escaping is not comprehensive Windows qualification.
  `QSaveFile` replacement under antivirus/indexer locks is unqualified. Store
  permissions/ACLs, concurrent instances and profile migration need explicit
  policies. `AppDataLocation` and `AppConfigLocation` are platform-specific.
- **Crypto host:** select a vetted sealer behind `ChatSealer`, with platform work
  under `src/platform/`; no implementation is present now.

## Validation coverage and follow-up boundaries

The existing general contract suite contains 29 test functions (QtTest reports
33 passes including data rows and init/cleanup), plus a dedicated plugin contract
suite and two UI smokes. It exercises refusal, uncertain
acceptance, ordered/interleaved projection, terminal/Stop sealing, exact Retry,
steering receipts, attachment ownership, auth invalidation, approvals/host
lifetimes, library/restart/checkpoint failure, mini/folder/lock state, ledger
round trips and Qt metatype copies. See [`tests/contract.cpp`](../tests/contract.cpp)
and [`tests/smoke.cpp`](../tests/smoke.cpp).

UI smokes drive existing QML controls, both themes/four base settings pages, native
renderers, splash handoff, pins/collapse and disconnected/fake behavior. They also
drive the conditional Plugins page over an explicitly injected scripted backend. The
reported splash artifact from an earlier pass was not reproduced/diagnosed;
handoff state checks are not frame-by-frame visual qualification. A subsequent
[fresh visual audit](visual-parity.md) adds the desktop-safe headless/offscreen
pixel-comparison harness and per-fixture measurements: 0/156 exact, six manual
cases excluded. It does not qualify animation phase or every GPU effect. There
is still no Windows/macOS CI or real provider/backend test here.

Future work should be independently scoped: address storage findings and GCC
warnings; qualify a crypto host before password UI; port missing reference views
without redesign; qualify platforms; then implement/test a chosen semantic
adapter and transport separately from rendering. No backend or new frontend
feature is part of this documentation pass.
