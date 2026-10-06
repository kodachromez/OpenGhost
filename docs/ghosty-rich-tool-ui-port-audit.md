# Ghosty native rich tool UI → OpenGhost frontend-plugin port audit

## Scope and evidence

**Source audit only. No production changes, commits, backend requests, application launches, or runtime/visual qualification were performed.** Proposed types, files and hooks below do not exist unless explicitly marked existing.

- Ghosty: `/home/brian/zoe-rust2`, branch `main`, HEAD `3b0e7a512df11dc535be90229df2a11aa80702c3`. The authority for this feature is **`native-ghosty/`**, not its Electron implementation or Pi's terminal renderer.
- OpenGhost: `/home/brian/openghost-native-qt`, branch `cpp-native-extraction`. Initial HEAD was `957e15c0c9dfd2138b6dee647ac530ce31604e60`; the audit included its uncommitted Pi process/session work. During final verification that work was **committed externally** as `e8dc3cc1f9adaa428f10f487119c665c21244fee`. This audit did not create that commit. Re-read the final adapter event/recovery paths and bridge/header; the map below reflects that final HEAD.
- Pre-existing Ghosty edits: `native-ghosty/qml/Main.qml`, `native-ghosty/tests/test_window.cpp`. Initial OpenGhost edits: `CMakeLists.txt`, `src/backend/pi/openghost-bridge.js`, `src/backend/pi_backend.{cpp,h}`, `src/main.cpp`, `tests/pi.cpp`, `tests/pi/pi`; initially untracked `src/backend/pi_process.{cpp,h}`. None were changed by this audit. At final verification, the only OpenGhost working-tree addition was this audit document.
- Installed Pi: `@earendil-works/pi-coding-agent` **1.0.4**, at `/home/brian/.bun/install/global/node_modules/@earendil-works/pi-coding-agent/`. `Pi/` below means that directory; `Agent/` means sibling `@earendil-works/pi-agent-core/`.
- Read `docs/cpp-port.md` for extraction/provenance constraints. Its “no adapter” statements, and parts of `docs/pi-backend-gap-audit.md`, predate the current working tree. The implementation, not those older status descriptions, determines current availability.
- This is a proposed, opt-in exception to the extraction's exclusion of tool transcript panels. It does **not** authorize importing Ghosty's source-port controls, subagent service, backend, custom splash, or changing `reference/openghost/`. Retain OpenGhost product names and existing attribution/licensing; native reuse does not create additional artwork/design rights.

Audited content fingerprints (SHA-256):

| File | SHA-256 |
| --- | --- |
| Ghosty `native-ghosty/qml/ToolCard.qml` | `63afe96ff49e927aefb6bfbdcfc51fbaa5025bcddb35c522fe429c828f89247a` |
| Ghosty `native-ghosty/src/toolcard.cpp` | `62ff5e81dcf8c2e13466445c5051a3e31497fa7be8a14893cc0af53ab8ec6ce6` |
| Ghosty `native-ghosty/src/transcript.cpp` | `590001c570c878c8dc4e7f9eec0aa8bbc30f50c81e7d164e168cb260fa56af86` |
| OpenGhost `src/backend/pi_backend.cpp` (final HEAD) | `bba857e88c2923cf9bc97c44637deb750dbfb464dc2a60b65fd29a92662a7ea0` |
| OpenGhost `src/frontend/chat_service.cpp` | `5d16eede2530b17f9f67322b05391485bef3aac735b896426c87e1b5e88ea7dc` |

## 1. Verdict in brief

**The ordinary card's visual implementation is highly reusable. The complete feature is not a drop-in copy.** OpenGhost already contains most of its Qt rendering infrastructure. Missing are a sufficiently rich tool contract, ordered tool/thinking transcript projection, restoration, and an actual local frontend-renderer plugin seam.

Three distinctions are essential:

1. **Current OFF is not plain tool rendering.** There are no tool rows at all. A basic/plain fallback must be implemented alongside the plugin; disabling rich cards must not discard calls or their data.
2. **Existing “Plugins” is backend plugin administration.** `openghost::Plugins`, `PluginModel` and Settings' capability-gated page list/enable/disable backend plugins. They do not load or select QML renderers. Pi's `registerToolRenderer` is also not a native frontend-plugin API: it renders TUI/HTML, not Qt over RPC.
3. **Ghosty does not animate partial tool-argument JSON into cards.** Ordinary native cards begin on a complete `toolCall` observation. Ghosty's core emits those after the model response is completed/validated/checkpointed. Cards can coexist with a still-revealing assistant row and the continuing multi-exchange run. Do not invent early “executing” cards from Pi's `toolcall_start`.

Recommended first scope: ordinary top-level tools, including unknown names via Ghosty's generic card; retain its layout, text, selection, motion and scrolling. Keep thinking as a separate optional renderer sharing the same ordered transcript infrastructure. Do not port subagent panels in this first plugin.

## 2. Exact Ghosty source map

Paths in this table are relative to `~/zoe-rust2/`.

| Files / symbols | Responsibility | Port disposition |
| --- | --- | --- |
| `native-ghosty/qml/ToolCard.qml` | Complete card: header/status, argument wells/diff, plain output, ending, disclosure, scrolling, selection; also contains a special subagent activity panel. | Copy ordinary-card implementation substantially intact into the plugin. Rename imports/facade bindings; separate the subagent-only branch. |
| `native-ghosty/src/toolcard.{h,cpp}`: `toolcard::Info`, `Lines`, `Row`, `describe`, `endLines`, `liveNote`, `omission`, `plain`, `units`; QML singleton `ToolText` | One formatting authority for both displayed text and offscreen selection/copy. `describe` starts at cpp:251; endings/notes at :436; selection at `units`; `steps` at :540. | Adapt largely directly. Add Pi argument-schema handling; remove the hard include/dependency on `activity.h` for ordinary tools. No backend execution belongs here. |
| `native-ghosty/src/transcript.{h,cpp}`: `Entry`, `Transcript::observe/project/rows/interrupt`; helpers `toolRow`, `result`, `bashShape`, `callArguments`, `latestThought`, `exchangeOf` | Worker-side live reduction, identity, bounded tails, canonical result pairing, live-to-saved reconciliation, ordering, row revisions and grouping. | Extract algorithms and tests, **not the whole Ghosty transcript/client**. Input shapes and run/exchange identity are incompatible with OpenGhost/Pi. |
| `native-ghosty/src/model.{h,cpp}`: `TranscriptModel`, `changedRoles`, `KeyedModel::diff` | Tool/thinking roles; keyed delegate-preserving updates; expansion keyed by row, not index; reset on conversation replacement. | Merge missing display roles/expansion behavior into OpenGhost's existing model. Reuse its existing keyed diff. |
| `native-ghosty/qml/ChatEntry.qml` | Tool/thinking delegate choice; common row width/spacing; 500 ms tool entrance; thinking disclosure; drop/selection/list host facade. | Extract tool-specific wrapper into the plugin. Add only a generic renderer slot to OpenGhost's file, not Ghosty's entire ChatEntry. Thinking component is separately portable. |
| `native-ghosty/src/selection.{h,cpp}`: `ReplySelection::extraOf/build`, `toolcard::units` integration | Includes tool contents in one conversation-wide selection even when delegates are virtualized; trims selection positions with live tails. | Adapt missing branches into a renderer-provided selection hook. Do not hardcode a new `toolcard.h` dependency into core selection. |
| `native-ghosty/qml/LiveText.qml`, `SelectArea.qml`, `SelectDriver.qml`, `SelectionWash` in `src/selection.*` | Incremental text-document append/front removal, drag selection, copy/menu and autoscroll. | Already retained in OpenGhost (facade is `frontend`, not `ghosty`). Reuse; no replacement needed. |
| `native-ghosty/src/exposure.{h,cpp}`: `WellScroll`, `WheelScroll`, `WindowExposure` | Nested scrolling ownership/gesture latching, transcript wheel behavior and exposure. | Already retained; `exposure.cpp` matches the inspected Ghosty file. Reuse. |
| `native-ghosty/src/rich.{h,cpp}`: `Theme`, `RichDocument`, selection display types; `theme.h`, `cssfont.*`, `icon.*`, `shadow.*` | Palette, font metrics, easing, glyphs/shadow; existing assistant text reveal can continue beside cards. | Reuse current OpenGhost copies. Card palette tokens agree; no global theme/splash replacement. |
| `native-ghosty/qml/Main.qml`: transcript `dropped/mark/followBottom`, `quiet`, `onAnswered/onWorked`, footer | Transcript anchoring, 1.5 s quiet-text ghost return, approval suppression, footer/working ghost placement. | Existing OpenGhost equivalents already present. Reconnect activity to normalized state and make renderer switching anchor-safe. Do not wholesale copy Main.qml. |
| `native-ghosty/qml/WorkingGhost.qml`, `WelcomeGhost.qml`; `src/ghost.*`, `motion.*` | One working ghost per run; procedural loop, entrance/exit reversal, glide and welcome handoff. | Already present. WorkingGhost differs only in module/product naming. No new loading mascot/assets. |
| `native-ghosty/src/window.{h,cpp}`: `WindowController::receive` (cpp:636) | Applies/reset rows; newest changed busy row produces `answered` for live assistant text, otherwise `worked`. | Adapt the activity rule, not the worker/service composition. |
| `native-ghosty/src/client.{h,cpp}`: `frame/settle/adopt/show/publish`, `acknowledgeView` | Session/run filtering, canonical snapshot settlement, live join, 30 ms publication and one outstanding GUI view. | Backend-specific; do not copy. Preserve its important publication/reconciliation semantics through OpenGhost's service. |
| `native-ghosty/src/rpc.{h,cpp}` | Ghosty observation/settled envelopes and connection failure. | Not portable; do not import. |
| `crates/protocol/src/event.rs::agent_data`; `crates/core/src/agent.rs::model_exchange/tool_batch` | Defines actual `toolCall`, append `toolProgress`, string `toolResult`, numeric exchange `turn`; emits calls after full response admission. | Contract evidence only. No Rust source belongs in the native target. |
| `crates/coding/src/claude_code/events.rs::assistant_frame/user_frame` | Ghosty's alternate backend also produces complete tool calls from assistant `tool_use` blocks and matched results. | Evidence only; no source-port/backend controls to port. |
| `native-ghosty/src/activity.{h,cpp}`: `ActivityItem`, `ChildActivity`; `Transcript::childJob/callKey/childJobAt`; `Client::loadActivity` | Special subagent jobs, child live observations, lazy saved child history, activity steps. | Audit includes them because ToolCard currently depends on them. **Exclude from first plugin**, not an unnoticed transitive import. |
| `native-ghosty/CMakeLists.txt`, `native-ghosty/NOTICE.md` | QML/C++ registration (`Ghosty.Native`, `NativeGhosty`), build and provenance. | Adapt registration in OpenGhost; preserve attribution, not product/module names or source build graph. |

No Electron CSS/JS runtime is needed. `openghost/tool-card.js`, `tool-card.css`, `projection.js` are provenance mentioned in native comments; the audited QML/C++ implementation, including its differences from the web UI, is the target.

## 3. Exact native behavior to preserve

### 3.1 Card appearance and argument rendering

- The reading column is `min(680, listWidth - 48)` and centered. Separate messages have 28 px gaps; same-exchange rows 14 px, or 12 px immediately after thinking; first row has none.
- Card shell: radius **18**, composer background/border, 1 px border; black shadow at .22 × theme shadow, blur 30, y offset 10. Header at (14,14), height 32, width minus 28. Closed height **60 px** (`46 + 14`); open height `46 + body.height + 12`.
- A 32×32, radius-10 icon tile; 17 px file/globe/terminal glyph. Header elements are vertically centered on that tile.
- Closed: capitalized tool name (fallback `Tool`), humanized single-line arguments, status, chevron. Name at most 35% of header; summary elides. **The collapsed summary is arguments, not the latest output**, despite the transcript also carrying a `preview` field.
- Open: action title and path replace name/summary. Header is one tab-focusable `AbstractButton`, accessible action title, visible keyboard-focus outline. Whole header toggles. No per-card Execute/Stop/Retry button.
- Body order: command/raw JSON → removed/added lines → descriptive text/query → URL label → omission note → output → incomplete-output note → terminal ending. Empty sections disappear. The URL label is plain text, not a newly enabled navigation action.
- Wells: radius 10, theme well background plus .05 foreground inset ring. Command/output cap at **184 px**; diff at **232 px**. Mono text **12.5/19 px**, foreground .88; no Markdown, HTML evaluation, syntax highlighting, or image renderer in ordinary tool output. Diff line fill uses success/danger .09, with the existing `diffAdded/diffRemoved` text colors.
- Diff previews show **14 lines per side**, then `N more lines`; CRLF normalized and one final newline removed. Added lines from a new write have no `+`/green diff treatment unless there is removed text. These are previews of requested edits, not a filesystem diff computed by the frontend.
- Thin scrollbar is implemented inside ToolCard, not `OverlayScrollBar.qml`: 10 px opaque track, 6 px thumb (minimum 18 px), 12 px arrow areas, 40 px arrow step, .875 viewport track step and draggable thumb.

`toolcard::describe` parses arguments only as JSON data. Its comments say “only strings”; the implementation also deliberately reproduces JS display conversions for booleans/numbers/arrays/objects. It never evaluates content. Invalid/clipped JSON is shown raw in the code well. Unknown arguments produce no guessed summary. Summary is simplified whitespace, capped at 160 UTF-16 units (159 + ellipsis).

Important exact schema mappings:

| Tool names | Ghosty fields / presentation | Pi compatibility |
| --- | --- | --- |
| `bash` | `script ?? command` → “Run a command”, code well | Pi `command` works already. |
| `bash_output` | `id`, `stream`, `offset` → “Read command output” | Not a stock Pi tool; keep only as a display formatter if such a name is observed. |
| `read` | `path` or `paths`, `symbol`, `offset/limit` → file/path and line range | Pi `path/offset/limit` works. |
| `write` | `path`, `content` → file and new-line preview | Pi schema works. |
| `edit` | `path`, nested `files/edits`, `search/replace`, or `lines`, line/position fields | **Does not work unchanged for Pi.** Pi 1.0.4 uses `edits:[{oldText,newText}]` (also normalizes legacy top-level oldText/newText). Adapt these to the identical removed/added presentation; don't rewrite the stored arguments or execution request. |
| `code_search`, `ast_search`, `lsp`, `get_repo_map`, `memory`, `skill`, `obs_recall`, `attach_file`, `web_search`, `web_fetch` | Named humanizations in `describe`; file/web/command icons | Ghosty-specific schemas. Unknown/custom Pi tools must retain generic fallback; e.g. lowercase Pi `grep/find/ls` are not these branches. |
| `Bash`, `BashOutput`, `TaskOutput`, `KillShell`, `TaskStop`, `Read`, `NotebookRead`, `Write`, `Edit`, `MultiEdit`, `NotebookEdit`, `Grep`, `Glob`, `LS`, `WebFetch`, `WebSearch`, `Agent`, `Task`, `TodoWrite`, `Skill`, `mcp__…` | Case-sensitive Claude-name formatting in `claudeCode`; MCP code retains compact JSON | Pure display support, not evidence Pi uses those names or that its MCP names follow that convention. Do not rename Pi tools to manufacture a match. |
| Other names | Name as title, compact JSON code, terminal icon | Direct generic fallback; essential for Pi extensions. |

### 3.2 States: what exists, and what does not

There is **no separate ordinary-card “starting” animation/state or scheduled queue state**. `toolCall` inserts `running`, even before actual tool execution/approval has finished admission. The state is a display of an unresolved call, not proof effects began.

| Stored display state | Header / ending | Transition source |
| --- | --- | --- |
| `running` | `Running…`; no ending well; with output: `Live output, incomplete until the result arrives` | Complete observed call. |
| `running` + correlated approval | `Waiting for approval`; still no terminal ending | `ToolCard.waiting`: own-run approval (`!a.job`) whose `callId` matches the row-key suffix. This is a separate approval list, not a tool event state. |
| `done` | Green `✓ Done`; ending `Done` unless explicit ending lines supplied | Valid matching `toolResult`, `isError:false`. Subagent header instead says `✓ Completed`. |
| `error` | Red `✕ Failed`; ending `Failed` or supplied ending lines | Valid matching `toolResult`, `isError:true`. |
| `cancelled` | Neutral `Cancelled`, plain ending | QML supports it; **ordinary `Transcript::observe` never sets it from a boolean result**. Explicit child job `cancelled` sets it. Run cancellation does not recolor every ordinary call as cancelled. |
| `missing` | `No result was saved`; plain ending; `Partial live output` if body exists | Canonical call has no valid saved result. Canonical rebuild normally has no transient tail to retain. |
| `unconfirmed` | `Result unconfirmed`; plain ending; `Partial live output` if body exists | Interrupted/disconnected live run with no canonical settlement; unresolved running calls only. Finished calls remain finished. |
| Unknown state | Literal state | Fallback; not invented success. |

Ordinary cancellation can therefore appear as a failed result with cancellation text, a missing saved result, or an unconfirmed result depending on what actually arrived. “Cancelled” support in QML must not be mistaken for a universally available per-tool cancellation contract.

### 3.3 Live output, bounds and final results

`Transcript::observe` (cpp:1035 onward) is the core behavior:

- Identity is **run + numeric exchange + call ID**, with matching full tool name for progress/result. Duplicate calls never replace the first card. Later exchanges may reuse a call ID safely.
- `toolProgress.output` is an **append chunk**. It applies only to an existing, running, same-name call in that exchange. Orphan, renamed and late-after-terminal output is discarded, not buffered into another card.
- Each ordinary tool retains the newest **32×1024 UTF-16 units** of live output. Trim to a line boundary when possible; don't split a surrogate pair. Count removed complete newlines separately from code points removed from the remaining partial first line. `trimmed` is the cumulative UTF-16 removal count for document/selection repair, not the user-visible character count.
- A valid result **replaces**, never appends to, the live body. Omission line/character counters clear. Generic final result retains the **first** 32 Ki UTF-16 units plus the display-limit notice; it is not the live-tail policy.
- A valid result without an observed call creates a terminal card with `argumentsKnown=false`. Canonical settlement may supply arguments later without changing its key/expansion.
- Arguments normally cap at **4 Ki UTF-16 units + ellipsis**. Tool IDs/names over 256 units are refused for live identity rather than truncated into collisions. Per-run caps: 256 tools, 1,024 exchanges, 1 Mi streamed text/thinking; per-message text 256 Ki, thinking 64 Ki. History retains 2,000 rows; publication uses newest suffix up to 400 rows / 2 Mi display characters, plus omission notices. These are display limits, not changes to model history.
- `LiveText.sync` edits a `QTextDocument`: append suffix; remove a known trimmed prefix; otherwise replace on authoritative correction/result. It defers sync until roles agree and repairs local selection. No decorative typewriter/wave is attached to tool output.
- Source publication is coalesced at **30 ms**, with at most one unacknowledged complete view in flight. Copying only the QML without a bounded/coalesced projection loses this performance behavior.

**Bash ending is backend-specific.** `transcript.cpp::bashShape/result` recognizes only lowercase `bash` content starting with `{` and an exact recognized report shape. Required/nullable fields include stdout/stderr, termination, exit/signal, launcher exit/signal, output completeness, cleanup, reaping, failures and status receipt; permitted optional preview/log fields are also checked. Wrong types, missing required fields or **unknown keys** fall back to raw saved text, not guessed Bash success.

For a recognized report, stdout then stderr become the output; ending rows contain termination/exit/signal, failure, unreceived status, incomplete output, cleanup problems, unreaped child and log problems. Launcher diagnostics appear only when a problem exists. Ending diagnostics survive long output truncation. Never synthesize this report shape from Pi's much narrower data.

### 3.4 Expansion, scroll and selection

- **All cards arrive collapsed**, running or restored. Only reader toggles add keys to `TranscriptModel::m_expanded`. Streaming and canonical keyed moves preserve expansion. A temporarily absent row keeps expansion while a run is active; absent keys are pruned when it ends. `reset` clears everything on conversation replacement. Expansion is not saved to disk.
- Closed body uses `Loader.active=false`: no command/output text layout. Reopening rebuilds body, not the outer card.
- Outer card keeps `followOutput`/`outputScroll` across collapse. When output becomes empty, both reset. This is delegate memory: it is **not guaranteed across outer delegate destruction/virtualization**, a chat switch or app restart.
- A fresh running/non-done/non-error output opens at its end. Finished `bash`, `bash_output`, `Bash`, `BashOutput` also open at their end. Other completed/failed outputs open from the remembered position, initially zero. An already-open non-Bash card is not explicitly forced to top at completion.
- Following resumes within **8 px** of the end. When the reader scrolls back, new output must not pull them down. A front trim subtracts removed pixel height from their remembered position. Before the well itself scrolls, `ChatEntry.dropped` delegates anchoring to the transcript; omission-note insertion/removal uses the same mechanism.
- `WellScroll` latches a wheel sequence (under 500 ms between turns) or touchpad gesture to the view that could initially move in that direction. Reaching an end mid-gesture does not leak the remainder into the transcript. A gesture starting at a blocked well can chain outward. Selection autoscroll also finds the innermost scrolling well.
- `toolcard::units` and QML share the exact strings and stable `t/…` paths. A closed card contributes **no selectable text**. Header, diff signs and disclosure are excluded; command, shown diff lines and “more” notes, query/link, omission/output/live note, and ending lines participate in conversation-wide selection. It copies the displayed bounded preview, not an imagined full raw tool result.
- Offscreen selection must work without constructing QML delegates. A plugin cannot just draw TextAreas and bypass `ReplySelection::build/extraOf`.

### 3.5 Multiple tools, assistant streaming and canonical ordering

Ghosty's **live** exchange stores one thinking string, one assistant-text string, and a vector of tools in first-observed order. `Transcript::rows` emits exchanges in numeric order, then within each:

```
thinking → assistant text → tool A → tool B → exchange notices
```

Results update the existing card; completion order never reorders siblings. A missed-call result gets a card at its first observation position. A retry clears only that exchange's provisional thinking/text, not already observed tools.

The **saved** projection preserves actual assistant content-array order, joining only adjacent text or adjacent thinking. A saved message can consequently be `text → call → thinking → text → call`. The first text/thinking keys retain `.a/.t`; later fragments get `.a2/.t2`. Settlement can move rows into canonical order via keyed model moves, retaining expansion. Saved results answer the latest preceding open call with that ID and matching full name inside the same run; they do not add separate result rows or cross a user-run boundary.

The core's `model_exchange` emits complete calls only after response validation/checkpoint (Rust :611 onward); no argument-delta card exists in native `Transcript`. `ChatEntry` still marks the latest exchange's assistant text `live` until a later exchange/interrupt/settlement, and `RichDocument` may still reveal it when a card arrives. The run also continues streaming later assistant exchanges while earlier cards remain. **This is the existing “cards while assistant is still streaming” behavior**, not a requirement to show half-parsed executable arguments. Ghosty's Claude adapter likewise announces full `tool_use` blocks, not partial JSON.

### 3.6 Thinking and idle/loading/ghost behavior

Thinking is an independent row, not the content of an ordinary tool card:

- `Thinking…` while live, plus newest heading/line preview. `latestThought` prefers the newest Markdown heading or whole/open bold heading; otherwise last nonblank line; removes `**`/`__`, bounds to 160.
- It arrives **closed even while live** (a deliberate native behavior). Toggle reveals plain 14 px / 1.65-line-height text, 1 px rule and 13 px text offset. No Markdown rendering. Finished header is just `Thinking`; newest-item label disappears.
- Thinking becomes done as soon as its exchange has text or a call, or is not the newest exchange, or the live run is interrupted. Tool updates do not reopen it. Redacted saved thinking displays `[Redacted thinking]`, not the provider signature.
- Shared coupling is row order, 12 px after-thinking spacing, expansion/selection infrastructure, and the run's activity signal. Ordinary ToolCard does not consume the thinking body.

The working ghost belongs to the host run, not any individual card:

- Busy transition shows it and increments a run counter. `WindowController::receive` examines the **newest changed row**: live assistant → `answered`; other newest row → `worked`. This is a display-driven rule, not “any tool progress always shows a ghost.”
- Main's `answered` hides it and restarts a **1,500 ms** quiet timer; `worked` stops that timer and shows it while busy. New thinking/tool/status activity can make it return. Turn end hides it. Approvals suppress the footer ghost while awaiting the reader.
- The native ghost is in the **list footer after rows/live metrics**, unlike descriptions of the web UI's per-exchange DOM placement. Preserve native placement.
- WorkingGhost: 30×33 procedural ghost; 450 ms entrance, 260 ms collapse/exit with reversal, 320 ms position glide. Same run's loop continues across text/tool phases; a new run restarts it. It is not a per-tool spinner or progress meter.
- Empty-chat `WelcomeGhost` and first-send handoff already exist in OpenGhost. No tool-specific idle animation, artwork, loading splash or alternate mascot is required. `toolStatus` enrichment in Ghosty is run-level status, not a card state transition.

### 3.7 Restore, interruption and malformed data

Ghosty does not persist QML cards. It rebuilds them from canonical backend history:

- `Client::show` constructs/project a fresh Transcript on open. `Client::settle` replaces live observations with the settlement snapshot; missing/null/unusable canonical history calls `interrupt` and reports uncertainty. No replayed tool execution.
- `project` requires `history` array, reconstructs calls/results, and initializes unmatched calls to `missing`. Malformed result content (not string), missing/non-boolean `isError`, missing/wrong tool name or unmatched ID does not finish a card. A later valid result still can. A duplicate terminal result cannot overwrite the first accepted one.
- Live malformed results neither mutate an existing card nor invent an orphan card. Valid orphan **results** are supported; orphan **progress** is not.
- `interrupt` preserves live partial output and marks only unresolved running calls `unconfirmed`; seals further observations. It also adds an explicit run notice. Run cancellation without per-tool confirmation is not success and not proof side effects were undone.
- Canonical projection can correct observations, supply missing arguments and replace partial output. Transient progress is not canonical history. A missing saved result after reopening does not magically restore lost live bytes.
- `Client::adopt` can join a running Ghosty service using saved calls/results plus `snapshot.live` thinking/text/tails, revision-gated. **Stock Pi RPC has no equivalent full live-tail reconnect snapshot.** Current OpenGhost owns Pi child processes rather than reconnecting to that Ghosty shared service.

### 3.8 Motion and shared dependencies

- Tool arrival is in **ChatEntry**, not ToolCard: opacity 0→1, translate y 8→0, scale .98→1 over **500 ms**, `Theme.motion = cubic-bezier(.32,.72,0,1)`. Only actual arrivals/conversation opening, not delegate reconstruction from scrolling. User entrance is 350 ms; the separate 450 ms “message-tools” animation is the reply's Copy control, **not tool-call cards**.
- Chevron rotates -45°→45° in **200 ms**, same motion curve. **No animated card-height expansion, result crossfade, spinner, pulse, or per-output-chunk entrance** exists to port.
- Reduced motion disables arrival and chevron Behavior; active row entrance animations complete on preference change. WorkingGhost settles entrance/exit/glide immediately and its procedural animation stops. Existing assistant reveal/scroll machinery has its own reduced-motion handling. Functional output and the quiet timer still operate; reduction must not hide state.
- Use OpenGhost `Theme.reducedMotion`, including current platform discovery/override, not Ghosty's Linux-only DBus implementation. No platform code change is needed for the plugin.
- Required host services/components: `Theme` (palette/typography/metrics/easing), `PathIcon`, `BoxShadow`, `WellScroll`, `Selection`/`SelectionWash`, `LiveText`, `SelectArea`, list anchoring and `RichDocument` for neighboring assistant rows. Existing QtQuick/Controls and fonts suffice. **No additional image, font, shader, WebEngine, network fetch or animation asset** is used by an ordinary card.

### 3.9 Subagent dependency boundary (audited, not part of first port)

The current source card contains a real special case, so copying the whole file blindly has observable consequences:

- Exact name `subagent` replaces ordinary output with a **320 px** activity well, prompt above it, separate activity scroll/follow latch and keyed step model. A finished card calls `row.ghosty.loadActivity(row.key)` when opened.
- `ChildActivity` consumes child `observation` frames with `jobId/parentSessionId/seq`; reads paginated `childHistory` after completion. Limits: 64 logs, 600 steps/log, 16 pages, 16,000 text/thinking characters or 4,000 result characters per step. It reports gaps, partial history, load failures and unavailable jobs.
- Child job overlay correlates parent run/exchange/call; background cards follow the child beyond the parent's launch result; foreground stays running until its ordinary tool result. Jobs have explicit cancelled/unknown states. No-job saved result can be shown as one activity step.
- Pi's nested `parentToolCallId` events are **not** this child-job/history protocol. Stock Pi has no Ghosty job inventory, childHistory or report delivery contract. An arbitrary Pi extension named `subagent` must not accidentally activate a nonfunctional Ghosty panel.

Separate this branch and `toolcard::steps`/activity fields from the ordinary card port. Treat such names as ordinary generic result cards unless a separately approved, capability-backed subagent renderer exists. This is a scoped dependency adaptation, not a redesign of ordinary cards. Full-source-file parity including that branch is blocked.

## 4. Current OpenGhost: exact losses and change sites

| Existing file / class | Current behavior | Required change (proposal) |
| --- | --- | --- |
| `src/backend/pi_process.{h,cpp}`: `PiProcess::line`, `onRecord` | JSONL process boundary; forwards non-response/non-bridge-status records. | Reuse. Tool-specific reduction does not belong here. Add transport fixtures only if needed; do not copy Ghosty RPC. |
| `src/backend/pi_backend.{h,cpp}`: `PiBackend::runEvent`, `Run`, `publish`, `rebuilt`, `entries`, `getSession` | Live accepts assistant text only. `message_update` rejects everything but `text_delta`; toolResult-role messages return early. `message_end` uses `textOf`, losing call/thinking blocks and indexes. `run.message` is cleared before execution events. Rebuilt history likewise includes only assistant text/usage/outcome. | Normalize complete assistant blocks, call announcements/execution updates/results, thinking and source identities. Maintain call→originating-message map after message end. Rebuild the same data from saved entries. Bound/coalesce partial data; don't append cumulative output repeatedly to journals. |
| `src/backend/types.h` | `ToolStarted{toolCallId,name,title?}` has **no arguments**. `ToolProgress/ToolCompleted` have opaque `QJsonObject detail` without update mode/outcome semantics. `ReasoningDelta{text}` is intentionally invisible. No content-block position/final reasoning snapshot. | Extend neutral typed display contract. Preserve raw structured payload separately from bounded visible text; distinguish announced/executing, append/replace, terminal outcome/uncertainty and message/part origin. No raw Pi event parsing in QML. |
| `src/frontend/chat_service.{h,cpp}`: `DisplayRow`, `ChatRecord::Turn`, `applyEvent`, `publishReply`, `endTurn`, `recoverTurn` | Roles only User/Assistant/Note/Moved/Preserved. Tools are a **QHash keyed only by call ID for whole OpenGhost turn**, holding name/state/progress/result; no row/order/arguments. State is started/completed/interrupted, not error-aware. Reasoning is not projected. `publishReply` joins multiple messages assigned to one reply row with blank lines. | Host-owned ordered neutral parts/calls, per-exchange identity, bounded output, canonical corrections, preserved terminal truth. Split display parts where tools/thinking must interleave; do not bolt cards to the end of one flattened reply. Retain core admission/sequence/recovery checks. |
| Same: `stop/applyEvent/endTurn/finishRows` | Local Stop freezes the turn; terminal/stopped guard drops later non-usage events. Later TurnCompleted may only clear pending markers. | Keep immediate freeze, but separately reconcile authoritative tool outcomes into underlying state/cache, without restarting reveal or claiming cancellation on every call. Generic `finishRows` must not overwrite per-tool done/error/missing truth with the run status. |
| Same: `entries/rowsOf`; `src/frontend/library.cpp::Library::displayMessages` | Cache allowlist retains user/assistant/compact/stats/moved only. Assistant content is a string. Restored row keys are new UUIDs; tool/thinking data is dropped. | Version/validate bounded neutral display parts and stable source-key metadata. Persist independently of enabled renderer. Unknown/unsupported data must survive safe downgrade policy rather than be silently erased on next save. |
| `src/presentation.h::Entry`, `src/model.{h,cpp}::TranscriptModel` | Only User/Assistant/Note kinds, no tool fields, expansion/toggle, omitted counts or AfterThinking join. Existing keyed diff already useful. | Add neutral row/part projection and view state; keyed revisions and role updates. Keep feature-specific formatter in plugin, not model. |
| `src/window.{h,cpp}::WindowController::sync`, `approvals` | Projects only current display types and revisions. Forwards ChatService `answered/worked`. Approval QML maps expose request/card/answered, **not tool-call identity**. | Publish tool view values and renderer registry; derive activity from ordered projection independent of rich/plain choice. Expose an optional correlated approval-wait flag without parsing key suffixes. |
| `qml/ChatEntry.qml` | No ToolCard/thinking branch; default unknown kind would fall into assistant rendering. Still owns list sizing/reveal/drop helpers. | Generic plugin/fallback loader slot with row context; keep ordinary user/assistant paths unchanged. Put tool-specific entrance wrapper in plugin. |
| `src/selection.{h,cpp}::ReplySelection::extraOf/build/followTrims/noteTrims` | No tool/thinking unit generation. Inner-scroll support remains; trim repair still explicitly recognizes unit path `t/b:t`. | Renderer-independent selection-provider registration, revision invalidation and removal/switch cleanup; generalize trim repair to provider-declared unit paths/counts. Plain fallback has its own units. |
| `qml/Main.qml` | WorkingGhost, quiet timer and list anchoring already present. | Preserve them. Add renderer-switch anchor/focus transaction; ensure unrendered reasoning/tool activity can still drive existing busy feedback. |
| `src/frontend/plugins.{h,cpp}`, `src/model.{h,cpp}::PluginModel`, `qml/SettingsDialog.qml` | Backend-runtime plugin projection; tab depends on `frontend.runtimePlugins`. Pi does not implement that backend plugin API. | **Do not repurpose backend toggles.** Add local frontend plugin model/controls, available disconnected and with Pi. Reuse existing Settings row/pill visuals and keep backend entries distinct. |
| `src/frontend/preferences.{h,cpp}` | Frontend preferences, no local renderer enable flag. | Optional durable local frontend-plugin enabled map with explicit version/default. Failure to persist must be reported separately from current in-memory activation. |
| `src/main.cpp`, `CMakeLists.txt` | Host composition, QML `OpenGhost.Cpp`/`OpenGhost.Ui`; no local renderer plugin registration. | Register/link the optional renderer and its QML resources; inject registry into facade/selection. Missing plugin must fall back cleanly. No Pi tool/extension registration required. |

Current Pi sessions are **not all ephemeral anymore**: the working tree gives chats their own child/session files and writes `openghost-turn` custom start/end markers through the bridge. `get_entries` is used for turn reconstruction. This is useful groundwork, **not rich-card recovery**:

- `rebuilt` ignores toolResult and thinking/call blocks.
- `entries` discards returned `leafId`; `rebuilt` scans append order. Full history import must use `leafId/parentId` to avoid mixing abandoned branches.
- `GetSession` returns a named recovered turn (or a running one), not all historical tool rows. Opening an already completed chat depends on the local display cache. Adding only live event handlers will not restore old cards, and old text-only caches cannot manufacture lost tool data.
- Recovery assigns new message UUIDs. Rich rows need stable originating-part aliases/keys across live, saved and cached views; never use just call ID or the currently displayed chat.
- `PiBackend::child` forwards records to `runEvent` only after `run.accepted`. The new mapping must test fast/early records around prompt admission and preserve any safely correlated pre-acceptance observations in a bounded buffer, without inventing acceptance. The plugin must not subscribe directly to raw process events to work around this gate.
- No frontend-plugin SDK currently exists. “First real frontend plugin” requires the small seam below, not pretending backend plugin management already supplies it.

## 5. Pi data mapping and parity requirements

Authoritative Pi references read: `Pi/docs/{rpc,rpc-commands,json,message-types,session-format,rpc-extension-ui,sdk,extensions}.md`; `Pi/dist/modes/json-event.js`; `Agent/dist/{agent-loop.js,types.d.ts}`; `Pi/dist/core/tools/{bash,edit,read}.js`; extension type declarations. The SDK minimal subscription and custom-tool examples corroborate the distinction between event observation and tool registration.

### Event/data mapping

“Current” below describes this OpenGhost working tree; “required” is the proposed adapter behavior, not an existing API.

| Ghosty input/behavior | Available Pi RPC data | Current OpenGhost | Required mapping |
| --- | --- | --- | --- |
| Run/session envelope, numeric exchange `turn` | RPC process/session ownership; `agent_start`, `turn_start`; assistant `message_start`; saved entry IDs/parentId. Most live events have no session/turn/message ID. | PiBackend supplies session, local turn/client ID and sequence, new UUID per assistant message. | Keep host envelope; allocate stable assistant-exchange/part identities and associate calls before `run.message` is cleared. Ghosty `turn` ≠ OpenGhost whole-run `turnId` ≠ Pi one-response `turn`. |
| Complete `toolCall{id,name,arguments}` | Assistant `message_end.message.content` contains ordered `{type:"toolCall",id,name,arguments}`. `tool_execution_start` has `{toolCallId,toolName,args}`. | Both call content and execution events discarded. | Announce cards in completed assistant call order; confirm execution phase on start without duplicate insertion. Missing-call start/result may create a bounded orphan display with appropriate argument knowledge. |
| No partial-argument card in Ghosty | `message_update.assistantMessageEvent.toolcall_start{contentIndex,id,toolName}`, `toolcall_delta{contentIndex,delta}`, `toolcall_end{contentIndex,toolCall}` | All discarded. | Retain block identity/final data if needed, but **do not present partial JSON as executing**. For strict ordinary Ghosty timing, publish announced cards on finalized message, not `toolcall_start`. |
| Text alongside tools | `text_start/delta/end{contentIndex,…}`; authoritative whole `message_end` | Only text deltas/final flattened text; block indexes lost. | Preserve part order; replace on final, don't duplicate text. Rich/plain switching must not reset `RichDocument` reveal of other rows. |
| Append `toolProgress.output` | `tool_execution_update{toolCallId,toolName,args,partialResult}` with content/details | Discarded. | Normalize explicit update semantics; built-in Bash is **replace snapshot**, not append. Keep live output separate from final result and assistant prose. |
| `toolResult` final body / `isError` | `tool_execution_end{toolCallId,toolName,result,isError}`; then toolResult-role `message_start/message_end` with content array, details, isError | Both discarded. | End supplies prompt live terminal display; reconcile/dedupe against finalized toolResult message, and use saved message for restore. Don't create two cards or charge usage twice. |
| Completed/failed | `isError` on execution end and result message | ToolCompleted has no typed outcome; service marks completed regardless. | Strict boolean validation; done/error enum mapped to Ghosty labels/tints. A valid empty content array is an empty successful/error result, not a missing result. |
| Per-tool cancelled | No universal terminal cancellation enum; abort often yields error content such as `Command aborted`/`Operation aborted`. `abort` confirms run idle, not a structured per-call cause. | Run-level cancellation exists; tool states interrupted. | Preserve actual isError/body; only use card `cancelled` with explicit supported evidence. Never parse arbitrary error text as authoritative cancellation or change earlier successes. |
| Missing/unconfirmed | Saved assistant call with no matching result; child-process loss; absent final event; run settlement | No rich state projection. | Missing for unmatched finalized-history pairs; unconfirmed for interrupted unresolved observations. Never settle tools done merely because `agent_settled` arrived. |
| Bash ending/cleanup report | Stock Bash final `content`, optional truncation/fullOutputPath in `details`; execution result may include `structuredContent{output,truncated,exit_code,wall_time_seconds,…}` | Not retained. | Generic Done/Failed plus real output works now. Optional explicit exit code can populate an ending; no invented signal/cleanup/reaping/output-complete fields. See loss below. |
| Thinking text/finality | `thinking_start/delta/end` with contentIndex; saved `{type:"thinking",thinking,redacted?,thinkingSignature?}` | Ignored; ReasoningDelta carries only text and is invisible. | Normalize visible thinking only, stable blocks and final replacement; preserve redacted marker, never display signatures. Separate renderer can consume same neutral parts. |
| Ghost activity / tool status | Tool/thinking/text events; run lifecycle; no Ghosty Action Fusion/reducer status objects | Text drives answered; tool events do not reach worked. | Derive ordered display activity in host; no fake Ghosty status enrichment or per-card ghost. Keep behavior same with plugin OFF. |
| Retry | `auto_retry_start/end`; failed message and later assistant message; `agent_settled` only after automatic continuation stops | Intermediate retry events ignored. | Mark attempt boundaries/replace provisional text deliberately; retain completed tools. Pi retries are not automatically Ghosty's same numeric exchange retry. Test rather than rename events blindly. |
| Restore canonical cards | `get_entries{since?}` → `{entries,leafId}`, message entry stable IDs; `get_messages` supplies current context messages | Turn reconstruction reads entries but loses tool/thinking and leaf. | Select active branch; reconstruct calls/results from finalized messages; retain historical cached rows, dedupe live/replayed records. `get_messages` alone is not pre-compaction history. |
| Live reconnect tails | No saved transient `tool_execution_update` history or Ghosty `snapshot.live` equivalent | Only current-process typed journal | Bound local display snapshots while alive; after restart restore saved final results. Missing transient bytes stay unknown. Do not rerun tools to refill UI. |
| Waiting for approval | Pi extension `tool_call` can block/await an RPC UI dialog; stock execution-start event has no approval correlation | PiBackend::answer empty; no rich reverse approvals | Independent policy/bridge work needed for exact waiting label. Native plugin consumes a read-only optional wait flag; does not implement permissions. |
| Subagent / nested tools | Nested execution events may have `parentToolCallId`; IDs `<parent>/<n>`; bounded `nestedCalls` metadata on parent result, not nested transcript results | Ignored | Retain relationship in neutral observations if supported; do not flatten into independent durable root cards or map to Ghosty's childHistory. Child job UI remains excluded. |

**Important source-level nuances:**

1. `Pi/dist/modes/json-event.js` strips cumulative SDK `partial`/`message` snapshots from **message updates**, injecting id/toolName on `toolcall_start`. RPC consumers must reconstruct by `contentIndex`; do not design against the in-process SDK shape. Tool execution updates are not similarly delta-converted.
2. `Pi/dist/core/tools/bash.js::emitOutputUpdate` publishes `OutputAccumulator.snapshot()` with latest content and truncation metadata. Appending every partialResult duplicates output. Pi may already have truncated its tail before OpenGhost sees it. Track replacement, unchanged prefix/overlap only when provable, and distinguish Pi omission from OpenGhost's own 32 Ki trim. Exact Ghosty code-point omission counts/scroll continuity cannot always be reconstructed from byte/line totals or ambiguous repeated text.
3. Unknown custom tools have tool-specific update contracts. Preserve the latest update as a snapshot unless an explicit supported contract says append; do not infer append from a field name. Exact streaming parity for every arbitrary extension is not guaranteed by stock events.
4. `Agent/dist/agent-loop.js` emits starts in call/preparation order, ends in **completion order** for parallel execution, and toolResult messages in **original call order** after that batch. Render in assistant call order, updating by identity, never completion order. Sequential execution starts each call later; canonical call announcement can still show the batch's unresolved cards as Ghosty does.
5. `createToolResultMessage` retains content/details/usage/isError but **does not retain `structuredContent`**. Bash's `exit_code` can be available live yet absent from saved result history, especially successful commands. A live “exited 0” ending based on that field is not automatically restorable. Keep generic endings consistently, or explicitly add display-only durable metadata via existing Pi extension/custom-entry facilities before promising parity. No upstream patch is required for the ordinary generic card.
6. Pi content can include images; Ghosty ordinary output is one plain string. Do not stringify image base64 into a card or claim image-output parity. Preserve typed content/omission information in the neutral contract; native media tool results are a separate scoped decision. Never auto-open a `fullOutputPath` as a new file authority.
7. Tool execution start is emitted **before argument preparation/validation** in the inspected Pi loop, and even for calls that are failed without execution (e.g. truncated assistant response). It is not proof of side effects. Final call arguments may differ from execution-start raw args after preparation/hooks. Preserve source/execution distinctions where known; the card describes observed arguments, not a verified execution audit log.

## 6. Smallest frontend-plugin API (proposed)

### Ownership: host owns truth; plugin owns presentation

```
PiProcess → PiBackend → typed events/saved parts → ChatService neutral transcript
                                                 ↓
                                WindowController / TranscriptModel
                                                 ↓
                   renderer registry → rich plugin OR plain fallback
                                      ↘ selection-unit provider
```

Both renderers consume the **same continuously maintained data**, including while disabled, disconnected/display-only, recovering or offscreen. The rich plugin must not become an event subscriber that loses disabled intervals, a second history store, or a Pi client. Core backend/service code knows tool semantics, not Ghosty's card design.

Use a small compiled-in, separately registered feature module for version 1. Logical live enable/disable is enough; no arbitrary shared-library loader, JS runtime, hot native-library unload, generic event bus or plugin dependency framework is needed. Build/remove the rich module without changing basic tool behavior. Static linking is compatible with a real plugin seam if selection, formatting and registration are replaceable and the host has no rich-card imports/branches.

### Neutral data required before rendering

Proposed additions to the host contract (names illustrative but responsibilities mandatory):

- `PartIdentity`: session/incarnation, client/remote run, assistant exchange/message, content-part ordinal and call ID; optional parent call; stable display key plus revision. Use structured identity, not delimiter parsing of untrusted IDs.
- `ToolCallObserved`: full known arguments as data, exact name, originating part order; retain absent/unknown separately from `{}`. Optional bounded raw argument preview for invalid/truncated display.
- Execution update: explicit `Append` or `Replace`, typed text/image content, details, omission/completeness metadata when actually known. Final result carries strict outcome, optional explicit cancellation cause, finality/source and supported ending metadata. Keep Pi-specific interpretation in the adapter, not QML.
- Tool phase distinguishes **announced, executing, done, error, cancelled, missing, unconfirmed**. Ghosty visual mapping of announced/executing can both be `running`; `waitingForApproval` is independent and only true when correlated. No extra new visual stage is required.
- Host `ToolView`: key/revision/name/arguments display text/argumentsKnown, mapped state, bounded body/ending/omittedLines/omittedCharacters/trimmed, grouping/order, optional waiting flag; distinguish reported vs locally clipped omission. Plugin formatting can consume typed args/details but never unbounded wire payloads.
- Thinking parts (if enabled later): identity/order, text, live/done/redacted, preview and authoritative-final replacement. Provider signatures are not renderer inputs.
- Saved/recovered parts use the same projection. Plugin enabled state never changes journal/history collection, pending-turn markers or acceptance.

### Exact hooks the feature actually needs

Proposed host interface in `src/frontend/render_plugin.h`; it may be an ordinary C++ struct plus callbacks rather than an ABI-heavy QObject hierarchy:

```cpp
// Proposed API, not current source.
struct TranscriptRenderer {
    QString id;                    // e.g. "openghost.rich-tools"
    int apiVersion;                // 1; reject incompatible registration
    QString rowKind;               // "tool" in this plugin
    QUrl delegate;                 // packaged QML, no backend-supplied URL
    SelectionProvider selection;   // pure projection(row, viewState), no QML needed
};
// Registry: registerRenderer(renderer), setEnabled(id, bool),
//           rendererFor(rowKind), renderersChanged(rowKind, generation).
```

`SelectionProvider` returns units, a selection revision, and per-unit cumulative removed-prefix counts (UTF-16 document positions). That last field replaces `ReplySelection::followTrims/noteTrims`' existing hardcoded `t/b:t` check: the rich provider declares its output unit; the plain provider declares its own. The host must not know plugin-specific unit paths. Retain source handling for CRLF/document-position normalization.

The QML delegate receives **one host row context**, replacing ToolCard's implicit dependency on the entire ChatEntry/controller:

| Hook / input | Exact use |
| --- | --- |
| Read-only row values and revision (`ToolView`) | `ToolText.describe`, status/ending/output; incremental role updates. |
| `column`, stable key, list/view handle, join/gap and `arriving`/arrival generation | Existing dimensions; plugin reproduces 500 ms scale/rise/fade only for actual arrivals, not toggles. Core can host the stable outer row while plugin owns its tool-specific transform. |
| `viewState(rendererId,key)` and `toggleExpanded(key)` | Initially collapsed expansion. Keep expansion outside disposable delegate; optional renderer-scoped outputScroll/followOutput for live switch preservation. Clear on conversation replacement as source does; no durable expansion requirement. |
| `dropped(item,height)` plus host list anchoring transaction | Live trim/omission compensation and height changes. |
| Existing `Selection.enroll/range`, `SelectArea` host/menu/view contract | Shared pointer selection; plugin pure selection projection replaces core's hardcoded kind/trim-path branches. Unit revision includes row revision, expansion and renderer generation. |
| Existing clipboard/selection-menu functions through a narrow host context | Reuse LiveText/SelectArea behavior; no raw backend/controller access. |
| Existing `Theme` and shared QML/native types | Exact fonts/palette/easing/reduced motion, shadow/icons, WellScroll. Make common components importable by the packaged renderer without copying app Main.qml or creating a core→plugin QML import cycle. |
| `aboutToChangeRenderer` / `rendererChanged` transaction (or equivalent registry signal handling) | Save scroll/focus, clear/remap invalid selection paths, switch only tool loaders, restore anchor. No new backend command. |

The plugin owns `ToolText`/description/selection formatting and the rich card QML. It can be registered with a plain fallback supplied by the host. No writable session, send/cancel/retry, tool execution, file/network, auth or model API is necessary. Approval waiting is a read-only optional input; no approval request handler belongs to this plugin.

For first-plugin simplicity, one active renderer per row kind plus one fallback is sufficient; no priority chains are required. Callback failure or QML load failure falls back to basic rendering and reports a **local plugin error**, not a failed agent turn.

## 7. Live ON/OFF behavior and thinking decision

### No-restart switching

1. Register rich renderer/resources at application composition. Local Settings entry is available even when backend `runtimePlugins=false` or disconnected. Keep backend plugin controls separate/labeled; do not send `EnablePlugin/DisablePlugin` or a Pi slash command for this toggle.
2. **ON:** registry selects Ghosty's rich tool delegate and matching selection provider for existing and future tool rows. Use the current neutral snapshot immediately; no tool replay or backend reload. New rows default collapsed; retain same-view rich expansion on OFF→ON if it already existed.
3. **OFF:** registry selects the host's basic/plain tool delegate and its selection units. Plain rendering exposes tool name, arguments, actual state and bounded output as text; it is not assistant prose and not hidden. This baseline is new infrastructure, not an existing OpenGhost feature.
4. Keep stable outer transcript row keys/models. Change only tool loader contents; do not reset the transcript, recreate unrelated Markdown, discard progress, restart elapsed clocks/ghost, clear composer drafts, or change tool availability/permissions.
5. Before switching, capture transcript top visible key + pixel offset and follow-end latch; stash rich well scroll/follow state while its delegate still exists. Restore after layout, or remain at end if already following. Prevent fake card-arrival animation on a renderer switch.
6. End in-flight selection gestures safely. Selection units differ (rich closed has none, plain has text): invalidate cached tool ranges on renderer generation change, preserve unaffected ranges, and explicitly clear unsupported cross-tool selections rather than copying stale hidden text. Move focus from a destroyed rich header to the equivalent plain row or transcript, not the composer unexpectedly.
7. Continue ingesting all tool events during/after switch. Apply changes on the GUI owner thread; defer the short visual transaction if needed without blocking pipe consumption. Pending/late events address stable identities, never a delegate pointer.
8. Logical disable disposes delegate connections/temporary UI state; do not unload code while QML or selection callbacks reference it. Unregister/teardown must invalidate callbacks before module lifetime ends. An incompatible/missing module is simply OFF with plain fallback.

Persist only the local enable preference if desired. It is not part of chat canonical history. Snapshot/output data remains host-owned and cached whether ON or OFF. Existing completed text-only caches require an explicit Pi-history backfill path; toggling ON cannot invent their earlier calls.

### Thinking: separate plugin, shared plumbing

**Recommend a separate thinking renderer, not bundling thinking into rich tools.** Its data already has a distinct kind/state, it renders a different disclosure, and ordinary ToolCard does not reference it. Shared order, joins, selection, expansion, coalescing and activity are host concerns, not reasons to couple enable switches.

Port normalized thinking/block identity while establishing transcript parts if that avoids a second contract migration, but keep rendering opt-in/separately registered. If thinking is hidden, grouping must be computed from **visible adjacent rows** so invisible thinking does not leave an unexplained 12 px gap. Busy feedback should still observe genuine reasoning activity without exposing its text. Exact combined Ghosty parity tests should enable both renderers; tool-only parity tests should document thinking as independently disabled. No changes to model effort/level controls are required by either renderer.

## 8. Exact files to port / change

### New feature files (recommended destinations; not created by this audit)

| Ghosty source | Proposed OpenGhost destination | Extent |
| --- | --- | --- |
| `native-ghosty/qml/ToolCard.qml` | `plugins/rich-tools/qml/ToolCard.qml` | Preserve ordinary layout/body/scroll/selection/chevron code; import `OpenGhost.Cpp` and shared UI; replace ghosty facade with row context; detach subagent/activity block. |
| Tool arrival portion of `native-ghosty/qml/ChatEntry.qml` | `plugins/rich-tools/qml/ToolEntry.qml` | Exact 500 ms arrival wrapper, transforms, reduced-motion behavior and row host forwarding. Not whole ChatEntry. |
| `native-ghosty/src/toolcard.{h,cpp}` | `plugins/rich-tools/src/toolcard.{h,cpp}` | `ToolText`, ordinary `describe/endLines/liveNote/omission/plain/units`; Pi edit display adaptation. No ChildActivity include/steps. |
| Registration/composition is new | `plugins/rich-tools/src/plugin.{h,cpp}`, `plugins/rich-tools/CMakeLists.txt` | Register `openghost.rich-tools`, packaged delegate and selection callback, separately removable target. |
| No Ghosty equivalent (first plugin seam) | `src/frontend/render_plugin.h`, `src/frontend/render_plugins.{h,cpp}` | Minimal versioned registry, local enabled state/model, renderer resolution/lifetime. |
| Extracted Ghosty projection algorithms, not Ghosty protocol | `src/frontend/tool_projection.{h,cpp}` | Neutral bounded tool rows/identity/order/finality and display-cache conversion, shared by plain and rich. |
| No current OpenGhost fallback | `qml/PlainToolCall.qml` | Basic inert text rendering/selection for OFF or unavailable plugin. |
| Optional separate thinking port from ChatEntry/latestThought | `plugins/thinking/qml/ThinkingEntry.qml`, `plugins/thinking/src/plugin.{h,cpp}`, `plugins/thinking/CMakeLists.txt` | Later/independent plugin; no requirement to copy Ghosty Main/client or subagent panels. |

Required modifications to **existing** files:

- `src/backend/types.h`; `src/backend/pi_backend.{h,cpp}`: normalized tool/part contract, live mapping and saved reconstruction.
- `src/frontend/chat_service.{h,cpp}`; `src/frontend/library.cpp`: ordered neutral data, stop/recovery reconciliation and versioned cache allowlist. Change `library.h` only if the chosen cache-version API needs declarations.
- `src/presentation.h`; `src/model.{h,cpp}`; `src/window.{h,cpp}`: display projection, revisions/view state, registry/facade/local plugin list, correlated approval visibility.
- `src/selection.{h,cpp}`: pure renderer selection-provider dispatch/invalidation.
- `qml/ChatEntry.qml`; `qml/Main.qml`; `qml/SettingsDialog.qml`: generic delegate slot, safe live switch and local toggle; preserve unrelated UI.
- `src/frontend/preferences.{h,cpp}` if persisting the toggle; `src/main.cpp`; root `CMakeLists.txt` for registration/build/QML resources.
- `tests/pi.cpp`, `tests/pi/pi`, `tests/contract.cpp`, `tests/plugin_smoke.cpp` or a separate frontend-plugin smoke; root CMake test registration. Keep existing backend-plugin tests intact.
- `docs/native-import.json`/`NOTICE.md` as appropriate for new imports and `docs/cpp-port.md` for the approved scope/architecture change, **during implementation**, not by this audit.

No ordinary-card production change is required in `src/backend/pi/openghost-bridge.js`, `src/backend/pi_process.*`, `qml/WorkingGhost.qml`, `qml/LiveText.qml`, `src/exposure.*`, `src/theme.h`, `src/ghost.*`, `src/platform/*`, or frozen `reference/`. Optional durable Pi-only ending metadata or future rich approvals would separately require bridge work. Keep renderer concerns out of that bridge.

### Exact test sources to adapt

Do not copy source fixtures that start Ghosty's backend. Reuse their assertions over the new neutral/Pi scripted fixture:

- `native-ghosty/tests/test_transcript.cpp`: `liveToolRules`, `malformedLiveResults`, `canonicalResultPairing`, `orderedParts`, `orderedKeysAreStable`, `orderedResultPairing`, `liveSettlesToCanonicalOrder`, `statusStaysRunLevel`, `retryKeepsTools`, `outputAndRowBounds`, `revisionsTrackContent`, `endedRunIgnoresObservations`, `oversizedIdentitiesAreNotRetained`, `omissionCountsLinesTruthfully`, `exchangeRowsJoin`; Bash diagnostics only for genuinely supported report input.
- `native-ghosty/tests/test_model.cpp`: keyed updates/expansion and `retryKeepsThinkingExpansion` for the optional thinking plugin.
- `native-ghosty/tests/test_selection.cpp::toolTextsAsTheCardShows`.
- `native-ghosty/tests/test_window.cpp`: `toolCardsAndThinking`, `expandedToolStreamsIntoDocument`, `wheelChainsPastToolWells`, `wheelStaysInAToolWell`, `toolCardEndingsAndWells`, `toolCardHeadsCentred`, `selectionTakesInToolsAndThinking`, `toolErrorSelectsWhole`, `selectionCrossesToolRows`, `toolWellScrollsUnderASelection`, `virtualizedToolRowsCopyWhole`, `workingGhostFollowsTheRun`; `thinkingHeaderShowsLatestItem` separately.
- Suggested new destinations: `tests/tool_projection.cpp`, `tests/rich_tool_ui.cpp`, `tests/frontend_render_plugins.cpp`. Add Pi-specific snapshot-vs-append, parallel end order, duplicate end/message, branch restore, interrupted result, multibyte trim, custom tool, image-content refusal/omission and ON/OFF mid-stream tests.

## 9. Exact SDK/plugin hooks required (summary)

**OpenGhost must add:**

1. Read-only stable ordered tool/part model with typed arguments, append/replace updates, final outcome/uncertainty, grouping and bounded trim metadata.
2. Local versioned renderer registration/resolution: row kind → packaged delegate + pure selection-unit/trim-metadata provider; guaranteed plain fallback.
3. Keyed transient view state (expansion; rich well scroll/follow for switching), row-context toggle/drop/menu/selection helpers.
4. Local enable/disable notification plus anchor/focus/selection-safe loader replacement, with no transcript reset and no backend mutation.
5. Renderer-independent persistence/recovery and coalesced row updates while disabled.
6. Shared Theme/reduced-motion, fonts/icons/shadow/WellScroll/LiveText/Selection access; no broad backend SDK for the plugin.

**Pi hooks required for ordinary cards: none beyond existing RPC events/history.** Consume `message_*`, `tool_execution_*`, run settlement and `get_entries`. No `registerTool`, `registerToolRenderer`, `ctx.ui.custom`, `ctx.ui.setToolsExpanded`, native Node embedding, or provider logic is needed. Pi's latter UI hooks do not render native Qt cards over RPC. A later custom metadata/approval bridge can use existing extension APIs, but is not prerequisite for honest generic Done/Failed tool cards.

## 10. Pi event mapping (implementation checklist)

```
assistant message_start + indexed updates
    → ordered exchange/parts (text, optional thinking; no executing partial args)
assistant message_end.content.toolCall[]
    → complete call announcements in canonical order, stable card keys
 tool_execution_start(args)
    → execution phase of same call (or known-args orphan), no duplicate card
 tool_execution_update(partialResult)
    → explicit replace/append normalized output; Bash = replace snapshot
 tool_execution_end(result,isError)
    → terminal body/state of same card
 toolResult message_end
    → authoritative finalized result reconciliation/persistence, deduplicated
thinking_delta/end + saved ThinkingContent.thinking
    → separate optional thinking parts/renderer, never assistant prose
agent_settled / confirmed abort / process loss
    → run end; unresolved calls remain missing/unconfirmed as evidence warrants
get_entries + leafId/parentId + openghost-turn markers
    → same keyed saved-call/result projection; no execution replay
```

Retain message/exchange/call association beyond `message_end`. Do not use unordered QHash iteration, tool completion order, or a bare call ID as transcript order/identity. Do not treat direct RPC `bash_execution_update`/`BashExecutionMessage` as model `bash` tool calls.

## 11. Blockers / gaps

| ID | Gap | Blocks |
| --- | --- | --- |
| G1 | No local renderer registry, no plain fallback; existing Plugins is backend-only. | Live ON/OFF as requested. |
| G2 | Pi adapter drops tool/thinking/part data; ToolStarted lacks args and ToolCompleted lacks typed outcome. | Any real rich call/output display. |
| G3 | Flattened reply rows, whole-run bare-call-ID hash, no durable source part keys. | Correct interleaving, parallel ordering and live→saved stability. |
| G4 | Display cache drops tool/thinking; recovery ignores them and active-branch leaf. | Reopened completed chats and branch-safe backfill. |
| G5 | Pi Bash snapshots vs Ghosty chunks; upstream truncation loses exact prefix/code-point/offset information; arbitrary extension update modes unspecified. | Universal byte-for-byte live-tail/omission/scroll parity. Ordinary bounded snapshot display is feasible. |
| G6 | No universal Pi per-tool cancellation cause, rich approval-wait identity, or Ghosty Bash cleanup/reaping report. `structuredContent` is not persisted as a tool result. | Exact diagnostics/cancelled/waiting parity in all cases. Must show unknown/generic truth, not fabricate. |
| G7 | Pi `edit` oldText/newText schema differs; result arrays can contain images. | Unchanged formatter/result projection. Small edit adaptation is straightforward; image output is outside Ghosty's ordinary string renderer. |
| G8 | Ghosty subagent activity is embedded in ToolCard/toolcard headers and depends on jobs/childHistory. | Copying the **entire** implementation unchanged; excluded from first-plugin scope. |
| G9 | Native selective imports and existing OpenGhost UI changes must be retained; Ghosty source/test edits remain dirty and target Pi work was integrated concurrently during this audit. | Blind whole-file replacement; pin/review source before implementation. |
| G10 | No runtime tests or pixel/animation comparisons run in this audit. | Any claim that visual parity or real Pi integration is already qualified. |

There is no identified need to add a new native rendering dependency. The hard gaps are contracts, projection, lifecycle and scope, not lack of Qt widgets.

## 12. Recommended implementation order

1. **Agree scope/contracts first:** ordinary top-level tool cards, generic fallback, separate thinking; explicitly exclude child-job UI. Decide generic versus durable Bash ending policy and cancellation/approval limitations. Pin source revisions after current Pi work integrates.
2. **Normalize events/history:** extend `types.h` and PiBackend, preserve message/block/call identity and finality; test snapshot updates and end/message deduplication with `tests/pi/pi`. No UI redesign or execution changes.
3. **Build host neutral projection/cache:** ordered parts, exact state transitions, bounds/coalescing, active-branch restore/backfill, stop/disconnect reconciliation and stable keys. Keep it active independently of plugins; add plain fallback.
4. **Add minimal plugin/selection seam:** separate local toggle from backend plugins; packaged delegate registration, selection units and view-state lifetime; verify module absent/OFF keeps plain tool behavior and no backend requests.
5. **Port ordinary visuals substantially intact:** ToolCard, ToolText/units and tool-arrival wrapper. Reuse existing dependencies. Adapt Pi edit fields and known result normalization; do not add new animations/controls/assets.
6. **Qualify live toggling:** ON→OFF→ON during arguments/updates/results, collapsed/expanded, scrolled-back well, active selection, keyboard focus, background chat, reopen, disconnected cache and plugin load failure. Assert no lost events, duplicate calls, resent prompts or changed tool effects.
7. **Optional thinking plugin:** reuse the host parts/selection/state seam and extract Ghosty's original disclosure/preview rules. Test thinking→text→tool, both plugin combinations and reduced motion mid-animation.
8. **Focused qualification:** Release with `OPENGHOST_BUILD_SMOKE_TEST=ON`; run `native_contract_test`, `native_browser_test`, `native_ui_smoke`, `native_fake_ui_smoke`, `native_pi_test`, `native_plugins_test` and new tool/plugin suites, then `git diff --check`. Add deterministic geometry/screenshots in both themes, motion-enabled and reduced-motion cases. Scripted Pi/fake success is not real-provider qualification; only run real Pi/provider tests with separate authorization.

Audit-only checks completed: verified 44 named existing source/test/provenance paths, document whitespace, final source fingerprints and `git diff --check`. Build, unit/UI tests and visual comparisons were intentionally not run for this documentation-only task.

## 13. GO / NO-GO

**GO — copy/adapt the ordinary Ghosty card and formatter largely as-is as a frontend rendering plugin.** Existing Qt infrastructure supports it; preserve the current visuals, lazy body, selection, scrolling, state labels and exact motion. Most new work belongs underneath and around the copied presentation.

**NO-GO — unchanged wholesale transplant, immediate full behavioral-parity claim, or calling it a plugin by adding only a QML boolean.** First supply neutral tool data/restoration, the plain fallback and real renderer/selection registration. Ghosty backend/client/transcript and subagent activity are not portable wholesale; Pi does not provide all cancellation, approval, live-tail and Bash diagnostic truth. Those limits must remain explicit rather than simulated.
