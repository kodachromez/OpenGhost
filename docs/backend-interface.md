# Backend interface (ABP v0)

OpenGhost on this branch is a frontend only. It contains no model or provider client, no agent loop, no prompts, no
tools of the agent's and no provider keys. Everything the chat shows comes from an **external backend** through one
boundary:

```text
OpenGhost UI (chat.js, settings*.js, model picker, approval cards, browser panel, …)
  │  window.Backend                          backend-client.js: JSON-RPC peer, capabilities, events, reverse requests
  ▼
preload bridge window.openghost.backend      desktop/preload.js: send / onMessage / onStatus / status
  │  Electron IPC  backend:send · backend:message · backend:status
  ▼
desktop/backend-host.js                      spawns the backend, relays JSON lines, reports its state, stops it on quit
  │  stdin / stdout, one JSON-RPC 2.0 object per line (UTF-8, "\n")
  ▼
external backend process                     the future Rust backend, or an adapter in front of another agent
```

The protocol is the "Agent Backend Protocol" proposed in the backend-removal audit (`docs/backend-removal-audit.md` §6–§8),
as far as this branch implements it. This document is what the frontend actually sends and handles.

## Connecting a backend

The host starts a backend only when one is configured, in this order:

1. `OPENGHOST_BACKEND`: a path to an executable, or a JSON array of the executable and its arguments,
   for example `OPENGHOST_BACKEND='["/opt/ghosty/bin/ghosty", "abp"]' npm start`.
2. `backend.json` in the app's user data folder (`~/.config/OpenGhost/` on Linux):
   `{ "command": ["/opt/ghosty/bin/ghosty", "abp"] }`.

With neither, nothing is started: the UI works, and a message gets "No backend is connected" in the chat.

The process starts in the user's home folder with the app's environment. **stdout carries protocol only**; logs go to
stderr, which the host prints with a `[backend]` prefix. A line that isn't a JSON-RPC 2.0 object is dropped and logged.
Lines are limited to 64 MiB of UTF-8 bytes, excluding LF but including whitespace/CR. Oversized input is discarded
through its next newline before decoding or parsing. Outbound messages have the same limit; the host rejects sends
that would exceed 64 MiB + 1 byte in Node's stdin write queue (including newlines). Accepted writes drain in order;
there is no extra host queue. Rejection returns `false` from the host's `send`; renderer IPC delivery remains unacknowledged.
There is no automatic restart. On quit the host sends `shutdown`, closes stdin, and kills
the process after 2 s if it is still running. When the backend exits, however it exits, the host ends whatever it left
behind in its session (on POSIX the backend leads a session of its own). A page reload sends `initialize` again on the same process.

## Envelope

JSON-RPC 2.0. Requests have an `id`, notifications don't, and **both sides send requests**. Either side may cancel a
request it sent with the notification `$/cancelRequest { id }`. A cancelled request is still answered.
The client gives up on any request it sent, `initialize` included, that goes unanswered for 60 seconds (15 minutes for
`auth.login`, 10 for `session.compact`): it fails locally with `timeout`, the client sends `$/cancelRequest`, and the
late answer is ignored.

Errors: a JSON-RPC error whose `data` is an `AbpError`:

```ts
type AbpError = {
  code: 'auth' | 'quota' | 'rate_limit' | 'network' | 'server' | 'context_overflow' | 'invalid_request'
      | 'model_unavailable' | 'cancelled' | 'unsupported' | 'unknown' | string,
  message: string,          // shown in the chat as it is
  provider?: string,        // a provider's display name, used in the app's own wording when message is empty
  action?: 'open-settings' | 'retry' | 'none',
  retryable?: boolean, status?: number,
}
```

The client adds two codes of its own: `backend_unavailable` (no backend, or not initialized) and `backend_crashed`
(the process exited while something was waiting for it). JSON-RPC `-32601` (method not found) becomes `unsupported`.

## Handshake

The client sends `initialize` once the backend process is running and the page has loaded:

```ts
initialize({
  protocolVersion: '0.1',
  connectionId: string,             // fresh per renderer; RPC ids are "<connectionId>:<counter>"
  client: { name: 'OpenGhost', version: '1.3.0', platform: 'linux' | 'darwin' | 'win32', locale: string },
  host: {
    tools: HostToolSchema[],      // the built-in browser's tools, see "Host tools"
    renderGuide: string,          // render-guide.js: what this renderer can draw (Markdown, mermaid, the app's own kinds)
    attachments: { localPaths: boolean },
  },
}) → {
  protocolVersion: '0.1',
  backend: { name: string, version: string, platform?: string },
  capabilities: Capabilities,
}
```

Until `initialize` succeeds every request fails with `backend_unavailable`. The frontend checks these capabilities
(anything not listed is read as false):

| Capability | What the frontend does with it |
|---|---|
| `auth.providers` | Settings → Providers asks `auth.providers`; without it the page says there is nothing to connect |
| `compaction.manual` | "Compact chat" in the plus menu is offered |
| `sessions.delete` | Deleting a chat (or clearing a mini chat) sends `session.delete` |
| `sessions.recovery` | Required for turns: `session.get`, durable turn replay, and atomic session/version guards below. Without it chats remain display-only |
| `usage.limits` | Settings → Usage asks `account.limits` for each connected provider |

The rest of the audit's `Capabilities` (turns, thinking, tools, approvals, attachments, titles, userContext) may be
sent and is kept in `Backend.capabilities` for later use; the 1.3.0 UI doesn't hide anything on them yet.

## Client → backend requests

```ts
// Providers, sign-ins, models
auth.providers() → Provider[]
auth.setKey({ provider, key: string | null }) → ProviderStatus      // null removes the key
auth.login({ provider }) → ProviderStatus                            // may take minutes; resolves when done or cancelled
auth.cancel({ provider }) → ProviderStatus
auth.logout({ provider }) → ProviderStatus
account.limits({ provider }) → AccountLimits | null
models.list({}) → Model[]

// Turns. sessionId is the frontend's chat id; a mini chat's is "<chat id>:mini".
// sessionVersion: null is explicitly create-only; a string requires that existing session incarnation.
turn.start({ sessionId, sessionVersion: string | null, clientTurnId, input: Input, ...SessionParams }) → { turnId, sessionVersion: string }
turn.retry({ sessionId, sessionVersion: string, clientTurnId, failedTurnId: string, ...SessionParams }) → { turnId } // retry this accepted failed turn, no new input
turn.steer({ sessionId, turnId, clientInputId, input: Input, host }) → { accepted: boolean }
turn.cancel({ sessionId, turnId }) → null

// Sessions
session.get({ sessionId, clientTurnId?: string }) → SessionRecovery
session.configure({ sessionId, sessionVersion: string, clientTurnId?, model?, provider?, thinking?, permissionMode? }) → { model, thinking, permissionMode }
session.compact({ sessionId, sessionVersion: string, clientTurnId }) → { ok: boolean }      // resolves when the compaction is over
session.delete({ sessionId }) → null

type SessionParams = {
  model: string, provider: string,          // Model.id and Model.provider as models.list gave them
  thinking: string,                         // one of the model's thinkingLevels
  permissionMode: 'ask' | 'auto' | 'full',
  cwd: string,                              // the chat's project folder, or its own folder under ~/OpenGhost/Chats/
                                            // (which may not exist yet: create it on first use)
  title: string,
  userContext: UserContext,                 // Settings → General, sent with every turn
  host: { browser: BrowserState | null },   // what the built-in browser holds right now
  side?: { parent: string, parentBusy: boolean, moved: boolean },   // mini chats only: the chat they hang off
}
type Input = { text: string, attachments: Attachment[] }     // quotes stay in text as "> " lines
type Attachment = {
  id: string, name: string, mime: string, size: number,
  kind: 'image' | 'text' | 'pdf' | 'video' | 'file',
  note?: string, path?: string,             // path: where the file is on this computer, when known
  dataUrl?: string, width?: number, height?: number,     // images, downscaled as the app reads them
  text?: string, truncated?: boolean,                    // text, Office documents and PDFs the app could read
  video?: { duration: number, width: number, height: number },
}
type UserContext = { instructions: string, files: { id, name, size, kind: 'text' | 'image' | 'file', path?, text?, truncated?, dataUrl?, width?, height? }[] }
type BrowserState = { open: boolean, tabs: { n: number, title: string, url: string, active: boolean }[], signedIn: { host: string, at: number }[] }
```

### Session recovery (`sessions.recovery`)

```ts
type DisplayInput = { text: string, attachments: DisplayAttachment[] }
// Display attachments use the frontend cache shape: name, size, image?, url?, width?, height?, note?,
// pasted?: {preview, lines}, video?: {path, duration, poster}. No model prompt/tool history is returned.
type SessionRecovery = { exists: false } | {
  exists: true,
  sessionVersion: string,                 // durable, opaque incarnation; changes if deleted/recreated
  revision: number,                       // safe integer: atomic high-water seq of this snapshot
  turn: null | {
    clientTurnId: string, turnId: string,
    input: DisplayInput | null,            // original accepted input; null for turn.retry
    events: { method: string, params: object }[], // ordered turn display events, from its beginning through revision
  },
}
```

- On opening a saved main/mini chat (including after reload/unlock), the frontend calls `session.get` before allowing
  continuation. The frontend keeps its chat index and display cache; it does **not** send that cache back as history.
  With no pending checkpoint, return the active turn, or `null` when idle. With `clientTurnId`, return exactly that
  accepted turn, even if completed while the UI was absent; return `null` if never accepted. Never create in `get`.
- Before dispatching start/retry/steer, the frontend durably saves its index and display checkpoint, with the turn/input
  client IDs. Storage failure prevents dispatch. Accepted turn input and its display-event journal must be durable
  **before** the backend acknowledges acceptance or emits events. Keep completed recovery journals addressable by
  `clientTurnId`: a renderer may have been absent for an arbitrary time. This journal is a display projection, not the
  backend's model-facing transcript. Include `input.accepted.input: DisplayInput` for accepted steering in replay.
- `session.get` atomically subscribes this connection to the session and returns its snapshot. Every subsequent
  session notification has a monotonically increasing safe-integer `seq` greater than `revision`. The frontend
  buffers notifications during get, ignores those already in the snapshot, rebuilds only the pending turn, and
  reattaches its original `turnId`. Replay includes message/compaction/usage/input events and, if terminal,
  `turn.completed`. Do not replay reverse requests as events. Completed cached chats and frontend annotations remain
  untouched; replay does not re-charge the frontend usage ledger. General live-stream gap repair is not added here.
- Repeated `initialize` with a new `connectionId` replaces the renderer connection, **not** its sessions or active
  turns. Invalidate old reverse RPCs and old replies; pause renderer-dependent work until that session's `get` has
  subscribed the new page. Send pending approvals under fresh RPC IDs after get. A host action whose old execution
  outcome is unknown must not be repeated automatically; reconcile/cancel that tool step in the backend. Unopened
  sessions retain their recovery journal until opened. App exit may stop execution, but sessions/journals must survive
  backend process restart and report the actual terminal or recoverable state.
- `turn.start` checks `sessionVersion` atomically with acceptance. `null` permits creation only when absent (new
  frontend chat or unused mini chat); a string requires the exact existing incarnation. Missing/mismatched versions
  fail with `session_conflict`/`session_missing`, **never** an implicit empty session. Retry/configure/compact require
  an existing incarnation too. Reject a different start while a turn is active. Deduplicate accepted starts/retries
  by `(sessionId, clientTurnId)` and steering by `(sessionId, turnId, clientInputId)` before executing anything again;
  repeated identical requests return the original identity, conflicting reuse fails.
- Missing sessions (including legacy visible chats), unsupported recovery, and unknown pending turns fail closed with
  an existing-style error and recovery Retry. The display cache is preserved. Restore the backend session or start a
  new chat; there is no automatic import, resend, reset, or `session.create`. A rejected Send keeps the composer text.

**Retry and steering (F12).** Retry retains the original full input/preparation context in memory when a start was
never dispatched (offline, preparation or checkpoint failure); an explicit click sends `turn.start`, not a history
retry. Display-only attachments are never reconstructed as model input. After dispatch, a missing/invalid reply or
connection loss is uncertain: Retry calls `session.get` with the saved `clientTurnId`, never blindly resends. Missing
history/identity fails closed. An accepted, terminal failed turn uses `turn.retry` with required `failedTurnId` and the
same session incarnation. The backend must reject an unknown/non-failed target, never substitute its latest turn.
The existing client-ID deduplication rules apply to retries too. Explicit `retryable:false`/`action:none` suppress Retry.

Steering preparation and RPCs are serialized in input order. `{accepted:false}` (or an RPC error) is surfaced on the
input bubble, retained for display, and never silently resent. `{accepted:true}` acknowledges durable acceptance;
`input.accepted` still places the bubble in the reply. Emit those events in accepted input order, before dependent
output. Turn completion/error/Stop aborts steering RPCs and drains preparation/start waiters; inputs without an
acceptance event are visibly marked unconfirmed, not assumed accepted or automatically carried into another turn.

**Chat/folder deletion.** `session.delete` deletes only the exact `sessionId` and acknowledges after erasure; an absent
session succeeds with `null` so retry is safe. The frontend explicitly deletes both `<chat id>` and `<chat id>:mini`,
then removes that chat's local records. Folder deletion snapshots its children and commits each acknowledged chat;
a failure preserves the failed/unattempted records and the folder, displays the error, and allows retry. Offline or
unsupported deletion fails visibly rather than deleting locally. Mini-chat Clear is unchanged.

**Model switch.** Picking another model in a chat with history sends `session.configure({ model, provider, thinking })`
and shows it the way 1.3.0 showed its model-switch compaction: if the backend sends `compaction.started` /
`compaction.completed` meanwhile, the line reads "Compacting the conversation for <new model>". If the request fails,
the chat stays on the old model.

**Permission mode.** Changing Ask/Auto/Full while a turn runs sends `session.configure({ permissionMode })`. Pending
approval cards stay until the backend answers them with `approval.resolved`.

## Backend → client events (notifications)

Every session event carries `sessionId` and a nonnegative safe-integer `seq`, strictly increasing within that session
incarnation (including across turns). Retransmit an event with its original `seq`, never a new one. Missing/invalid,
duplicate and backwards sequences are ignored, including live events; gaps are allowed, not repaired. Recovery replay
has its own sequence gate, and the snapshot revision is the live boundary. Global auth/catalog/log notifications are
exceptions. Notifications are delivered only while the client is ready; listener failures do not block other listeners.

Turn events must name the accepted `turnId`. `turn.started` may bind it early only with the exact `clientTurnId` from
start/retry; its ID must agree with the RPC response. Never reuse turn IDs or message IDs within a session incarnation.
Message delta/completion and usage may omit `turnId` only when a known `messageId` identifies their owner. Up to 256
ordered early events (including message-only deltas) wait for the start identity; overflow and unknown-message events
are ignored. Standalone compact/model-switch requests carry `clientTurnId`: echo it, without `turnId`, on their
compaction/usage events. Uncorrelated session-only usage/compaction is ignored, never assigned to the current turn.

```ts
turn.started      { sessionId, turnId, clientTurnId }
message.started   { sessionId, turnId, messageId, role: 'assistant', model? }
message.delta     { sessionId, messageId, text }                 // appended
message.completed { sessionId, messageId, text?, finishReason? } // text, when sent, replaces the message's text
reasoning.delta   { sessionId, messageId, text }                 // ignored: 1.3.0 shows no reasoning
tool.started      { sessionId, turnId, toolCallId, name, title? }// shows the ghost "working" status
tool.progress     { sessionId, turnId, toolCallId, ... }           // no tool cards; ignored
tool.completed    { sessionId, turnId, toolCallId, ... }           // seals the call; no tool cards
approval.resolved { sessionId, turnId, approvalId, decision: 'allow' | 'deny' }   // settles a pending card
compaction.started   { sessionId, turnId?, clientTurnId?, reason?: 'auto' | 'manual' | 'model-switch' }
compaction.completed { sessionId, turnId?, clientTurnId?, ok: boolean }
usage             { sessionId, turnId?, messageId?, clientTurnId?, provider, model, modelName?, input, cached, written, output,
                    requests?, context?: { used, window } }
input.accepted    { sessionId, turnId, clientInputId }           // a steered message joined the history
session.updated   { sessionId, title? }                          // the chat's title, unless the user renamed it
turn.completed    { sessionId, turnId, status: 'done' | 'cancelled' | 'error', finishReason?, error?: AbpError }
auth.changed      { provider, status: ProviderStatus }
models.changed    { provider? }                                  // the frontend reads models.list again
log               { level, message }                             // printed in the devtools console
```

How the UI uses them, in the same terms as 1.3.0:

- A reply is a **part** in the chat. All messages of a turn go into the same part, separated by a blank line, until a
  steered message is accepted (`input.accepted`: the user's bubble joins the history and the reply goes on below it) or
  a compaction starts mid-turn. Each message keeps its own buffer and original part in start order; interleaving and
  finalizing an earlier message are supported. A duplicate start never resets text. Deltas/finals before a message's
  start are ignored; its first completion seals it (even without final text). A tool start is likewise idempotent and
  cannot reopen a completed call.
- `turn.completed` is terminal immediately: it freezes existing text even if some messages have not finalized.
  Later text/tool/completion events are ignored, not allowed to reopen the turn. Send authoritative final text before
  turn completion; no message-finalization wait is imposed on otherwise valid streams.
- `finishReason` `length`, `content_filter` or `insufficient_system_resource` puts the matching note under the reply.
  A turn with no text gets "The model returned an empty response".
- `usage` is incremental, deduplicated by `seq`. With `messageId`, it charges that message's original part; turn-only
  usage charges the first reply part, and standalone compaction usage charges its summary. Unknown or conflicting
  identities are ignored. The last 16 local turns retain accounting entry references until recovery/lock/unload;
  correlated late usage after completion/Stop can still be counted and saved there, without reviving output or changing
  the current context meter. Older/unrecognized accounting is ignored. The meter updates only from the active turn's
  usage (`context.used / context.window`).
- **Stop** (button or Escape) freezes output at once, settles pending cards as denied, and sends `turn.cancel`.
  Only the correlated accounting exception above remains; session titles are session-scoped.
- A message sent while a turn runs is sent with `turn.steer`, and any pending approval card is answered `deny` with
  `reason: 'superseded'`. Messages never accepted stay in the chat unanswered, as in 1.3.0. Messages sent during a
  manual compaction or a model switch start a new `turn.start` after it, joined into one input.
- `turn.completed` with `status: 'error'` shows the error box with Retry when allowed (targeting that accepted failed
  turn via `failedTurnId`), plus "Open settings" when `error.action` is `open-settings` or the code is `auth`.

## Backend → client requests (reverse)

```ts
approval.request({ sessionId, turnId, approvalId, toolCallId, tool: string, args: object, presentation?: Presentation })
  → { decision: 'allow' | 'deny', reason?: 'superseded' | 'cancelled' }

host.tool({ sessionId, turnId, toolCallId, name: string, args: object })
  → { content: ({ type: 'text', text } | { type: 'image', dataUrl, label? })[], isError?: boolean,
      status: 'ok' | 'error' | 'cancelled' | 'handed-back', reason?: 'message', data?: { refs?: Record<number, string> } }
```

**Approvals.** The card shows `presentation` (checked field by field in `ApprovalCard.present`):

```ts
type Presentation = {
  kind: 'command' | 'file' | 'web', title: string,
  effect?: 'read' | 'change' | 'delete' | 'install' | 'system' | 'online' | 'record' | 'run', badge?: boolean,
  places?: { kind: 'file' | 'folder' | 'site', label: string, title: string }[],
  code?: string, removed?: string, added?: string, quote?: string, reveal?: 'command' | 'content' | 'changes',
}
```

Without one, the card names the tool and shows its arguments as JSON. **The frontend has no approval policy**: the
backend decides what needs asking, what Ask/Auto/Full mean, and words the card. (1.3.0's own shell/git effect
analysis was keyed to the removed backend's tool names and went with it; see `docs/frontend-only.md`.)

**Host tools.** The built-in browser lives in the app: its webviews hold the user's logins, the user can take control
and hand it back, and the panel shows the agent's cursor. So the frontend publishes its tools at `initialize`
(`host.tools`: `browser_navigate`, `browser_snapshot`, `browser_click`, `browser_type`, `browser_select`,
`browser_press`, `browser_scroll`, `browser_screenshot`, `browser_read`, `browser_wait`, `browser_tabs`, as
`{ name, description, parameters }` JSON Schemas) and runs them when the backend calls `host.tool`. While the user has
taken control, a call waits until they hand it back, then returns a fresh snapshot with `status: 'handed-back'`
instead of doing the step; if the user sends a message instead, it returns `status: 'cancelled', reason: 'message'`.
Whether a browser step needs approval is the backend's call, made with `approval.request` before `host.tool`.
`data.refs` names the snapshot's numbered elements, for wording approval cards.

**Turn identity.** Both requests belong to the turn named by `turnId`, which must be the session's running turn. One
for a turn that ended or was stopped is answered as cancelled (`deny`/`cancelled`, `status: 'cancelled'`); one naming
another turn, or with no `turnId`, fails with `stale_turn` before a card shows or a step runs, as does a second
request with an `approvalId` or `toolCallId` already asked in that turn. A request that arrives before `turn.start`
has answered waits for the turn's id. A browser step still running when its turn ends is cancelled. A reverse request
reusing the JSON-RPC `id` of one still being answered fails with `duplicate_request` (`-32600`) and is not run.

## Provider, model and account types

```ts
type Provider = {
  id: string, name: string, group?: string,   // group: the heading over its models in the picker
  limits?: boolean,                           // Settings → Usage keeps a place for its plan limits
  methods: ({ type: 'apiKey', label: string, hint?: string, url?: string /* https only */, placeholder?: string }
          | { type: 'oauth', label: string, hint?: string, action?: string /* the sign-in button's text */ })[],
  status: ProviderStatus,
}
type ProviderStatus = { connected: boolean, checking?: boolean, waiting?: boolean, keySaved?: boolean,
                        account?: { email?: string, plan?: string }, error?: AbpError | string }
type Model = { id: string, provider: string, name: string, contextWindow?: number, vision?: boolean,
               thinkingLevels?: string[], defaultThinking?: string }
type AccountLimits = {
  plan?: string,
  windows?: { seconds: number, used: number /* 0–100 */, resets: number /* epoch ms */ }[],
  models?: { name: string, windows: AccountLimits['windows'] }[],
  balances?: { currency: string, total: number }[],
  credits?: { unlimited?: boolean, balance?: string },
}
```

Keys and sign-ins belong to the backend. The frontend never receives a saved key back: the field shows "Saved · type to
replace" when `keySaved` is true, and an emptied field sends `auth.setKey({ key: null })`.

The provider UI validates catalog/status fields and renders at most one `apiKey` and one `oauth` method per provider;
unknown kinds are ignored (v0 has no method selector). `auth.changed` is an unversioned invalidation, not an ordered
snapshot: the frontend re-reads `auth.providers`, again after any pending local mutation settles. `models.changed`
refreshes both providers and models. Return current authoritative statuses from `auth.providers`; the backend must
order its own credential mutations. Frontend freshness guards prevent stale UI updates, not backend-side effects.

## Not in this version

From the audit's ABP v0, these are not used yet: `session.create`/`session.list` (the frontend keeps its
own chat list and a display copy of each chat; `session.get` is the recovery contract above), `session.rename`, `session.editMessage` (editing a diagram changes only
the frontend's copy), `turn.followUp`, `context.update` (the browser state goes with each turn instead), `seq` gap
detection, `tool.*` and `reasoning.*` rendering, and capability gating of the mode picker, effort slider and mini chat.

## Testing a backend against the frontend

`test/fixtures/scripted-backend.js` is a scripted ABP backend **for tests only** (no model behind it, never packaged).
It shows a minimal working backend and is what `npm run test:e2e` drives the real app with.
