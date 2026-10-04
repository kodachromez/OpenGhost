# Frontend-only backend-boundary audit — OpenGhost 1.3

- **Date:** 2026-10-04
- **Worktree / branch:** `~/og-wt-frontend-only`, `frontend-only-rust-backend`
- **Audited revision:** `81ea022` (`Make OpenGhost a frontend-only client for an external backend`)
- **Scope:** UI → `backend-client.js` → `desktop/preload.js` → `desktop/backend-host.js` → external process, including browser host tools, approvals, settings, and persistence.
- **Changes:** audit documentation only. No production fixes, restored backend, or committed test changes. No changes to `main`.
- **Method:** source and tracked-file/dependency searches; existing unit/Electron smoke suites; additional disposable Node/VM and headless Electron probes using synthetic messages/backends, local `data:` browser pages, and isolated app HOME directories. No AI-provider calls. Findings marked **reproduced** were exercised; other findings are source-level conclusions, not claims of comprehensive runtime testing.
- **Related:** [current interface](../backend-interface.md), [removal record](../frontend-only.md), [pre-removal audit/proposal](../backend-removal-audit.md). The proposal is historical, not the implemented contract.

## Executive summary

**The agent/provider runtime was removed, but this is not yet a reliable, cleanly bounded Rust integration target.** The layering is sensible and a sequential scripted backend works. There are no remaining provider SDKs, provider API implementations, model request builders, agent loops, non-browser agent tools, OAuth executors, or model-driven compaction/title implementations in the shipped code. However, some prompt policy, token estimation, provider defaults, and model-shaped compatibility/storage remain.

The highest-priority problems are:

1. **HIGH — stale reverse requests can act on a new turn.** `approval.request` and `host.tool` route by session only, not `turnId`.
2. **HIGH — Stop / Take Control do not reliably stop browser work.** Cancellation can miss queued/not-yet-registered jobs; multiple operations and main/mini chats share unsafe identifiers/state.
3. **HIGH — persistence/deletion are not a complete backend-session lifecycle.** Folder deletion never requests backend deletion; reload loses in-flight display state without a recovery path; locks protect only the frontend copy.
4. **HIGH — transport/process lifecycle is not hardened.** The advertised line limit is bypassable, writes are unbounded, RPCs have no deadlines, shutdown does not reap descendants, and a spawn-error shutdown race never resolves.
5. **HIGH — browser URL display parses untrusted URL text as HTML.** Reproduced markup injection in app chrome; not a demonstrated script-execution exploit.
6. **HIGH — tool cards do not exist.** Start is a generic ghost; progress/results/failure are discarded. This is documented preservation of 1.3, but fails the requested arbitrary-tool UI requirement.

**Browser readiness:** usable for a carefully serialized demo, **not ready for safe general Rust agent use**. **Provider/model/auth readiness:** the catalog and auth requests are broadly backend-driven, but defaults, capability gating, OAuth presentation, races, and validation still need work. OpenRouter is not excluded by a four-provider allowlist; the four-color palette is only one of several remaining assumptions.

**Results:** 29/29 unit tests; 14/14 source E2E checks; Linux distribution build succeeds; an additional packaged-app smoke run passes 14/14. Passing these tests does not establish boundary correctness: the browser smoke test only lists tabs, Retry is never clicked, and “opens again” does not actually reopen the application.

No **CRITICAL** finding is established. **HIGH** means a safety/data-loss/reliability blocker or an explicit major integration requirement missing; **MEDIUM** means a significant compatibility or correctness gap; **LOW** means a limited-impact issue; **CLEANUP** means unnecessary coupling/dead or historical code.

## 1. Architecture and what is already sound

- `backend-client.js` is the renderer's single agent RPC peer. Provider/model/auth UI and turns use it, not old provider globals.
- `desktop/preload.js:35–42` exposes four backend operations: `send`, `onMessage`, `onStatus`, `status`. Electron IPC carries structured-clone objects; stdio carries UTF-8 JSONL. Event objects themselves are not exposed to the page.
- `desktop/main.js:243–264` is a relay/lifecycle owner, not an agent. Backend/browser IPC checks `fromApp`; guest browser execution checks the webContents owner. Browser guests are sandboxed, context-isolated, and have no Node integration.
- `desktop/backend-host.js` spawns directly, with `shell` omitted/false. Paths with spaces and argument arrays do not undergo shell interpolation. Stderr is separated from protocol stdout. UTF-8 decoding uses `setEncoding`, so split multibyte characters are not individually decoded as corrupt chunks.
- With no configured backend, the app remains a UI and shows an error rather than silently using a model. Missing executable startup also remains usable (reproduced).
- API keys entered in Settings are passed to `auth.setKey`; there is no new frontend credential vault or provider login implementation. Saved key values are not requested back.
- Normal assistant Markdown, errors, provider labels, model labels, and approval text use escaping or `textContent` in the inspected paths. The URL-chrome exception is F10.
- The explicit frontend host-tool exception is the built-in browser. File selection, attachment/PDF previews, and display persistence are frontend services, not remnants of shell/git/file agent execution.
- Packaged `app.asar` contains the new boundary and no old backend modules, provider SDK, `test/`, or `docs/`.

## 2. Findings: bugs, reliability, and security

### F01 — HIGH: reverse requests ignore turn identity (**reproduced**)

**Evidence:** `chat.js:883–935,1491–1519`. `owner()` selects a chat by `sessionId`. `onApproval()` and `onHostTool()` check only that a current non-aborted turn exists; neither compares `p.turnId` to `turn.remote`. They also do not deduplicate `toolCallId` or `approvalId`.

**Reproduction:** during a scripted hanging turn, inject a JSON-RPC `host.tool` request with the correct session and `turnId: "DEFINITELY-OLD"`, `name: "browser_tabs"`, `args: {action:"new"}`. The tab count changes from 0 to 1. An analogous stale `approval.request` displays an approval for an arbitrary Rust tool.

**Impact:** a late request after Stop, Retry, or a new turn can execute against the new turn/browser, or ask the user to approve an obsolete action. Reverse requests arriving before acknowledgement have no correlation check either. Duplicate incoming RPC IDs overwrite the client's controller map while both handlers may already have side effects (`backend-client.js:193–211`).

**Recommendation:** validate session + turn + connection generation, including the start-in-progress case; reject stale/duplicate work before presentation or execution. Unknown sessions already produce `unknown_session`; extend that discipline to turns and call IDs.

**Resolution (fixed):** `Chat.claim` (`chat.js`) now gates `onApproval`/`onHostTool`: the request's `turnId` must equal the chat's running `turn.remote` (a request before the start ACK waits for it); a missing/other `turnId` or a repeated `approvalId`/`toolCallId` in the turn fails with `stale_turn` before any card or browser step; a stopped/ended turn answers cancelled. `end()` now cancels a browser step still running (or waiting for hand-back) for that turn, and the client refuses a reverse request reusing an in-flight JSON-RPC id (`duplicate_request`). Connection generation is covered by `close()` aborting every in-flight reverse request. Regression tests: `test/turn-identity.test.js`, plus the duplicate-id case in `test/backend-client.test.js`; contract in `docs/backend-interface.md` (Turn identity).

### F02 — HIGH: browser cancellation and user control are not execution barriers (**partly reproduced**)

**Evidence:** `chat.js:540–547,905–935,973–984`; `browser-panel.js:453–549`; `desktop/main.js:112–127`; `desktop/browser.js:519–623`.

- A turn holds only one `turn.tool` and one `turn.release`. Concurrent calls overwrite them. Stop cancels only the last browser job; reverse-request cancellation can cancel each request, but Stop does not fan out to all requests.
- Job IDs are `${conv.id}-${++this.tools}`. Main and mini chats use the same `conv.id` with separate counters: their first browser jobs can collide in the global `browserJobs` map. Reopened mini chats reset their counters too.
- Cancellation during `BrowserPanel.ensure()` occurs before `browser:run` registers a main-process job. `browser:cancel` is then a no-op; the operation may dispatch after readiness. `browser_tabs` mutations run in the renderer without an abort signal at all.
- `take()` changes UI/control state, not an in-flight operation's signal. A call already waiting in a per-tab queue passed the user-control check earlier. It can run while the user owns the browser.
- Main-process cancellation checks are sparse. A click checks before the 420 ms pointer delay, not after it; typing can click, clear, insert and submit after its last check. Navigation, screenshot, read, and sleep-only wait are not interrupted by abort. Timing out a Promise does not stop the underlying navigation/CDP operation.
- On normal `turn.completed`, `end()` releases browser drive UI but does not cancel outstanding reverse requests/browser jobs. Their turn controller is not aborted. Backend crash aborts incoming controllers, but the same execution gaps remain.
- With multiple hand-back waiters, `turn.release` only reaches the last one on message/Stop. The others may remain waiting or resume with an inappropriate snapshot. Abort listeners/waiters are not all removed on early return.

**Reproductions:** (1) two browser requests (2-second wait, then type), followed by Stop at ~100 ms: UI becomes idle, but neither request answers until ~2.4 seconds; both eventually report cancelled. (2) start a click on a local button, Take Control during its visible cursor delay: the button still clicks and the host result is `ok` while `userHas === true`.

**Recommendation:** one globally unique job ID, a per-turn set of jobs/reverse requests, cancellation latched before registration, and an explicit browser control lease checked immediately before side effects. Decide concurrency at panel level, not just per guest. Do not mistake a cancelled result for proof that no side effect occurred.

**Resolution (2026-10-04):** every `host.tool` step now gets its own abort controller, tied to both the request's signal and the turn's, with a job ID unique across main and side chats. Stop therefore fans out to every step and hand-back waiter of the turn and answers `cancelled` at once, and late browser results are dropped. Before it acts, `BrowserPanel.run` checks for Stop and for user control (after `ensure()`, and before `browser_tabs` mutations). Take Control cancels the steps under way: a step stopped that way waits for the hand-back and reports `handed-back`, while one that already finished keeps its real result. The main process checks for abort after the cursor delay and between the input actions of click and type, the sleep-only wait can be interrupted, and a cancelled run is marked `stopped`. Regression tests: `test/cancellation.test.js`. A turn that ends (including on `turn.completed`) stops its outstanding steps too. Navigation, screenshot and read are still not interrupted mid-call.

### F03 — HIGH: folder deletion does not delete backend sessions (**reproduced**)

**Evidence:** `chat-list.js:491–496,530–532`; `library.js:212–233`; `chat.js:427–443,1474–1488`.

Folder deletion calls `library.removeFolder()` **before** `chat.removeFolder()`. `Chat.remove()` then looks up the already-deleted record; its `if (record && Backend.can('sessions.delete'))` guard skips `session.delete`. A synthetic request recorder observed **zero backend calls** for folder deletion with deletion capability enabled.

Single-chat deletion sends only the parent session ID, not `<id>:mini`; recursive backend deletion is not specified. Deletion failures are swallowed. Offline deletion is not queued for reconnect. Mini-chat clear reuses its session ID immediately, without waiting for deletion acknowledgement, so a new start can race the old delete on an async Rust server.

**Impact:** invisible backend histories survive user deletion, potentially including sensitive inputs and backend files. Clearing can race new work.

**Recommendation:** capture session IDs before index removal; specify parent/side deletion semantics, acknowledgement/error handling, and durable deletion intent. Do not make successful-looking local deletion imply backend erasure.

**Resolution (chat/folder deletion):** deletion now snapshots child IDs and awaits exact parent/mini `session.delete` acknowledgements before removing each chat locally. Errors (including offline/unsupported deletion) restore the row/folder and display the failure for retry; unrelated sessions are untouched, overlapping deletes are serialized, and new work on a deleting chat is blocked. Partial folder failure retains failed/unattempted children. Focused regression coverage: `test/deletion.test.js`; idempotent exact-ID deletion contract: `docs/backend-interface.md`. Mini-chat Clear and durable offline deletion queues remain out of scope.

### F04 — HIGH: session persistence/reload has no reconciliation path

**Evidence:** `chat.js:714–746,973–995,1093–1096,1189–1197`; `library.js:237–269`; `script.js:43–47`; `desktop/main.js:185–196`; interface “Not in this version”.

The backend owns authoritative history, but the frontend only persists its display copy and never calls `session.get/list/create`. A turn sends **only new input**, not the prior transcript. There is no session-existence handshake/import, cache cursor, replay, or way to repair a partial display transcript.

- Reload during a turn destroys the renderer and webviews but leaves the backend process/turn alive. Main only cancels browser jobs on window closure/quit, not reload. The new client sends `initialize` again and resets numeric RPC IDs. Pending responses/reverse requests from the old page have no connection epoch and may be lost or confused with new requests.
- Messages are saved at turn end/compaction, not durably as events arrive. Reload/crash can leave the sidebar entry with missing user/assistant text. Backend work may have continued, but the UI cannot retrieve it.
- Old chats still display, but the new backend has never received their history. Starting in one creates an empty backend session with the same ID unless a separate migration exists. There is no import in this contract.
- A backend restart/replacement that loses sessions has the same silent “visible history, empty model history” problem.

**Recommendation:** define page-reinitialization semantics and durable session recovery before integration. Keep display caches frontend-owned, but make backend session existence, revision, and recovery explicit. Rust must currently persist sessions independently and treat repeated `initialize` as a connection reset; that still cannot restore missing frontend text.

**Resolution:** saved chats now reconcile through capability-gated `session.get` before continuing. Durable pre-dispatch turn/input checkpoints and backend display replay recover active or missed completed turns without resending them; a snapshot revision fences replay from live events. Missing sessions/unknown turns fail visibly without replacing the display cache or importing it as model history. Explicit session-incarnation/create-only guards prevent implicit empty-session creation; renderer-scoped RPC IDs and connection-reset semantics cover reload. Contract: `docs/backend-interface.md`; focused regressions: `test/session-recovery.test.js`. The external backend must implement the negotiated recovery contract; legacy backends fail closed.

### F05 — HIGH: “locked chat” does not protect backend history

**Evidence:** `library.js:296–356`, `chat-lock.js`, `chat.js:359–423,703–710`; capabilities are stored but `sessions.encrypted` is not checked.

PBKDF2/AES-GCM still encrypts the frontend display copy. No lock/unlock/protect message reaches the backend, and turns send plaintext input/context. A Rust backend persisting sessions in plaintext defeats any expectation that a locked conversation is encrypted everywhere. Even a frontend-protected chat's original title can be present in its workspace folder name. Backend credentials and browser cookies are outside this lock as well.

This is not proof that the future backend will store plaintext; it is an **unresolved security promise** in the current UI/protocol. The old audit already identified it, but removal did not resolve it.

**Recommendation:** explicitly scope the lock UI to the display cache until a negotiated backend protection capability/lifecycle exists. Do not advertise whole-session confidentiality based on this frontend lock alone.

**Resolution:** labels now say “local view”; setup, management and locked-screen notices explicitly limit encryption to this app’s display cache and exclude backend/session history and other data. Forgotten-password copy is cache-only. Existing frontend encryption/locking and backend storage are unchanged; focused regressions: `test/lock-ui.test.js`.

### F06 — HIGH: JSONL size limit and flow control do not enforce bounded transport (**limit reproduced**)

**Evidence:** `desktop/backend-host.js:14–15,97–125`; `desktop/main.js:247`; `desktop/preload.js:38`.

- The 64 MiB check applies only to the **remaining unterminated buffer after complete lines are parsed**. A complete oversized line is accepted; the counter uses JS UTF-16 length, not bytes. A direct receive probe delivered a valid message containing **67,108,865 ASCII payload characters plus envelope** and it was forwarded.
- Even realistic small pipe chunks can complete a just-over-limit line in the same chunk as its newline; the complete line is parsed before a length check. Non-ASCII data can exceed the byte limit much earlier.
- Dropping an oversized fragment resets the buffer, but does not discard through the next newline. Its suffix can be treated as a new line/message.
- Outbound messages/attachments have no size bound. `stdin.write()`'s return value and drain/backpressure are ignored. There is no bounded IPC queue/pending-request count/rate limit; a nonreading child can accumulate memory.
- JSON depth, event text, model lists, approvals, and the `turn.early` buffer have no limits. Parsing and some rendering are synchronous. Stderr is streamed wholesale to the app console with no rate/size cap or secret redaction; malformed stdout logs the first 200 characters, which can include secrets.

**Recommendation:** byte-based framing before parse, a discard-until-newline state, bounded queues/write acknowledgements, field-specific limits, log throttling/redaction, and controlled failure instead of indefinite waiting after discarded responses.

**Resolution (transport bounds):** the host now enforces 64 MiB of raw UTF-8 bytes per line (excluding LF, including whitespace/CR) before decoding/parsing, and discards oversized input through the next newline. Outbound messages have the same limit; Node's stdin queue is capped at 64 MiB + 1 byte including framing, with oversized/full-queue sends returning `false` rather than accumulating another queue. Accepted writes drain normally. Regressions: `test/backend-transport.test.js` (complete/split oversize, exact limits, UTF-8 splits/bytes, discard/resync, stalled writes and drain). IPC delivery acknowledgements, field/depth/event limits and log policy remain unchanged.

### F07 — HIGH: shutdown reaps only the direct child; error/reentrancy paths can hang (**partly reproduced**)

**Evidence:** `desktop/backend-host.js:66–94,128–137`; `desktop/main.js:285–300`.

Normal quit sends `{id:"shutdown",method:"shutdown"}`, ends stdin, then SIGKILLs the direct child after 2 seconds. This is useful but incomplete:

- Descendants are not tracked/killed. A scripted wrapper spawned a local idle Node child and exited on shutdown; `BackendHost.stop()` resolved while that descendant remained alive. The audit explicitly killed the probe descendant afterward. No POSIX process-group/Windows job-object ownership is present.
- `stop()` only resolves on `exit`. Calling it immediately after spawning a missing executable produces `error`/`close`, not `exit`; the promise never resolves (**reproduced**). Kill failure also has no fallback completion.
- Multiple `before-quit` invocations can run concurrent `stop()` calls, timers, and writes after `stdin.end()`. There is no shared stopping promise or quit-in-progress flag.
- Status moves to exited on `exit`, not stream `close`; trailing stdout can arrive after “closed”. Child stream listeners are not generation-guarded for a future `start()`.
- Abrupt application termination has no supervisor/parent-death mechanism. The E2E harness's SIGTERM shutdown is not a test of the normal Electron quit path.

**Recommendation:** idempotent stop, bounded completion on `error/close/exit`, tested reaping policy for the entire backend tree, and one lifecycle generation per process. Rust should close cleanly on both `shutdown` and stdin EOF, but frontend cleanup must not depend exclusively on backend cooperation.

**Resolution (2026-10-04), descendants only:** on POSIX the backend now starts in a session of its own (`detached`), and whenever it exits (after `shutdown`, killed after the grace period, or crashed by itself), the host kills its process group and every process still in that session. On Linux, the session's members are read from `/proc`, so the separate process groups a backend runs its tools in are included. Only a descendant that calls `setsid` itself escapes. On Windows, the grace-period kill is now `taskkill /T /F`, but descendants of a backend that exits by itself are not tracked there, and macOS gets only the process-group kill. Regression tests: `test/backend-host.test.js` (graceful, stubborn and crashing backends, each with a same-group and a separate-group descendant). The other bullets above (stop hanging after a spawn error, concurrent `before-quit`, close-vs-exit ordering, parent death) are still open.

### F08 — HIGH: unbounded waits and handshake races can leave incorrect frontend state (**partly reproduced**)

**Evidence:** `backend-client.js:70–115,129–153`; `chat.js:24–42,686–744,777–787`.

There is no initialize/request timeout, heartbeat, or turn inactivity policy. A running but silent backend stays initializing forever; an unanswered settings/login/configure/cancel request stays pending forever. A child that closes stdin but stays alive still looks running: stdin errors are ignored, send failure is not returned through IPC, and the client waits for a response that cannot come.

`initialize()` has no connection generation/state check after either await. In a fake-transport probe, delivering its result and then an exit before its continuation runs produces **`Backend.state === 'ready'` while `Backend.process.state === 'exited'`**. This proves a client-peer race, not its frequency under Electron's separate IPC tasks. Waiting for DOMContentLoaded can also outlive closure and send initialization to an already-dead process. The one-shot `ready` promise does not describe later reconnects.

Stop aborts `turn.done`, but `drive()` is still awaiting `work()`. Attachment reading and `UserContext.ready` do not race the turn signal. **Reproduced:** a deferred local attachment followed by Stop leaves `chat.busy` true until the attachment resolves. Crash notification also only finishes `turn.done`, so a turn waiting in such preparation does not immediately end. A `turn.completed` event does not release a still-unanswered `turn.start` RPC either.

**Recommendation:** per-operation deadlines (with intentionally long/user-interactive auth exceptions), transport delivery errors, epoch-checked initialization, and cancellation racing *all* preparation/lifecycle waits. Keep the UI responsive without claiming backend cancellation has finished.

**Resolution (2026-10-04), request deadlines only:** every request the client sends (`BackendClient.call`, so `initialize` too) now has a deadline: 60 seconds by default, 15 minutes for `auth.login` and 10 for `session.compact`. An unanswered request rejects with a `timeout` BackendError, leaves the pending map, its abort listener is removed, and the backend gets `$/cancelRequest`. A reply that arrives afterwards finds no pending entry and is ignored. A backend that never answers `initialize` now ends `unavailable` instead of initializing forever. Answered requests clear their timer, so they behave as before. Regression tests: the timeout cases in `test/backend-client.test.js`; contract in `docs/backend-interface.md` (Envelope). The other parts of this finding (stdin-closed delivery errors, epoch-checked initialization, preparation waits racing Stop, `turn.completed` releasing `turn.start`) are still open.

### F09 — HIGH: event bookkeeping requires stricter ordering than the interface says (**partly reproduced**)

**Evidence:** `chat.js:799–876`; `backend-client.js:164–190`; interface event definitions.

- `seq` is ignored; duplicated deltas append twice and usage is counted twice. **Reproduced:** the same `message.delta` with `seq:77` appended `DUPDUP`.
- There is one `turn.part.text/base`, not a buffer per `messageId`. Interleaved messages or a delayed final text for an earlier message overwrite the current message. **Reproduced:** start A → delta `alpha` → start B → delta `beta` → complete A with `ALPHA` leaves `alpha\n\nALPHA`, losing B.
- Events with `turnId` are buffered before the remote ID is known; documented deltas/completions **without** `turnId` are dropped if their message's buffered `message.started` has not been processed yet. “Events before the turn ID are held” is not generally true.
- A `turn.started` without `clientTurnId` may bind an old turn to a newly starting local turn. First remote ID wins even if the later start response disagrees.
- Session-only usage/compaction events are applied to whichever turn currently exists; late old-turn events can affect a new turn. With no current turn, usage is dropped entirely, including valid final cost after Stop/completion. `session.updated` is accepted regardless of a cancelled turn, but only for loaded conversations.
- Notifications are not state-validated before listeners run, and listener exceptions are not isolated. One malformed callback can prevent wildcard routing of that notification.

**Recommendation:** explicit event IDs/sequence and epoch rules; turn identity on every turn event; per-message state; bounded early buffering; idempotent usage; and a definition of post-cancellation accounting separate from visible text suppression. Until fixed, Rust must serialize messages, emit each event once, and obey the ordering recipe in §7.

**Resolution:** session sequence gates now reject duplicate/backwards events; exact turn correlation and bounded early buffering protect new turns. Per-message buffers/finalization preserve interleaved text and original-part usage; completion/Stop freeze output immediately, with bounded, correlated late accounting kept separate. Tool starts are idempotent, and notification state checks/listener isolation protect routing. `docs/backend-interface.md` defines these rules (including explicit identity for standalone compaction), superseding the F09 ordering workarounds in §7. Focused regressions: `test/event-ordering.test.js` and notification cases in `test/backend-client.test.js`; recovery and reverse-request identity tests also pass. No tool-card UI or live gap repair was added.

### F10 — HIGH: untrusted browser URL becomes HTML in app chrome (**reproduced**)

**Evidence:** `browser-panel.js:29–33,337–355`.

`element(tag, className, html)` sets `innerHTML`. `syncBar()` passes `decodeURI(rest)` from the active URL to it. A URL path such as:

```text
https://example.invalid/%3Cb%20id=%22audit-injected%22%3Einjected%3C/b%3E
```

creates a real `#audit-injected` element in `.browser-url-view`. The probe only changed the local tab record and called `syncBar()`; it made no external request. Backend navigation, user navigation, and page redirects all eventually feed this display path.

**Impact:** HTML/UI spoofing in the trusted renderer, including potentially resource-loading markup and inline styling. The CSP blocks ordinary inline script/event-handler execution; **arbitrary JavaScript execution was not demonstrated**. Do not equate that mitigation with safe HTML insertion.

**Recommendation:** use `textContent` for all URL pieces; test encoded delimiters/quotes/markup. Review other calls to HTML-taking helpers with untrusted values.

**Resolution (2026-10-04):** `browser-panel.js`'s `element()` helper now sets `textContent`, so the host, scheme and decoded path/query of the address bar are drawn as text in the same spans as before; the tab's fixed button markup is set separately. The other `innerHTML` sinks were reviewed for backend or page text (provider/model names and auth rows, usage and limit rows, stats card, model confirm, approval cards, chat errors and compaction notices, link chips, tab titles and load errors): each already escapes or uses `textContent`, so nothing else changed. Regression tests: `test/untrusted-text.test.js` (address bar, approval card and chat error with `<img onerror>`, `<script>` and attribute-breaking strings); the address-bar case fails on the previous revision.

### F11 — HIGH: arbitrary tool-card lifecycle is not implemented

**Evidence:** `chat.js:828,839–841`, interface “tool.progress / tool.completed — ignored”.

There is **no tool-card implementation or tool-call view state**. `tool.started` only shows a ghost and ignores the name/title/arguments. Progress, completion, result, failure, unknown tools, concurrent calls, and late results have no display. On backend crash there are no cards to mark failed/cancelled. This is an intentional preservation of old 1.3 behavior, not an undocumented regression, but it fails the requested tool-card readiness check.

Approvals are generic enough to name unknown tools and display JSON fallback arguments. They are not substitutes for result cards. Their completion dismisses them rather than leaving a durable audit trail.

**Recommendation:** define generic tool lifecycle/payload/error schemas and a per-call display model in a later implementation. Do not restore old tool-name-specific execution or approval policy. Rust cannot implement its way around absent frontend rendering.

### F12 — MEDIUM: Retry and steering lose important intent/acceptance information (**Retry reproduced**)

**Evidence:** `chat.js:737–775,988–992,1244–1267`.

- Every error shows Retry, even `retryable:false`/`action:none`, invalid requests, or no backend.
- Retry always sends `turn.retry` without original input or the target failed turn ID. **Reproduced:** a message failed before delivery while unavailable, then Retry after readiness sent `turn.retry` with no input. A real backend has no history to retry. The same issue applies to attachment-preparation/start-validation failures and backend history loss.
- No idempotency/acceptance record differentiates “request reached backend, response lost” from “never delivered”. Retrying can repeat effects unless Rust invents semantics beyond the document.
- `turn.steer` ignores `{accepted:false}` and swallows errors; only `input.accepted` changes history. The user sees no explicit rejection. Unaccepted bubbles are saved locally anyway, but are not sent again on the next ordinary turn.
- Steering calls have no abort signal. Attachment reads for successive interjections can complete in a different order from the bubbles; RPC order then differs from user order. A waiter on `turn.started` never resolves if start fails/cancels without a remote ID.
- Sending an interjection denies **all** pending approvals as superseded. That is documented UI behavior, but Rust must not treat the denied action as completed. A permission-mode change is sent only for the Chat instance whose picker changed, despite sharing the global setting with other main/mini chats.

**Recommendation:** distinguish retry-start from retry-accepted-turn; correlate retries explicitly and define idempotency. Honor steering acceptance/errors and preserve input order. Abort/drain pending steering work when its turn ends.

**Resolution:** Retry now preserves undelivered input, targets accepted failures by `failedTurnId`, and reconciles uncertain dispatches without resending; missing identity/context and explicit no-retry errors fail closed. Steering is serialized, reports rejection/errors, and aborts/drains on turn end with unconfirmed inputs visibly retained. Contract: `docs/backend-interface.md`; focused regressions: `test/retry-steering.test.js` plus session-recovery coverage. Approval/mode behavior is unchanged.

### F13 — MEDIUM: model/capability defaults invent backend behavior

**Evidence:** `settings.js:4–10,75–85,247–312`; `model-stage.js:56–60,389–442`; `script.js:70–83`; `mini-chat.js:107–113`.

- Missing **or empty** `thinkingLevels` becomes `['none','low','high','max']`. Missing `vision` becomes true. Missing context becomes a 1,000,000-token fallback in context UI. **Reproduced:** a model with `thinkingLevels:[]` gained all four levels and vision support.
- `defaultThinking` is secondary to the saved/global default `high`, even on first selection when high is supported. A Rust model with no thinking cannot express that using an empty list.
- Output limits and broader model capabilities are not represented by the current `Model` type. Attachment acceptance/size is not gated by model vision or backend attachment capabilities. `config.vision` is effectively display metadata, not input enforcement.
- Stop, Retry, steering, mini chats, and the three modes are not gated by their advertised capabilities. Only `auth.providers`, `compaction.manual`, `sessions.delete`, and `usage.limits` are checked.
- A removed model silently resolves to a global/first model, which may switch providers unexpectedly. Picker IDs use `${provider}:${model}` without escaping, allowing collisions when both identifiers contain colons.
- Model-switch confirmation promises the old model will compact first, regardless of backend behavior. `session.configure`'s canonical returned model/thinking/mode is ignored. `session.compact`'s `{ok:false}` is also ignored as a result (only events create/finalize the marker).

**Recommendation:** explicit unsupported/unknown capabilities, no invented thinking/vision/window values, backend-authoritative configuration results, capability-driven controls and loss-of-model UX. UI selection belongs here; runtime capabilities and whether compaction is needed do not.

### F14 — MEDIUM: provider/auth UI is extensible but not fully general or race-safe

**Evidence:** `settings.js:20–73,104–111,320–353,356–415,419–499`; `settings-usage.js:9–10,31,174–198,331–370`.

- Arbitrary provider IDs/names/groups and model lists are accepted, including an OpenRouter-like fifth provider. No runtime four-provider allowlist was found. No provider-specific icon is required; the picker uses its generic glyph.
- Only `apiKey` and `oauth` method kinds are rendered. There is no method ID on auth requests, device-code/verification-link challenge UI, backend-requested external-login URL handler, or generic auth form schema. Multiple same-kind methods collide in DOM IDs/maps; API-key and OAuth status nodes are keyed only by provider. Rust must execute OAuth itself and communicate only waiting/connected/error/account status with today's UI.
- Provider add/remove refresh happens on `ready` or Settings open. `models.changed` refreshes only models; there is no provider-catalog change event. An `auth.changed` for a new ID does not create its UI section.
- Debounced key saves can overlap. `auth()` applies returned status *before* `saveKey()` checks whether typed text changed. Older saves/login results can overwrite newer cancel/logout/status. Refreshing the whole provider list can also overwrite an intervening auth event; it has no generation check.
- Provider `methods`, account plan, model name, limits arrays, currency codes, etc. are only partially validated. For example non-array `methods`, a non-string plan/name, or an invalid currency can throw in rendering. String labels are generally escaped, but malformed types/huge values remain a denial-of-service risk.
- Plain objects keyed by arbitrary provider IDs are unsafe for reserved names. For example usage totals for `__proto__` access `Object.prototype` instead of a fresh accumulator (`usage.js:71–84`). Provider IDs containing `|` are silently excluded from usage; this restriction is undocumented.
- Account limits refresh can leave stale/infinite loading displays: null data paints placeholders, late responses can outlive logout, and a provider without `limits:true` does not gain its missing limits container merely because the first response contains windows. Backend closure does not immediately clear/re-render an already-open usage page.

**Recommendation:** validate catalog/status types, use Maps/safe keys, define method IDs/challenge types and catalog invalidation, and serialize/version auth mutations. No frontend provider API/auth execution should be added.

**Resolution (frontend auth/rendering scope):** Per-provider operation tokens and guarded catalog refreshes prevent stale key/login/logout/cancel results from replacing newer state; unversioned `auth.changed` triggers a fresh read, reconciled after pending mutations. Provider/method/status fields are normalized, opaque IDs use safe maps and generated DOM IDs, and both existing method rows retain status without collisions. Dynamic providers/models remain backend-driven. Focused regressions: `test/provider-auth.test.js`, `test/usage.test.js`. New auth challenge/method-ID protocols, usage-ledger `|` encoding and account-limits lifecycle changes are not included.

### F15 — MEDIUM: malformed envelopes are accepted and errors are inconsistent

**Evidence:** `desktop/backend-host.js:43,97–125`; `backend-client.js:27–31,82–98,175–211`; `approval-card.js:181–207`.

`isMessage` only requires a non-array object with `jsonrpc:'2.0'`. Method/id/params/result/error shape, mutually exclusive result/error, and safe integer IDs are not checked. A response with neither result nor error resolves as `undefined`. **Reproduced:** both an empty initialize result and a `protocolVersion:'999'` result make the client ready. Response IDs use JS Map identity; numeric and string IDs are different.

JSON parse failures and oversized dropped responses do not reject the corresponding request. Unknown reverse methods correctly receive `-32601`, but most runtime errors become `-32000`/`unknown`, while browser failures are often successful RPC results with `isError:true`. Invalid params are not consistently `invalid_request`/`-32602`. Unknown `browser_tabs.action` silently lists tabs; many malformed browser fields are coerced/defaulted rather than rejected.

An empty/malformed approval presentation object suppresses the JSON-arguments fallback, and an omitted/invalid `reveal` hides code that may have been supplied. Long fallback arguments are serialized and inserted in full; long titles create one animated span per word. Diff *display* is limited to 14 lines, but the full strings are still parsed/split.

**Recommendation:** validate envelopes, negotiated protocol, known message payloads, and host command parameters before dispatch; define application errors vs RPC errors; cap presentation sizes while preserving accessible full details. Unknown tool names should remain displayable, not executable by frontend fallback.

### F16 — MEDIUM: browser lifecycle/results are not deterministic enough for unconstrained agent scheduling

**Evidence:** `host-tools.js`; `browser-panel.js:218–278,484–568`; `desktop/browser.js:101–108,154–205,288–347,406–435,484–623`.

- Discovery always publishes all 11 schemas, even without a desktop browser bridge. A closed/empty browser snapshot is `null`, indistinguishable from unavailable; there is no capability/status notification for ready/gone/control owner.
- `ensure()` waits for first `dom-ready` with no timeout/rejection on tab destruction, guest crash, or failed first load. Closing/replacing a tab during readiness can strand the call. Later crashes have no explicit renderer recovery handling.
- Tabs are positional 1-based numbers, not stable public IDs; opening beyond 12 silently evicts another tab. All other operations use the active tab; callers cannot atomically bind a page revision/tab to an action. Main queues per guest, while renderer tab mutations are unqueued.
- User tabs/address/reload controls remain usable while the browser stage overlay intercepts page clicks. Multiple chats share one active tab/control owner. Manual navigation, redirects, popups, and overlapping operations can change the target between observation and action.
- Snapshot refs are per-document generated element IDs; they reset on navigation and do not include document identity. A stale ref may resolve to a different element on a new page. A covered click deliberately clicks the covering element and reports that fact *afterward*.
- The snapshot uses heuristic accessibility/DOM/viewport rules and timed DOM quiet, not a stable load transaction. Cross-origin iframes are unreadable; shadow DOM coverage differs between snapshot and full-page extraction.
- `browser_read` takes `outerHTML`, truncates at 4 Mi characters, removes scripts/styles/form controls and flattens to Markdown. It can include hidden text (not computed visibility), omit shadow-root/dynamic form content, and cut HTML without a truncation flag. Pagination recomputes the page each time, so changing pages can skip/duplicate text. Offsets are JS string characters, not UTF-8 bytes.
- Navigation failures/timeouts and wait-for-text expiry can return `status:'ok'` with a warning only in text. Screenshot scale/dimensions are only described in prose externally, not structured fields. Downloads are a global, one-shot “told” list and can be attributed to the wrong chat/operation.
- `signedIn` is a heuristic password-submission observation, not verified login: failed submits count, OAuth/passwordless flows may not, and logout does not clear it. This must not be treated as provider auth state.

**Recommendation:** explicit availability/control/page identity, structured metadata and error codes, cancellation-aware readiness, stable tab handles, and operation serialization/leases. Rust should currently re-snapshot after navigation/control changes and never reuse refs across documents.

### F17 — MEDIUM: “renderer cannot network” is broader than what CSP enforces

**Evidence:** `index.html:7`; `link-chip.js:75–105`; `media-embed.js:124–193`; browser navigation/guard; `docs/frontend-only.md` network section; `test/boundary.test.js`.

No provider endpoint or direct AI-provider request implementation remains, and `connect-src 'none'` blocks renderer fetch/XHR/WebSocket/EventSource-style provider clients. However, `img-src` permits **all HTTP(S)**: renderer favicons, video thumbnails, and permitted image loads do make network requests. The browser partition can also navigate arbitrary HTTP(S)/local/data URLs. There is no AI-provider hostname deny rule across these channels.

Thus **“no frontend model API calls” passes source inspection; “the renderer literally cannot contact an AI-provider host” is not proven and is not enforced** (an image URL can target any HTTPS host). A connect-src regex test cannot establish the latter. Do not make real provider calls to test this; use a local synthetic endpoint/request interceptor.

**Recommendation:** accurately document network exceptions and decide whether a strict provider-domain network policy is required. Keep AI runtime requests exclusively in Rust; do not break the intended general-purpose browser by confusing browsing with a provider client.

### F18 — MEDIUM: prompt policy and model-shaped legacy data remain

**Evidence:** `render-guide.js:8–19,40,54–56,64–67,86–99`; `chat.js:1107,1189–1197`; `library.js:237–267`.

The render guide is a legitimate frontend capability/syntax description, and the backend chooses whether to use it. But it still contains old **behavioral instructions**, e.g. “You must visualize”, “An answer ... carries several drawings”, “Never use horizontal rules”, and restrictions on how to obtain image URLs. This is prompt policy, not just a schema of what renders. It is not a running agent loop, but the assertion that no prompt remnants remain is too strong.

Existing transcripts are returned and re-saved wholesale, not projected to a display-only schema. `steps`, provider-native blocks, cache/reasoning fields from pre-removal chats can survive every subsequent save. `promptOf()` explicitly reads old OpenAI-shaped `content[].image_url.url`; stats examines old `entry.steps`. These are real compatibility paths, not old runtime calls. Their necessity/migration lifetime is undocumented, and malformed legacy image objects can throw.

**Recommendation:** keep render syntax/examples, move behavioral prompting to the backend. Define an explicit legacy display migration/projection and supported retention policy instead of quietly retaining arbitrary model-facing history. Do not revive the old backend to open these chats.

### F19 — LOW: residual frontend token estimate and provider-specific presentation defaults

**Evidence:** `settings-general.js:10,17–19,131`; `user-context.js:8–15`; `settings-usage.js:9–10,31`; `settings.js:4,91`.

- `CHARS_PER_TOKEN = 3.2` still estimates pinned-text-file tokens. This is display-only, not runtime token counting/compaction, but is exactly a leftover token estimator. Backend/context measurements should replace it or the UI should show characters/bytes.
- Pinned-file quotas use a local 200,000-character budget and weights of 4,000 for an image and 300 for a path-only file. These are frontend heuristics, not model limits. Keep upload/resource limits clearly separate from context budgets.
- Usage has four cyclic colors and known-ID overrides for `chatgpt`, `openai-codex`, `openai`, `anthropic`, `deepseek`. A fifth provider renders, but colors collide/change with ordering; names/icons/colors are not all backend-described.
- `deepseek.effort`, removal of `deepseek.model`, and fallback API-key placeholder `sk-…` remain historical conventions.

**Recommendation:** remove misleading provider/token defaults; migrate preferences once if needed. These do not justify restoring provider code.

### F20 — CLEANUP: misleading recovery UX and avoidable coupling

- `BackendClient` can reinitialize on a synthetic later `running` status, and the unit test says it “comes back”, but the production main process has **no restart/reconnect command or automatic restart**. After crash/configuration changes the user must relaunch the app. This is documented, not a hidden feature. Raw spawn/config errors exist in `Backend.process` but the normal UI says only “No backend is connected”.
- The interface says Stop “button or Escape”; `send-button.js` only dispatches Send. Main Stop is Escape (`script.js:194–201`), and dialogs/menus/focused webviews can consume it. Mini-chat Escape has its own behavior. There is no general Stop-button path to verify.
- Preload subscriptions do not return unsubscribe functions. The single long-lived client masks this, but replacement/reconnect instances would accumulate listeners. Client listener errors are not isolated.
- `chat.js` combines rendering, turn protocol state, approval lifecycle, browser scheduling, and display persistence. Global `window` dependencies/load order and the three cancellation layers make ownership hard to reason about. Extract a small typed/stateful protocol adapter later rather than adding Rust-specific branches here.
- Browser URL normalization is duplicated in renderer/main (including different IPv6 handling). Browser timeout helpers time out callers rather than underlying work. `SNAPSHOT`/some imports in `desktop/browser.js` are unused. These are cleanup candidates, not reasons to redesign the UI during this audit.
- Historic README/UI claims about compaction, supported agent tasks and Ask/Auto/Full are not capability-qualified. The earlier audit describes a proposed protocol/mock/adapter architecture beyond what this branch implements; it should not be mistaken for the current compatibility specification.

### F21 — LOW: executable configuration needs an explicit trust/portability contract

**Evidence:** `desktop/backend-host.js:19–40,70–72`; `desktop/main.js:231,247–264`; `desktop/preload.js`.

- `OPENGHOST_BACKEND` takes precedence over `backend.json` in Electron userData. A plain string is the **whole executable name**, not a shell command line; `"/path/backend --flag"` tries to execute that whole name. Use a JSON string array for arguments. Shell metacharacters are literal in this path, so no shell-injection sink was found. Choosing a shell explicitly in trusted configuration is of course still arbitrary code execution by design.
- Arguments are passed directly as an array, but all entries must be nonempty strings, unnecessarily rejecting a valid empty argument. Whitespace in a plain path is trimmed; `~`, `$HOME`, quotes and escapes are not shell-expanded. A command beginning `[` is parsed as JSON. Bare executable names use inherited PATH; relative paths resolve from the home working directory, not the repository or `backend.json` directory.
- Whitespace-only environment configuration suppresses the file fallback. Some invalid `command` types quietly mean no backend, while malformed JSON throws. Errors from the environment are misleadingly prefixed `backend.json:` in main. There is no config-file size/permission/ownership check or executable allowlist. The UI cannot alter this command over the backend bridge, which is a useful restriction.
- The child inherits **all of the app environment**, including any API keys, proxy variables, PATH and runtime injection variables present there; its cwd is `os.homedir()`. The turn's `cwd` does not change the process spawn cwd. There is no privilege separation/backend sandbox: an installed backend is trusted user-level code, not an untrusted plugin contained by Ask mode. Rust should not log secrets to stderr.
- Backend-provided content has no direct `eval`/shell execution path in the inspected relay/rendering code, but it can request the authorized browser host tools. `fromApp` checks a window sender and a `file:` frame URL, not the exact owning app webContents/root URL; older store/folder IPC channels also lack uniform sender checks. Browser guests lack the app preload and browser jobs verify ownership, so this is **defense-in-depth hardening**, not a demonstrated guest-to-main exploit. Prefer exact sender/frame ownership checks if the shell gains additional windows.

**Recommendation:** document trusted-local-backend execution, supported command forms, inherited environment/cwd and platform differences; validate configuration consistently; preserve direct spawning and narrow IPC. Do not claim provider isolation means the configured executable is sandboxed. Windows/macOS spawning/cleanup were source-reviewed only, not run here.

## 3. Stale backend search and responsibility audit

Searches covered tracked app JS/HTML, desktop code, package/lockfile, imports/script tags, and the built ASAR; historical docs/tests were distinguished from shipped behavior. No exhaustive third-party Chromium security audit is implied.

| Searched responsibility/remnant | Result |
|---|---|
| Provider API endpoints/requests; Anthropic/OpenAI/DeepSeek SDKs; old `llm/auth/keys/tool` IPC | Removed from production source and package. Only generic `auth.*` over Backend remains. |
| Model request construction, provider history serialization, agent loop, tool decisions | Removed. `SessionParams`/structured attachments are protocol input, not model API messages. |
| Agent system prompt assembly/environment/mode text; title/compaction model requests | Removed. Render-guide behavioral policy remains (F18); UI still assumes model-switch compaction (F13). |
| Shell/git/file/search/media agent execution | Removed. Browser host automation, attachment/PDF processing and fixed YouTube oEmbed remain as explicit frontend services. Google search is browser navigation, not restored old web-search scraping. |
| Token estimation/provider retry/auth implementation | Provider runtime retry/auth gone. Pinned-file token estimate remains (F19). User Retry is an RPC/UI action, not a provider backoff loop. |
| Old imports/files/dependencies | Removed modules enumerated in `docs/frontend-only.md` are absent; no runtime npm dependencies. No hidden bundled legacy backend in ASAR. |
| Compatibility data/code | Old `image_url`, `steps`, catalog preference migration and unprojected transcripts remain (F18/F19); no active reads of old provider-native blocks for runtime. Existing old credentials on disk are not migrated/erased by this build; no production reader remains. |

### Ownership decisions (documented, not redesigned)

| Subject | Actual owner / recommendation |
|---|---|
| Rendering, composer, sidebar, animations, tool/approval presentation, browser panel, theme, attachment selection/previews | Correctly frontend-owned. Tool-card rendering is missing, not a backend responsibility. |
| Providers/models/auth execution, model calls, loops, prompts, tool policy/execution, compaction and usage generation | Backend-owned; keep it that way. Browser is the explicit host exception. Remove the policy part of render guide and invented capability defaults. |
| Chat IDs | Frontend creates time/random IDs on first Send; Rust must use them verbatim as session IDs. Mini is `<id>:mini`. Reasonable client-assigned identity if uniqueness/idempotency is specified. Not UUID-grade collision guarantees. |
| Titles | Frontend gives a first-input/attachment-name fallback (`Library.titleFrom`); backend `session.updated` may replace it unless manually renamed. This fallback is **not LLM title generation**. Manual rename is frontend-only; subsequent turns carry `title`, but no immediate `session.rename`. |
| Chat index/history/storage | Frontend owns organization/display cache; backend owns model history. Current lack of import/reconciliation is a mistake, not a reason to make the UI an agent again. Backend must not ingest display-only stats/moved/diagram overlays as authoritative model turns. |
| Delete / clear / rename | User interaction is frontend-owned. Backend erasure/rename semantics must be acknowledged through the interface. F03/F04 remain unresolved. |
| Compaction markers | Frontend renders/persists markers from events; backend owns summaries/context policy. `hasHistory`/model-switch copy must not dictate when/how Rust compacts. |
| Drafts | Draft Conversation exists until first Send; composer/attachments live in the UI. No backend session is created for an empty chat. No per-chat persisted composer-draft protocol; do not infer unsent text is in backend history. |
| Workspace/folders | Picker, organization and user workspace selection can stay frontend-owned. Frontend currently invents `~/OpenGhost/Chats/<first words>` for no-project chats and sends `cwd`; backend must lazily create/use it. Main only removes empty directories. Allocation uses index names, not filesystem reservation, so an existing/nonempty directory can be reused after deleting/recreating a chat. Treat path as a requested workspace, with creation/validation/ownership behind the backend/host interface rather than provider/runtime policy in UI. |
| Ask/Auto/Full | Selection UI may remain here. Allowed values, meanings, approval rules and defaults should come from backend capabilities; currently three modes and explanatory semantics are fixed. No frontend `needsApproval` policy remains. |
| Usage | Backend generates numbers; frontend aggregates/display-caches them. Current lack of deduplication/post-Stop accounting must not make the ledger authoritative billing state. |
| Locks | Local UI/cache encryption is frontend-owned; backend persistence protection needs an explicit agreement (F05). |

## 4. Browser host-tool contract: what Rust actually sends and receives

### Discovery and request envelope

At initialize the frontend sends **11** `host.tools` entries of `{name, description, parameters}` with object JSON Schemas. This is discovery, not a registration request that Rust must echo. `host.attachments.localPaths` reports desktop attachment-path support, **not browser availability**. Browser availability/control changes are not streamed.

Backend reverse request, with an ID distinct in the backend's request namespace:

```json
{"jsonrpc":"2.0","id":"rust-host-42","method":"host.tool","params":{"sessionId":"frontend-chat-id","turnId":"rust-turn-id","toolCallId":"call-42","name":"browser_navigate","args":{"url":"https://example.invalid"}}}
```

This is an illustrative address, not an endpoint used during audit. A known loaded session/current turn is required in practice. Approval is a **separate** `approval.request` before execution if Rust policy requires it; frontend `host.tool` does not enforce a preceding approval.

### Published operations

| Tool | Arguments and effective behavior |
|---|---|
| `browser_navigate` | Required `url:string`. HTTP(S), file/about/data URLs, absolute POSIX/Windows paths; bare host becomes HTTPS, localhost/IP becomes HTTP, otherwise Google search. Case-insensitive `back`, `forward`, `reload` control navigation. Returns page snapshot. No separate search/open-URL RPC. |
| `browser_snapshot` | Optional `full:boolean`; viewport snapshot (~9,000 text characters) or whole-page traversal (~40,000), refs and scroll/page information in text. |
| `browser_click` | `ref:integer` **or** `x,y:number`; optional `double`. Schema does not express the either/or constraint. Ref targets latest live DOM mapping; coords are page viewport pixels. Returns snapshot; covered-element warning does not prevent click. |
| `browser_type` | Required `text:string`; optional `ref`, `submit:boolean`, `clear:boolean` (default true). No ref means focused field; clears with Control+A then inserts text, optionally Enter. Returns snapshot. |
| `browser_select` | Required `ref:integer`, `option:string`; matches visible option text/value case-insensitively, with substring fallback. Fires input/change and returns snapshot. |
| `browser_press` | Required `key:string`, optional `times:integer`; key names/combinations (Control/Alt/Shift/Meta aliases); count coerced/clamped 1–20. Returns snapshot. |
| `browser_scroll` | Optional `direction:'down'\|'up'`, `amount:number` (screen shares, default .8, clamp .1–10), or `ref` to reveal. Returns snapshot. |
| `browser_screenshot` | Optional `full_page:boolean`; viewport or from page top up to four viewport heights. JPEG data URL, quality 82, width at most 1280. External scale/dimensions live in explanatory text, not machine-readable result fields. |
| `browser_read` | Optional `start:integer`; recomputed readable Markdown-like text, 40,000-character slices after HTML's 4 Mi-character cap. Text includes URL and continuation hint, not structured total/end/truncation metadata. Links become `[label](absolute-url)` where recognized. |
| `browser_wait` | Optional `text`, `seconds`; default 15 seconds with text, 2 without, clamp .5–60. Text substring matching; expiry is a snapshot with a textual “did not appear” note, not `isError:true`. |
| `browser_tabs` | Required schema `action:'list'\|'new'\|'switch'\|'close'`; optional `url`, `tab` (1-based position). Default/unknown action actually lists. New blank returns text; new URL navigates; switch returns snapshot; close returns list text. All other tools operate on active tab, not public tab parameter. |

### Results, state, cancellation, and error semantics

Success example:

```json
{"jsonrpc":"2.0","id":"rust-host-42","result":{"status":"ok","content":[{"type":"text","text":"Page: ...\nURL: ...\n\n[1] button ..."}],"data":{"refs":{"1":"button ..."}}}}
```

- Content is an array of text items, or text plus `{type:'image',dataUrl,label:'Screenshot of the built-in browser'}`. `data.refs` is optional, string-keyed in JSON, and contains **element descriptions**, not a URL map or durable handles. Do not parse it as DOM objects.
- Ordinary execution errors usually return `{status:'error',isError:true,content:[{type:'text',text:'Error: ...'}]}`. Unknown host tool/session returns a JSON-RPC `-32000` with `data.code:'unsupported'/'unknown_session'`. Some thrown renderer errors become RPC `unknown`. Rust must handle both error channels.
- When the user **already** has control at call receipt, the frontend waits. Hand-back substitutes `browser_snapshot {}` for the requested operation and returns `status:'handed-back'`. The requested click/type/etc. did **not** run. Sending a chat message while waiting yields `{status:'cancelled',reason:'message',content:[]}`; abort yields cancelled with empty content. Rust must re-plan after hand-back and must not assume that success-like content means the requested action occurred.
- Backend cancels a reverse request with `{"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":"rust-host-42"}}`. For a still-connected client the handler normally eventually responds. Browser cancellation is best-effort and subject to F02; there is no acknowledged side-effect rollback.
- Every turn/start/retry and steering input carries `host.browser`, either null or `{open,tabs:[{n,title,url,active}],signedIn:[{host,at}]}`. This snapshot is stale as soon as browser state changes; there is no `context.update` or browser event stream to Rust. UI take-control is not a backend request/notification.
- The main browser time budgets are load 30 s, loading settle 15 s, isolated-world calls/screenshot 12 s, DOM quiet 300 ms capped at 2 s, pointer delay 420 ms, wait up to 60 s. These do **not** add up to an end-to-end RPC deadline; CDP attach/some commands and first `dom-ready` can wait unboundedly. User hand-back waits intentionally have no deadline.
- Page guests and cookies live in `persist:browser`. Downloads go automatically to the user's Downloads directory; tools report downloaded paths in later snapshot text. There is no cookie/password-export RPC; snapshots mask password input values, but page text/screenshots can contain sensitive signed-in content.

### Removed-JS-backend dependencies

The browser executor itself was retained, not restored. The old agent loop previously provided effectively serial tool dispatch and interpreted browser results/control flow; Rust must now explicitly await and interpret reverse requests. Old tool schemas/result formatting moved to `host-tools.js`. Old approval effect/ref wording/policy disappeared; Rust must produce presentations and may use `data.refs` to describe a browser action. General web search/fetch/media tools did not move into the browser protocol—Rust must provide them or intentionally browse/search through these host tools.

**Compatibility stopgap, not acceptance criteria:** serialize browser calls globally across sessions, never reuse refs after navigation, use unique call IDs, cancel every outstanding reverse RPC on turn cancellation, and inspect `status/isError` rather than just JSON-RPC success. Frontend fixes are still required; these precautions cannot repair Take Control or stale-turn checks by themselves.

## 5. Tool and approval UI matrix

| Case | Current behavior / readiness |
|---|---|
| Tool start / progress / success / failure | Start = ghost only; other lifecycle messages discarded. No result cards, schemas, long-result rendering, or per-call crash state. F11. |
| Unknown tool name | Safe generic approval title/JSON fallback; unknown **host** tool rejected. No assumption that an arbitrary backend tool is locally executable. |
| Unknown/malformed args | No schema-aware form; JSON fallback accepts any JSON value despite documented `args:object`. Supplied invalid presentation may hide args. No length limits. |
| Allow / deny | Reverse RPC answer `{decision:'allow'\|'deny',reason?}`; explicit supersession/cancellation reasons. Card dismissed on settlement. Backend executes policy/action. |
| Multiple approval cards | Set of pending cards supports more than one; `approval.resolved` matches `approvalId`. No duplicate-ID suppression or cross-turn validation; changing modes is backend-driven, not frontend auto-allow. |
| Stop / late requests / backend crash | Current cards settle deny; close aborts reverse controllers. Old requests after a new turn can create cards (F01). Late tool results have no card to update. |
| Long arguments/results | Arguments rendered in full `<pre>`, diffs show only 14 lines, title per-word animation unbounded. Results ignored entirely. No durable tool/approval transcript. |
| Old tool assumptions | Old shell/git approval analyzers/tool-name allowlists are gone. Presentation taxonomy remains command/file/web and a fixed effect/reveal vocabulary; all unknown tools fall back visually to command/run, even if semantic type is different. |

## 6. Provider/model/auth compatibility verdict

A Rust catalog can describe OpenAI API, OpenAI Codex, Anthropic/Claude, DeepSeek and OpenRouter without adding provider execution code to this app. IDs need not match historical names. To work well **today**, Rust should:

- Return correctly typed provider arrays with `id`, `name`, optional `group/limits`, `methods`, and status; send model IDs separately from provider IDs.
- Use at most one API-key and one OAuth method per provider; execute OAuth/browser launch/token exchange/refresh/storage externally. Send `auth.changed` for waiting/error/connected updates and finish the original auth request. There is no device-code or callback URL UI contract.
- Explicitly set `vision`, `contextWindow`, nonempty `thinkingLevels`, and `defaultThinking`. For no thinking, current workaround is a single supported string such as `none`, not an empty list. These are compatibility constraints, not good final defaults.
- Publish only actually available models, because the frontend treats “listed model” as ready and does not reliably gate turns on provider connection state.
- Send ordinary short string names/status fields and valid limit/currency values. Avoid `|`, reserved object property names, and ambiguous colon-composed IDs until validation is fixed.

The UI is therefore **partially, not sufficiently, backend-driven** for a general replaceable backend. The usage color map does not block new providers, but capability defaults/auth schema/control gating can.

## 7. Protocol comparison and Rust implementation checklist

### 7.1 Actual methods Rust must handle

There is no HTTP/SSE/provider-specific alternate path. Emit one flushed UTF-8 JSON object plus newline per stdout message; log to stderr; read requests and reverse responses concurrently. Do not block the input reader waiting for a UI approval.

| Client request | Required current behavior |
|---|---|
| `initialize` | Accept version `0.1`, `client`, `host.tools`, `host.renderGuide`, attachment metadata. Return `{protocolVersion,backend:{name,version,platform?},capabilities}`. Repeated initialize on same process must be tolerated; current UI does not verify version. |
| `models.list` | Return `Model[]`; called even without a models capability, including on `models.changed`, refresh, auth changes. |
| `auth.providers` | Return provider/status/method array if `capabilities.auth.providers`. |
| `auth.setKey` | `{provider,key:string\|null}` → status. Null removes; never return saved secret. |
| `auth.login/cancel/logout` | `{provider}` → status; execute authentication outside UI, publish `auth.changed`. Login can be long-lived. |
| `account.limits` | `{provider}` → documented limits or null if `usage.limits`; queried for connected providers. Beware current null placeholder bug. |
| `turn.start` | `{sessionId,clientTurnId,input,...SessionParams}` → `{turnId}` promptly; create/recover session on first ID. Persist input/history before claiming acceptance. |
| `turn.retry` | Same session/config/clientTurnId, **no input or failed turn ID** → `{turnId}`. Backend must define retry from its history; UI cannot retry an undelivered message correctly. |
| `turn.steer` | `{sessionId,turnId,clientInputId,input,host}` → `{accepted:boolean}` plus `input.accepted` to actually update UI history. Handle repeated/cancelled/stale inputs. |
| `turn.cancel` | `{sessionId,turnId}` → null; stop model/tool execution, cancel pending reverse RPCs; final cancelled event may be ignored by already-idle UI. Idempotency matters. |
| `session.configure` | `{sessionId,model?,provider?,thinking?,permissionMode?}` → canonical config; potentially compaction events. UI currently ignores returned canonical values. Standalone model switching has no remote turn ID. |
| `session.compact` | `{sessionId}` → `{ok:boolean}` after completion, with `compaction.started/completed` notifications to show marker. For this standalone operation, omit `turnId` on compaction events: the local quiet turn has no remote ID to match. |
| `session.delete` | `{sessionId}` → null if deletion capability. Parent/mini recursion and errors must be specified; F03 is frontend work too. |
| `shutdown` | Main-originated request, ID literal `"shutdown"`, no params; terminate cleanly within 2 s and on stdin EOF. Reap backend-created children. Do not rely on renderer to consume response. |
| `$/cancelRequest` | Notification with outbound RPC ID. Handle especially start/retry before `turnId` is known and configure/compact cancellation; cancelled requests should still be answered when connected. |

Inputs are structured `text` + `attachments[]`; pinned instructions/files are `userContext`, not assembled model prompts. `cwd` is selected by the frontend and can point to a not-yet-created directory. Mini turns include `side:{parent,parentBusy,moved}`. `thinking` and `permissionMode` are sent on every turn even if the capability says unsupported. Backend must reject unsupported values clearly until UI gating changes.

### 7.2 Notifications and reverse requests Rust must support

**Visible turn lifecycle:** `turn.started`, `message.started`, `message.delta`, optional `message.completed` final text/finish reason, `turn.completed`. Status must be `done`, `cancelled`, or `error`; error carries `AbpError`. `length`, `content_filter`, and `insufficient_system_resource` are the recognized finish notes.

**UI support events:** `usage` (canonical input/cached/written/output, requests/context), `session.updated` title, `input.accepted`, `compaction.started/completed`, `approval.resolved`, `auth.changed`, `models.changed`, `log`.

**Reverse requests to UI:** `approval.request` and `host.tool`, with their results/errors described above and in the current interface. Unknown method gets `-32601`; unknown session/tool gets application error in `error.data`. IDs must be preserved by type and be unique while pending; string IDs for Rust's reverse calls avoid numerical precision issues (frontend numeric IDs are ordinary JS numbers). Batch JSON-RPC arrays are not accepted. The sender should not use unsafe 64-bit numeric IDs or raw newlines inside JSON strings.

**Tool/reasoning events:** optional `tool.started` only changes working status. `tool.progress`, `tool.completed`, reasoning events and any `tool.failed` have no implemented cards. Do not claim full tool UI compatibility just because the backend emits them.

### 7.3 Safe ordering recipe for the current implementation

1. Finish handshake and let frontend discover models before accepting user turns. Do not send unsolicited turn data at process startup; there is no ready-page queue.
2. Acknowledge start/retry promptly with `{turnId}`; emit matching `turn.started` with the **same `clientTurnId`** before messages/reverse tool requests. An early `turn.started` is tolerated, but the request response is still required.
3. Include `sessionId` and `turnId` on **all** turn-scoped events even where TypeScript snippets omit the latter. Send `message.started` before its deltas. Stream/complete one assistant message before starting another; do not finalize an earlier message while a later one is current.
4. Use globally unique message IDs for the connection; no replay/duplicate deltas or usage. `seq` is currently informational only.
5. Emit `input.accepted` before text belonging below that interjected user bubble. Serialize steering acceptance in user input order. A response alone does not move the bubble into history.
6. Emit usage/compaction completion and final text **before** `turn.completed`. Usage after completion/Stop is otherwise lost from the ledger. Do not repeat cumulative usage as incremental events: the frontend sums every event and forces at least one request for a nonzero usage event.
7. Resolve/cancel all pending approvals/host calls before terminal turn state; never emit further host work after cancellation. Frontend guards are insufficient, so this is essential but not a substitute for fixing them.
8. Persist sessions across process restarts; avoid assuming a renderer reload has recovered its display or outstanding calls. Handle repeated initialize as a new UI connection and reconcile/reset in-flight reverse requests by an agreed policy.

### 7.4 Mismatches / underspecified constraints

| Documented claim/type | Implementation / gap |
|---|---|
| Lines over 64 MiB are dropped | Complete lines parsed before limit; UTF-16 count; unsafe resynchronization (F06). |
| Negotiated `protocolVersion:'0.1'` result | Not checked; even empty/undefined result accepted (F15). |
| Every event carries `sessionId`; turn events are filtered; early events held | Auth/catalog/log events are global exceptions. Reverse requests ignore turn ID; ID-less deltas can be lost; session-only events can attach to wrong turn (F01/F09). |
| Stop ignores anything further for the turn | New-turn reverse requests still execute; optional-ID events/usage/title behave differently; browser side effects may continue (F01/F02/F09). |
| Stop button or Escape | No Stop button implementation; Send stays Send (F20). |
| `thinkingLevels?`, `vision?`, `contextWindow?` | Omission/empty list invents four levels/vision/million-token context rather than unsupported/unknown (F13). |
| `session.configure` returns selected canonical config; `session.compact` returns ok | Returned values are ignored; event/response ordering required for UI outcome. Standalone compaction/configure events must omit `turnId`, otherwise they buffer against a remote ID that never arrives. |
| `{accepted:boolean}` from steering | Boolean ignored; `input.accepted` indispensable and rejection invisible (F12). |
| `Provider.id:string`, methods array; backend-driven auth | Hidden key restrictions/collisions, only two method kinds, no method ID/device-code flow/catalog event (F14). |
| Browser schema “required”, enums and argument types | Not validated as JSON Schema. Defaults/coercions and unknown tabs action accepted; no browser availability field (F15/F16). |
| Browser read/snapshot/screenshot result content | Public doc omits timeout/coercion/truncation/page/scale/control/concurrency limitations; results often textual, not structured. §4 records actual contract. |
| `tool.progress/completed`, reasoning, seq | Explicitly documented as ignored: not an accidental mismatch, but missing requested functionality. Failure payload has no defined tool-card schema. |
| Backend-generated title/session events | Only loaded conversation owners receive them; manual renames/diagram edits not transmitted immediately. |
| Renderer has no network | connect-src is none, but images/browser network exceptions remain (F17). |
| All backend prompt/token/history logic removed | Runtime removed; prompt policy, display token estimator and model-shaped legacy storage still present (F18/F19). |

No undocumented top-level production `host.*` method beyond `host.tool` was found. `browser:run/cancel/shown/event` are **internal Electron IPC**, not messages Rust should send. `shutdown` is described in prose but not included in the document's request type inventory; `unknown_session` and several browser semantic defaults/error combinations are implementation-defined. `client.platform:'web'` is possible in `hello()` although the document enumerates only desktop platforms (there is no stock web transport).

**Explicitly not implemented/used from the earlier proposal:** `session.create/get/list/rename/editMessage`, `turn.followUp`, `context.update`, replay/gap repair, generic tool/reasoning rendering, broad capability gating. Current docs admit most of these. A full Rust backend should not implement the historical proposal blindly and expect the frontend to consume it; several require coordinated frontend protocol changes.

## 8. UI behavior verification matrix

| Surface/scenario | Evidence / result |
|---|---|
| Startup, empty/new chat, sidebar/composer/settings without backend | Source E2E passes; user input shows unavailable error. Empty draft has no backend session. |
| Configured missing executable | Additional isolated Electron probe: state unavailable with ENOENT detail in process status; Send shows generic unavailable error. No automatic recovery UI. |
| Send/normal streaming | Smoke passes. Only simple sequential messages covered; ordering/race caveats F08/F09. |
| Stop/Escape | Hanging turn smoke passes. No Stop button; deferred attachment Stop remains busy; browser work not reliably cancelled. |
| Retry | Error actions render in smoke, but click not tested there. Additional probe confirms undelivered-input problem. Real history retry not established by fixture's canned “retry” response. |
| Steering/interjection | Source path present; acceptance boolean/errors ignored and local/backend history can diverge. Not covered by smoke. |
| Model picker / effort | Catalog arrival and three effort strings verified; actual picker switching/confirm/capability matrix not covered. Empty effort-list/vision defaults reproduced. |
| Attachments | Structured input and preview paths inspected; no ordinary attachment E2E coverage. Deferred-read Stop reproduced. Input image downscale returns original width/height despite smaller encoded pixels (`attachment-reader.js:36–54`): metadata semantics need specifying/testing. |
| Approval cards | One allow action smoke passes. Stale approval reproduced. Deny/parallel/cancel/mode/schema/size cases not covered. |
| Tool cards | Absent, not verified by the approval test. |
| Browser | Smoke only lists tabs. Additional local navigation/read/ref snapshot works; Take Control race, stale tool, delayed cancellation and URL HTML injection reproduced. Full navigation/search/link/click/form/tab lifecycle matrix remains untested. |
| Settings/auth/usage | One API-key provider and token/title events pass. No OAuth interactions, key mutations, fifth provider, account-limits UI assertion or outage/race tests. |
| Chat switching/history/deletion | Display store is read in smoke, not actual reopen/switch. Folder delete bug reproduced; backend session reconciliation absent. |
| Backend crash during acknowledged turn | Smoke passes: error shown and backend unavailable. Mid-preparation/reload/browser/auth/configure crashes are not covered. |
| Mini chat / locks / drafts / workspace | Source-reviewed only, except main/mini ID collision reasoning; no production E2E coverage. Encryption promise and deletion/workspace choices need explicit ownership agreement. |

## 9. Tests/build: actual coverage and missing cases

### Commands and outcomes

Environment: Linux x64, Node `v26.10.0`, npm `12.2.0`, Electron `44.4.5`, electron-builder `26.15.3`.

| Command | Result |
|---|---|
| `npm test` | **PASS — 29 tests**, no failures/skips. |
| `npm run test:e2e` | **PASS — 14 checks**, no reported page errors in the tested scenarios. |
| `npm run dist:linux` | **PASS** — `dist/OpenGhost-1.3.0-linux.tar.gz`; warning: package author missing. |
| `OPENGHOST_E2E_APP="$PWD/dist/linux-unpacked/openghost" npm run test:e2e` | **PASS — 14 checks** against packaged app (additional audit check). |
| ASAR inventory | 84 entries; expected boundary files present; no old backend/provider SDK/test/docs modules. |

Build/test artifacts and disposable probe scripts/logs are not committed. They were run from the requested worktree or `/tmp`; synthetic process descendants created for reaping verification were explicitly cleaned up. The first deferred-attachment probe used incomplete mock file metadata and was corrected before its reported reproduction; that harness error is not a product finding.

### What the 29 + 14 actually establish

The 29 units comprise 11 client-peer tests, 7 host tests, 9 largely static boundary tests, and 2 usage tests. They establish removal/load order, happy-path JSONL splitting and RPC/errors/cancellation, process exit/missing executable status, generic usage, and static endpoint/dependency bans. They **do not instantiate chat state, browser execution, approval DOM, provider UI, or session ownership**.

The E2E's 14 items are checks in two shared app launches, not isolated adversarial scenarios. It uses one provider/model, one approval allow, `browser_tabs list`, a canned hanging turn, an auth error, manual compaction, a display-store read, and crash. “The chat is saved ... and opens again” only calls `library.conversation`; it neither reloads nor proves backend persistence. “Browser host tool runs” never creates/drives a webview. “Retry” is only text on an error button. The harness stops Electron with SIGTERM; it does not test File/Quit, shutdown acknowledgement, or descendant cleanup. Static network assertions miss image/browser channels. Fake reconnect does not establish a production restart path.

### Missing tests in priority order

1. **HIGH — turn/connection identity:** stale approval/host tool after Stop→new turn; before start ACK; duplicate RPC/tool/approval IDs; parent/mini ID isolation; backend emits after completion/crash/reinitialize.
2. **HIGH — cancellation/control:** before spawn/initialize/start ACK; pending attachment/context read; during approval, browser first DOM load, queue, cursor delay, typing/submit, navigation, sleep, screenshot and read; several reverse requests; Take Control while work is in flight; interjection during hand-back; verify *no later side effects*, not just cancelled text.
3. **HIGH — process/transport:** missing executable then immediate quit, graceful shutdown/EOF, ignored shutdown and forced kill, kill failure, grandchildren, repeated quit, main crash, stdout after exit/EOF without newline, stdin EPIPE/nonreader, stderr floods, split UTF-8/CRLF, both complete/fragmented oversized lines and recovery, bounded memory/depth/event queues.
4. **HIGH — lifecycle/recovery/persistence:** initialize hang/version mismatch/exit ordering, missing start response/turn ID, backend restart/replacement, renderer reload mid-turn and reused RPC IDs, actual reopen with persisted Rust-like sessions, old chat import, folder/parent/mini/offline deletion, clear→new-turn race, protected backend history.
5. **HIGH — rendering security:** encoded browser URL markup, names/errors/approval text containing HTML, unknown schema/types, huge titles/arguments/results, malformed catalogs/limits, reserved provider IDs; intercept local image/navigation requests to verify intended CSP/network policy without AI calls.
6. **HIGH — event integrity/tool UI:** duplicate/out-of-order/missing seq, two interleaved messages, early ID-less deltas, final text replacement, usage before/after terminal state/cancel, arbitrary tool start/progress/success/failure/parallel calls and crash cards once implemented.
7. **MEDIUM — Retry/steering:** Retry after unavailable/preparation/validation/partial delivery/crash; retryability policy/idempotency; steer accepted=false/error/out-of-order attachment completion; multiple queued inputs and manual-compaction/model-switch queues.
8. **MEDIUM — providers/models/auth:** zero/one/five-plus providers, add/remove while open, OpenRouter-like arbitrary IDs, no thinking and custom levels, vision false/unknown, output/context limits, unavailable selected model, configure response normalization/failure; API key set/remove/rapid changes; OAuth cancel/logout races/device flow, auth events racing refresh; account null/error/invalid/freshness limits.
9. **MEDIUM — browser conformance:** all 11 tools on controlled pages, search URL normalization without external network, back/forward/reload, redirects/popups/close/crash, stable/stale refs and covered elements, page changes between read slices, hidden/shadow/iframe text, screenshot scale, downloads, sign-in heuristic, malformed args, 12-tab eviction, user navigation during agent work.
10. **MEDIUM/LOW — UI regressions:** actual Send/Stop affordances, picker/effort/mode interactions, attachments image/text/PDF/Office/video and reopen, mini-chat lifecycle, chat switch/delete/rename/lock, workspace collisions, usage failure state and animations with reduced motion.

## 10. Recommended fixes, in priority order (not implemented)

1. **Safety before integration:** F01/F02/F10 — validate reverse-request turn/epoch; make browser cancellation/control an execution barrier with globally unique per-call state; stop interpreting URLs as HTML.
2. **Data/lifecycle correctness:** F03–F05 — fix deletion ordering and parent/mini semantics; define reload/restart/session import/reconciliation; clearly scope chat-lock security. Preserve frontend-only display annotations without pretending they are model history.
3. **Bounded process boundary:** F06–F08/F15 — hard byte limits and backpressure, validated protocol/version, reliable delivery/timeout errors, cancellation across preparation, epoch-safe initialization, idempotent shutdown and full child-tree cleanup. Add diagnosable unavailable/crash status; automatic restart is a separate policy decision, not a required hidden retry loop.
4. **Deterministic event model:** F09/F12 — message/call identity, ordering/deduplication, final usage, steer acceptance and retry/idempotency semantics. Write a small state-machine conformance suite before adding backend-specific behavior.
5. **Complete the promised UI boundary:** F11/F13/F14 — generic tool cards; capability-driven modes/thinking/attachments; validated dynamic provider catalog/auth challenges and canonical session configuration. Resolve which new protocol features are actually in the next version.
6. **Browser contract hardening:** F16 and §4 — readiness/page/control IDs, explicit end-to-end timeouts, structured errors/results/truncation and deterministic tab ownership. Test controlled local pages, not just tab listing.
7. **Remove remaining coupling/clarify claims:** F17–F21 — syntax-only render guide, no heuristic token counts presented as backend facts, explicit legacy-display migration, safe stable provider presentation, migrated storage keys, and accurate networking/Stop/recovery documentation.

**Acceptance gate for `openghost-backend`:** the existing green tests are a baseline, not a conformance certificate. A Rust backend can connect now for a serialized synthetic demo, but production integration should wait for the HIGH findings and an agreed session/cancellation/event contract. None of these recommendations requires restoring the removed JS backend.

No reviews index existed at audit time; this review is the only added repository file.
