# Browser, web and file-host frontend audit

## Scope and verdict

Audit of `cpp-native-extraction`, finalized against native source revision
`346e242af1db010cdbffabc9211e602fefb96af7`. **The only parity authority is
`reference/openghost/`.** No other frontend or backend implementation was used
as a feature specification.

This is a behavior/reachability audit, not a pixel-parity certification. Work
began at `1f3048c4d69e9b2815b31c05fb4315f437b8c3d5` with unrelated effort-control
changes in progress. Those changes were left untouched and committed externally
as `346e242` during the audit; their source diff was checked for impact on these
findings. This commit contains only this document. No features, transport, Rust,
RPC or FFI were added, and the reference was not changed.

**The native application has no built-in browser, and implements none of the 11
reference browser tools.** Browser-shaped C++ values and a scripted contract test
are not a browser implementation. File support is a real but restricted UTF-8
text-attachment path. Image previews and pinned-file settings are stubs; reply
media and the browser download service are absent.

### Summary counts

Each matrix row is one audited behavior. Cross-cutting lifecycle and individual
tools are counted separately; the eleven tools are individually identified.
Reference absences listed later are not counted as missing features.

| Status | Features |
| --- | ---: |
| MATCH | 6 |
| PARTIAL | 10 |
| MOCK ONLY | 4 |
| MISSING | 54 |
| INTENTIONAL DIFFERENCE | 1 |
| **Total** | **75** |

- **MATCH:** the scoped frontend behavior exists, including deliberately inert
  presentation. Does not imply a connected agent or identical pixels.
- **PARTIAL:** real native behavior exists, but some reference behavior is absent
  or observably different.
- **MOCK ONLY:** only a stub, UI shell, or injected test-host path exists for this
  behavior. This does **not** mean `--fake-backend` simulates a browser.
- **MISSING:** no working implementation of the reference behavior. Mere value
  types, icons or unreachable controls do not count.
- **INTENTIONAL DIFFERENCE:** an explicit native policy differs from the reference;
  recorded as a gap, not silently treated as parity. The absence of the whole
  browser is still classified MISSING, not excused by the native dependency policy.

## Verified end-to-end paths

### Reference

1. `index.html` loads `host-tools.js`, `browser-panel.js`, media and attachment
   modules; `script.js` constructs the desktop `BrowserPanel` independently of
   backend availability. The user can browse without an agent connection.
2. `backend-client.js::hello` advertises eleven schemas when the browser bridge
   exists, the renderer guide, and desktop local-path support. `chat.js::sessionParams`
   and steering include the browser snapshot.
3. `chat.js::onHostTool` claims the **named** session/turn, coordinates hand-back,
   and invokes `BrowserPanel.run`; `desktop/preload.js` forwards to
   `desktop/main.js::runBrowser`, then `desktop/browser.js::run/act` acts on the
   guarded guest. `HostTools.result` formats the response back to the caller.
4. The browser is shown as a panel, activity/ownership overlays and normal page
   contents. Tool request/result bodies are **not** a separate chat transcript.
   Screenshots are image parts of host-tool responses, not an automatic screenshot
   viewer in chat. Approvals are a separate reverse-request UI.
5. Pick/drop/paste → `Attachments` → `AttachmentReader` → `chat.js::inputOf` is the
   file-input path. `slim`/`Library.displayMessages` save display previews, not the
   original input. Reply media follows `Markdown` → `StreamView` → `MediaEmbed` /
   `MediaSlider`. Browser downloads use a separate host service.

### Native

1. `src/main.cpp` creates either no `Backend` or `FakeBackend`.
   `WindowController` constructs `ChatService` **without** a `HostServices`
   argument. `ChatService` therefore selects `NoHost` in both modes.
2. `src/frontend/host.h::NoHost` returns no browser and no tools.
   `ChatService::initialize` publishes that empty list; `params` and steering
   supply a null browser. `reverse` refuses unpublished host tools before calling
   `run`. The no-op `NoHost::run` is not an executable browser fallback.
3. `CMakeLists.txt`, the native source tree and the QML registration list have no
   browser panel, engine, browser controller, PDF extractor or media service.
   `FakeBackend` explicitly refuses browser context, non-text attachments and
   pinned files. The test-only `ScriptedHost` advertises just `browser_snapshot`,
   records calls, and relies on the test to emit a result.
4. `WindowController::pick` → `AttachmentStore::prepare/resolve` → `ChatService`
   genuinely reads and sends owned text payloads to the injected backend. It is
   gated by `ready()`, so the ordinary disconnected application cannot prepare a
   selection. There is no upload service despite dormant `uploads` UI strings.
5. `WindowController::sync` reduces every display attachment to name, size and
   `text/plain`. `previewState` always reports unavailable; `previewImage` is
   empty. `GeneralPreview::files` is always empty and `add` always refuses.
6. `src/markdown.h::Kind` has no media block; `qml/Block.qml` has no remote-media
   renderer. `src/main.cpp` installs `denyNetwork()`. Ordinary allowed links can
   still launch the OS application through `platform::openLink`.

## Matrix notation and contracts

**Reference column paths are relative to `reference/openghost/`; native paths are
relative to the repository root.** `::name` identifies the function/component,
not a separate file. “None” means no native implementation was found; the cited
native file is the verified absence, stub, or closest actual code path.
Native symbol shorthand throughout the matrix resolves to these exact files:
`ChatService` → `src/frontend/chat_service.cpp` (state in
`src/frontend/chat_service.h`); `WindowController` → `src/window.cpp` (inline
preview stubs in `src/window.h`); `AttachmentStore` →
`src/frontend/attachments.cpp`; `Library` → `src/frontend/library.cpp`;
`FakeBackend` → `src/backend/fake_backend.cpp`; `Backend` →
`src/backend/backend.h`; `GeneralPreview` → `src/offline_services.h`.

Work categories describe the missing work, not the ownership of agent policy:
**UI-only**, **host-service-only**, **persistence-only**, or **mixed**. A browser
host is a frontend-owned desktop capability; it is not the agent backend.

Existing C++ contracts, all in `src/backend/types.h` unless noted:

- **BS:** `BrowserState` / `HostContext`: lifecycle/control, stable tab IDs,
  revisions, URLs/titles, sign-in observations and `signedInVerified=false`.
- **HT:** `HostToolSchema`, `HostToolRequest`, `HostToolResult`: JSON schemas and
  arguments, text/image parts, error/status/reason and open JSON `data` for refs,
  page/read IDs, pagination, screenshots and downloads.
- **HS:** `src/frontend/host.h::HostServices`: `browser`, `tools`, `run`, `cancel`,
  `finished`, `browserChanged`. No user-facing tab controls, guest surface,
  pointer/download events, PDF reader, file picker or media lookup service.
- **AP:** `ApprovalRequest/Presentation/Answer`, `ApprovalResolved`,
  `PermissionMode`, `ConfigureSession`.
- **AT:** semantic `Attachment`, `DisplayAttachment`, `Input`, `DisplayInput`.
  These already distinguish full input from cached previews and include
  image/text/PDF/video/file kinds, notes, paths, sizes and appropriate media data.
  `DisplayAttachment` also has pasted-text and video-poster metadata.
- **UC:** `ContextFile`, `UserContext`, `Preferences`.

In the contracts column, **“no boundary type”** means the current typed
frontend/backend vocabulary can carry the reference behavior: new RPC methods or
new model-facing DTOs are not required. **“Local extension”** means the native
UI/host/storage interface or presentation values need extending, not the backend
contract. A new service implementation is not, by itself, a need for new DTOs.
Generic JSON capacity is explicitly not credited as an implemented feature.

## A. Browser panel, lifecycle and desktop session

| ID / reference feature | Status | Exact reference implementation | Native implementation and absent/different behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| B01 — Open/close browser panel and resize alongside chat | MISSING | `script.js` desktop composition; `browser-toggle.js::BrowserToggle`; `browser-panel.js::build/setOpen/fit/resizer`; `styles.css` browser selectors | None in `qml/Main.qml` or the `CMakeLists.txt` QML list. No globe toggle, sidebar panel, empty view, draggable width, closed-panel inertness or responsive chat/browser split. | BS has `open`; no boundary type. Local panel/control and width state needed. **UI-only** shell, with live content dependent on the host. |
| B02 — User tab strip and tab management | MISSING | `browser-panel.js::addTab/newTab/select/close/render` | None; `src/frontend/host.h` is only a service seam. No title/favicon/loading tab strip, new/select/close or middle-click close. Reference manual creation may evict an older non-active tab above 12; tool creation instead refuses at capacity (T11). | BS has tab summaries; no boundary type. Local tab model, icons, guest ownership and UI operations needed. **Mixed** UI/host. |
| B03 — Address entry, search and local-page navigation | MISSING | `browser-panel.js::normalize/go/syncBar/build`; `desktop/browser.js::normalize` | No address field or navigation host. Missing HTTPS/HTTP host normalization, Google query fallback, absolute local paths, `file:`, `about:` and `data:` pages; focus-select, Enter and Escape restoration, and dimmed URL components. Reference renderer and host normalizers differ slightly, e.g. host explicitly handles `[::1]`. | HT JSON and BS URLs suffice at the boundary. Local address/control interface needed. **Mixed** UI/host. |
| B04 — Per-tab back/forward, reload/stop and external-open controls | MISSING | `browser-panel.js::build/syncBar`; `desktop/browser.js::navigate/adopt` | `src/platform/desktop.cpp::openLink` supports external links only. There is no guest navigation stack, enabled-state updates, reload-to-stop swap or browser toolbar. Reference has guest history, not a separate history-list UI. | BS does not contain `canGoBack/canGoForward`; local navigation state/actions needed, no boundary type. **Mixed**. |
| B05 — Lazy guests, readiness, failure and crash recreation | MISSING | `browser-panel.js::constructor/createView/ensure/close`; `desktop/browser.js::adopt/entry` | No guest lifecycle under `NoHost`. Lazy saved tabs do not exist; no `dom-ready`, destroyed/failed/gone transitions or recreate-on-next-attempt behavior. | BS represents states but performs none. Local guest/readiness ownership required; no boundary type. **Host-service-only** lifecycle, projected by B06. |
| B06 — Visible loading/error/retry states | MISSING | `browser-panel.js::createView/render/syncBar/build`; `styles.css` browser progress, spinner and error styles | No browser-specific spinner/progress, error host/code, retry button or crashed-page message in `qml/Main.qml`. A general disconnected notice is not this UI. | BS has loading/status, not detailed per-tab error text. Local error/toolbar presentation extension required; no boundary type. **UI-only**, fed by B05. |
| B07 — Browser keyboard shortcuts and focus lending | MISSING | `desktop/browser.js::adopt` `before-input-event`; `browser-panel.js::constructor/run/giveBack/take/handBack` | No guest focus to transfer. Native composer Escape handling is not address/new/close/reload/history shortcuts, guest DevTools, or lending keyboard focus back to chat between agent steps. | BS control and HT calls insufficient for focus commands; local host/UI interface required, no boundary type. **Mixed**. |
| B08 — Guest popup/new-tab routing and context menu | MISSING | `desktop/browser.js::adopt` window-open and context-menu handlers; `browser-panel.js::onEvent` | No equivalent in `src/platform/` or QML. Missing foreground/background tabs from guest links, separate `new-window` popup, open/copy link/image, editable cut/copy/paste/select-all, selection copy, history/reload and Inspect. | Local guest event/menu APIs needed; existing external-link helper is not enough. No boundary type. **Mixed** UI/host. |
| B09 — Agent ownership overlays and pointer visualization | MISSING | `browser-panel.js::drive/sync/point`; `browser-toggle.js` live badge; `desktop/browser.js::pointer` | No driving badge, Take Control affordance, user-control banner, live toggle, animated cursor or click ripple. Generic working ghost in `qml/Main.qml` is not browser ownership. | BS control exists; HS has no pointer event or driver UI. Local event/driver projection needed, no boundary type. **Mixed**. |
| B10 — Take Control / Hand Back without replaying the interrupted action | MISSING | `browser-panel.js::take/handBack/waitForAgent/release`; `chat.js::onHostTool/awaitHandBack/end` | `HostToolResult::Status::HandedBack` is unused behavior. No user hand-back wait, fresh snapshot instead of the old action, message-superseded wait, or turn-end release of hand-back UI. `cancelHost` only handles generic outstanding calls. | HT/BS can carry statuses and control. Local handoff/wait/driver interface and implementation needed; no boundary type. **Mixed** UI/host/orchestration. |
| B11 — Guest/app trust boundary | MISSING | `desktop/browser.js::guard/adopt/entry/install/world`; `desktop/main.js::fromApp` and browser handlers; `desktop/preload.js` | No guest host at all. `denyNetwork()` and absence of WebEngine do not implement isolated browsing. Missing dedicated guest preload/partition, Node-disabled sandboxed guests, isolated observations and host ownership/sender checks. | Existing HT/HS envelopes suffice; a real isolated host and local guest ownership API are needed. No boundary type. **Host-service-only**. |
| B12 — Browser site permission handling | MISSING | `desktop/browser.js::setup`, `ALLOWED` | No native site-permission service. Reference allows only `clipboard-sanitized-write`, `fullscreen`, `pointerLock` and denies other checked/requested permissions. These are not Ask/Auto/Full approvals or an interactive site-permission dialog. | Internal host policy, not AP and not new backend types. **Host-service-only**. |
| B13 — Persistent cookies/site state and browser session configuration | MISSING | `browser-panel.js` and `desktop/browser.js` `persist:browser`; `desktop/browser.js::setup` user agent | No browser session/profile store or guest networking. Existing preferences and library JSON do not retain site logins, storage or the configured guest user agent. | BS is only a context summary. Local browser profile/session storage needed, no boundary type. **Mixed** host/persistence. |
| B14 — Site sign-in submission observations, never verified auth | MISSING | `desktop/browser-preload.js::check`; `browser-panel.js::signedIn/snapshot`; `openghost.browser.accounts` | BS declares the fields and constant false verification, but no observation producer or saved account hints exists. Reference records hostname/time only, at most 30; it does not export password values or confirm login success. | BS already fully carries these hints; local observation event and storage required, no boundary type. **Mixed** host/persistence. |
| B15 — Restore panel layout and tab URLs/titles after reload | MISSING | `browser-panel.js::constructor/save/select`; `openghost.browser` | No browser persistence in `src/frontend/preferences.cpp`, `src/frontend/store.cpp` or `src/main.cpp`. Reference saves open/width/nonblank tabs/active index, restores lazily and generates fresh handles; it does not persist guest navigation stacks or pending actions. | BS lacks panel width and is not a save format. Local persistence record needed; no boundary type. **Persistence-only**, once panel exists. |
| B16 — Appearance follows the selected light/dark/system theme in sites | MISSING | `desktop/main.js` `nativeTheme.themeSource` and `theme:set`; `browser-panel.js`; `styles.css` browser theme variables | `src/appearance.cpp` and `qml/AppearancePage.qml` theme the native app, but there is no browser to receive the appearance setting. | Existing native appearance state can be reused; local host theme setter needed, no boundary type. **Mixed** UI/host. |
| B17 — Browser downloads to the OS downloads directory and completion toast | MISSING | `desktop/browser.js::setup/uniqueFile` `will-download`; `browser-panel.js::onEvent/notify` | No download service or toast. Reference picks a nonexisting filename by suffix, saves via the browser and notifies on completed downloads only. It has no download manager, progress/cancel list, or persistent download UI. | HT `data` can return metadata, but HS lacks unsolicited download events. Local host completion event needed, no boundary type. **Mixed** host/UI. |

## B. Browser request/response, ownership and recovery plumbing

| ID / reference feature | Status | Exact reference implementation | Native implementation and absent/different behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| H01 — Discover and publish the eleven available browser tools | MOCK ONLY | `host-tools.js::SCHEMAS/schemas`; `backend-client.js::hello` | `src/frontend/chat_service.cpp::initialize` forwards `HS.tools()`, but `NoHost` returns empty. `tests/contract.cpp::ScriptedHost` publishes one fixture schema. No native reference schema set or working registered browser capability. | HT/HS already support registration; no new types, implement schemas and a real host before advertising them. **Host-service-only**. |
| H02 — Current browser context on start/retry/steer and change notifications | MOCK ONLY | `browser-panel.js::snapshot/report`; `chat.js::sessionParams/steer`; `backend-client.js` | `ChatService::params/steer` and constructor's `browserChanged` forwarding exist, but the shipped composition always produces null and the fake ignores changes. No live empty/closed/loading/ready context producer. Null is correct for *no panel*, not parity with the desktop panel. | BS/HostContext and `Backend::browserChanged` already suffice. No new types for reporting; host implementation required. **Host-service-only**. |
| H03 — Reverse-call correlation, admission and duplicate prevention | PARTIAL | `chat.js::claim/onHostTool`; `backend-client.js` reverse-ID handling | `ChatService::reverse` verifies published tool, session, accepted turn, nonempty call ID and outstanding duplicates. It refuses host calls before `remoteId` exists instead of awaiting acceptance/recovery as JS does. On `finished`, it removes `turn.hostCalls`, so a later repeat of the same tool-call ID is not remembered for the rest of the turn; JS `turn.requests` remembers it. | HT/RequestId already suffice. No new types; need retained seen-call bookkeeping and early-call admission logic. **Host-service-only** frontend orchestration. |
| H04 — Stop/request-cancel/terminal cleanup and late-result suppression | PARTIAL | `chat.js::onHostTool/end/stop`; `backend-client.js` reverse cancellation; `browser-panel.js::cancel`; `desktop/main.js::cancelBrowser` | `ChatService` cancels injected calls on stop/end/disconnect and drops late `finished` results; tested by `hostToolsRoutedAndReleased`. No real guest cancellation or app-close browser cleanup. Native `reverseCancelled` removes the call without answering it; reference cancellation settles the reverse handler with cancelled content while connected. Native terminal cancellation returns error text as well as cancelled status; reference uses empty content. | HT already has status/reason/error. No boundary types needed; real host cancellation/teardown and orchestration parity required. **Host-service-only**. |
| H05 — Serialize browser steps across chats and pin receipt-time targets | MISSING | `browser-panel.js::run/tabsTool`; `desktop/browser.js::run` queues | `ChatService::reverse` directly invokes an injected host; `NoHost` has no queue. No cross-main/mini-chat serialization, receipt-time active-tab capture, queued cancellation barriers or predecessor-slot retention. | HT args already carry `tabId`; local queue/operation lease state needed, no boundary type. **Host-service-only**. |
| H06 — Fresh page/ref/target validation before further input | MISSING | `desktop/browser.js::revise/check/install/point/world/act`; `browser-panel.js::run` revision checks | No validator or observations. BS revisions and JSON `pageId`/`ref` capacity do not reject stale pages, disconnected refs, covered/moved targets, tab switches or navigation during pointer delay. No invalidation after partial input. | HT args/data and BS sufficient. Local observation/lease/ref state required, no boundary type. **Host-service-only**. |
| H07 — Readiness/operation/dispatch deadlines and truthful errors | MISSING | `browser-panel.js::interruptible/ensure/run`; `desktop/browser.js::timed/check/settle/navigate/run` | No browser deadlines, loading settle or browser error mapping. Reference bounds readiness at 15 s, renderer operation at 90 s, host operation at 75 s, individual calls at 12 s and navigation at 30 s. Late timed-out continuations cannot issue more input; cancellation does not undo already-issued actions. | HT status and `data.code` can represent failures. Local deadline/cancellation implementation required, no boundary type. **Host-service-only**. |
| H08 — Browser response formatter: text/images, metadata and errors | MISSING | `host-tools.js::result/read/readable/flatten`; `chat.js::onHostTool` status assignment | `HostToolResult` and `ChatService` can forward a supplied response; no native equivalent builds it. Missing refs, IDs, coverage/truncation/scroll, text-error formatting, screenshot labels and read pagination output. This is a caller response, not a tool card UI. | HT fully accommodates reference output through text/image variants and JSON `data`. No new boundary types; formatter/extraction implementation needed. **Host-service-only**. |
| H09 — Attribute downloaded files to the initiating guest/operation | MISSING | `desktop/browser.js::setup/state`; `host-tools.js::result`; `test/browser-lifecycle.test.js` download test | No producer of `{file, at, operationId}` metadata. Reference snapshots report only downloads completed for that operation; later/other calls cannot consume them. Late completion may still toast (B17). | HT `data.downloads` already fits. Local download ownership/tracking needed, no boundary type. **Host-service-only**. |
| H10 — Restoring chat display does not replay browser actions | MATCH | `chat.js::reconcile/recoverTurn/applyEvent`; `library.js::displayMessages` | `src/frontend/chat_service.cpp` recovery replays display events, not `HostToolRequest`s; `src/frontend/library.cpp::displayMessages` discards tool histories. Restored browser-looking text is not executable. This narrow safety match does not establish recoverable browser operation state. | Existing recovery/display contracts suffice. **No missing work** for this behavior. |
| H11 — Advertise supported picture/video/file presentation syntax | MISSING | `render-guide.js`; `backend-client.js::hello` | `Initialize::renderGuide` exists but `ChatService::initialize` leaves it empty. No advertised native guide for media or file diagrams, even for file diagram rendering that exists. Do not simply copy claims for media that native cannot render. | Existing `QString renderGuide`; no new types. **UI-only** capability-description work, enabled only alongside actual render support. |

## C. All eleven browser host tools

All rows below have **no native execution implementation**. Exact native seams
are `src/frontend/host.h::NoHost`, `src/frontend/chat_service.cpp::reverse`, and
`src/backend/types.h::HostToolRequest/HostToolResult`. `tests/contract.cpp` is not a
production browser. HT's JSON schema/args/data already represent every operation
below; **none requires a new frontend/backend boundary type**. Local browser
state/services are needed as described in B/H rows.

| ID / feature | Status | Exact reference implementation | Missing behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| T01 — `browser_navigate` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::act/navigate/normalize` | Address/search/local-file navigation; `back`, `forward`, `reload`; settled fresh snapshot; stop/error on failed load. | HT sufficient; local navigation host. **Host-service-only**. |
| T02 — `browser_snapshot` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::install.snapshot/state` | Viewport or full DOM heuristic text, labels, numbered refs, masked password values, same-origin iframe/open-shadow observations, unreadable iframe notices, scroll/coverage/truncation metadata. Reference is bounded (9,000/40,000 text budget; 20,000 visited elements), not a full accessibility-tree guarantee. | HT sufficient; local observation/ref engine. **Host-service-only**. |
| T03 — `browser_click` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::act/point/pointer/mouse` | Ref or page-coordinate input, optional double click, covered/moved recheck and post-click snapshot. | HT sufficient; real input host plus H06. **Host-service-only**. |
| T04 — `browser_type` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::act/press` | Ref/focused-field typing, clear-or-append, empty-text delete, optional Enter submit, navigation-safe sequencing and snapshot. | HT sufficient; real input host. **Host-service-only**. |
| T05 — `browser_select` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::install.choose/act` | Native select option matching by text/value, input/change dispatch, error on no matching option/non-select, returned selection note/snapshot. | HT sufficient; local DOM operation host. **Host-service-only**. |
| T06 — `browser_press` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::keyOf/press/act` | Key combinations and modifiers, bounded repeat count (1–20), key release and page-change barrier between repetitions. | HT sufficient; real keyboard host. **Host-service-only**. |
| T07 — `browser_scroll` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::install.reveal/act` | Ref reveal or up/down wheel by viewport share (default 0.8, clamped 0.1–10), settle and snapshot. | HT sufficient; local guest metrics/input host. **Host-service-only**. |
| T08 — `browser_screenshot` | MISSING | `host-tools.js::SCHEMAS/result`; `desktop/browser.js::screenshot/act` | Viewport/full-page capture capped at four screens, at most 1,280 px output width, JPEG quality 82, image data URL, dimensions/scale/page dimensions/truncation and coordinate guidance. No native capture; no missing *chat screenshot card* is inferred. | `HostToolResult::Image` and JSON `data` suffice. **Host-service-only**. |
| T09 — `browser_read` | MISSING | `desktop/browser.js::act` read case; `host-tools.js::read/readable/flatten/code` | Freeze source HTML with `readId`; reject missing/stale continuation; cap source at 4 Mi UTF-16 units; convert headings/lists/code/tables/links; 40,000-unit text slices with exact continuation metadata. Reference may include hidden text and excludes form controls, iframes and shadow roots. | HT sufficient; frozen read storage and HTML-to-text service, no new boundary type. **Host-service-only**; frozen reads are runtime state, not durable recovery. |
| T10 — `browser_wait` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::install.has/act` | Timed/text wait including open shadow roots, bounded 0.5–60 s, fresh snapshot, `wait_timeout` instead of fabricated success. | HT sufficient; cancellable local guest polling. **Host-service-only**. |
| T11 — `browser_tabs` | MISSING | `host-tools.js::SCHEMAS`; `browser-panel.js::tabsTool/tabsText` | List/new/switch/close, stable IDs (positional `tab` not executable targeting), new-at-12 refusal without eviction, switch snapshot and optional new-tab navigation. | BS/HT sufficient; local tab owner/controller. **Mixed** UI/host; unlike the other ten, tab management is in the reference panel. |

## D. Tool presentation, permissions and approvals

| ID / reference feature | Status | Exact reference implementation | Native implementation and absent/different behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| A01 — Ordinary browser/web tools use activity, not request/result cards | MATCH | `chat.js::applyEvent/showGhost`; `stream-view.js`; `library.js::displayMessages` | `ChatService::event` tracks tools and emits `worked`; `qml/Main.qml` shows the working ghost. Progress/results are not appended to the visible transcript or saved as tool cards. Native retains some transient detail internally, unlike JS's minimal bookkeeping. Neither reference nor native has a separate `web_search`/`web_fetch` card renderer. | Existing `ToolStarted/Progress/Completed` suffice. **No missing tool-card UI**. |
| A02 — Web/file approval cards and untrusted request presentation | PARTIAL | `approval-card.js::ApprovalCard/present`; `chat.js::onApproval` | `qml/ApprovalCard.qml`, `WindowController::approvals`, and `ChatService::reverse/approve` render kind=web/file, site/file/folder places, quote, code/diff, Allow/Deny and fallback args. However native bypasses JS's kind/effect/reveal/place allowlists and nonempty-title fallback; fallback JSON is indented rather than compact. No real browser supplies reference labels/refs. | AP already holds every field; no new types. Need frontend normalization and real request source, not client-side permission policy. **Mixed** UI/orchestration; actual policy remains backend-owned. |
| A03 — Correlated consent, live mode change and supersession | PARTIAL | `chat.js::claim/onApproval/onModeChange/steer/end`; `mode-picker.js`; `backend-client.js` | `ChatService` gates approvals by identity, queues early approvals, sends mode configuration, handles resolution/cancel/disconnect and dismisses current approvals on steering. But unlike JS `onApproval`'s `turn.queue.length` check, `reverse` does not deny a newly arriving approval just because input is already queued. No browser/hand-back flow (B10), and no real backend policy. Native correctly does not infer approval inside HS. | AP and existing steering state suffice; no new types. **Mixed** UI/orchestration, with real approval decisions a backend integration blocker. |
| A04 — Remember expanded approval details across reloads | MISSING | `approval-card.js::remembered/remember`, `openghost.approval.details` | `qml/Main.qml::approvalDetails` and `qml/ApprovalCard.qml::detailsOpen` remember only within the window. `src/frontend/preferences.cpp` does not persist the setting. This is presentation preference, never saved consent. | Local preference/store field required; no AP/boundary type. **Persistence-only**. |

## E. Links and intentional web-resource behavior

| ID / reference feature | Status | Exact reference implementation | Native implementation and absent/different behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| L01 — Safe assistant Markdown links, bare-URL labels and OS navigation | MATCH | `markdown.js::parseLink/autolink`; `link-chip.js::label/html`; `desktop/main.js::external` | `src/markdown.cpp::safeUrl/InlineParser::link/autolink/chipLabel`, `qml/InlineText.qml`, `qml/SelectArea.qml`, `WindowController::openLink`, `src/platform/desktop.cpp::openLink` form a real click-to-OS path for HTTP(S)/mailto. Unsafe Markdown destinations become plain labels. Favicons are separately L03; user/composer links are L02. OS launch success depends on installed handlers and was not exercised here. | Existing local link values/helper suffice. **No missing work** within this scope. |
| L02 — Link chips in user bubbles and composer mirror | MISSING | `chat.js::userMessage` → `LinkChip.fill`; `composer-text.js`; `script.js` mirror/watch wiring | `qml/ChatEntry.qml` uses `qml/LiveText.qml` with `TextEdit.PlainText`; composer in `qml/Main.qml` is plain text with no reference mirror. Assistant link support does not make these URLs chips/click targets. | Existing native inline link structures can be reused; no boundary type, local editor/display projection required. **UI-only**. |
| L03 — Network favicons with fallback and caching | MISSING | `link-chip.js::probe/load/watch/paint` | `qml/InlineText.qml` always draws a local `link-globe`; no DuckDuckGo icon-service requests, host/base-host fallback, 2.5 s probe, shared load cache or arrival animation. B02 separately covers actual page favicons. | No backend types. Local icon resource/cache service needed and subject to L04. **Mixed** UI/host. |
| L04 — Allow intentional frontend media networking, not provider networking | INTENTIONAL DIFFERENCE | `index.html` CSP; `link-chip.js`; `media-embed.js`; `desktop/main.js::videoInfo`; separate browser guests | Native deliberately installs `src/window.cpp::DenyAll` through `src/main.cpp::denyNetwork`, suppressing QML network resources. Reference blocks renderer connections but permits HTTP(S) images, trusted previews, favicon images and bounded host oEmbed; guests browse separately. Native external-link launches still work. Blanket network denial is stricter, not parity. | No backend type needed. A future explicit frontend-resource policy/host seam would be required; do not globally weaken protections to repair individual cards. **Mixed** host/UI policy. |

## F. Reply media and image presentation

| ID / reference feature | Status | Exact reference implementation | Native implementation and absent/different behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| M01 — Recognize image/linked-image/YouTube media blocks during streaming | MISSING | `markdown.js::mediaItems/mediaHtml/parse/render`; `stream-view.js::render/paint`; `media-embed.js::mount/arrange` | `src/markdown.h::Kind`, `src/markdown.cpp`, `qml/Block.qml` have no media kind/mount. Image syntax cannot become a media block; YouTube links remain ordinary links. Missing quiet live-image placeholder and stable card reuse while surrounding text streams/restores. | Assistant text already sufficient at boundary. **Local Markdown media item/block type and QML projection required. UI-only**. |
| M02 — Remote-image loading consent, failure fallback, captions/source links | MISSING | `media-embed.js::TRUSTED/probe/gallery` | No native gallery or policy implementation. Reference auto-probes only named preview/Wikimedia sources, holds other URLs behind a click, times out after 9 s, retains failed images as links and updates caption/source for the active image. Native blanket refusal is not the selective consent behavior. | Local media items/load state and resource service required; no boundary type. **Mixed** UI/host; L04 applies. |
| M03 — Shared image/video-poster stack interaction | MISSING | `media-slider.js::MediaSlider`; `chat.js::attachmentViews`; `media-embed.js::gallery` | No carousel in QML. Missing fitted single image, layered stack/backdrops, arrows, dots/counter, keyboard/Home/End, drag/fling, horizontal wheel/swipe, active-slide accessibility, notes/duration and reduced-motion behavior. `RichImage` for diagrams/math or a stub preview is not this component. | AT covers sent media semantics; local slide/display model needed, no boundary type. **UI-only**, with image producers elsewhere. |
| M04 — YouTube cards and thumbnail fallback | MISSING | `media-embed.js::videoId/videoWords/videoCard`; `markdown.js::mediaItems` | No native preview/play card, optional title/channel/duration parsing, hq720→mqdefault thumbnail fallback/missing state or click-through card. Reference opens the link externally; it does not embed a YouTube player in chat. | Local card/presentation type required; existing text input suffices. **Mixed** UI/thumbnail host. |
| M05 — Validated YouTube metadata host bridge | MISSING | `desktop/preload.js::videoInfo`; `desktop/main.js::videoInfo` and `media:video-info`; `media-embed.js::videoInfo` | No native oEmbed lookup. Reference validates an 11-character ID, fetches only YouTube oEmbed with a 10 s timeout, returns title/by or null, and checks IPC sender. This is not a generic web fetch API. | HS does not provide this service. Local video-info request/result/service needed; no backend DTO. **Host-service-only**. |
| M06 — Media metadata cache and shared in-flight lookups | MISSING | `media-embed.js::asked/keptInfo/keep/videoInfo`, `openghost.media.info` | No equivalent in `src/frontend/store.cpp`, preferences or media code (absent). Reference caches up to 300 successful metadata entries and shares requested promises so reopening does not re-fetch known metadata. It does not save gallery slide positions or image-consent decisions here. | Local metadata store format required; no boundary type. **Persistence-only** plus runtime cache bookkeeping. |

## G. File intake, previews and display persistence

| ID / reference feature | Status | Exact reference implementation | Native implementation and absent/different behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| F01 — Photos/files picker, bounded draft tray and removal | PARTIAL | `add-menu.js::choose`; `attachments.js::pick/add/remove/take`; `script.js::send` | `qml/Main.qml` FileDialog/tray/remove and `WindowController::pick/release` work for UTF-8 text with an available injected backend. No separate photos/videos filter/dock; disconnected preparation refuses. JS takes up to remaining 20 individually; native refuses an overlarge selection whole and caps retained payloads at 64/8 MiB. JS clears on local send admission; native keeps cards until validated acceptance. | AT exists; `AttachmentStore::Prepared` and local draft UI require richer fields, not new boundary types. **Mixed** UI/local file service. |
| F02 — File/image clipboard paste and drag/drop into the active composer | MISSING | `attachments.js::onPaste/onDragEnter/onDragOver/onDragLeave/onDrop`; `script.js::isActive`; `mini-chat.js` attachment composition | `WindowController::pasteRefused` rejects clipboard images/local URLs; `qml/Main.qml::dropRefusal` refuses external file URLs. No drop overlay, active composer/settings routing, clipboard image preparation or mini-chat attachment entry path. | AT can carry results. Local clipboard/drop-to-owned-source interface needed; no boundary type. **Mixed** UI/host. |
| F03 — Text-file decoding, size bounds and preparation failure behavior | PARTIAL | `attachment-reader.js::decodeText/looksBinary/contents/limit`; `attachments.js::loaded/paintMeta` | `src/frontend/attachments.cpp::prepare` reads regular non-symlink local files, strict UTF-8/NUL-free, ≤256 KiB; preparation is synchronous and all-or-nothing. Reference accepts BOM UTF-16, UTF-8 or Windows-1251 fallback, binary sniffing, ≤20 MB generic input, 400,000-character truncation and per-item name-only fallback. Missing async readiness/projection, encoding support, truncation and partial per-file outcomes. | AT text/truncated/file fields suffice. Local preparation/status interface must expand; no boundary type. **Mixed** UI/file service. |
| F04 — Image decoding/resizing and prepared data URLs | MISSING | `attachment-reader.js::sniffImage/readImage/dataUrl`; `attachments.js::loaded/chip` | Native `AttachmentStore` has no image decoder or preparation route. No immediate object preview or image-to-file fallback. Reference preserves supported small originals (≤2,560 px edge and ≤6,000,000 bytes) or rescales/re-encodes to WebP; these are the reference limits, not another backend's image limits. | AT already has Image/dataUrl/dimensions. Local source/preview pipeline and prepared display data needed, no boundary type. **Mixed** file host/UI. |
| F05 — Office/OpenDocument text extraction | MISSING | `attachment-reader.js::OFFICE/unzip/readOffice/sheetText` | No extractor in `src/frontend/attachments.cpp`. Missing DOCX/DOCM, PPTX, XLSX/XLSM, ODT/ODS/ODP text with sheet/slide labels and 5,000-row spreadsheet cap. Reference does not promise all legacy Office formats. | AT text/truncated suffice. No new boundary type; local extraction service needed. **Host-service-only**. |
| F06 — PDF text host: disk/byte input, bounded serial extraction and cleanup | MISSING | `attachment-reader.js::readPdf`; `desktop/preload.js::readPdf`; `desktop/main.js` `pdf:read`; `desktop/pdf.js`; `desktop/pdf.html` | No PDF service or reader. Reference serializes hidden viewer jobs, checks signature/256 MiB host cap, has 45 s opening timeout, cleans byte-source temporary files and aborts on app close; empty/scanned/password/unreadable files do not become text. Native text reader rejects `%PDF-`, not a PDF implementation. | AT Pdf/text/path/truncated suffice for output. Local PDF source/result/cancel API needed; no backend type. **Host-service-only**; reference has no OCR or general PDF preview dialog. |
| F07 — Video metadata/poster preparation and decoder fallback | MISSING | `attachment-reader.js::readVideo/poster/draw/once`; `attachments.js::FRAMED/loaded/paintFrame` | No native video probe/decoder/poster pipeline. Reference extracts duration/dimensions, samples a nonblank poster up to 768 px, times out stalled media, passes local path/metadata (not video understanding), and falls back to a file card without a frame. | AT Video/path and DisplayAttachment.video cover output; local decoder/preview service required, no boundary type. **Mixed** host/UI. |
| F08 — Unsupported-file metadata/path fallback and genuine local paths | MISSING | `desktop/preload.js::pathOf`; `attachment-reader.js::read/contents`; `attachments.js::blind/paintMeta`; `chat.js::attachmentOf`; `backend-client.js::hello` | Native rejects non-text selection rather than retaining name/size/path with a name-only warning. Even accepted text has no `Attachment.path`; `ChatService::initialize` leaves `localPaths=false`. No file-host path bridge beyond reading user-selected URLs. That false advertisement is honest, but not desktop-reference parity. | AT path/File and Initialize.localPaths already suffice. Need actual user-selected-path ownership and fallback semantics, no boundary type. **Mixed** UI/file host. |
| F09 — Per-attachment notes, edit/cancel and sent-note presentation | MISSING | `attachments.js::openNote/onNoteKey/onNoteClosing/paintMeta`; `chat.js::fileCard/slim/attachmentOf` | No note controls in native composer; `AttachmentStore` cannot update a draft note. Semantic `note` is saved if supplied, but `src/presentation.h::Attachment` and `WindowController::sync` omit it. | AT already supports notes. Local draft setter and presentation field required; no boundary type. **UI-only** with local state. |
| F10 — Long-paste cards and reversible unpaste | MISSING | `attachments.js::PASTE/addText/unpaste/onPaste`; `composer-text.js::place`; `chat.js::slim/fileCard` | Native composer pastes ordinary text, not >50-line/>3,000-character cards with first-line preview, line/size label, note and undoable return-to-editor. No Ctrl+Shift+V bypass distinction for this conversion. | AT has text input and DisplayAttachment.pasted. Need local pasted-source/draft state and UI projection, no boundary type. **UI-only**. |
| F11 — Sent generic file cards and preparation/error metadata | PARTIAL | `chat.js::attachmentViews/fileCard`; `file-kinds.js`; `attachments.js::paintMeta` | `qml/ChatEntry.qml`, `src/filekinds.cpp` render names/type icons/sizes; selection supports those labels. Notes, pasted preview/counts, video duration and per-item unreadable/name-only state are absent. Additionally each card calls the always-unavailable `previewState`; its error label is not gated on `picture`, so even non-image cards can show the unavailable preview message. | AT has most semantic fields; native `src/presentation.h::Attachment` only has name/mime/size. **Local display type/projection extension required**, no boundary type. **UI-only**. |
| F12 — Sent image/video-poster previews rather than inert preview controls | MOCK ONLY | `chat.js::attachmentViews`; `media-slider.js`; `chat.js::promptOf` | `qml/ChatEntry.qml` has a Preview button/`RichImage` branch, but `WindowController::preview` only emits a signal, `previewState` always refuses and `previewImage` is empty. `sync` labels all attachments text/plain, so ordinary restored image entries cannot even enter the picture branch. Reference shows prepared/saved media directly as a stack, not idle-only backend fetches. | AT already carries previews; extend native display type and decoding/cache API. No new backend preview/download method is warranted. **Mixed** UI/local image host. |
| F13 — Full input is separate from display cache and cannot be resent from preview | MATCH | `chat.js::inputOf/slim`; `library.js::displayAttachments/displayMessages` | `src/frontend/attachments.cpp` owns full payloads behind tokens; `ChatService::entries/attachmentObject` and `Library::displayMessages` project display-only data. Resolving an unknown/released token refuses; cached paths/text payloads are not reconstructed as input. Actual media production is separately missing. | AT already explicitly separates input/display. **No missing work** for this boundary invariant. |
| F14 — Restore saved/legacy image, video and pasted previews | PARTIAL | `library.js::displayAttachments`; `chat.js::promptOf/entryView/attachmentViews`; `test/legacy-display.test.js` | `src/frontend/library.cpp` preserves permitted image/dataUrl/legacy image_url, video poster/duration, pasted metadata and notes; `ChatService::attachmentOf` retains them. But `WindowController::sync` discards them when making QML entries. `displayOf` also cannot create image URLs/video posters from a new prepared send. Disk retention is not visible restoration. | AT sufficient; local presentation/prepared-display extension required, no boundary type. **Mixed** display/preparation; existing allowlist storage should be reused rather than replaced. |

## H. File settings, desktop folders and generated-file descriptions

| ID / reference feature | Status | Exact reference implementation | Native implementation and absent/different behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| S01 — Settings → General pinned-file picker/drop/list/remove | MOCK ONLY | `settings-general.js::GeneralSettings/add/remove/meta/row`; `user-context.js::add` | `qml/GeneralPage.qml` has controls and rows, but `src/offline_services.h::GeneralPreview` always returns an empty list, refuses add/remove, and supplies no thumbnails/tips. No populated previews or re-add/replace behavior. Even the dormant row template estimates text tokens instead of JS's character count and omits the path-only metadata label. General instructions themselves work and are not a missing file feature. | UC already holds context files. Local prepared-file/settings projection/service extension required; no boundary type. **Mixed** UI/file host. |
| S02 — Persist bounded pinned snapshots and include them each start/retry | MISSING | `user-context.js::load/add/drop/forBackend`, keys `context` and `context/<id>`; `chat.js::sessionParams` | `src/frontend/preferences.cpp::save` expressly refuses nonempty `userContext.files`; no payload store. `ChatService::params` can forward UC, but only instructions can be saved. Missing 20-file/200,000-text-unit total, replacement by path or name/size, payload deletion, load validation and snapshots rather than live watchers. | UC sufficient at boundary; local metadata/payload persistence records and limit bookkeeping needed. **Mixed** preparation/persistence, not new backend context assembly. |
| W01 — Select/create a project folder and choose it for the draft | PARTIAL | `desktop/main.js` `folder:pick`; `desktop/preload.js::pickFolder`; `library.js::pick/folder`; `folder-pill.js::pick`; `script.js` | `ChatService::addFolder/newChat`, `Library::addFolder/persist` and sidebar grouping exist, but `qml/Main.qml::onFolderWanted` only displays “Folder management is not connected in this UI shell.” No folder chooser or reference composer folder pill; `WindowController` does not expose addFolder. | Existing folder values and SessionParams.cwd suffice. Local picker/result and UI facade needed, no boundary type. **Mixed** UI/desktop host. |
| W02 — Default per-chat workspace path without a project | PARTIAL | `desktop/main.js::CHATS` and `folder:chats`; `library.js::load/create/space/cwdOf`; `chat.js::sessionParams` | `src/frontend/library.cpp::create/cwdOf` supports a supplied homePath and space names. `WindowController` constructs Library without a homePath, so home-chat cwd is empty; there is no `~/OpenGhost/Chats` host wiring. This path is separate from display-cache storage and does not imply frontend creation of every workspace directory. | SessionParams.cwd and Library homePath already suffice. Local platform folder service/composition needed, no boundary type. **Host-service-only**. |
| W03 — Release only empty owned home workspaces after deletion | MISSING | `desktop/main.js::inChats/release`, `folder:release`; `desktop/preload.js::releaseFolder`; `library.js::remove` | Native `src/frontend/library.cpp::remove` removes display records/cache only. No bounded home-workspace `rmdir` service after acknowledged deletion. Reference leaves nonempty generated files and project folders alone. | Existing cwd/path values sufficient; local release service needed, no boundary type. **Host-service-only**. |
| W04 — Exposed folder-reveal desktop bridge | MISSING | `desktop/preload.js::revealFolder`; `desktop/main.js` `folder:reveal` → `shell.openPath` | No local-folder open helper in `src/platform/desktop.cpp`; `openLink` refuses file schemes. **Reference bridge exists but has no renderer caller in this tree**: this is a host-surface gap, not a missing visible “Open generated file” button. | Local folder-open interface needed, no backend type. **Host-service-only**, low priority until an existing reference caller requires it. |
| G01 — Display generated/described file trees and sizes | MATCH | `markdown.js` files/folder diagram recognition; `diagram.js::parseFiles/filesScene/fvIcon`; `render-guide.js` files example | `src/markdown.cpp`, `src/diagram_core.cpp` kind dispatch, `src/diagram_page.cpp::filesKind/filesScene`, `src/diagram_paint.cpp`, `qml/DiagramBlock.qml` render names/path/tree/size/date/count metadata as diagrams. They are not filesystem listings or file-host download handles on either side. | Existing diagram display types suffice. **No missing file-host action** is implied by this diagram. |
| G02 — File/document summary card in facts/passport syntax | MATCH | `diagram.js::parseFacts/factsScene`; `render-guide.js` facts/file syntax | `src/diagram_subjects.cpp::parseFacts/factsScene/factsKind`, `src/diagram_core.cpp`, `qml/DiagramBlock.qml` implement the document-name/metadata header and fact rows. This is presentation supplied by text, not a verified generated artifact or clickable file preview. | Existing diagram display types suffice. **No missing work** within this scope. |

## What the reference does not implement

These negative findings prevent an audit from manufacturing port requirements:

- **No generic web-search/fetch frontend implementation or special cards.** The
  browser's search fallback is Google navigation; `browser_read` reads its guest.
  There are no shipped `web_search`, `web_fetch` or `fetch_url` host schemas or
  corresponding frontend settings. Normal tools use generic events and approvals.
- **No regular tool transcript or browser request/response inspector.** `tool.*`
  notifications do not render request args, result text, progress or screenshot
  galleries. Screenshot data is returned to the backend through `host.tool`.
- **No general file-host server, remote upload protocol, artifact ID download API,
  generated-file shelf, upload progress service, or arbitrary file-preview window.**
  Browser downloads (B17/H09), user-selected inputs (F rows), and descriptive
  files/facts diagrams (G rows) are distinct. A Markdown HTTP(S) download link is
  an ordinary external link. `file:` Markdown links are not allowed.
- **No chat audio/video player.** Local video attachments have posters/metadata;
  YouTube cards open externally. Unsupported audio and other files can fall back
  to metadata/path. Embedded web pages can of course provide their own media.
- **No full browsing-history/bookmark manager or durable action recovery.** Guest
  back/forward exists, but only URL/title tabs and panel layout are saved. Page
  IDs, DOM refs, frozen reads, jobs, drivers, waiters and download attribution are
  transient. There is no “restore and re-execute the last browser action.”
- **No dedicated browser/web/file-host settings page** for search engines, proxy,
  downloads, cookie clearing or permissions. Relevant saved state is panel layout,
  account hints, site partition, theme, approval details, media metadata and pinned
  files. Several limits/defaults are constants, not user settings.
- **No verified browser login, credential export, PDF OCR/password unlock or live
  pinned-file synchronization.** A snapshot can label a file input and a user
  can interact with a site, but no dedicated browser file-upload schema exists.
- `desktop/preload.js::revealFolder` is an exposed service, not evidence of a
  reachable generated-file action. Conversely the browser is actually constructed
  and usable without a backend: lack of agent integration does not explain away
  its missing native manual UI.

## Highest-priority gaps

1. **A real frontend-owned browser host and panel** (B01–B13, T01–T11). No native
   surface can navigate, retain site state, take screenshots, read a page or act
   on a page today. Advertising the existing types as browser support would be
   misleading.
2. **Ownership and safety before enabling any automation** (B10–B12, H03–H07).
   In particular, retain tool-call IDs after settlement and handle early calls;
   then implement global serialization, stable targets, page/ref invalidation,
   late-continuation barriers and hand-back snapshots. A generic `cancel()` method
   is not proof a browser action stopped.
3. **Complete the existing file-preview path** (F04–F08, F11–F14). Fixing only an
   image widget will not help while preparation is text-only, `sync` throws away
   preview data, and the preview facade always refuses. Cached previews already
   survive the semantic store and should not require a backend fetch API.
4. **Reply media and selective resource loading** (L03–L04, M01–M06). Missing
   Markdown media recognition, galleries and YouTube cards are visible frontend
   deficits independent of agent implementation. Network policy must be resolved
   explicitly, not bypassed piecemeal through unrestricted image sources.
5. **Pinned files and user-selected file/folder ergonomics** (F01–F03, F09–F10,
   S01–S02, W01–W03). These are largely frontend/desktop tasks. The settings shell,
   picker labels and folder buttons currently promise substantially more than
   the connected native services provide.
6. **Browser download ownership and truthful reporting** (B17/H09), followed by
   browser state/settings persistence. Do not turn a late download into another
   call's result or claim browser state is protected by a chat display lock.

## Recommended port order — future work only

No implementation is authorized or performed by this audit.

| Order | Scope | Can be developed without an agent backend? | Completion gate |
| --- | --- | --- | --- |
| 1 | Keep truthful absence/refusal; correct H03/H04 admission/dedup/cancellation using the existing typed seam; define the local host/UI boundaries | Yes, deterministic contract tests | No fake published capabilities or duplicate/late dispatch. No new transport required. |
| 2 | Repair file display projection and implement local image/text/PDF/Office/video preparation, notes/paste/drop and General pinned storage | Yes; fixture files and native host services suffice | Picker → owned payload → preview/cache restore tested for success, unreadable inputs, limits, cancellation/teardown and save failure. Sending to an actual agent remains blocked. |
| 3 | Implement media blocks/slider/cards and narrowly scoped icon/thumbnail/oEmbed services, after deciding resource policy | Yes, using local deterministic fixtures | Streaming/restored views, consent-before-fetch for untrusted images, failed-load fallbacks and metadata cache. Do not add players/upload APIs absent from the reference. |
| 4 | Choose an approved browser-host technology/trust model, then manual panel, tabs, address/history, lifecycle, permission rules and site profile | Yes with a **real browser host**, not with current NoHost | Real guest readiness/crash/navigation/focus/isolation checks. This cannot be achieved with a drawing-only panel. |
| 5 | Introduce tab ownership and snapshot/read/screenshot/wait; then enable navigation, tab mutations and input only behind targeting/cancellation barriers; add Take Control/Hand Back and download attribution | Can test locally without an agent, but needs real host | Reference-style local-page lifecycle tests, stale/covered/moved-target refusal, queued cancellation and late completion. A snapshot's IDs must bind subsequent input. |
| 6 | Persist browser layout/account hints, apply browser theme; finish folder services and approval-details persistence | Yes, after owners/services exist | Reload restores only intended state; fresh guest/page identities; no action replay, no cookie/credential leakage into display history. |
| 7 | Only under a separately approved integration task, connect the semantic backend boundary and advertise supported schemas/guide/localPaths | No, requires real backend **and** completed host for browser flows | Actual discovery → named-turn approval → host call → response → terminal/recovery behavior, including real image/file support. Never infer success from the fake demo. |

## Blockers and ownership boundaries

- **Browser host selection is a project-scope blocker.** The current native target
  excludes Electron/Chromium/WebEngine and contains no alternative browser host.
  Reference DOM/CDP algorithms cannot execute in Qt Quick alone. Porting their
  semantics requires an explicitly approved host architecture, not quietly
  adding a forbidden engine or pretending an external OS browser exposes the
  same automation/partition/hand-back behavior.
- **Current UI resource policy blocks reference media networking.** A deliberate
  frontend-host resource policy is needed for favicons, permitted remote images,
  thumbnails and oEmbed. This is not provider/model networking and is not a
  reason to connect an agent backend.
- **File/PDF/media host services are absent.** UI and payload contracts can be
  improved independently, but actual decoding/extraction/metadata, clipboard
  bytes, local paths and downloads cannot be completed by UI mocks. They need
  explicit service ownership, bounds and teardown. A real agent does not supply
  the reference's desktop PDF reader or video poster generator.
- **Actual agent workflows remain blocked by the deliberately disconnected
  backend.** Policy evaluation, model calls, general web search/fetch execution,
  agent history, provider capability decisions and accepted turn recovery belong
  to the real backend. This audit neither chooses one nor defines a transport.
- **Most boundary types are already adequate.** Browser operation args/results
  use open JSON plus text/image variants; AT/UC cover reference file payloads.
  Necessary additions are chiefly local presentation/state/service interfaces:
  browser controls/guest events, draft/media data, preview decoding, PDF/video
  services and persistence records. Do not invent a new backend file-host API to
  compensate for a dropped native display field.
- **Two recovery domains must stay separate.** Browser cookies/layout/account
  hints belong to the frontend host; chat snapshots/reconciliation are not a
  browser action journal. Display-cache restoration must never revive tool input,
  refs, downloads or authorization. Neither side currently promises exactly-once
  browser effects across a crash.

## Evidence and focused validation

Source tracing covered the actual reference entry/bridge paths, the whole browser
panel/host/preload and tool schemas, attachment reader/tray, PDF host, media/link
modules, relevant Markdown/stream/diagram dispatch, General settings/context,
chat claims/events/handoff/input/display/recovery, library projection and their
focused tests. Native tracing followed composition through service, typed values,
QML adapter/models, rendering, platform helpers and persistence—not comments or
contract declarations alone. Reference documentation was supporting context;
reachable implementation determined findings (including manual-tab eviction,
non-rendered screenshot responses, and the unused folder-reveal bridge).

Commands run from the repository root:

```sh
node --test \
  reference/openghost/test/browser-lifecycle.test.js \
  reference/openghost/test/cancellation.test.js \
  reference/openghost/test/turn-identity.test.js \
  reference/openghost/test/legacy-display.test.js

cmake --build build-release --target native_contract_test -j2
build-release/native_contract_test \
  approvalsToolsAndHostRefusal reverseOwnershipAndMockHost \
  hostToolsRoutedAndReleased attachmentsAreOwnedPayloads \
  storeAndLibraryPersistence preferencesRoundTripAndFailures

git diff --check
```

Results: **40/40 reference tests passed**; the focused native target built and
**6/6 selected native test functions passed** (8 QtTest passes including setup
and cleanup). These are headless contract/stub checks, not real-browser or live
backend tests. No app launch, Electron E2E, OS link launch, provider/network call,
full build/test suite or visual-parity run was performed. Matrix count/path and
Markdown fence checks were also performed for this document. Only this audit is
staged/committed; no push.
