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

**Update — browser foundation slice.** The desktop build now has the reference's
built-in browser panel on Qt WebEngine guests (`OPENGHOST_BROWSER`, default ON,
chosen because the reference renders its pages in embedded Chromium webviews):
panel/toggle/width, tabs, address and navigation controls, guest lifecycle,
loading/failure/crash states, saved layout, focus lending, Take control / Hand
back state and turn-end release, and the live `host.browser` snapshot. Rows moved
by that work are marked in the matrix and listed under
[this pass](#browser-foundation-slice). **It still implements none of the 11
reference browser tools:** no tool is published and the snapshot says
`available: false`. The rest of this audit's original findings stand. File
support is a real but restricted UTF-8 text-attachment path. Image previews and
pinned-file settings are stubs; reply media and the browser download service are
absent.

### Summary counts

Each matrix row is one audited behavior. Cross-cutting lifecycle and individual
tools are counted separately; the eleven tools are individually identified.
Reference absences listed later are not counted as missing features.

| Status | Features |
| --- | ---: |
| MATCH | 12 |
| PARTIAL | 19 |
| MOCK ONLY | 3 |
| MISSING | 40 |
| INTENTIONAL DIFFERENCE | 1 |
| **Total** | **75** |

The original audit counted 6 / 10 / 4 / 54 / 1; the browser foundation slice
moved B01–B04, B06 and B15 to MATCH, B05, B07, B09–B13 and H07 from MISSING to
PARTIAL, and H02 from MOCK ONLY to PARTIAL.

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

1. `src/main.cpp` creates either no `Backend` or `FakeBackend`. With
   `OPENGHOST_BROWSER` it initializes Qt WebEngine and creates an
   `openghost::Browser` host (`src/frontend/browser.*`), passed through
   `WindowController` to `ChatService` in both modes; an OFF build passes none,
   so `ChatService` selects `NoHost`.
2. `Browser::tools()` is empty, like `NoHost`'s: `ChatService::initialize`
   publishes no host tools and `reverse` refuses host calls before `run`.
   `Browser::browser()` supplies the panel's snapshot (`available: false`) to
   `params`, steering and `browserChanged`; `NoHost` supplies null.
   `HostServices::turnEnded` is called on every turn end and disconnect.
3. `qml/Main.qml` loads `qml/BrowserPanel.qml` (desktop build only), whose
   `qml/BrowserGuest.qml` WebEngine views report page events to `Browser` and
   carry out its `act` commands. There is still no PDF extractor or media service.
   `FakeBackend` ignores browser context and refuses non-text attachments and
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
| B01 — Open/close browser panel and resize alongside chat | MATCH | `script.js` desktop composition; `browser-toggle.js::BrowserToggle`; `browser-panel.js::build/setOpen/fit/resizer`; `styles.css` browser selectors | `qml/Main.qml` globe toggle (`GlobeIcon` in `src/icon.cpp`: meridian spin 170/19, open fill 170/24, live dot), `browserLoader`, `chatRight`; `qml/BrowserPanel.qml` card, `.97` closed scale and inert (`enabled: open`) state, grip; `Browser::setOpen/fit/resize`. The chat card makes room (16 px + width, 600 ms motion, none while dragging); width is 44 % of the room, ≥360, leaving the chat 400, and the dragged width is saved. Only the desktop build (`OPENGHOST_BROWSER`, default ON) has it, as only the reference's desktop build does. Pixel parity not certified; stage corners are drawn over, not clipped. | BS `open`; local panel state in `Browser`, no boundary type. Tested: `native_browser_test::panelOpenCloseAndWidth`, both UI smokes. |
| B02 — User tab strip and tab management | MATCH | `browser-panel.js::addTab/newTab/select/close/render` | `qml/BrowserPanel.qml` strip over `BrowserTabs` (`src/frontend/browser.h`): title/host label, URL tooltip, favicon (engine `image://favicon`, globe fallback) or spinner, active/hover fills, rising entrance, New tab, select, close button and middle-click close; `Browser::addTab` evicts the oldest other inactive tab above 12 on manual creation. Tool creation's refusal at capacity is T11. Guest-initiated tabs are B08. | BS tab summaries; local `BrowserTabs` model, no boundary type. Tested: `tabsNavigationAndLoading`, `tabLimitAndLazyRestore`, UI smoke New tab/middle click. |
| B03 — Address entry, search and local-page navigation | MATCH | `browser-panel.js::normalize/go/syncBar/build`; `desktop/browser.js::normalize` | `Browser::normalize` ports the panel's rules (schemes kept, absolute and drive paths to `file:`, local hosts to `http:`, host names to `https:`, else Google `q=` with `encodeURIComponent` escaping); `Browser::go` loads the live guest or creates one. `qml/BrowserPanel.qml` address: select-all on focus, Enter navigates and gives up focus, Escape restores, placeholder, and `Browser::urlParts`' dimmed scheme / host / dimmed rest at rest. The host-side `desktop/browser.js::normalize` (e.g. `[::1]`) serves `browser_navigate` and lands with T01. | HT/BS URLs; no boundary type. Tested: `normalizesLikeTheReference`, UI smoke typed `data:` page. |
| B04 — Per-tab back/forward, reload/stop and external-open controls | MATCH | `browser-panel.js::build/syncBar`; `desktop/browser.js::navigate/adopt` | Bar buttons over `Browser::back/forward/reloadOrStop` → `act` → the guest's own history; enabled from the guest's `canGoBack/canGoForward` once ready; reload swaps to stop while loading; external open uses `WindowController::openExternal` (OS handler, HTTP(S)/mailto only, as before). No history-list UI, as in the reference. | Local navigation state (`Tab::back/forward`), no boundary type. Tested: `tabsNavigationAndLoading`, UI smoke Back. |
| B05 — Lazy guests, readiness, failure and crash recreation | PARTIAL | `browser-panel.js::constructor/createView/ensure/close`; `desktop/browser.js::adopt/entry` | `Browser` keeps restored tabs lazy (no guest) until shown; `createView` starts a guest (`qml/BrowserGuest.qml`) with a 15 s readiness deadline; first finished document = `dom-ready`; a non-aborted main-frame failure or the deadline fails a pending readiness (`failed` stays, as in the reference); a renderer exit (`renderProcessTerminated`) drops the guest (`gone`, crash message) and Try again creates a new incarnation. Late events from replaced guests are ignored. Guests are only created on `https?/file/about/data` (guard()). Missing: `ensure()`'s recreate-before-a-tool-step, which belongs to the tool path. | BS states; local guest ownership, no boundary type. Tested: `tabsNavigationAndLoading`, `errorsCrashesAndRetry`, `readinessDeadlineAndGuard`; UI smoke kills the real renderer and recreates it. |
| B06 — Visible loading/error/retry states | MATCH | `browser-panel.js::createView/render/syncBar/build`; `styles.css` browser progress, spinner and error styles | `qml/BrowserPanel.qml`: sweeping 2 px progress band and tab spinner while loading, empty state, failure card (`host · error`, Chromium's `ERR_*` text with Qt's `net::` prefix removed, Try again) and the crash message. | Local presentation; no boundary type. Tested: `errorsCrashesAndRetry`; UI smoke refused port, retry and crash. |
| B07 — Browser keyboard shortcuts and focus lending | PARTIAL | `desktop/browser.js::adopt` `before-input-event`; `browser-panel.js::constructor/run/giveBack/take/handBack` | Focus lending is ported: `BrowserFocus` (`src/browserfocus.*`) takes the keyboard back from a page on any press outside the panel (yieldKeys); driving blurs the page; Take control focuses it; Hand back blurs it and records the lent page (`Browser::lent`). Missing: the guest shortcuts (F5/Ctrl+R, F12/Ctrl+Shift+I DevTools, Alt+arrows, Ctrl+L/T/W) and `giveBack` after a tool step. | Local focus interface, no boundary type. Tested: `takeControlAndHandBack`; UI smoke click-in/click-out, drive, take, hand back. |
| B08 — Guest popup/new-tab routing and context menu | MISSING | `desktop/browser.js::adopt` window-open and context-menu handlers; `browser-panel.js::onEvent` | No equivalent in `src/platform/` or QML. Missing foreground/background tabs from guest links, separate `new-window` popup, open/copy link/image, editable cut/copy/paste/select-all, selection copy, history/reload and Inspect. The WebEngine guests suppress the engine's own page menu and ignore new-window requests rather than substitute non-reference behavior. | Local guest event/menu APIs needed; existing external-link helper is not enough. No boundary type. **Mixed** UI/host. |
| B09 — Agent ownership overlays and pointer visualization | PARTIAL | `browser-panel.js::drive/sync/point`; `browser-toggle.js` live badge; `desktop/browser.js::pointer` | `qml/BrowserPanel.qml` driving ring/badge (input-blocking), hover-revealed Take control, user banner with Hand back; the toggle's pulsing live dot; all from `Browser::drive` state. Nothing drives yet without host tools. Missing: the animated cursor and click ripple (they need the tools' pointer events). | BS control; local driver state, no boundary type. Tested: `takeControlAndHandBack`, UI smoke overlays. |
| B10 — Take Control / Hand Back without replaying the interrupted action | PARTIAL | `browser-panel.js::take/handBack/waitForAgent/release`; `chat.js::onHostTool/awaitHandBack/end` | Panel side ported: `Browser::take/handBack/waitForAgent/release`, the last driver ending releases waiters and returns control to the agent, and `ChatService` now calls `HostServices::turnEnded` on every turn end and disconnect so a chat's hold ends with its turn (`chat.js::end` → `drive(conv, false)`). Missing (needs host tools): `onHostTool`'s claim/drive, `awaitHandBack` (back/message/abort), the fresh snapshot instead of the interrupted action, and the `HandedBack` status. | HT/BS statuses; local handoff interface, no boundary type. Tested: `takeControlAndHandBack`, `turnEndReleasesOnlyItsChat`, `chatTurnsReportAndReleaseTheBrowser`. |
| B11 — Guest/app trust boundary | PARTIAL | `desktop/browser.js::guard/adopt/entry/install/world`; `desktop/main.js::fromApp` and browser handlers; `desktop/preload.js` | Guests are separate Chromium renderer processes with no web channel, app objects or Node; their profile is separate from the app; creation is limited to the guard()'s schemes; they only report events to `Browser` and execute its `act`. Missing: the guest preload (`browser-preload.js`) and isolated observation world, which come with the tools and B14; there is no host-tool bridge yet to sender-check. | Existing envelopes suffice. No boundary type. **Host-service-only**. |
| B12 — Browser site permission handling | PARTIAL | `desktop/browser.js::setup`, `ALLOWED` | `qml/BrowserGuest.qml` grants pointer lock (`MouseLock`) and fullscreen requests and denies every other permission request. Differences: Qt has no separate sanitized-clipboard-write permission or synchronous permission-check handler; not yet covered by a test. | Internal host policy, not AP and not new backend types. |
| B13 — Persistent cookies/site state and browser session configuration | PARTIAL | `browser-panel.js` and `desktop/browser.js` `persist:browser`; `desktop/browser.js::setup` user agent | `qml/BrowserPanel.qml` builds one persistent WebEngine profile (`storageName: browser`) under `AppDataLocation/browser` (smoke runs: a temporary directory) with the reference's plain Chrome user agent on the engine's Chromium version. Not yet verified across an application restart. | Local browser profile; no boundary type. |
| B14 — Site sign-in submission observations, never verified auth | MISSING | `desktop/browser-preload.js::check`; `browser-panel.js::signedIn/snapshot`; `openghost.browser.accounts` | BS declares the fields and constant false verification, but no observation producer or saved account hints exists. Reference records hostname/time only, at most 30; it does not export password values or confirm login success. The panel's snapshot always carries an empty `signedIn` list and `signedInVerified=false`. | BS already fully carries these hints; local observation event and storage required, no boundary type. **Mixed** host/persistence. |
| B15 — Restore panel layout and tab URLs/titles after reload | MATCH | `browser-panel.js::constructor/save/select`; `openghost.browser` | `Browser::save/load` → `browser.json` beside the preferences (atomic `QSaveFile`): open, width, non-blank tabs' URL/title and active index. Restore is lazy (only the shown active tab gets a guest), with fresh handles; unreadable data falls back to an empty closed panel; navigation stacks and pending actions are not saved. | Local persistence record; no boundary type. Tested: `tabLimitAndLazyRestore`. |
| B16 — Appearance follows the selected light/dark/system theme in sites | MISSING | `desktop/main.js` `nativeTheme.themeSource` and `theme:set`; `browser-panel.js`; `styles.css` browser theme variables | `src/appearance.cpp` and `qml/AppearancePage.qml` theme the native app, but there is no browser to receive the appearance setting. No theme choice is passed to the guests. | Existing native appearance state can be reused; local host theme setter needed, no boundary type. **Mixed** UI/host. |
| B17 — Browser downloads to the OS downloads directory and completion toast | MISSING | `desktop/browser.js::setup/uniqueFile` `will-download`; `browser-panel.js::onEvent/notify` | No download service or toast. Reference picks a nonexisting filename by suffix, saves via the browser and notifies on completed downloads only. It has no download manager, progress/cancel list, or persistent download UI. The guests accept no download, so WebEngine cancels it; nothing is saved. | HT `data` can return metadata, but HS lacks unsolicited download events. Local host completion event needed, no boundary type. **Mixed** host/UI. |

## B. Browser request/response, ownership and recovery plumbing

| ID / reference feature | Status | Exact reference implementation | Native implementation and absent/different behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| H01 — Discover and publish the eleven available browser tools | MOCK ONLY | `host-tools.js::SCHEMAS/schemas`; `backend-client.js::hello` | `src/frontend/chat_service.cpp::initialize` forwards `HS.tools()`, but `NoHost` returns empty. `tests/contract.cpp::ScriptedHost` publishes one fixture schema. No native reference schema set or working registered browser capability. | HT/HS already support registration; no new types, implement schemas and a real host before advertising them. **Host-service-only**. |
| H02 — Current browser context on start/retry/steer and change notifications | PARTIAL | `browser-panel.js::snapshot/report`; `chat.js::sessionParams/steer`; `backend-client.js` | The desktop build's `Browser` is the composed `HostServices`: `ChatService::params/steer` send its explicit snapshot (also when empty or closed: tabs with stable IDs, state, loading, revision, title, URL, active; control; `signedInVerified=false`) and `browserChanged` reports deduplicated changes. Difference: `available` is false and status `unavailable` until browser tools are published (the reference's desktop snapshot is available); sign-in hints are B14. The fake accepts the snapshot as ignored context. OFF builds keep the null browser. | BS/HostContext and `Backend::browserChanged` suffice; no new types. Tested: `panelOpenCloseAndWidth`, `chatTurnsReportAndReleaseTheBrowser`, fake UI smoke sends with a panel. |
| H03 — Reverse-call correlation, admission and duplicate prevention | PARTIAL | `chat.js::claim/onHostTool`; `backend-client.js` reverse-ID handling | `ChatService::reverse` verifies published tool, session, accepted turn, nonempty call ID and outstanding duplicates. It refuses host calls before `remoteId` exists instead of awaiting acceptance/recovery as JS does. On `finished`, it removes `turn.hostCalls`, so a later repeat of the same tool-call ID is not remembered for the rest of the turn; JS `turn.requests` remembers it. | HT/RequestId already suffice. No new types; need retained seen-call bookkeeping and early-call admission logic. **Host-service-only** frontend orchestration. |
| H04 — Stop/request-cancel/terminal cleanup and late-result suppression | PARTIAL | `chat.js::onHostTool/end/stop`; `backend-client.js` reverse cancellation; `browser-panel.js::cancel`; `desktop/main.js::cancelBrowser` | `ChatService` cancels injected calls on stop/end/disconnect and drops late `finished` results; tested by `hostToolsRoutedAndReleased`. No real guest cancellation or app-close browser cleanup. Native `reverseCancelled` removes the call without answering it; reference cancellation settles the reverse handler with cancelled content while connected. Native terminal cancellation returns error text as well as cancelled status; reference uses empty content. The browser host now also hears `turnEnded`, releasing that chat's driving and hand-back waiters (B10). | HT already has status/reason/error. No boundary types needed; real host cancellation/teardown and orchestration parity required. **Host-service-only**. |
| H05 — Serialize browser steps across chats and pin receipt-time targets | MISSING | `browser-panel.js::run/tabsTool`; `desktop/browser.js::run` queues | `ChatService::reverse` directly invokes an injected host; `NoHost` has no queue. No cross-main/mini-chat serialization, receipt-time active-tab capture, queued cancellation barriers or predecessor-slot retention. | HT args already carry `tabId`; local queue/operation lease state needed, no boundary type. **Host-service-only**. |
| H06 — Fresh page/ref/target validation before further input | MISSING | `desktop/browser.js::revise/check/install/point/world/act`; `browser-panel.js::run` revision checks | No validator or observations. BS revisions and JSON `pageId`/`ref` capacity do not reject stale pages, disconnected refs, covered/moved targets, tab switches or navigation during pointer delay. No invalidation after partial input. | HT args/data and BS sufficient. Local observation/lease/ref state required, no boundary type. **Host-service-only**. |
| H07 — Readiness/operation/dispatch deadlines and truthful errors | PARTIAL | `browser-panel.js::interruptible/ensure/run`; `desktop/browser.js::timed/check/settle/navigate/run` | The 15 s guest readiness deadline and truthful navigation/crash failures exist (B05/B06). Missing with the tools: the 90 s renderer and 75 s host operation, 12 s call and 30 s navigation deadlines, loading settle and the tool error codes. | HT status and `data.code` can represent failures. Local deadline implementation, no boundary type. |
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
| T08 — `browser_screenshot` | MISSING | `host-tools.js::SCHEMAS/result`; `desktop/browser.js::screenshot/act` | Viewport/full-page capture capped at four screens, at most 1,280 px output width, JPEG quality 82, image data URL, dimensions/scale/page dimensions/truncation and coordinate guidance. No native capture; no missing *chat screenshot card* is inferred. The panel presents live guest frames (B01/B06); no capture/encoding for the tool exists. | `HostToolResult::Image` and JSON `data` suffice. **Host-service-only**. |
| T09 — `browser_read` | MISSING | `desktop/browser.js::act` read case; `host-tools.js::read/readable/flatten/code` | Freeze source HTML with `readId`; reject missing/stale continuation; cap source at 4 Mi UTF-16 units; convert headings/lists/code/tables/links; 40,000-unit text slices with exact continuation metadata. Reference may include hidden text and excludes form controls, iframes and shadow roots. | HT sufficient; frozen read storage and HTML-to-text service, no new boundary type. **Host-service-only**; frozen reads are runtime state, not durable recovery. |
| T10 — `browser_wait` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::install.has/act` | Timed/text wait including open shadow roots, bounded 0.5–60 s, fresh snapshot, `wait_timeout` instead of fabricated success. | HT sufficient; cancellable local guest polling. **Host-service-only**. |
| T11 — `browser_tabs` | MISSING | `host-tools.js::SCHEMAS`; `browser-panel.js::tabsTool/tabsText` | List/new/switch/close, stable IDs (positional `tab` not executable targeting), new-at-12 refusal without eviction, switch snapshot and optional new-tab navigation. The panel's tab owner (B02) exists; the tool does not. | BS/HT sufficient; local tab owner/controller. **Mixed** UI/host; unlike the other ten, tab management is in the reference panel. |

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

1. **The browser tools on the new host** (T01–T11, B05/B07/B09–B11 remainders,
   B08, B14, B16, B17). The panel now navigates and keeps site state for the
   user, but nothing can snapshot, read, screenshot or act on a page for the
   agent. Advertising the existing types as browser support would be misleading.
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
| 4 | Choose an approved browser-host technology/trust model, then manual panel, tabs, address/history, lifecycle, permission rules and site profile | Yes with a **real browser host**, not with current NoHost | **Largely done** (optional Qt WebEngine host; see [this pass](#browser-foundation-slice)). Remaining: guest shortcuts, popups/context menu, restart-verified profile, permission tests. |
| 5 | Introduce tab ownership and snapshot/read/screenshot/wait; then enable navigation, tab mutations and input only behind targeting/cancellation barriers; add Take Control/Hand Back and download attribution | Can test locally without an agent, but needs real host | Reference-style local-page lifecycle tests, stale/covered/moved-target refusal, queued cancellation and late completion. A snapshot's IDs must bind subsequent input. |
| 6 | Persist browser layout/account hints, apply browser theme; finish folder services and approval-details persistence | Yes, after owners/services exist | Reload restores only intended state; fresh guest/page identities; no action replay, no cookie/credential leakage into display history. |
| 7 | Only under a separately approved integration task, connect the semantic backend boundary and advertise supported schemas/guide/localPaths | No, requires real backend **and** completed host for browser flows | Actual discovery → named-turn approval → host call → response → terminal/recovery behavior, including real image/file support. Never infer success from the fake demo. |

## Blockers and ownership boundaries

- **Browser host selection was resolved** for this slice: the owner approved
  Qt WebEngine as an optional build feature (`OPENGHOST_BROWSER`, default ON),
  because the reference's browser is embedded Chromium webviews, not an
  externally controlled OS browser. The reference's DOM/CDP tool algorithms still
  need porting onto it (Qt WebEngine has no `webContents.debugger`; see the
  remaining blockers under [this pass](#browser-foundation-slice)).
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

## Browser foundation slice

Scope: host/service lifecycle, panel UI, visibility/open/close, tab and active-page
state, navigation/loading/error state, hand-back/focus, the live page surface and
the `host.browser` snapshot. No browser tool, Rust, RPC or FFI was added, and the
reference was not changed.

| Piece | Native implementation |
| --- | --- |
| Host state and snapshot | `src/frontend/browser.h/.cpp` (`Browser`, `BrowserTabs`); QtCore only, in `native_contract` |
| Panel and guests | `qml/BrowserPanel.qml`, `qml/BrowserGuest.qml` (only with `OPENGHOST_BROWSER`) |
| Toggle and layout | `qml/Main.qml` (`browserToggle`, `browserLoader`, `chatRight`), `GlobeIcon` and `browser-*` glyphs in `src/icon.*` |
| Focus yield | `src/browserfocus.*` (`BrowserFocus`) |
| Composition | `src/main.cpp`, `WindowController(…, host)`, `HostServices::turnEnded` called from `ChatService` |
| Fake | `FakeBackend` accepts a browser snapshot as ignored context |

Remaining blockers before the 11 browser tools:

1. **An observation/automation layer on Qt WebEngine.** The reference runs its
   DOM heuristics in an isolated world through Electron's preload and acts through
   CDP (`webContents.debugger`: input events, focus emulation, screenshots).
   Qt WebEngine offers `runJavaScript` in an `ApplicationWorld`/user world and
   `QWebEngineScript` injection, but no public CDP client; trusted mouse/key input
   must instead be synthesized as Qt events into the guest, and full-page capture
   needs its own approach. This choice gates T02–T10 and H06.
2. **Publishing tools and flipping `available`.** `Browser::tools()`/`run` and the
   snapshot's `available`/status must change together, with the reference schemas
   (`host-tools.js::SCHEMAS`) and the response formatter (H08).
3. **Chat-side orchestration** (B10/H03–H05): `onHostTool`'s claim and driving,
   `awaitHandBack`, the global step queue with receipt-time targets, revision
   checks, deadlines (H07) and retained seen-call IDs.
4. **Guest preload equivalents**: sign-in observation (B14), page events for the
   pointer overlay (B09), popups/context menu (B08) and downloads with operation
   attribution (B17/H09).

## Evidence and focused validation

Browser foundation slice, Release builds of this commit's tree, with each setting
of `OPENGHOST_BROWSER`:

```sh
cmake -S . -B build-browser -DCMAKE_BUILD_TYPE=Release -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build build-browser --parallel 8
ctest --test-dir build-browser --output-on-failure
cmake -S . -B build-browser-off -DCMAKE_BUILD_TYPE=Release \
  -DOPENGHOST_BUILD_SMOKE_TEST=ON -DOPENGHOST_BROWSER=OFF
cmake --build build-browser-off --parallel 8
ctest --test-dir build-browser-off --output-on-failure
git diff --check
```

Results: 4/4 tests passed in each build (`native_contract_test`,
`native_browser_test` with 12/12 functions, `native_ui_smoke`,
`native_fake_ui_smoke`). With the browser, the UI smokes (offscreen, software Qt
Quick) open the real panel with the toggle, type a `data:` page into the address
bar, check its frame in the window, go Back, show and retry a refused-loopback
failure, move focus into and out of the page, drive/Take control/Hand back/turn
end, kill the guest's renderer and recreate it, open and middle-click-close a tab,
and close the panel. Without it, they check that no panel, toggle or host exists;
`ldd` showed no WebEngine library linked. No visual-parity suite or live backend
was run.
Original audit evidence follows.

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
