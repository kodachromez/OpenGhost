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
findings. That original audit changed only this document, not implementation or
reference files. Subsequent foundation/automation slices are recorded below.

**Update — browser foundation slice.** The desktop build now has the reference's
built-in browser panel on Qt WebEngine guests (`OPENGHOST_BROWSER`, default ON,
chosen because the reference renders its pages in embedded Chromium webviews):
panel/toggle/width, tabs, address and navigation controls, guest lifecycle,
loading/failure/crash states, saved layout, focus lending, Take control / Hand
back state and turn-end release, and the live `host.browser` snapshot. Rows moved
by that work are marked in the matrix and listed under
[that pass](#browser-foundation-slice).

**Update — native automation slice, based exactly on `ce4cbce`.** The browser now
owns `BrowserAutomation` using public Qt APIs, isolated ApplicationWorld scripts
and native viewport grabs. Snapshot, tabs, navigation, wait and read work;
screenshot and scroll have explicit partial support. Seven unchanged prepared
schemas are published with `available: true`; click/press/type/select remain
unpublished. No CDP/debug endpoint was enabled. See
[native automation](#native-automation-slice) and
[browser-tools-port.md](browser-tools-port.md) for limitations and focused tests.
Unrelated file/media findings remain unchanged.

**Update — non-input browser gaps, based exactly on `4b9218c`.** Full-page and
covered-guest screenshots, guest popups and tab requests, the page menu,
sign-in hints, and downloads with per-step attribution and a completion toast.
Input tools, their cursor overlay and CDP are untouched. See
[non-input slice](#non-input-browser-slice).

### Summary counts

Each matrix row is one audited behavior. Cross-cutting lifecycle and individual
tools are counted separately; the eleven tools are individually identified.
Reference absences listed later are not counted as missing features.

| Status | Features |
| --- | ---: |
| MATCH | 29 |
| PARTIAL | 17 |
| MOCK ONLY | 2 |
| MISSING | 26 |
| INTENTIONAL DIFFERENCE | 1 |
| **Total** | **75** |

The original audit counted 6 / 10 / 4 / 54 / 1; the browser foundation slice
moved B01–B04, B06 and B15 to MATCH, B05, B07, B09–B13 and H07 from MISSING to
PARTIAL, and H02 from MOCK ONLY to PARTIAL. Native automation then moved
B05/B10/B11/H02/H04/H05/H07/H08 and T01/T02/T09/T10/T11 to MATCH;
H01/H06/T07/T08 to PARTIAL. The non-input slice moved B08, B14, B17 and H09
from MISSING to MATCH; T08 stays PARTIAL with only a page-observable side
effect left. MATCH scopes the row, not the whole browser or a connected agent;
input gaps have separate rows.

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
2. `Browser` owns `BrowserTools` and `QtBrowserAutomation` in desktop composition.
   `ChatService::initialize` publishes seven supported schemas, `reverse` admits
   named live-turn steps, and the owner serializes real guest work.
   `Browser::browser()` supplies the panel snapshot (`available: true`) to
   `params`, steering and `browserChanged`; OFF builds use `NoHost` and null.
   `HostServices::turnEnded` cancels/releases work on turn end/disconnect.
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
| B05 — Lazy guests, readiness, failure and crash recreation | MATCH | `browser-panel.js::constructor/createView/ensure/close`; `desktop/browser.js::adopt/entry` | Browser retains lazy tabs, 15 s readiness, guarded guest creation and incarnation checks. BrowserTools::ensure recreates failed/gone guests before a later step. BrowserGuest observes an isolated document-creation marker and DOM readyState separately from native resource loading; replaced guests cannot acquire an old operation. Failures/crashes settle owned work. | BS unchanged. Foundation tests; real slow-resource DOM-readiness and renderer-crash/recreation tests. |
| B06 — Visible loading/error/retry states | MATCH | `browser-panel.js::createView/render/syncBar/build`; `styles.css` browser progress, spinner and error styles | `qml/BrowserPanel.qml`: sweeping 2 px progress band and tab spinner while loading, empty state, failure card (`host · error`, Chromium's `ERR_*` text with Qt's `net::` prefix removed, Try again) and the crash message. | Local presentation; no boundary type. Tested: `errorsCrashesAndRetry`; UI smoke refused port, retry and crash. |
| B07 — Browser keyboard shortcuts and focus lending | PARTIAL | `desktop/browser.js::adopt` `before-input-event`; `browser-panel.js::constructor/run/giveBack/take/handBack` | Focus lending is ported: `BrowserFocus` (`src/browserfocus.*`) takes the keyboard back from a page on any press outside the panel (yieldKeys); driving blurs the page; Take control focuses it; Hand back blurs it and records the lent page (`Browser::lent`). Missing: the guest shortcuts (F5/Ctrl+R, F12/Ctrl+Shift+I DevTools, Alt+arrows, Ctrl+L/T/W) and `giveBack` after a tool step. | Local focus interface, no boundary type. Tested: `takeControlAndHandBack`; UI smoke click-in/click-out, drive, take, hand back. |
| B08 — Guest popup/new-tab routing and context menu | MATCH | `desktop/browser.js::adopt` window-open and context-menu handlers; `browser-panel.js::onEvent` | `qml/BrowserGuest.qml::onNewWindowRequested`: popups (`InNewWindow`/`InNewDialog`, Electron's `new-window`) open `qml/BrowserPopup.qml`, a separate white 520×700 window in the same site profile via `openIn` (opener kept; no tool target, page menu, sign-in hint or download attribution; its own popups open further windows; `window.close()` closes it). Foreground/background tab requests for http(s)/file open through `Browser::openFrom` next to the opener; other schemes are refused. The page menu is `Browser::menu` (reference items, order, separators and enabled flags) shown with the app's Qt Quick Controls `Menu`, not a native OS menu: Open/Copy link, Open/Copy image, Cut/Copy/Paste/Select all, selection Copy, Back/Forward/Reload and Inspect (the engine's in-process DevTools in their own window, no remote-debugging endpoint, never used by automation). Copy link uses the engine's link copy, not a text-only write. Guest keyboard shortcuts are B07. | Local guest event/menu APIs; no boundary type. Tested: `pageMenuMatchesTheReference`, `guestOpenedTabsFollowTheirOpener`, real-guest `pageMenuPopupsAndGuestOpenedTabs` (right click on a link, Open link in new tab, `_blank`, refused `javascript:`, popup window size/URL/title/close). |
| B09 — Agent ownership overlays and pointer visualization | PARTIAL | `browser-panel.js::drive/sync/point`; `browser-toggle.js` live badge; `desktop/browser.js::pointer` | `qml/BrowserPanel.qml` driving ring/badge (input-blocking), hover-revealed Take control, user banner with Hand back; the toggle's pulsing live dot; all from `Browser::drive` state. BrowserTools now drives this state for admitted host calls. Missing: the animated cursor and click ripple. The reference emits `pointer` only from click/type targeting (`desktop/browser.js::pointer`), so they belong with the unpublished input tools and were deliberately left to that work. | BS control; local driver state, no boundary type. Tested: `takeControlAndHandBack`, UI smoke overlays. |
| B10 — Take Control / Hand Back without replaying the interrupted action | MATCH | `browser-panel.js::take/handBack/waitForAgent/release`; `chat.js::onHostTool/awaitHandBack/end` | BrowserTools::controlChanged cancels continuations and holds active/queued calls without a user-wait deadline. Hand-back substitutes fresh current-tab snapshots with empty args and handed-back status, even on error, never the original action. inputQueued returns empty cancelled/message results only to that session; turn end drops work and releases driving. | HT/BS unchanged; local HS inputQueued hook. Real hand-back and adversarial held-call tests. Early turn admission remains H03. |
| B11 — Guest/app trust boundary | MATCH | `desktop/browser.js::guard/adopt/entry/install/world`; `desktop/main.js::fromApp` and browser handlers; `desktop/preload.js` | Separate WebEngine guests/profile; no app objects, WebChannel, Node or debug endpoint. QtBrowserAutomation sends host-owned scripts through ApplicationWorld; the main world cannot replace observation/ref/document state. Incarnation, page/document token, expiry and call-token checks guard completion. The QML adapter is not exposed to page scripts. The only channel (sign-in hints, B14) exists in ApplicationWorld alone. | Existing envelopes, local browser-owned abstraction. Real hostile main-world globals, frame/shadow and isolation tests. |
| B12 — Browser site permission handling | PARTIAL | `desktop/browser.js::setup`, `ALLOWED` | `qml/BrowserGuest.qml` grants pointer lock (`MouseLock`) and fullscreen requests and denies every other permission request. Differences: Qt has no separate sanitized-clipboard-write permission or synchronous permission-check handler; not yet covered by a test. | Internal host policy, not AP and not new backend types. |
| B13 — Persistent cookies/site state and browser session configuration | PARTIAL | `browser-panel.js` and `desktop/browser.js` `persist:browser`; `desktop/browser.js::setup` user agent | `qml/BrowserPanel.qml` builds one persistent WebEngine profile (`storageName: browser`) under `AppDataLocation/browser` (smoke runs: a temporary directory) with the reference's plain Chrome user agent on the engine's Chromium version. Not yet verified across an application restart. | Local browser profile; no boundary type. |
| B14 — Site sign-in submission observations, never verified auth | MATCH | `desktop/browser-preload.js::check`; `browser-panel.js::signedIn/snapshot`; `openghost.browser.accounts` | `qml/BrowserGuest.qml` installs the preload's check (filled password field on submit, Enter or a button click; once per document; main frame) as an ApplicationWorld document-creation script. Its only outlet is a `WebChannel` whose `webChannelWorld` is ApplicationWorld: the page's own world has neither `qt` nor the channel library and cannot report a host. `Browser::signedIn` accepts host-name characters only, for the current guest, removes `www.`, keeps most recent first, at most 30, with time; saved in `browser.json` (`accounts`), reported in `host.browser.signedIn`; `signedInVerified` stays false. No password, URL path or login result is observed. | BS unchanged; local observation event and storage. Tested: `signInHintsAreHostOnlyBoundedAndSaved`, real-guest `signInHintsComeOnlyFromTheIsolatedObserver`. |
| B15 — Restore panel layout and tab URLs/titles after reload | MATCH | `browser-panel.js::constructor/save/select`; `openghost.browser` | `Browser::save/load` → `browser.json` beside the preferences (atomic `QSaveFile`): open, width, non-blank tabs' URL/title and active index. Restore is lazy (only the shown active tab gets a guest), with fresh handles; unreadable data falls back to an empty closed panel; navigation stacks and pending actions are not saved. | Local persistence record; no boundary type. Tested: `tabLimitAndLazyRestore`. |
| B16 — Appearance follows the selected light/dark/system theme in sites | MISSING | `desktop/main.js` `nativeTheme.themeSource` and `theme:set`; `browser-panel.js`; `styles.css` browser theme variables | `src/appearance.cpp` and `qml/AppearancePage.qml` theme the native app, but no theme choice is passed to the browser guests. | Existing native appearance state can be reused; local host theme setter needed, no boundary type. **Mixed** UI/host. |
| B17 — Browser downloads to the OS downloads directory and completion toast | MATCH | `desktop/browser.js::setup/uniqueFile` `will-download`; `browser-panel.js::onEvent/notify` | The site profile's `downloadRequested` → `QtBrowserAutomation::download` → `Browser::downloadStarting`: saved under `QStandardPaths::DownloadLocation` (smoke runs: a temporary folder; tests and an unconfigured host refuse every download rather than guess a folder), named like `uniqueFile` (`NAME`, else `STEM (K)EXT`, also never a file another running download holds; path components stripped). Completed panel-guest downloads show `qml/BrowserPanel.qml`'s `Downloaded NAME` toast for 4.2 s; cancelled/failed ones and popup downloads are not announced. No download manager, progress or cancel UI, as in the reference. The toast has no backdrop blur. | HS unchanged; local engine report (`BrowserAutomation::observeDownloads`) and host event. Tested: `downloadsBelongToTheStepRunningAtTheirStart`, real-guest `downloadsSavedUniquelyAndAttributedToTheirStep`. |

## B. Browser request/response, ownership and recovery plumbing

| ID / reference feature | Status | Exact reference implementation | Native implementation and absent/different behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| H01 — Discover and publish the eleven available browser tools | PARTIAL | `host-tools.js::SCHEMAS/schemas`; `backend-client.js::hello` | Browser::tools publishes seven unchanged schemas reused from prep; four input tools remain unpublished. src/browser/schemas.json is checked against the prepared/reference oracle. Screenshot/scroll explicitly refuse unsupported branches. NoHost and uncomposed Browser advertise nothing. | HT/HS unchanged. Real schema/refusal test, prepared contract fixtures and Node oracle. |
| H02 — Current browser context on start/retry/steer and change notifications | MATCH | `browser-panel.js::snapshot/report`; `chat.js::sessionParams/steer`; `backend-client.js` | Composed Browser reports available=true and real empty/lazy/loading/ready/failed/gone status, control, stable tabs/revisions and deduplicated changes, even closed/empty. Existing params/steering forwarding remains. OFF builds supply null. Sign-in hints remain separately B14 and never verified. | BS/HostContext unchanged. Foundation context tests and real adapter/schema tests; no connected backend qualification. |
| H03 — Reverse-call correlation, admission and duplicate prevention | PARTIAL | `chat.js::claim/onHostTool`; `backend-client.js` reverse-ID handling | ChatService::reverse checks publication/session/accepted turn and now retains spent tool-call IDs after completion/cancellation. BrowserTools independently owns session/turn/call identity. Remaining: early calls refuse instead of awaiting accepted identity/recovery; some unknown-session failures still collapse to stale_turn. | HT/RequestId unchanged. Focused host-routing test rejects a repeated completed call. |
| H04 — Stop/request-cancel/terminal cleanup and late-result suppression | MATCH | `chat.js::onHostTool/end/stop`; `backend-client.js` reverse cancellation; `browser-panel.js::cancel`; `desktop/main.js::cancelBrowser` | BrowserTools owns queued/active calls and timers; cancel/turn end/destruction invalidate continuations and engine callback tokens and stop issued navigation. Already-issued effects are not undone. ChatService answers reverse/terminal cancellation once with empty cancelled content, drops late completion and releases driving. | HT unchanged. Real cancellation/turn-end tests plus adversarial delayed callbacks; no action replay. |
| H05 — Serialize browser steps across chats and pin receipt-time targets | MATCH | `browser-panel.js::run/tabsTool`; `desktop/browser.js::run` queues | One BrowserTools queue for sessions and tab operations pins targets/revisions at receipt. Explicit targets select only that tab; implicit switch-away-and-back invalidates the active lease. Cancelled queued entries cannot release the predecessor; late callbacks cannot dispatch a next stage. | Local owner, no boundary type. Real cross-session queue/target tests and delayed-adapter barrier tests. |
| H06 — Fresh page/ref/target validation before further input | PARTIAL | `desktop/browser.js::revise/check/install/point/world/act`; `browser-panel.js::run` revision checks | Page/document/incarnation/active-target checks run before dispatch and after awaits. Ref reveal requires pageId and a connected same-page ref map; partial/cancelled reveal invalidates observations. Frozen reads expire on page change. Covered/moved point rechecks and multi-input/final-navigation sequencing remain with click/type/press/select. | HT/BS unchanged. Real stale-page/ref/read/history tests, document-version tests and partial-reveal cancellation tests. |
| H07 — Readiness/operation/dispatch deadlines and truthful errors | MATCH | `browser-panel.js::interruptible/ensure/run`; `desktop/browser.js::timed/check/settle/navigate/run` | BrowserTools enforces 90 s receipt/queue/readiness, 75 s page operation after readiness, 12 s JS/capture, 30 s navigation and 15 s loading settle; isolated JS checks dispatch expiry. Wait uses reference bounds/codes and 300 ms mutation quiet (2 s cap). Failures never fabricate successful loads. Already-issued work is not rolled back. | HT unchanged. Injected short deadlines/late callbacks, loopback failure, slow-resource readiness and real wait_timeout tests. |
| H08 — Browser response formatter: text/images, metadata and errors | MATCH | `host-tools.js::result/read/readable/flatten`; `chat.js::onHostTool` status assignment | BrowserTools builds the prepared SS/TAB/IMG/READ families, allowlisted metadata, error text/status, screenshot text-then-image/label, UTF-16 pagination and hand-back status. The reference HTML converter runs on a capped frozen string in an isolated inert DOM. No chat cards or invented download/artifact fields. | HT unchanged. Prepared result comparisons, real parser/pixels, frozen reads and surrogate-boundary tests. |
| H09 — Attribute downloaded files to the initiating guest/operation | MATCH | `desktop/browser.js::setup/state`; `host-tools.js::result`; `test/browser-lifecycle.test.js` download test | A download belongs to the step acting on its panel guest when it starts (`BrowserTools::running`, the reference's `found.running`), as a per-step `operationId`. Each engine report is handled exactly once (start, then one end: completed, cancelled, failed or dropped request). Only that step's snapshot lists it (`Downloaded: FILE`, `data.downloads` `{file, at, operationId}`); later steps, other chats and other guests never claim it. Turn end releases its steps' ownership, including downloads still running. Records stay per guest incarnation, at most 100. | HT `data.downloads` unchanged. Fake-engine owner tests and real `a[download]` during `browser_wait`. A navigation that becomes a download revises the page identity, as `did-start-navigation` does. |
| H10 — Restoring chat display does not replay browser actions | MATCH | `chat.js::reconcile/recoverTurn/applyEvent`; `library.js::displayMessages` | `src/frontend/chat_service.cpp` recovery replays display events, not `HostToolRequest`s; `src/frontend/library.cpp::displayMessages` discards tool histories. Restored browser-looking text is not executable. This narrow safety match does not establish recoverable browser operation state. | Existing recovery/display contracts suffice. **No missing work** for this behavior. |
| H11 — Advertise supported picture/video/file presentation syntax | MISSING | `render-guide.js`; `backend-client.js::hello` | `Initialize::renderGuide` exists but `ChatService::initialize` leaves it empty. No advertised native guide for media or file diagrams, even for file diagram rendering that exists. Do not simply copy claims for media that native cannot render. | Existing `QString renderGuide`; no new types. **UI-only** capability-description work, enabled only alongside actual render support. |

## C. All eleven browser host tools

Implementations are `src/frontend/browser_tools.*`, the browser-owned
`BrowserAutomation` seam, `src/browser/qt_browser_automation.*` and its isolated
observation library. Five tools work, two have explicit partial support, four
remain unpublished. **No frontend/backend boundary type changed.** Passive
contract fixtures remain plumbing tests; real-guest tests are separate.

| ID / feature | Status | Exact reference implementation | Native implementation / remaining behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| T01 — `browser_navigate` | MATCH | `host-tools.js::SCHEMAS`; `desktop/browser.js::act/navigate/normalize` | BrowserTools host normalization (including IPv6 loopback), Qt native load/back/forward/reload/stop, navigation/quiet settling and fresh SS. Failure is navigation_failed; deadline/abort stops loading, not a retry. | HT unchanged. Real local-file history/reload, stale-page and refused-loopback tests. |
| T02 — `browser_snapshot` | MATCH | `host-tools.js::SCHEMAS`; `desktop/browser.js::install.snapshot/state` | src/browser/observe.js ports the reference viewport/full heuristic, labels/refs, masking, same-origin iframe/open-shadow traversal and unreadable-frame notices. BrowserTools formats SS with coverage/scroll/truncation and pageId. Same 9,000/40,000 text and 20,000 visited-element bounds; not a full accessibility-tree guarantee. | HT unchanged. Real hostile-world, masks, frame/shadow, stable-ref, full/bounded observation tests. |
| T03 — `browser_click` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::act/point/pointer/mouse` | Ref or page-coordinate input, optional double click, covered/moved recheck and post-click snapshot. | HT sufficient; real input host plus H06. **Host-service-only**. |
| T04 — `browser_type` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::act/press` | Ref/focused-field typing, clear-or-append, empty-text delete, optional Enter submit, navigation-safe sequencing and snapshot. | HT sufficient; real input host. **Host-service-only**. |
| T05 — `browser_select` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::install.choose/act` | Native select option matching by text/value, input/change dispatch, error on no matching option/non-select, returned selection note/snapshot. | HT sufficient; local DOM operation host. **Host-service-only**. |
| T06 — `browser_press` | MISSING | `host-tools.js::SCHEMAS`; `desktop/browser.js::keyOf/press/act` | Key combinations and modifiers, bounded repeat count (1–20), key release and page-change barrier between repetitions. | HT sufficient; real keyboard host. **Host-service-only**. |
| T07 — `browser_scroll` | PARTIAL | `host-tools.js::SCHEMAS`; `desktop/browser.js::install.reveal/act` | Working isolated ref scrollIntoView with required pageId, connected-ref checks, settle/SS and dirty invalidation. Direction/amount returns unavailable: targeted exact-delta trusted wheel delivery is not qualified. No scrollBy or synthetic wheel substitute. | Exact prepared schema. Real reveal/stale-ref/partial-cancel tests and native-wheel prototype; see automation limitations. |
| T08 — `browser_screenshot` | PARTIAL | `host-tools.js::SCHEMAS/result`; `desktop/browser.js::screenshot/act` | Viewport: native guest-only grab. `full_page`: reference dimensions and semantics: CSS viewport width × min(content height, 4 viewports), from the page origin, `pageHeight` = capture height, `truncated` when content is taller; JPEG 82, width ≤1280, scale guidance. A tall or scrolled page is captured by briefly giving the guest the capture height under a frozen copy of its pixels (the visible panel never changes), scrolling it to the origin, grabbing a frame drawn at that size, then restoring size and scroll before the result. Cancellation, deadline, page replacement and renderer/tab loss restore view state and deliver nothing late; the original scroll is restored only in the same document. Bounds: 16384 px per edge and 64 Mi CSS pixels, else `unavailable`. Remaining difference: pages can observe the temporary resize and the scroll to the origin and back (resize/scroll events, viewport units) — CDP's beyond-viewport capture also resizes but is not claimed to scroll. | HT unchanged. Real `fullPageFromTheTopWithoutDisturbingThePanel`, `fullPageCancellationPageChangeAndRendererLossRestore`, `coveredGuestCapturesOnlyItsOwnPixels`, plus earlier JPEG/scale tests. |
| T09 — `browser_read` | MATCH | `desktop/browser.js::act` read case; `host-tools.js::read/readable/flatten/code` | Isolated capture caps source HTML at 4 Mi UTF-16 units and uses the reference inert DOM converter. BrowserTools freezes derived text/URL under readId/pageId and returns 40,000-unit slices with exact metadata. Refresh/navigation/dirty reveal/loss invalidate continuation; hidden text may remain, form controls/frame/shadow contents are excluded. | HT unchanged. Real parser/exclusions/cap/frozen refresh tests; UTF-16 split-surrogate/beyond-end tests. Ephemeral only, never action recovery. |
| T10 — `browser_wait` | MATCH | `host-tools.js::SCHEMAS`; `desktop/browser.js::install.has/act` | Timed/text waits use isolated body/open-shadow search, 400 ms text polls, ≤250 ms sleep chunks, 0.5–60 s bounds, loading/mutation settle and SS. Missing text returns wait_timeout. Navigation, cancellation, tab/renderer loss or timeout blocks further stages. | HT unchanged. Real shadow/timed/expiry tests, cross-session cancellation and late-result deadline tests. |
| T11 — `browser_tabs` | MATCH | `host-tools.js::SCHEMAS`; `browser-panel.js::tabsTool/tabsText` | BrowserTools serializes list/new/switch/close through Browser's native tab owner. Stable tabId required for switch/close; capacity 12 refuses without eviction. Exact TAB families, switch SS without tabs, new+URL SS with tabs, ignored optional pageId. No replay after hand-back/cancel. | BS/HT unchanged. Real empty/list/new/history/switch/close/unknown-ID/positional-ID/capacity tests plus queued cancellation. |

## D. Tool presentation, permissions and approvals

| ID / reference feature | Status | Exact reference implementation | Native implementation and absent/different behavior | Contracts / new types; work |
| --- | --- | --- | --- | --- |
| A01 — Ordinary browser/web tools use activity, not request/result cards | MATCH | `chat.js::applyEvent/showGhost`; `stream-view.js`; `library.js::displayMessages` | `ChatService::event` tracks tools and emits `worked`; `qml/Main.qml` shows the working ghost. Progress/results are not appended to the visible transcript or saved as tool cards. Native retains some transient detail internally, unlike JS's minimal bookkeeping. Neither reference nor native has a separate `web_search`/`web_fetch` card renderer. | Existing `ToolStarted/Progress/Completed` suffice. **No missing tool-card UI**. |
| A02 — Web/file approval cards and untrusted request presentation | PARTIAL | `approval-card.js::ApprovalCard/present`; `chat.js::onApproval` | `qml/ApprovalCard.qml`, `WindowController::approvals`, and `ChatService::reverse/approve` render kind=web/file, site/file/folder places, quote, code/diff, Allow/Deny and fallback args. However native bypasses JS's kind/effect/reveal/place allowlists and nonempty-title fallback; fallback JSON is indented rather than compact. No real browser supplies reference labels/refs. | AP already holds every field; no new types. Need frontend normalization and real request source, not client-side permission policy. **Mixed** UI/orchestration; actual policy remains backend-owned. |
| A03 — Correlated consent, live mode change and supersession | PARTIAL | `chat.js::claim/onApproval/onModeChange/steer/end`; `mode-picker.js`; `backend-client.js` | `ChatService` gates approvals by identity, queues early approvals, sends mode configuration, handles resolution/cancel/disconnect and dismisses current approvals on steering. But unlike JS `onApproval`'s `turn.queue.length` check, `reverse` does not deny a newly arriving approval just because input is already queued. Hand-back is now implemented (B10), but no real backend policy is connected. Native correctly does not infer approval inside HS. | AP and existing steering state suffice; no new types. **Mixed** UI/orchestration, with real approval decisions a backend integration blocker. |
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

1. **Remaining browser input**: T03–T07 remainders, covered/moved-target
   checks and navigation-safe input sequences, the B09 cursor/ripple they drive;
   also B07 shortcuts and B16 theme. The public-Qt automation slice must not
   advertise four absent tools or disguise ref reveal as wheel parity.
2. **Remaining admission and safety qualification**: H03 early accepted-turn
   waiting and H06 multi-input barriers. Queues, versions, late-callback guards
   and hand-back now exist; cancellation still cannot undo already-issued work.
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
6. **Browser state/settings persistence.** Download ownership (B17/H09) now
   exists; do not turn a late download into another call's result or claim
   browser state is protected by a chat display lock.

## Original recommended port order

Historical roadmap; automation now completes parts of steps 1 and 5.
This does not authorize unrelated future work.

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
  externally controlled OS browser. The public-Qt automation slice now ports
  observation/navigation/read/wait and bounded capture/reveal. A CDP requirement
  is not established; unresolved full-page capture/native input must first be
  qualified through the narrow host abstraction.
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

## Non-input browser slice

Implemented in the separate worktree/branch `feat/browser-noninput-gaps`, based
exactly on `4b9218c8ff04e4f8415f579823330ab0fc03850a`. No input tool, schema,
reference file, CDP/remote-debugging endpoint, Rust, RPC or FFI was added or
changed; the browser-input worktree was not touched. No push.

- **Screenshot (T08):** tall and scrolled `full_page` captures now return the
  reference's region and metadata, from public Qt APIs only: `QQuickItem`
  grabs, a non-live `ShaderEffectSource` cover and isolated-world scrolling.
  Covered guests (app chrome over a closed panel) capture their own pixels.
- **Popups, tab requests, page menu (B08):** see the matrix row. DevTools is
  the user's Inspect item only.
- **Sign-in hints (B14):** isolated-world observer, host names only, never
  verified.
- **Downloads (B17/H09):** OS downloads folder, unique names, per-step
  attribution with exactly-once start/end handling and turn-end release, toast.
- **Not done here:** B09's agent cursor and ripple (driven by input-tool
  pointer events), B07 shortcuts, B16 theme, all input tools.

The UI smokes fail five browser checks (`the page loads and is ready` and the
four that depend on it) **identically on the untouched base `4b9218c`**: Qt
WebEngine does not run the document-creation marker script on `data:` pages,
and `location.href` encodes spaces unlike `view.url`, so DOM readiness never
opens for the smoke's typed `data:` URL. Not changed in this slice.

## Native automation slice

Implemented in a new worktree/branch starting exactly at `ce4cbce`; schemas,
result fixtures and the port map were selectively reused from `prep/browser-tools-port`
(`0c41d69`), without its older production tree. The original checkout's unrelated
plugin edits and the Rust checkout were not modified. No push.

- **Working tools:** snapshot, tabs, navigate, wait, read.
- **Partial:** screenshot (viewport and fitting full page); scroll (ref reveal).
- **Blocked branches:** tall/scrolled non-mutating full-page capture, targeted
  exact-delta trusted wheel delivery. Click/press/type/select remain unpublished.
- **CDP:** none enabled or implemented. Public Qt mouse/key input produces trusted
  events in the focused prototype; DOM `.click()` does not. Direct item wheel
  delivery fails; ordinary window wheel hits the app shield and uses angle-derived
  rather than the requested pixel delta. This does not prove all public input
  paths impossible. Native Quick grabs cannot capture off-viewport pixels without
  altering layout/scroll. No fake replacements were shipped.
- **Architecture:** `Browser` owns `BrowserTools` (QtCore queue/leases/results) and
  `BrowserAutomation`; `QtBrowserAutomation` contains public Qt navigation,
  isolated observation and native capture. QML/chat/tool contracts contain no
  CDP concepts or debugging endpoint. OFF builds link no WebEngine.
- **Rows moved to MATCH:** B05, B10, B11, H02, H04, H05, H07, H08,
  T01, T02, T09, T10, T11.
- **Rows moved to PARTIAL:** H01, H06, T07, T08. H03 remains PARTIAL because early
  reverse calls still refuse rather than await accepted turn identity.

Focused validation and exact commands are in the current landing section of
[browser-tools-port.md](browser-tools-port.md): ON/OFF Release app builds,
`native_browser_test`, `native_browser_operations_test`, ON-only real-guest
`native_browser_automation_test`, four browser-only contract functions, prepared
Node oracle and the reference browser-lifecycle suite. No full UI smoke or visual
parity suite, provider credentials, external websites, Rust/RPC/FFI, or reference
modification. Tests use temporary files/profiles and loopback HTTP only.

## Browser foundation slice

Historical record at `ce4cbce`, before the automation landing above.

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

Blockers recorded at the foundation revision (now superseded by the automation
matrix/limitations above):

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

Historical browser foundation slice, Release builds of that commit's tree, with each setting
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
