# OpenGhost-Frontend security model

This document describes the current frontend's security boundaries and limits. Read it with the
[architecture](architecture.md), [backend contract](backend-interface.md) and
[browser host-tool contract](browser-host-tools.md). Protocol requirements on an external backend are not guarantees
that this repository can enforce. These statements are based on the implementation and existing audit findings and
resolutions; they are not a security certification.

**The configured backend is trusted local executable code. It is not sandboxed by the frontend.** Electron renderer
isolation, approval cards and chat passwords do not constrain that executable's operating-system privileges.

## 1. Trust boundaries

| Component | Authority and boundary |
| --- | --- |
| App renderer | Presents conversations, approvals, settings and browser controls. Runs local scripts without Node integration, with Electron sandboxing and context isolation. Backend/model output, file contents, URLs and site/provider labels remain untrusted presentation data. |
| App preload | Exposes a fixed `window.openghost` API through `contextBridge`, not `ipcRenderer`, general filesystem/process APIs or a command-spawning API. It is a privileged bridge, not an authorization policy for every exposed operation. |
| Electron main process | Owns native services, disk storage, backend launch/transport, PDF extraction and browser automation. It runs with the app user's authority and is part of the trusted computing base. |
| Browser guests | Separate web contents with a dedicated preload and persistent browser partition, not the app bridge. Remote/local pages are untrusted, but intentionally have browsing capabilities and can contain signed-in accounts. |
| External backend | Owns agent execution, provider/model APIs, credentials, model-facing history, prompt assembly, compaction and approval policy. It can also request frontend browser host tools. It has the app user's filesystem/network privileges independently of those tools. |

[`desktop/main.js`](../desktop/main.js) loads the local `index.html`. Navigation away from the app page and app-window
new-window attempts are prevented; allowed HTTP(S)/mailto links are delegated to the OS browser/application. This does
not establish the safety of their destinations. Markdown escapes text and restricts link schemes; approval text, provider labels, errors
and browser address/title chrome use text assignment or escaping. These defenses do not prove the absence of renderer
vulnerabilities or make supplied claims trustworthy.

[`desktop/preload.js`](../desktop/preload.js) exposes backend messaging, storage, folder UI, theme/window appearance,
file-path lookup, PDF reading, browser operations and a narrow video-metadata service. Backend subscriptions strip the
IPC event and return unsubscribe functions. A compromised app renderer could use these exposed capabilities, including
sending backend RPCs; the bridge is not an independent user-consent check.

### IPC checks and filesystem limits

Main's `fromApp` check requires a sender of type `window` and a sender-frame URL beginning with `file:`. It guards
backend, browser, PDF, video metadata, theme mutation and folder-release paths. **It does not match the exact owning
app web contents or exact HTML URL.** Store and folder pick/reveal/location handlers do not apply that check; the
window-titlebar handler has its own window/color validation. Do not describe all IPC as uniformly sender-authenticated.

Store keys are restricted to one or two lowercase alphanumeric/hyphen segments under `userData/store`. Folder release
only attempts to remove an empty directory lexically beneath `~/OpenGhost/Chats/`. PDF reading accepts absolute paths,
and folder reveal accepts path strings, without restricting them to a selected project. These are narrow services,
**not a per-project filesystem permission system**, and do not defend against a hostile same-user process modifying
the filesystem or app profile.

### Browser guest isolation

[`desktop/browser.js`](../desktop/browser.js) replaces guest preloads, disables Node integration (including subframes),
and enables sandboxing, context isolation and web security. Attachment requires the `persist:browser` partition and
an initial `http:`, `https:`, `file:`, `about:` or `data:` URL. Automation checks that the guest belongs to its requesting
host. The partition's permission handlers allow only sanitized clipboard write, fullscreen and pointer lock.

Guest isolation is not a network sandbox, a project-root restriction or a guarantee against Electron/Chromium defects.
The browser supports site popups, local pages and downloads; sites share the browser session across chats, not a
separate profile per conversation. Browser host-tool access can observe and act on signed-in pages. The guest preload's
password-submission observation sends only a hostname, not the password, but is **not proof of login**;
`signedInVerified` remains false. It is unrelated to backend provider authentication.

Browser context sent to the backend includes tab URLs/titles and sign-in hints; host-tool reads/screenshots can expose
page content. Observations are bounded and heuristic, not complete or transactional page exports. Cross-origin iframe
DOM content is unreadable; text reads omit form controls, iframe contents and shadow roots but can include hidden text.
These omissions are not a sensitive-data filter or a promise that screenshots conceal private information.

## 2. External backend and configuration trust

[`desktop/backend-host.js`](../desktop/backend-host.js) is a stdio process host, not an agent-method dispatcher. There
is one configured backend child per normal app instance, no bundled agent fallback and no remote HTTP/WebSocket backend
transport. Without successful backend initialization, agent operations are unavailable.

Configuration is executable authority:

- A nonblank `OPENGHOST_BACKEND` overrides `<Electron userData>/backend.json`. Blank/unset environment falls back to
  that file. Invalid nonblank configuration is an error, not permission to silently select another backend.
- A plain string names one whole executable, not a shell command line. Arguments use an argv array (JSON-encoded for
  the environment). Arrays preserve argument whitespace and permit empty arguments; invalid types/NUL are rejected.
- Launch uses `shell: false`. There is no shell word splitting, variable/tilde expansion, redirection or interpolation.
  Explicitly configuring a shell/interpreter still grants that interpreter its normal execution authority.
- Bare executable names use inherited `PATH`; relative paths resolve from the user's home directory, not the config
  directory. Prefer an absolute executable path. Windows `.cmd`/`.bat` files require an explicit interpreter.
- The backend starts in `os.homedir()` with the app user's privileges and **the entire inherited environment**, including
  credentials, proxy settings and runtime injection variables. A turn's `cwd` is protocol data, not a spawn-directory
  change or confinement boundary.

There is no executable allowlist, signature/ownership/permission vetting, environment scrubber or configuration-file
size limit. The renderer bridge cannot set the command, but that does not protect configuration from another process
running as the same user. Trust the executable, arguments, configuration file, launch environment and executable
search path as carefully as any program you run locally. Do not run an unfamiliar backend expecting Ask mode to
contain it; use independently provided OS isolation if such containment is required.

The frontend does not verify backend credential storage, provider TLS behavior, tool policy or data retention. Typed
API keys pass through renderer memory and ABP `auth.setKey`; successful saves clear the input, and saved key values
are not requested back or stored as frontend preferences. Actual authentication and durable secret storage belong to
the backend. UI freshness checks on auth status do not order or roll back backend credential mutations.

## 3. JSON-RPC / JSON Lines transport

The local pipe carries plaintext UTF-8 JSON Lines: one JSON-RPC 2.0 envelope per LF-terminated line. It is not an
encrypted or cryptographically authenticated channel. The selected child and OS pipe ownership establish the peer;
`connectionId`, request IDs and turn IDs are correlation identifiers, not security credentials.

| Check | Current behavior |
| --- | --- |
| Inbound line bound | 64 MiB (67,108,864 bytes), excluding LF and including CR/whitespace, checked before UTF-8 decoding/JSON parsing. Oversized input is discarded through the next newline. |
| Framing | Byte assembly tolerates split UTF-8 characters. Blank lines are skipped; invalid JSON is logged/dropped. An unterminated final line is not dispatched. |
| Outbound bounds | Same 64 MiB line limit. Node's pending stdin queue is capped at 64 MiB + 1 byte including framing. Rejected sends are not retained for retry. |
| Envelopes | A single non-array JSON-RPC `2.0` object; no batches. Named object `params` when present; string or safe-integer IDs, with numeric/string identity kept distinct. Responses contain exactly one of `result` or a typed `error`. Null IDs are only accepted for uncorrelated errors. |
| Readiness | An initialize result must carry the exact protocol version `0.1` before ordinary incoming notifications/reverse requests and outgoing requests are enabled. This is compatibility negotiation, not backend attestation. |
| Invalid/correlated traffic | Malformed matching responses reject their pending call as `protocol_error`. Invalid notifications and unknown/late responses are ignored; identifiable malformed reverse requests receive errors without executing handlers. In-flight duplicate reverse RPC IDs are rejected. |

[`backend-protocol.js`](../backend-protocol.js) validates envelopes on the host send path and renderer receive path.
The host forwards parsed malformed envelopes so the renderer can reject a correlated call. This is **not full schema
validation of every method, result, event or advertised capability**; individual consumers perform additional checks,
and browser schemas are not a general JSON Schema enforcement layer.

[`backend-client.js`](../backend-client.js) uses connection-scoped request IDs. Response deadlines are 60 seconds by
default, 15 minutes for `auth.login` and 10 minutes for `session.compact`. Timeout/abort removes the pending request,
sends `$/cancelRequest` and ignores late answers. These are response deadlines, not a maximum duration for a streamed
turn. Incoming approval requests and browser hand-back waits can wait for the user indefinitely.

Important residual limits:

- `backend:send` IPC is fire-and-forget. A host rejection (unavailable child, invalid/oversized line or full queue) is
  not acknowledged to the renderer; the RPC may only time out. Timeout is not proof of nondelivery or nonexecution.
- The host has no durable queue, replay buffer or delivery acknowledgement. Events sent while no page is listening can
  be lost. Session replay requires the backend's recovery journal.
- Line/stdin bounds are not a total memory, CPU or traffic quota. There are no general JSON-depth, IPC-queue,
  pending-request-count, event-rate, accumulated-output or approval-text limits. Large valid values and repeated
  traffic can still exhaust resources; serialization/parsing/rendering can allocate beyond the line size.
- Stderr is logged without secret redaction or a rate/size policy. Invalid stdout logs its first 200 characters, and ABP
  `log` notifications reach the developer console. Diagnostics can disclose secrets; backends must not log them.

## 4. Process lifecycle, shutdown and reaping

Main starts the configured process at app startup. Process `running` status and RPC readiness are separate states.
A renderer reload initializes a new connection to the **same child**, then recovers opened sessions. Disposing the
client removes subscriptions and aborts/rejects its outstanding work; it does not stop the process.

On backend loss, pending calls fail, reverse handlers are aborted and active chats are marked for reconciliation.
There is **no automatic backend restart or production restart command**. Fix configuration/process failures and
relaunch OpenGhost; Retry and page reload do not launch another child.

Closing the app window cancels browser jobs and PDF readings. All windows closed triggers app quit, including on
macOS. The normal quit path waits for store writes already registered in main, then sends
`{"jsonrpc":"2.0","id":"shutdown","method":"shutdown"}`, closes stdin and allows two seconds for backend exit.
It waits for the process's `exit`, not the shutdown RPC result. If still running, forced termination follows.

| Platform | Cleanup implementation and evidence |
| --- | --- |
| Linux | Backend starts detached as a session/process-group leader. On backend exit, including crash, cleanup sends SIGKILL to its process group and scans `/proc` for remaining non-zombie session members, including other process groups, for at most 20 passes. Existing regressions exercised graceful, stubborn and crashing backends with same-/separate-group descendants on Linux. |
| Other POSIX, including macOS | Detached launch and process-group kill exist, but the additional session scan depends on Linux `/proc`. Do not assume descendants in other process groups are found. macOS execution/cleanup is source-reviewed, not runtime-verified by the cited audit checks. |
| Windows | Forced shutdown invokes `taskkill /T /F` while the backend parent is alive, followed by direct-child kill as fallback. Normal/crash exit does not track/reap the already-exited parent's tree. Windows execution/cleanup is source-reviewed, not runtime-verified by the cited audit checks. |

**Cleanup is best-effort, not containment or a bounded-quit guarantee.** A descendant that creates its own session can
escape POSIX cleanup. Kill failures are ignored; cleanup does not wait for every descendant's disappearance or perform
OS `wait` reaping for arbitrary grandchildren. No supervisor/parent-death mechanism ensures cleanup after abrupt
Electron termination or machine failure.

`stop()` settles on `exit`, `close`, or a spawn `error`; overlapping stops and quits share one shutdown; events from an
older child never reach a newer one; and a backend that survives its kill is given up (reaped where possible, status
`error`) 2 seconds after the kill, so the backend part of quit is bounded by about 4 seconds. Open lifecycle issues
remain: status is emitted on child exit rather than stream close, so the same child's trailing stdout can arrive
afterward. Initialization checks disposal but lacks a process-generation check after its awaits; a narrowly timed exit
can race the ready-state transition. Store writes awaited before quit are not bounded by the shutdown deadline.

## 5. CSP and actual networking boundary

The app page's CSP in [`index.html`](../index.html) is:

```text
default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline';
img-src 'self' data: blob: https: http:; media-src 'self' data: blob:;
font-src 'self' data:; connect-src 'none'; object-src 'none';
base-uri 'none'; form-action 'none'
```

This blocks app-renderer fetch/XHR/WebSocket/EventSource/beacon-style connections, remote/inline scripts and eval-style
script execution. Inline styling is intentionally permitted. CSP supplements safe DOM construction; it does not make
HTML injection safe, and it does not govern Electron main, the external backend, browser guests or the OS browser.

**Provider/model API traffic must remain in the external backend.** The frontend has no provider API client, agent
loop or provider OAuth executor. Do not add one to the renderer, preload, main process or browser host-tool service,
or weaken CSP to accommodate one. This architectural rule is **not a provider-hostname firewall**.

Intentional frontend networking includes:

- **Images and galleries:** CSP allows HTTP(S) images from any host. [`media-embed.js`](../media-embed.js) automatically
  loads gallery previews matching its selected Bing, YouTube, Wikimedia and Google thumbnail patterns; other gallery
  images wait for a click. That application-level policy is not a global network allowlist or a privacy guarantee.
- **Link icons:** [`link-chip.js`](../link-chip.js) requests favicon images from DuckDuckGo's icon service, disclosing
  the requested hostname (including a base-domain fallback). This can happen when a link is displayed, without opening it.
- **Video cards:** YouTube thumbnails load as images. Main's `videoInfo` validates an 11-character video ID and requests
  YouTube oEmbed metadata with `net.fetch`; it is not an arbitrary-URL fetch IPC service. Video cards link out rather
  than embedding a remote player in the app page. App-page media elements are limited to self/data/blob sources.
- **Browser guests:** browsing, search, site resources, cookies, popups and downloads use their separate browser
  session. Local/file/data pages are also supported. Downloads go to the OS Downloads directory.
- **External links:** allowed URLs open in an OS application outside the app page's CSP.

Images, metadata and browsing requests reveal ordinary network metadata and requested URLs to their destinations.
Model-supplied URLs can influence those requests. HTTP images are permitted without transport encryption. There is no
whole-application offline mode, provider-domain deny rule, private-address egress filter or assurance that opening a
cached chat causes no network access. A provider host could be contacted by an allowed image/browser request even
though the frontend does not implement its model API.

## 6. Approval cards, browser authority and cancellation

**Approval cards are decision UI, not an OS security boundary or an independently verified account of execution.**
The backend decides which actions require approval and what Ask/Auto/Full mean. It supplies the wording, effects,
commands/diffs and arguments. [`approval-card.js`](../approval-card.js) validates presentation fields and renders text;
it neither analyzes command safety nor verifies that later execution matches the card.

With no presentation object, the card shows the tool name and JSON arguments. A supplied but incomplete presentation
object can suppress that fallback; missing/invalid `reveal` can hide supplied details. Diff previews show at most
14 lines per side, while full strings are still processed. There is no comprehensive presentation-size limit. Users
must not infer a complete action description from a short title or preview.

[`chat.js`](../chat.js) routes reverse requests by session, waits for pending recovery/start identity, checks the
accepted turn and rejects stale turns and repeated supplied approval/host-call identities. No running turn yields a
cancelled result. These checks prevent misattribution/duplicate handling; they do not authorize an otherwise hostile
backend. Missing per-method fields are not all uniformly rejected by the envelope validator.

Sending a new input supersedes pending cards with deny; Stop/request cancellation denies them with cancellation
semantics. A backend `approval.resolved` can settle an existing card. Changing permission mode does not locally
auto-allow pending approvals. Remembering expanded details is a display preference, not remembered authorization.
Cards are dismissed after settlement and are not a durable approval audit trail. **Normal tool calls are not rendered
as tool cards, and reasoning is not rendered.** A working indicator is not a complete execution transcript.

Browser `host.tool` calls are not checked against a local approval ledger. The backend must obtain any required
approval **before** requesting the browser action. The frontend advertises browser tools, not a shell/general-tool
runner. Browser jobs are serialized across chats, pin their target, and check stable tab/page identities before input.
Take Control cancels current steps; hand-back returns a fresh observation instead of replaying the interrupted step.

Stop freezes local output and cancels approvals/browser work, requests `turn.cancel` when the remote turn ID is known,
and cancels outstanding turn RPCs with `$/cancelRequest`. Correlated late usage can still update accounting without
reviving output. Stop does not wait for confirmed remote termination. A start stopped before acknowledgement, an
unacknowledged write, reload during a host action, or timed-out browser input can have an **unknown or partial outcome**.
Already-issued input/JavaScript and backend side effects cannot be rolled back. Some attachment/PDF preparation waits
are not interruptible by Stop; response deadlines do not bound all preparation or
turn activity. Recover/re-observe before retrying uncertain work, never assume cancellation means nothing happened.

## 7. Session ownership, display caches and persistence

The backend owns durable sessions, session incarnations, accepted input, model-facing history and recovery journals.
The frontend owns its chat index, display messages/previews, local annotations, preferences and observed usage ledger.
Main/mini sessions have separate identities; the mini ID is `<chat id>:mini`.

[`library.js`](../library.js) allowlists display fields on read/save, including recovery identifiers. It drops arbitrary
provider/tool histories and full attachment input payloads. Display messages are **never submitted as model history**;
a visible cache is not proof that the backend session exists. Nor is the projection a secret detector: user/assistant
text, images and notes can themselves contain sensitive information.

Saved sessions must reconcile with `session.get` before continuation. Exact session/turn identities, create-only
`sessionVersion: null`, sequence gates and recovery revisions prevent silent replacement or blind resend of uncertain
input when the backend follows the contract. Required index/display checkpoints are awaited before start/retry/steer;
write failure blocks dispatch. Missing sessions, unsupported recovery or invalid snapshots leave the cache display-only
with an error, not permission to rebuild backend history from it.

These checks depend on truthful, durable backend acceptance/deduplication/replay. They are not exactly-once transport
or side-effect guarantees. Session sequence gaps are allowed and not automatically repaired; replay does not reconstruct
a complete billing ledger. The frontend cannot verify the backend's persistence or retention implementation.

On desktop, [`chat-store.js`](../chat-store.js) uses `<Electron userData>/store`: index, main/mini display caches,
standing instructions/pinned-file payloads and usage. Browser-only storage falls back to prefixed localStorage without
providing an alternate agent connection. Preferences/catalog/media metadata also use renderer localStorage; browser
cookies/site state use `persist:browser`. Workspaces/project files are separate from all of these caches.

Main serializes writes per file and uses temporary-file rename; there is **no explicit `fsync` or multi-file transaction**.
Ordinary display saves are best-effort. Page-exit flushes and waiting for already-registered main-process writes do not
prove every pending renderer change reached disk or survived power loss. Local file-removal errors can also be ignored.

Full chat deletion awaits backend acknowledgements for both main and mini IDs before removing local records. Folder
deletion commits acknowledged children individually and retains failures for retry. **Mini-chat Clear awaits deletion
of only the mini session before clearing its local display/cache; failures preserve the chat and surface an error.**
The parent session is untouched. Acknowledgement is a
backend claim, not verified secure erasure of backend files, backups, provider-held data or logs. Local removal is not
secure disk wiping, and generated/project files are not recursively deleted.

## 8. Chat-lock encryption: local view/cache only

[`chat-lock.js`](../chat-lock.js) derives a non-extractable AES-256-GCM key using PBKDF2-HMAC-SHA-256 with 600,000 iterations
and a random 16-byte per-chat salt; each seal uses a random 12-byte IV. Passwords are NFC-normalized and not stored.
Successfully sealed title/display bodies receive AES-GCM confidentiality and integrity protection under that key.
Security against offline password guessing still depends on password strength.

Protection is intended for the title and main/mini **local display cache**, not the backend session. Unlocked keys,
titles and cleartext live in renderer memory. Relocking closes the view; an active reply can retain its cleartext/key
until it finishes saving. This is not immediate memory erasure, backend locking or backend cancellation.

The scope excludes:

- backend history, journals, credentials and logs; turn/context payloads sent over ABP;
- standing instructions and pinned-file payloads, the global usage ledger and renderer preferences/media metadata;
- browser cookies/site data, downloads, clipboard copies and project/workspace/source files;
- index metadata such as IDs, timestamps, model/folder information and workspace names. A workspace name may expose
  words from the original chat title even when the displayed title is sealed.

**Changing protection is not transactional.** Protect writes lock metadata before sealing the main body; failure or a
crash between them can leave a lock-labelled chat with plaintext messages on disk. Unprotect writes cleartext before
removing lock metadata. Mini-cache resealing catches errors and is best-effort. A lock icon alone is therefore not proof
that every associated file is encrypted. Replacement writes do not securely erase previous plaintext, temporary files,
filesystem snapshots or backups.

There is no password-recovery mechanism, whole-profile/end-to-end encryption or protection against a compromised app,
backend, OS or same-user code inspecting an unlocked session. No frontend password operation changes backend storage
protection; verify that separately with the selected backend.

## 9. Attachments and local-path exposure

Files selected, dropped or pasted are prepared locally by [`attachment-reader.js`](../attachment-reader.js), then sent
as structured ABP input. Preparation is best-effort, not malware scanning or a guarantee that a document is safe.

- Up to 20 composer attachments are allowed. Text/Office reading has a 20,000,000-byte source guard and a
  400,000-UTF-16-unit text limit; spreadsheet extraction caps rows. Compressed/decoded resource costs are not fully
  bounded by these input/output limits.
- Images are decoded and may be resized/re-encoded (2,560-pixel maximum side; suitable originals retained up to
  6,000,000 bytes). Original reported dimensions can differ from encoded dimensions. This is not a metadata-stripping
  guarantee; a retained original can preserve its metadata.
- [`desktop/pdf.js`](../desktop/pdf.js) serializes extraction through a hidden sandboxed Electron PDF-viewer helper.
  Sources are absolute paths or bytes, bounded at 256 MiB. Byte sources create temporary files with best-effort cleanup
  in `finally`, not guaranteed cleanup after a crash or secure erasure. Extraction is selectable text, not OCR; encrypted
  or unreadable files need not yield text. PDF readings are cancelled on app-window closure, not by a per-turn PDF API.
- Videos provide path/metadata when available, not uploaded video bytes; a generated poster is display-only. Generic
  unreadable files can still provide metadata and a local path.

`pathOf(File)` uses Electron `webUtils.getPathForFile`; a path may be absent. A supplied path reveals local filesystem
layout to the backend and is neither an uploaded-file handle nor a scoped filesystem capability. It does not guarantee
that the file still exists, is unchanged, or is accessible in the backend's environment. The trusted backend already
has user-level filesystem access and decides whether/how file data reaches a provider. Ordinary prepared chat images
are self-contained without a source path; pinned user-context images can include one.

The chat cache retains display previews/notes, not full extracted files or general file/video paths for resending.
[`user-context.js`](../user-context.js) is different: it stores prepared pinned-file snapshots, including paths, outside
chat-lock protection and supplies them with starts/retries. Its 8,000-unit instruction, 20-file and 200,000-unit combined
text limits are local payload limits, not model token budgets. Files are not continuously watched. Backend attachment
capability/`maxBytes` flags do not enforce input limits here; the backend must validate its own supported inputs. The
whole serialized request still must fit the transport bound.

## 10. What the frontend does not guarantee

Do not rely on this frontend for:

- containment of the configured executable, per-project filesystem access control, or approval enforcement outside UI;
- complete tool/reasoning visibility, a durable approval log, or proof that an approved description matches execution;
- whole-application network isolation, provider-host blocking, anonymous/offline media display or provider privacy;
- confirmed remote cancellation, rollback, exactly-once effects, automatic restart or guaranteed descendant termination;
- backend durability/credential protection, power-loss-proof local storage, verified deletion or secure erasure;
- whole-session encryption, encrypted browser/workspace/context data, immediate memory wiping or password recovery;
- comprehensive payload validation, denial-of-service resistance, safe parsing of every file/page, or immunity to
  Electron/Chromium/OS vulnerabilities.

## 11. Security-sensitive contributor invariants

1. **Keep agent authority external.** Provider/model networking, credential persistence, prompt policy, general tools
   and model-history assembly belong in the backend. Do not turn main/preload into a provider client or generic runner.
2. **Preserve Electron boundaries.** Keep sandbox/context isolation, no renderer/guest Node integration, fixed preload
   APIs, event stripping, guest ownership checks and guarded IPC. Do not give browser pages the app preload. New
   privileged IPC needs explicit sender/frame and argument validation; do not assume the current broad check is exact.
3. **Treat supplied text as data.** Preserve escaping/text assignment and URL restrictions for backend/model/file/site
   content, including approval fields and browser chrome. Use safe maps/keys for opaque provider IDs. CSP is not a
   substitute for avoiding HTML injection.
4. **Preserve the actual network policy.** Keep `connect-src 'none'` and local scripts; do not remove intentional
   image/video/browser services or describe them as isolated. A new network surface needs an explicit security review.
5. **Keep configuration privileged and launch literal.** Do not add renderer-controlled executable selection, implicit
   shell parsing, silent fallback on invalid configuration or hidden backend restart loops.
6. **Keep framing and correlation defenses.** Enforce byte bounds before parsing, discard oversize through newline,
   bound stdin writes, preserve envelope/version checks, typed/connection-scoped IDs, duplicate reverse-ID rejection,
   subscription disposal and response deadlines. Never turn a timeout into a nondelivery claim.
7. **Bind work to its owner.** Preserve session incarnations, turn/message/input identities, sequence/replay boundaries,
   bounded early events and reverse-request deduplication. Stopped/terminal turns must not resume output from late events.
8. **Do not replay uncertainty.** Require checkpoints before dispatch and reconciliation before uncertain retry; never
   reconstruct backend input/history from slim display caches or automatically repeat an unknown-outcome browser action.
9. **Preserve browser targeting/control checks.** Serialize calls, pin stable targets, check page identity and cancellation
   after waits/before further input, and release every turn's jobs/hand-back waiters. Do not equate cancellation with undo.
10. **Keep approval policy honest.** No local auto-allow on mode changes, no tool-name-specific safety inference and no
    claim that browser calls are independently approval-enforced. Backend presentation is a claim, not verified policy.
11. **Keep storage/encryption scope explicit.** Apply the display allowlist to plain and sealed caches, propagate required
    checkpoint failures, preserve write ordering and deletion acknowledgement rules, and label locks as local-view/cache
    protection. Never promise backend encryption or erasure based on frontend state.
12. **Preserve platform-qualified lifecycle behavior.** Retain shutdown/EOF handling and descendant cleanup, and keep
    Linux evidence separate from unverified macOS/Windows behavior. Tests with mocks are not platform guarantees.

## 12. Evidence and review scope

Relevant findings and resolutions are in the
[backend-boundary audit](reviews/2026-10-04-frontend-only-backend-boundary-audit.md), especially F01–F10 and F14–F21.
The source links above describe the implemented controls and remaining limits, not a security certification.
Existing focused regression coverage includes:

- [envelopes](../test/backend-envelope.test.js), [transport bounds](../test/backend-transport.test.js),
  [configuration](../test/backend-config.test.js) and [process cleanup](../test/backend-host.test.js);
- [turn ownership](../test/turn-identity.test.js), [cancellation](../test/cancellation.test.js),
  [browser lifecycle](../test/browser-lifecycle.test.js) and [untrusted text](../test/untrusted-text.test.js);
- [recovery](../test/session-recovery.test.js), [deletion](../test/deletion.test.js),
  [local-lock scope](../test/lock-ui.test.js) and [CSP/source boundary](../test/boundary.test.js).

The recorded process-tree/configuration runtime checks are Linux-tested; macOS/Windows equivalents are not established
by them. Source assertions and mocked tests do not establish whole-application network isolation, backend conformance
or cross-platform runtime security.
