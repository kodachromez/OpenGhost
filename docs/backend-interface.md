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
Lines over 64 MiB are dropped. There is no automatic restart. On quit the host sends `shutdown`, closes stdin, and kills
the process after 2 s if it is still running. A page reload sends `initialize` again on the same process.

## Envelope

JSON-RPC 2.0. Requests have an `id`, notifications don't, and **both sides send requests**. Either side may cancel a
request it sent with the notification `$/cancelRequest { id }`. A cancelled request is still answered.

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

// Turns. sessionId is the frontend's chat id; a mini chat's is "<chat id>:mini". The backend creates a session the first
// time it sees an id.
turn.start({ sessionId, clientTurnId, input: Input, ...SessionParams }) → { turnId }
turn.retry({ sessionId, clientTurnId, ...SessionParams }) → { turnId }      // run again from the history, no new input
turn.steer({ sessionId, turnId, clientInputId, input: Input, host }) → { accepted: boolean }
turn.cancel({ sessionId, turnId }) → null

// Sessions
session.configure({ sessionId, model?, provider?, thinking?, permissionMode? }) → { model, thinking, permissionMode }
session.compact({ sessionId }) → { ok: boolean }      // resolves when the compaction is over
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

**Model switch.** Picking another model in a chat with history sends `session.configure({ model, provider, thinking })`
and shows it the way 1.3.0 showed its model-switch compaction: if the backend sends `compaction.started` /
`compaction.completed` meanwhile, the line reads "Compacting the conversation for <new model>". If the request fails,
the chat stays on the old model.

**Permission mode.** Changing Ask/Auto/Full while a turn runs sends `session.configure({ permissionMode })`. Pending
approval cards stay until the backend answers them with `approval.resolved`.

## Backend → client events (notifications)

Every event carries `sessionId`; the frontend routes it to the chat window that holds that session and ignores events
for sessions it doesn't have. Turn events carry `turnId` and are ignored unless they belong to the turn on screen (events
that arrive before the frontend knows the turn's id are held until it does). `seq` may be sent; it isn't checked yet.

```ts
turn.started      { sessionId, turnId, clientTurnId }
message.started   { sessionId, turnId, messageId, role: 'assistant', model? }
message.delta     { sessionId, messageId, text }                 // appended
message.completed { sessionId, messageId, text?, finishReason? } // text, when sent, replaces the message's text
reasoning.delta   { sessionId, messageId, text }                 // ignored: 1.3.0 shows no reasoning
tool.started      { sessionId, turnId, toolCallId, name, title? }// shows the ghost "working" status
tool.progress / tool.completed                                   // ignored: 1.3.0 has no tool cards
approval.resolved { sessionId, turnId, approvalId, decision: 'allow' | 'deny' }   // settles a pending card
compaction.started   { sessionId, turnId?, reason?: 'auto' | 'manual' | 'model-switch' }
compaction.completed { sessionId, turnId?, ok: boolean }
usage             { sessionId, turnId?, messageId?, provider, model, modelName?, input, cached, written, output,
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
  a compaction starts mid-turn.
- `finishReason` `length`, `content_filter` or `insufficient_system_resource` puts the matching note under the reply.
  A turn with no text gets "The model returned an empty response".
- `usage` feeds the local usage ledger (Settings → Usage), the per-reply numbers of the stats card, and the context
  fill shown in the plus menu (`context.used / context.window`).
- **Stop** (button or Escape) ends the turn on screen at once, settles pending cards as denied, and sends `turn.cancel`.
  Anything the backend still sends for that turn is ignored.
- A message sent while a turn runs is sent with `turn.steer`, and any pending approval card is answered `deny` with
  `reason: 'superseded'`. Messages never accepted stay in the chat unanswered, as in 1.3.0. Messages sent during a
  manual compaction or a model switch start a new `turn.start` after it, joined into one input.
- `turn.completed` with `status: 'error'` shows the error box with Retry (which sends `turn.retry`), plus "Open
  settings" when `error.action` is `open-settings` or the code is `auth`.

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

## Not in this version

From the audit's ABP v0, these are not used yet: `session.create`/`session.get`/`session.list` (the frontend keeps its
own chat list and a display copy of each chat), `session.rename`, `session.editMessage` (editing a diagram changes only
the frontend's copy), `turn.followUp`, `context.update` (the browser state goes with each turn instead), `seq` gap
detection, `tool.*` and `reasoning.*` rendering, and capability gating of the mode picker, effort slider and mini chat.

## Testing a backend against the frontend

`test/fixtures/scripted-backend.js` is a scripted ABP backend **for tests only** (no model behind it, never packaged).
It shows a minimal working backend and is what `npm run test:e2e` drives the real app with.
