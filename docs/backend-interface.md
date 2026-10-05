# OpenGhost-Frontend backend contract

This is the public contract between OpenGhost-Frontend and an external backend. It describes the implemented Agent
Backend Protocol (ABP), identified on the wire by `protocolVersion: '0.1'`. Method signatures below describe JSON
payloads, not JavaScript calls; optional fields may be omitted. Requirements on backend persistence, ordering and
identity are necessary for the frontend's existing recovery behavior, even where the frontend cannot enforce them.

## Responsibilities and intentional frontend behavior

- **The backend is authoritative for the model catalog, canonical session configuration and model-facing session
  history.** It owns provider access, credentials, prompts, the agent loop, tool execution policy and compaction.
- **The frontend keeps display state/cache:** its chat index, rendered messages, attachment previews, local annotations,
  preferences, recovery checkpoints and usage ledger. It never submits that cache as model history. Local renames and
  diagram edits do not rewrite backend history. Chat locking protects the local view/cache, not backend storage.
- **Reasoning events are not rendered.** `reasoning.delta` does not produce visible reasoning.
- **Normal tool calls are not shown as tool cards.** `tool.started` can show the ghost working indicator;
  `tool.progress` is ignored and `tool.completed` closes local call bookkeeping. Approval cards are separate.
- **There is no automatic backend restart.** Retry reconciles or retries a turn; it does not launch a process.

The boundary is:

```text
OpenGhost UI
  → window.Backend                         backend-client.js
  → window.openghost.backend               desktop/preload.js
  → Electron IPC                           backend:send / backend:message / backend:status
  → desktop/backend-host.js
  → external process stdin/stdout           UTF-8 JSON Lines, JSON-RPC 2.0
```

The desktop host frames and relays messages; it does not run the agent or interpret its methods. The built-in browser
is a frontend host-tool service the backend may call. Without a configured, initialized backend, cached chats remain
viewable, but backend operations are unavailable.

## Connecting a backend

### Command configuration

The desktop app resolves the backend command in this order:

1. A nonblank `OPENGHOST_BACKEND`, containing an executable string or a JSON array of executable and arguments.
   For example, in a POSIX shell:
   `OPENGHOST_BACKEND='["/opt/backend/bin/backend", "abp"]' npm start`.
2. `backend.json` in Electron's `app.getPath('userData')`, with a `command` field:

   ```json
   { "command": ["/opt/backend/bin/backend", "abp"] }
   ```

The usual user-data locations are `~/.config/OpenGhost/` on Linux, `~/Library/Application Support/OpenGhost/` on macOS
and `%APPDATA%\OpenGhost\` on Windows. The file's `command` accepts an executable string, an array, or a JSON-encoded
array string.

Unset, empty and whitespace-only environment values fall back to the file. A nonblank override wins, even if the file
is invalid. An invalid override reports `OPENGHOST_BACKEND: ...` without falling back. A missing file means no backend;
an existing malformed/unreadable file or invalid `command` reports `backend.json: ...` rather than silently disabling it.

Command parsing is literal:

- A plain string is the entire executable name/path, with outer whitespace trimmed. Internal spaces are preserved:
  `/opt/My Backend/backend` is a path; `/opt/backend --flag` is also one executable name, not an argument list.
- Arrays must be nonempty. The executable must be a nonblank string; arguments must be strings and may be empty.
  Array elements retain whitespace. No element may contain NUL.
- A trimmed string beginning with `[` must parse as a JSON array. Use an array for a literal executable beginning
  with `[` or having significant outer whitespace.
- The host uses direct `spawn`, **without a shell**. There is no word splitting, quote removal, variable/tilde expansion,
  globbing, redirection, pipes or command substitution. JSON escaping still applies, for example
  `["C:\\Tools\\backend.exe", "--abp"]`.
- Bare names use the inherited `PATH`. Relative paths resolve from the **user's home directory**, not the repository,
  application directory or configuration directory. Prefer absolute paths; desktop and terminal launches can inherit
  different environments. Executable formats and permissions follow the OS. Windows `.cmd`/`.bat` files require an
  explicitly configured interpreter; the host does not choose one. If trusted configuration explicitly selects a
  shell, that shell's interpretation applies.

### Trust, environment and working directory

The configured backend is **trusted local code, not a sandboxed plugin**. It inherits the app user's privileges,
filesystem/network access and the entire app process environment at spawn time, including credentials, proxy settings,
`PATH` and runtime injection variables such as `NODE_OPTIONS`. There is no environment scrubber, executable allowlist or
configuration ownership/permission check. Ask/Auto/Full and provider separation are not OS privilege boundaries.

The app sets the backend's working directory to `os.homedir()`. A standalone `BackendHost` without a `cwd` inherits
its parent's directory. A turn's `cwd` is protocol data; it does not change the process spawn directory. The renderer
bridge cannot change the configured command.

Use **stdout only for protocol messages**. Write diagnostics to stderr; the host logs them with a `[backend]` prefix
without secret redaction. Do not log credentials or other secrets.

### Startup, reload, exit and shutdown

The host starts the configured process on app startup. Process status is carried over IPC, not as ABP events:
`none`, `stopped`, `running` (with `pid`), `error` (with `error`), or `exited` (with `code` and `signal`). Configuration,
spawn, handshake and exit diagnostics are retained in the frontend's unavailable/error messages.

A page reload initializes a new renderer connection on the **same process**. It does not reset backend sessions.
On process loss, pending calls fail, reverse handlers are aborted, active chats end with an error, and sessions require
reconciliation before continuation. After a crash or configuration change, correct the problem and **relaunch
OpenGhost**. There is no production restart/reconnect command or automatic retry loop.

On app quit, the host sends the following request, closes stdin, and allows 2 seconds for process exit:

```json
{"jsonrpc":"2.0","id":"shutdown","method":"shutdown"}
```

The backend should answer with `result: null` and exit. The host does not wait for the RPC response to decide when to
terminate it. On POSIX it force-kills the backend after the grace period and reaps its process group on exit; Linux also
scans its session for descendants in other process groups. Descendants that deliberately create another session are
outside that cleanup. On Windows the forced shutdown path uses `taskkill /T /F` while the backend is still alive;
normal/crash exit does not provide the POSIX session-reaping guarantee. If the backend is still there 2 seconds after
the kill, the host gives it up (reporting status `error`) so quitting cannot hang on it.

The preload's `onMessage` and `onStatus` subscriptions return unsubscribe functions. Replacing a client requires
`BackendClient.dispose()`: it detaches subscriptions, rejects pending calls, aborts reverse handlers and clears local
listeners/handlers without stopping the process. Re-evaluating `backend-client.js` disposes the old `window.Backend`.

## Transport and JSON-RPC envelopes

### JSON Lines and limits

Communication is JSON-RPC 2.0 over UTF-8 JSON Lines: **one JSON object per line, terminated by LF (`\n`)**, in each
direction. Strings containing newlines use JSON escapes. There are no batches. Blank lines are ignored; non-JSON lines
are dropped and logged. The host assembles bytes before UTF-8 decoding, so multibyte characters may cross read chunks.
An unterminated final line is not dispatched.

- The maximum line is **64 MiB (67,108,864 UTF-8 bytes)**, excluding LF but including whitespace and CR.
- Oversized inbound lines are discarded through the next newline before decoding/parsing. The next line can be read
  normally. Parsed values, including malformed envelopes, reach the renderer for validation.
- Outbound messages must pass the same envelope validator and line-size limit. Node's pending stdin write queue,
  including all framing newlines, is capped at **64 MiB + 1 byte**. Accepted writes drain in order; there is no extra
  host retry queue.
- The host's `send` returns `false` for unavailable, invalid, oversized or queue-full sends, and reports a failed stdin
  write (for example `EPIPE` from a backend that closed its stdin) through its write callback. Renderer `backend:send`
  is an IPC invoke that resolves `false` in either case, and the RPC client then rejects **that request** at once with
  `backend_unavailable` instead of waiting for its deadline. `true` means only that the line was written to the pipe;
  a timeout is still not proof that the backend did not receive or accept a request.

### Envelope validation and identity

Both peers can send requests. Requests have an `id`; notifications do not. Named `params`, when present, must be a
non-null, non-array object. IDs are strings or JavaScript-safe integers and match exactly, including type (`7` is not
`"7"`). Frontend request IDs are `"<connectionId>:<counter>"`.

```json
{"jsonrpc":"2.0","id":"connection:2","method":"models.list","params":{}}
{"jsonrpc":"2.0","id":"connection:2","result":[]}
{"jsonrpc":"2.0","method":"models.changed","params":{}}
```

A valid envelope is a non-array object with `jsonrpc: '2.0'`. Requests/notifications have a string `method` and no
`result`/`error`. Responses have an ID, no `method`/`params`, and exactly one of:

- `result`, which may be any JSON value, including `null`;
- `error`, an object with a safe-integer `code` and string `message`, optionally with `data`.

A null response ID is accepted only for an uncorrelated RPC error and is ignored. Malformed notifications, batches,
primitives and uncorrelatable responses are ignored without dispatch. A malformed response with a matching pending ID
rejects that call locally as `protocol_error`; unknown/late IDs never settle another call. Identifiable malformed reverse
requests receive `-32600`/`invalid_request`, or `-32602`/`invalid_request` for non-object params, before any handler runs.
An invalid request ID is returned as null. Unknown reverse methods receive `-32601`/`unsupported`.

A reverse request reusing an ID still being answered fails with `-32600`/`duplicate_request` and does not execute twice.
Connection IDs separate renderer lifetimes; they are not session or turn IDs.

### Request deadlines and RPC cancellation

Every frontend RPC request has a deadline for its **response**, not for a subsequent event stream:

| Request | Deadline |
|---|---:|
| `auth.login` | 15 minutes |
| `session.compact` | 10 minutes |
| All other renderer requests, including `initialize`, `turn.start`, `turn.retry` and `session.configure` | 60 seconds |

On timeout the client rejects locally with `timeout`, sends `$/cancelRequest { id }`, and ignores a late answer.
Aborting a local request also sends that notification, rejects locally with `AbortError`, and ignores late responses.
Either peer may cancel a request it sent:

```json
{"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":"connection:2"}}
```

The backend's cancellation of a reverse request aborts that handler only. A cancelled request is still answered while
the connection remains live; cancellation is not a rollback guarantee. Disposing/closing the connection abandons old
replies. The frontend does not apply the table's deadlines to incoming reverse RPCs: approvals and browser hand-back
may wait for the user. Browser operations have separate deadlines described under [Host tools](#host-tools).

## Initialization and capabilities

Once the process is running and the page's scripts are loaded, the frontend sends:

```ts
initialize({
  protocolVersion: '0.1',
  connectionId: string,             // fresh for each BackendClient / renderer lifetime
  client: {
    name: 'OpenGhost', version: '1.3.0',
    platform: 'linux' | 'darwin' | 'win32' | 'web', locale: string,
  },
  host: {
    tools: HostToolSchema[],
    renderGuide: string,
    attachments: { localPaths: boolean },
  },
}) → {
  protocolVersion: '0.1',
  backend?: { name: string, version: string, platform?: string },
  capabilities?: Capabilities,
}
```

`host.tools` contains the available browser tool schemas, or `[]` without the desktop browser bridge.
`host.renderGuide` describes supported display syntax; it is not an agent-behavior prompt. `localPaths` reports whether
the desktop file-path bridge is available, not a promise that every attachment has a path.

The result must be an object with the exact string `protocolVersion: '0.1'`. Missing/non-string versions produce a
protocol error; another string version is unsupported. Either failure leaves the client unavailable and retains a
handshake diagnostic. Until initialization succeeds, ordinary outbound and reverse requests fail with
`backend_unavailable`, and backend event notifications are not dispatched. Backend metadata defaults to null and
capabilities to `{}` if omitted; only the protocol version is strictly checked at this handshake.

`Capabilities` is an object with nested fields. The frontend checks these paths (absent values are false;
implementations should send booleans):

| Capability | Frontend behavior |
|---|---|
| `auth.providers` | Reads `auth.providers` for provider settings. Without it there are no provider connection rows. |
| `compaction.manual` | Offers manual compaction for an idle, reconciled chat with history. |
| `sessions.delete` | Required for acknowledged chat/folder deletion and mini-chat Clear. |
| `sessions.recovery` | Required for starting/retrying turns and continuing saved sessions through `session.get`. Without it chats are display-only. |
| `usage.limits` | Reads `account.limits` for connected providers in Settings → Usage. |

Other capability fields are retained in `Backend.capabilities` but do not gate frontend behavior. In particular,
turn cancellation/steering/retry, approval modes, mini chats, attachment submission and reasoning/tool rendering do not
change based on corresponding capability flags. Model-specific thinking/vision/context metadata is handled separately.
Do not advertise a capability as a substitute for implementing the methods this frontend calls.

## Providers, models and authentication

```ts
auth.providers() → Provider[]
auth.setKey({ provider: string, key: string | null }) → ProviderStatus
auth.login({ provider: string }) → ProviderStatus
auth.cancel({ provider: string }) → ProviderStatus
auth.logout({ provider: string }) → ProviderStatus
models.list({}) → Model[]
account.limits({ provider: string }) → AccountLimits | null

type Provider = {
  id: string, name: string, group?: string, limits?: boolean,
  methods: (
    { type: 'apiKey', label: string, hint?: string, url?: string, placeholder?: string }
    | { type: 'oauth', label: string, hint?: string, action?: string }
  )[],
  status: ProviderStatus,
}
type ProviderStatus = {
  connected: boolean, checking?: boolean, waiting?: boolean, keySaved?: boolean,
  account?: { email?: string, plan?: string }, error?: AbpError | string,
}
type Model = {
  id: string, provider: string, name: string,
  contextWindow?: number, vision?: boolean,
  thinkingLevels?: string[], defaultThinking?: string,
}
type LimitWindow = { seconds: number, used: number, resets: number }
type AccountLimits = {
  plan?: string,
  windows?: LimitWindow[],
  models?: { name: string, windows: LimitWindow[] }[],
  balances?: { currency: string, total: number }[],
  credits?: { unlimited?: boolean, balance?: string },
}
```

`group` labels a provider's models in the picker; `limits` reserves UI space for its plan limits. Limit-window `used` is
a percentage (0–100) and `resets` is epoch milliseconds. `auth.login` resolves when sign-in completes or is cancelled;
credential storage, provider communication and sign-in implementation belong to the backend.

Provider IDs are opaque strings, not DOM IDs or tool names. The provider UI validates catalog/status fields, requires
boolean `connected`, and shows at most one `apiKey` and one `oauth` method per provider. Unknown method kinds are
ignored; there is no method-ID selector. API-key links must use HTTPS. `action` is the OAuth button label, not a command.

The frontend sends a trimmed key after typing settles; an emptied field sends `key: null` to remove it. Saved keys
must **not** be returned. `keySaved: true` makes the empty field show “Saved · type to replace”.

Global backend notifications are:

```ts
auth.changed   { provider: string, status: ProviderStatus }
models.changed { provider?: string }
```

`auth.changed` is an unversioned invalidation, not an ordered status snapshot. A valid notification triggers fresh
provider/model reads; if it races a local auth mutation, the frontend reads again after that mutation settles.
`models.changed` refreshes both providers and models. The backend must return current authoritative provider statuses
and order its own credential mutations. Frontend freshness guards prevent stale UI updates, not backend-side effects.

Models and their capabilities come only from the backend. The UI caches the last catalog, refreshes on notifications
and settings reads, and refreshes stale models when the picker opens (10-minute freshness interval).

- Requests use the exact `Model.id` and `Model.provider` separately; the local selection key combines them as
  `provider:id`. Removed/unavailable selections are retained rather than silently replaced by another model.
- Only `vision: true` advertises vision; `false` explicitly reports no photo support. Missing/non-boolean vision stays
  unknown (`null` in the UI catalog), with neither claim shown in the picker. Old caches that lost this distinction are discarded.
- An absent, nonnumeric, nonfinite or nonpositive context window is unknown (`0` in the UI), not an inferred limit.
  Unknown windows show no fullness percentage. Catalog windows are display metadata, never outgoing token-budget or
  compaction overrides; the backend owns budgeting and compaction.
- Missing/empty/malformed `thinkingLevels` means no selectable levels; non-string and blank entries are ignored. A
  supported user preference takes precedence over an advertised `defaultThinking` that belongs to those levels;
  otherwise `thinking` is omitted. No default level or generic effort list is inferred.
- A canonical `session.configure` result can set or clear thinking independently of the advertised selectable levels.
  Backend defaults are display state, not saved user preferences.

## Turns and input

### Start and acceptance

```ts
turn.start({
  sessionId: string, sessionVersion: string | null, clientTurnId: string,
  input: Input, ...SessionParams,
}) → { turnId: string, sessionVersion: string }

type SessionParams = {
  model: string, provider: string,
  thinking?: string,
  permissionMode: 'ask' | 'auto' | 'full',
  cwd: string, title: string,
  userContext: UserContext,
  host: { browser: BrowserState | null },
  side?: { parent: string, parentBusy: boolean, moved: boolean },
}
type Input = { text: string, attachments: Attachment[] }
```

The main session ID is the frontend chat ID. A mini-chat uses `<chat id>:mini`; `side.parent` names the main session,
`parentBusy` reports whether it is running, and `moved` reports that the parent changed since the mini-chat's last input.
The backend decides how to use that context; the frontend does not copy parent model history into the request.

`cwd` is the selected project folder or the chat's own folder under `~/OpenGhost/Chats/`. That folder may not yet exist;
the backend should create it on first use if needed. `title` is the current display title. Quotes remain in `input.text`
as `> ` lines. `userContext` accompanies starts and retries; steering sends input and current browser context instead.

The start response acknowledges acceptance, **not completion**. Return it within the request deadline; events may
arrive before that response, but do not replace it. Stream through `turn.completed`. The `turnId` must be a nonempty
string and must agree with an early `turn.started`. Every start acknowledgement must also return a nonempty string
`sessionVersion`. For an existing-session start it must exactly match the requested incarnation; for a create-only
start (`sessionVersion: null`) it identifies the newly created incarnation. The frontend validates both identities
before adopting the returned version, and rejects acknowledgements if the local turn or incarnation changed while
waiting. A `turn.completed` that arrives first ends the turn locally at once without waiting for the response; the
start stays pending until its response or deadline, keeps its pending markers and offers no Retry meanwhile, and a
following start in that chat waits for it before reading the incarnation. A late valid acknowledgement is then
adopted; a late invalid one, or none by the deadline, fails closed as below. Missing, malformed or mismatched versions fail closed and require reconciliation, even if completion events
arrived first; they never replace the current version or authorize an automatic resend.

Before dispatching start/retry/steer, the frontend durably saves its chat index and display checkpoint with the relevant
client IDs. Storage failure prevents dispatch. The backend must durably record accepted input and its display-event
journal **before acknowledging acceptance or emitting events**. A new accepted start also needs live event delivery to
its initiating connection; it is not preceded by `session.get` for a new main chat.

### Retry

```ts
turn.retry({
  sessionId: string, sessionVersion: string, clientTurnId: string,
  failedTurnId: string, ...SessionParams,
}) → { turnId: string }
```

Main-chat and mini-chat Retry distinguish three cases:

1. **Never dispatched:** an offline, input-preparation or checkpoint failure retains the original full input/preparation
   context in memory. An explicit retry sends `turn.start`, not `turn.retry`; display attachment previews are never
   reconstructed as model input.
2. **Dispatched but uncertain:** an RPC error, invalid/missing acknowledgment or connection loss requires `session.get`
   with the saved `clientTurnId`. Retry does not blindly resend. Missing session/turn history fails closed.
3. **Accepted and terminally failed:** Retry sends `turn.retry` with the exact `failedTurnId`, a new `clientTurnId` and
   the same session incarnation, without new `input`. The backend must reject an unknown or non-failed target, never
   substitute its latest turn. An undelivered retry retains the same failed-turn target.

Retry requires retained context and an unchanged session incarnation. `retryable: false`, `action: 'none'`,
`invalid_request` and `invalid_params` suppress the turn-error Retry action. The separate reconciliation notice offers
a recovery Retry that calls `session.get`.

`SideChat.resume` forwards the same retry intent to `Chat.resume`: reconciled mini-chat Retry dispatches
`turn.start` for retained undelivered input or `turn.retry` for an accepted failure, using `<chat id>:mini` and the
same session/turn identity checks. The parent session is not retried.

### Steering

```ts
turn.steer({
  sessionId: string, turnId: string, clientInputId: string,
  input: Input, host: { browser: BrowserState | null },
}) → { accepted: boolean }
```

Messages sent during an ordinary running turn are steered into that turn. Input preparation and RPCs are serialized in
submission order, including waiting for attachment readiness and the start identity. Sending a new input denies
pending approval cards with `reason: 'superseded'` and releases browser hand-back waits with `reason: 'message'`.

`accepted: true` acknowledges durable acceptance; `input.accepted` places the user bubble in the reply. Emit acceptance
events in accepted-input order, before output that depends on them. `accepted: false` or an RPC error is shown on the
input bubble, retained for display, and never silently resent. Accepted steering must include `input: DisplayInput` in
its recovery journal so the bubble can be reconstructed when absent from local storage.

Turn completion/error/Stop aborts steering RPCs and drains preparation/start waiters. Inputs without an acceptance
event remain visible as unconfirmed; they are not assumed accepted or carried into another turn automatically.
Inputs submitted during standalone compaction/model switching instead wait for that operation, then become one new
`turn.start` input: text joined with blank lines and attachments concatenated in order. They retain separate display
bubbles, and are not concurrent steering requests.

### Turn cancellation

```ts
turn.cancel({ sessionId: string, turnId: string }) → null
```

Stop freezes local output immediately, aborts outstanding turn/steering work, denies pending approvals, and cancels
browser work/waits. If the backend turn ID is known, the frontend sends `turn.cancel`; it does not wait for the reply
to freeze the UI. An outstanding start RPC is also cancelled with `$/cancelRequest`; without a known `turnId`, that
is the only remote cancellation sent. The backend should finish the turn with `turn.completed { status: 'cancelled' }` and retain its recovery record.
An uncertain stopped turn is reconciled before continuation, not silently restarted.

In the main chat, Escape invokes Stop when not consumed by a menu, dialog, field or focused browser view. There is no
Stop button; Send remains Send. Mini-chat Escape has its own UI behavior. Output cannot resume after Stop, although
correlated late usage can still be counted as described below. Cancellation cannot undo already-executed side effects.

## Sessions and reconciliation

### Session operations and version guards

```ts
session.get({ sessionId: string, clientTurnId?: string }) → SessionRecovery
session.configure({
  sessionId: string, sessionVersion: string, clientTurnId?: string,
  model?: string, provider?: string, thinking?: string, permissionMode?: 'ask' | 'auto' | 'full',
}) → { model?: string, provider?: string, thinking?: string | null, permissionMode?: 'ask' | 'auto' | 'full' }
session.compact({ sessionId: string, sessionVersion: string, clientTurnId: string }) → { ok: boolean }
session.delete({ sessionId: string }) → null
```

`sessionVersion` is an opaque durable session incarnation, changed whenever a deleted session ID is recreated. It is
**not** the snapshot revision or an event counter.

- `turn.start` checks the version atomically with acceptance. `null` is explicitly create-only and succeeds only if the
  session is absent. A string requires that exact existing incarnation; missing/mismatched versions must fail with
  `session_missing`/`session_conflict`, never create empty replacement history.
- Retry/configure/compact also require an existing matching incarnation. Reject a different start while a turn is active.
- Deduplicate accepted starts/retries by `(sessionId, clientTurnId)` and steering by
  `(sessionId, turnId, clientInputId)` before executing again. Identical repeated requests return the original identity
  or acceptance; conflicting reuse fails. IDs do not authorize work in a different session incarnation.
- `session.get` never creates a session. The frontend does not call `session.create`, import its display cache as
  history, or enumerate backend sessions to build its chat list.

Picking a model in a reconciled chat with history sends `session.configure` with model/provider/thinking and a
`clientTurnId` for the operation. The backend's returned fields are canonical. A nonempty returned `model` updates
selection using its returned `provider`, or the requested provider if omitted; a provider alone does not change it.
`thinking: null` or `''` clears the selection, while omission leaves it unchanged. A returned provider/model can remain
selected even if absent from the catalog, preventing silent substitution. A failed model switch restores the old model.
If compaction events occur during the switch, the UI labels them for the new model. Switching a chat without history
updates local selection, which is sent with its next turn.

Changing Ask/Auto/Full during a turn sends `session.configure { permissionMode }`. The backend defines those modes and
reevaluates any pending approval; the frontend keeps its card until answered or `approval.resolved` arrives.
Manual compaction's RPC resolves when the operation is over. The frontend finishes its wait on RPC settlement; the
streamed `compaction.completed.ok` controls the compaction notice, not the RPC result's `ok` field alone.

### Recovery snapshot

```ts
type SessionRecovery = { exists: false } | {
  exists: true,
  sessionVersion: string,
  revision: number,                    // nonnegative safe integer; atomic session high-water seq
  turn: null | {
    clientTurnId: string, turnId: string,
    input: DisplayInput | null,        // original accepted start input; null for turn.retry
    events: { method: string, params: object }[],
  },
}
type DisplayInput = { text: string, attachments: DisplayAttachment[] }
type DisplayAttachment = {
  name: string, size?: number, image?: boolean, url?: string,
  width?: number, height?: number, note?: string,
  pasted?: { preview?: string, lines?: number },
  video?: { duration?: number, poster?: string },
}
```

Display attachments are cache projections, **not** `Attachment` payloads: they contain no full text, local paths or
model/tool transcript. In particular, `video.path` is not retained by the current cache projection.

Opening a saved main/mini chat, including after reload/unlock, calls `session.get` before continuation:

- Without a pending checkpoint, return the active turn or `null` if idle.
- With `clientTurnId`, return exactly that accepted turn, even if it completed while the UI was absent; return `null`
  if it was never accepted. Retain completed journals addressable by client ID for a renderer absent an arbitrary time.
- Include the turn's display events in order, from its beginning through the snapshot revision: message, input,
  compaction and usage events, and `turn.completed` if terminal. Do not replay reverse requests as events.
- `session.get` must atomically subscribe this connection to the session and return its snapshot. Subsequent live
  session events have `seq > revision`. The frontend buffers notifications racing the read, discards those covered
  by the snapshot, rebuilds only the recovered turn and reattaches its original `turnId`.
- Completed cached turns and frontend annotations outside that recovered turn remain untouched. Replay rebuilds
  per-message accounting but does not charge the global frontend usage ledger again. There is no general live gap repair.

Missing sessions, unsupported recovery, malformed snapshots and unknown pending turns fail closed with a visible
reconciliation error and recovery Retry. The display cache remains available; restore the backend session or start a
new chat. A main chat with an empty cache is still an existing chat, not permission to recreate it. An unused mini chat
with no local messages and no backend session is treated as new (`sessionVersion: null`). A Send refused before
submission leaves composer text in place; uncertain submitted input remains in the display checkpoint instead.

### Renderer replacement and backend persistence

Repeated `initialize` with a new `connectionId` replaces the renderer connection, not sessions or active turns.
Invalidate old reverse RPCs and replies, and pause renderer-dependent work until that session's `session.get` subscribes
the new page. Reissue pending approvals under fresh RPC IDs after that subscription.

Do not automatically repeat a host action whose execution outcome across renderer replacement is unknown. Reconcile
or cancel that tool step in the backend. Unopened sessions retain journals until opened. App exit can stop execution,
but backend sessions/journals must survive backend process restart and report their actual terminal or recoverable state.
This is session recovery, not automatic process recovery.

### Deletion and mini-chat Clear

`session.delete` erases only the exact `sessionId` and acknowledges after erasure. An absent session succeeds with
`null` so a retry is safe. Main-chat deletion explicitly deletes both `<chat id>` and `<chat id>:mini` before removing
that chat's local records. Folder deletion snapshots its children and commits each acknowledged chat. A failure keeps
failed/unattempted records and the folder, shows the error and permits retry. Offline/unsupported deletion fails
visibly rather than silently removing those records.

**Mini-chat Clear deletes only `<chat id>:mini`, never the parent.** It requires backend availability and
`sessions.delete`, blocks new work, settles recovery/stopped-turn saves, and awaits `session.delete` before wiping
local messages/cache. Repeated Clear shares the pending deletion; close/reopen also waits for it. Failure (including
offline, unsupported deletion or timeout) keeps the mini chat and shows the error, restoring the Clear animation.
Only success resets the version to `null` and clears the old sequence/accounting state for a new create-only start.

## Sequencing, event identity and rendering

All backend **session notifications** carry `sessionId` and a nonnegative safe-integer `seq`. Sequences increase
strictly within a session incarnation, including across turns. Retransmit the same event with its original sequence,
never assign it a new one. Missing/invalid, duplicate and backwards sequences are ignored. Gaps are allowed but not
repaired. Recovery replay has a separate sequence gate, with the snapshot revision establishing the live boundary.

Global `auth.changed`, `models.changed` and `log` notifications have no session sequence requirement. Events are delivered
only after initialization; a failing listener does not block other listeners. Session events for which no chat owns
that session are not rendered.

Turn identity is independent of sequence identity:

- Turn events name the accepted `turnId`. `turn.started` can bind it before the RPC response only when it echoes the
  exact pending start/retry `clientTurnId`; the response must agree. Never reuse turn IDs or message IDs within an incarnation.
- Message deltas/completions and usage may omit `turnId` only when a known `messageId` identifies their owner. An
  explicit conflicting turn/message/client identity is rejected, not assigned to whichever turn is current.
- Up to 256 early events wait in arrival order while a non-quiet turn lacks its remote identity, including message-only
  deltas behind their starts. Overflow and unknown-message events are ignored; this is not general reorder buffering.
- Standalone compact/model-switch events echo the operation's `clientTurnId` **without `turnId`**. Uncorrelated
  session-only compaction/usage is ignored. Auto-compaction during a running turn uses that turn's `turnId`.

In the following signatures, **every row includes `sessionId: string` and `seq: number`**, omitted here for readability:

```ts
turn.started         { turnId, clientTurnId }
message.started      { turnId, messageId, role: 'assistant', model? }
message.delta        { turnId?, messageId, text: string }
message.completed    { turnId?, messageId, text?: string, finishReason?: string }
reasoning.delta      { turnId?, messageId, text: string }
tool.started         { turnId, toolCallId, name: string, title?: string }
tool.progress        { turnId, toolCallId, ... }
tool.completed       { turnId, toolCallId, ... }
approval.resolved    { turnId, approvalId, decision: 'allow' | 'deny' }
compaction.started   { turnId?, clientTurnId?, reason?: 'auto' | 'manual' | 'model-switch' }
compaction.completed { turnId?, clientTurnId?, ok: boolean }
usage                { turnId?, messageId?, clientTurnId?, provider, model, modelName?,
                       input, cached, written, output, requests?, context?: { used, window } }
input.accepted       { turnId, clientInputId, input?: DisplayInput }
session.updated      { title?: string }
turn.completed       { turnId, status: 'done' | 'cancelled' | 'error', finishReason?: string, error?: AbpError }
```

`input.accepted.input` is required in recovery replay even though live acceptance can use the locally queued input.
An extra `messageId` on an accepted user input does not make it an assistant message.

Message rendering follows these rules:

- All assistant messages in a reply part are displayed in message-start order, separated by blank lines. Acceptance
  of a steered input or a mid-turn compaction starts a new reply part. Each message retains its original part and buffer,
  so interleaved deltas and finalization of an earlier message work without replacing another message's text.
- `message.delta.text` appends. `message.completed.text`, if supplied, replaces that message's entire text, including
  when it is empty. The first completion seals it even without final text; later deltas/completions are ignored.
  Duplicate starts do not reset buffers. Deltas/finals before a message's start are ignored.
- `turn.completed` is immediately terminal, even if messages have not finalized. Later text/tool/completion events
  cannot reopen it. Send authoritative final text before terminal completion; the frontend does not wait for missing finals.
- `reasoning.delta` and `tool.progress` are intentionally not rendered. `tool.started` is idempotent, shows working
  status, and cannot reopen a completed tool call. `tool.completed` seals bookkeeping without a normal tool card.
- `finishReason` values `length`, `content_filter` and `insufficient_system_resource` add the matching note. A successful
  turn with no text shows an empty-response note. Error completion shows the backend error and permitted actions.
- `session.updated.title` changes the display title only if the user has not renamed it. Whitespace is normalized and
  the title is limited to 60 characters. Titles are session-scoped rather than turn output.

A global `log { level, message }` notification goes to the developer console (`error` selects error logging; other levels
use ordinary logging). It is distinct from process stderr.

## Approvals and reverse-request ownership

```ts
approval.request({
  sessionId: string, turnId: string, approvalId: string, toolCallId: string,
  tool: string, args: object, presentation?: Presentation,
}) → { decision: 'allow' | 'deny', reason?: 'superseded' | 'cancelled' }

type Presentation = {
  kind: 'command' | 'file' | 'web', title: string,
  effect?: 'read' | 'change' | 'delete' | 'install' | 'system' | 'online' | 'record' | 'run',
  badge?: boolean,
  places?: { kind: 'file' | 'folder' | 'site', label: string, title: string }[],
  code?: string, removed?: string, added?: string, quote?: string,
  reveal?: 'command' | 'content' | 'changes',
}
```

**The backend owns approval policy**, including what Ask/Auto/Full mean and which actions require approval. The frontend
validates presentation fields and renders the card. Without presentation it displays the tool name and JSON arguments.
Tool names, arguments, presentation strings and errors are displayed as text, not executable markup.

The user answers allow/deny. Sending steering input supersedes pending approvals with deny/`superseded`; stopping or
cancelling the request denies it with `cancelled`. A backend `approval.resolved` notification settles an existing card,
including after a permission-mode change. It does not create a card.

Both `approval.request` and `host.tool` must belong to the named session's running turn:

- An unopened/unowned session fails with `unknown_session`.
- While recovery or start identity is pending, the request waits for it, then checks the `turnId`.
- With no running turn, or with that turn locally stopped, the request is answered as cancelled: deny/`cancelled` for
  approvals or `{ status: 'cancelled', content: [] }` for host tools.
- With a different turn running, an old/missing/mismatched `turnId` fails with `stale_turn` before showing a card or
  running a step. Reusing an `approvalId` for another approval or a `toolCallId` for another host call in the same turn
  also fails with `stale_turn`. Approval and host-call identifiers have separate bookkeeping, so the approved tool
  can subsequently execute through `host.tool`.
- A browser step still running when its turn ends is cancelled. RPC-ID duplication is separately rejected as
  `duplicate_request` before handler execution.

## Host tools

```ts
type HostToolSchema = { name: string, description: string, parameters: object } // JSON Schema
host.tool({ sessionId: string, turnId: string, toolCallId: string, name: string, args: object }) → {
  content: ({ type: 'text', text: string } | { type: 'image', dataUrl: string, label?: string })[],
  isError?: boolean,
  status: 'ok' | 'error' | 'cancelled' | 'handed-back',
  reason?: 'message',
  data?: object,
}

type BrowserState = {
  available: boolean,
  status: 'unavailable' | 'empty' | 'lazy' | 'loading' | 'ready' | 'failed' | 'gone',
  control: 'agent' | 'user', open: boolean, signedInVerified: false,
  tabs: {
    n: number, tabId: string, state: string, loading: boolean, revision: number,
    title: string, url: string, active: boolean,
  }[],
  signedIn: { host: string, at: number }[],
}

// Frontend → backend global notification; no session sequence.
host.browser.changed { browser: BrowserState }
```

The desktop browser advertises `browser_navigate`, `browser_snapshot`, `browser_click`, `browser_type`,
`browser_select`, `browser_press`, `browser_scroll`, `browser_screenshot`, `browser_read`, `browser_wait` and
`browser_tabs`. Use the advertised schemas for arguments. An unknown tool name or missing browser panel fails with
`unsupported`; browser-service unavailability can instead be a structured tool-level failure.

The browser state is an explicit object even when closed/empty; callers use null only if the panel itself is absent.
`host.browser.changed` reports lifecycle/control/context changes. `signedIn` entries are submission observations,
**not verified login or provider-auth state**. Tabs and revisions in context are hints; dispatch rechecks live targets.

All browser tool calls, including tab mutations, are serialized across chats. A target is captured at receipt, not after
queuing. Use stable `tabId` handles; `browser_tabs` switch/close require `tabId`, not positional `tab`. Opening at the
12-tab limit fails without evicting a tab. Page results include `data.tabId` and opaque `data.pageId`. Click/type/select/
press/scroll require the current `pageId`; other page tools may use it as a precondition. Navigation/input invalidates
old page identities and refs. Re-observe after stale-target failures rather than silently retargeting.

While the user has taken control, a call waits for hand-back, then returns a fresh snapshot with `status: 'handed-back'`
instead of performing the original step. A new user message during that wait returns `status: 'cancelled'` with
`reason: 'message'`. Taking control during a step interrupts it before further dispatch. Already-issued input/JS cannot be rolled
back; failure, cancellation or timeout is not proof that no partial side effect occurred. The backend must request any
required approval before calling the host tool; the frontend does not infer browser approval policy.

Browser deadlines are 15 seconds for first readiness, 90 seconds for the renderer operation including its queue,
75 seconds for the main-process operation including its queue, 12 seconds for CDP/isolated-world calls, 30 seconds for
navigation and 15 seconds for loading settle. User hand-back waiting is intentionally unbounded.

Tool-level failures remain successful JSON-RPC results with `isError: true`, explanatory text and `data.code`, normally
with `status: 'error'` (a hand-back response retains `status: 'handed-back'`). They are not rewritten as RPC errors. Result metadata can include page/tab/read identities, snapshot
`refs`/coverage/truncation/scroll, screenshot dimensions/scale, read continuation offsets and operation-scoped downloads.
`data.refs` maps numbered snapshot elements to descriptions usable in approval wording. For detailed argument
preconditions, structured failure codes and read/screenshot limits, see [Browser host-tool lifecycle](browser-host-tools.md).

## Attachments and user context

```ts
type Attachment = {
  id: string, name: string, mime: string, size: number,
  kind: 'image' | 'text' | 'pdf' | 'video' | 'file',
  note?: string, path?: string,
  dataUrl?: string, width?: number, height?: number,
  text?: string, truncated?: boolean,
  video?: { duration: number, width: number, height: number },
}
type UserContext = {
  instructions: string,
  files: {
    id: string, name: string, size: number, kind: 'text' | 'image' | 'file',
    path?: string, text?: string, truncated?: boolean,
    dataUrl?: string, width?: number, height?: number,
  }[],
}
```

Attachment preparation finishes before dispatch. Input attachment IDs are local to that input (`a1`, `a2`, ...), not
session-global identities. `size` is the source file size in bytes; `mime` may be empty. `path`, when present, is a local
filesystem path on the app/backend machine, not a URL or an uploaded-file handle.

- Images carry a data URL; large images are downscaled/re-encoded by the reader. The reader reports the original
  dimensions in `width`/`height`, so they can differ from encoded dimensions. Ordinary chat image payloads do not
  include a source path; pinned user-context images can include one.
- Text and successfully extracted Office/PDF text use `text` and `truncated`. PDFs retain `kind: 'pdf'` when identified
  by the PDF reader; an unreadable attachment can instead have only metadata/path. A file's kind is based on the
  prepared payload, not merely its extension.
- Videos carry a path when known and duration/dimensions, not video bytes or the display poster. Unreadable metadata
  can be zero. Generic files may have a path without extracted contents. Pinned files use the simpler
  text/image/file kinds rather than the chat attachment kinds.

The frontend permits up to 20 composer attachments. The generic text/Office reader skips files over 20,000,000 bytes
and limits extracted text to 400,000 JavaScript string units; PDF extraction uses a separate reader and the same final
text truncation. Image processing uses a 2560-pixel maximum side, retaining suitable originals up to 6,000,000 bytes
before re-encoding. These are local preparation rules, not a negotiated backend attachment limit.

Settings → General stores instructions and pinned files locally and sends `UserContext` with each start/retry.
Local controls cap instructions at 8,000 string units, pinned files at 20, and combined pinned text at 200,000 string
units. These are storage/payload guardrails, **not model token/context limits**. Backend attachment capability flags
and `maxBytes` are not enforced by the frontend; the backend must validate its own supported inputs. The JSON Lines
transport limit still applies to the whole serialized request. Display caches never substitute for full attachment input.

## Usage and accounting

`usage` events are **increments**, not cumulative totals. Their session `seq` deduplicates delivery. Supply provider
and model IDs and numeric `input`, `cached`, `written`, `output`, optional `requests`, and optional `context`:

- `input` counts tokens sent, including cached input; `cached` counts cache reads, `written` cache writes, and `output`
  generated tokens. Totals are `input + output`, not input plus cache counts again.
- The frontend clamps counts to nonnegative values and `cached` to at most `input`. An event with neither input nor
  output adds no token/request accounting; otherwise requests defaults to at least 1. Context can still update from
  a correlated event without token increments.
- The global local ledger requires nonempty provider/model IDs and otherwise treats both as opaque, `|` included: it
  keys each model by the JSON pair `[provider, model]` and upgrades older `provider|model` keys when it loads.
  `modelName` supplies a display label. No per-event monetary cost is consumed; account balances/credits come from
  `account.limits`.
- With `messageId`, usage belongs to that message's original reply part. Turn-only usage belongs to its first reply
  part. Standalone compaction usage, correlated by `clientTurnId`, belongs to its compaction entry. Unknown or conflicting
  identities and uncorrelated session-only usage are ignored.
- The last 16 local turn/operation records retain accounting references. Correlated late usage after completion/Stop
  can still be counted and saved without reopening output or changing the current context meter. Older/unrecognized
  accounting is ignored. Recovery, locking and unloading discard this local accounting tail.
- Replay reconstructs per-entry usage but never charges the global ledger again. This ledger is a local display count,
  not an authoritative backend billing history or a backfill of activity while the renderer was absent.

`context.used` and `context.window` update the context meter only for the active, nonterminal turn. A reported model
context window can supply the display fallback; a missing window remains unknown. The frontend does not estimate a
model limit from text length or pinned-file counts.

## Errors

Use JSON-RPC's numeric error envelope with a named application error in `error.data`:

```json
{"jsonrpc":"2.0","id":"connection:3","error":{"code":-32000,"message":"Sign-in required","data":{"code":"auth","message":"Sign-in required","provider":"Example","action":"open-settings"}}}
```

```ts
type AbpError = {
  code: string,
  message: string,
  provider?: string,
  action?: 'open-settings' | 'retry' | 'none',
  retryable?: boolean,
  status?: number,
}
```

Named codes include `auth`, `quota`, `rate_limit`, `network`, `server`, `context_overflow`, `invalid_request`,
`model_unavailable`, `cancelled`, `unsupported` and `unknown`. Session/identity errors include `session_missing`,
`session_conflict`, `turn_missing`, `invalid_recovery`, `invalid_turn`, `stale_turn`, `unknown_session`,
`steering_rejected` and `duplicate_request`. Named backend codes are preserved, not restricted to this list.

The frontend displays a nonempty application message, falling back to the RPC message or its own known-code wording.
`provider` is a display name for fallback wording. `auth` or `action: 'open-settings'` offers settings. Retry availability
also depends on the saved turn context and reconciliation state; `action: 'retry'` does not bypass those requirements.

Without a nonempty named application code:

| JSON-RPC code | Frontend application code |
|---|---|
| `-32601` | `unsupported` |
| `-32700`, `-32600`, `-32602` | `invalid_request` |
| Other numeric errors | `unknown` |

Local failures include `backend_unavailable` (no bridge/process or not initialized), `backend_crashed` (loss of a ready
connection), `timeout`, and `protocol_error` (a malformed correlated response). Initialization failures leave subsequent
requests unavailable with the initialization diagnostic. Request aborts reject locally as `AbortError`.

Reverse handlers throwing `unsupported` or `invalid_request` use `-32601` or `-32602`; other execution failures use
`-32000` with their named code, `cancelled` for `AbortError`, or `unknown` for an ordinary exception. This is separate
from a host tool returning an `isError: true` result.

## Contract scope and implementation references

There is no session-list/import/create/rename/edit-message RPC used by this frontend, no automatic live sequence-gap
repair, no automatic backend restart, and no reasoning or normal tool-card renderer. Browser context uses
`host.browser.changed` and turn payloads, not a generic context-update RPC. These absences do not change backend
ownership of durable sessions or approval policy.

The implementing sources and focused existing tests are:

| Area | Sources | Tests |
|---|---|---|
| Envelopes, negotiation, deadlines, client lifetime | `backend-protocol.js`, `backend-client.js`, `desktop/preload.js` | `test/backend-envelope.test.js`, `test/backend-client.test.js`, `test/backend-lifecycle.test.js` |
| Process configuration and transport | `desktop/backend-host.js`, `desktop/main.js` | `test/backend-config.test.js`, `test/backend-host.test.js`, `test/backend-transport.test.js` |
| Turns, recovery, ordering, deletion | `chat.js`, `library.js` | `test/session-recovery.test.js`, `test/retry-steering.test.js`, `test/event-ordering.test.js`, `test/turn-identity.test.js`, `test/deletion.test.js`, `test/mini-chat-clear.test.js` |
| Catalog, auth, accounting | `settings.js`, `settings-usage.js`, `usage.js` | `test/provider-auth.test.js`, `test/model-capabilities.test.js`, `test/usage.test.js` |
| Reverse requests and display/input boundary | `approval-card.js`, `host-tools.js`, `browser-panel.js`, `desktop/browser.js`, `attachment-reader.js`, `user-context.js` | `test/cancellation.test.js`, `test/browser-lifecycle.test.js`, `test/legacy-display.test.js`, `test/lock-ui.test.js`, `test/untrusted-text.test.js` |

`test/fixtures/scripted-backend.js` is a test-only scripted peer used for boundary and Electron smoke coverage. It has
no model, is never packaged or launched by default, and is not a complete backend conformance implementation: its
in-memory sessions do not provide production durability or all required retry/version checks.
