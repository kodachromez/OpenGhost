# Browser tools: native implementation map

## Mutating input landing (based exactly on `4b9218c`)

Branch `feat/native-browser-mutating-tools` in a new worktree started at
`4b9218c8ff04e4f8415f579823330ab0fc03850a`. It supersedes the input rows of the
automation landing below. No reference, Rust/RPC/FFI, plugin, other worktree or
visual-parity change. **No CDP endpoint, remote-debugging flag or client.**

### Reachable tools

All eleven unchanged prepared schemas are published, in reference order.

| Tool | Status | Notes |
| --- | --- | --- |
| `browser_click` | MATCH | Ref or `Number()` x/y, covered/moved/stale refusals, pointer delay, double click, final-press navigation |
| `browser_select` | MATCH | The reference's own isolated `choose` (untrusted input/change there too) |
| `browser_scroll` | MATCH | Ref reveal and exact direction/amount wheel |
| `browser_type` | PARTIAL | Behavior matches; the Control+A keydown `key` is `a`, not the reference's literal `A` |
| `browser_press` | PARTIAL | Key table/errors/repeats match; DOM key-field differences listed below |
| `browser_screenshot` | PARTIAL | Unchanged: tall/scrolled full page refuses `unavailable` |
| Snapshot, tabs, navigate, wait, read | MATCH | Unchanged |

### Trusted input through public Qt

- `BrowserAutomation::input` is the QtCore seam: one reference-shaped event
  (Move/Press/Release with click count, KeyDown/KeyUp with keyOf()'s key, code,
  virtual key, text and modifier bits, Text, Wheel with CSS deltaY). Engines never
  reply inline; cancel drops completion, not issued effects.
- `QtBrowserAutomation` sends `QMouseEvent`, `QKeyEvent`, `QInputMethodEvent` and
  `QWheelEvent` with `QCoreApplication::sendEvent` directly to the WebEngine
  view's input item (`RenderWidgetHostViewQtDelegateItem`, found by class name
  among the view's children). Pages see `isTrusted` events. Window hit-testing is
  skipped, so the input-blocking driving shield cannot intercept, as CDP input
  bypasses the reference's embedder. The earlier prototype's direct wheel failed
  only because it used a touchpad phase; a phase-less wheel works.
- **Click counts:** WebEngine counts clicks from event timestamps, so mouse
  events use a private clock starting at 2^40: each sequence jumps 10^6 ms, the
  second press of a double click is 1 ms later. Separate steps never merge into a
  double click with each other or with user clicks.
- **Keys:** the reference `KEYS`/`MODIFIERS`/`keyOf` table is ported verbatim
  into `BrowserTools` (errors included). Named keys and A–Z/0–9 carry the Qt key
  and US XKB scan code (WebEngine derives `code` from it). An unmodified other
  character carries only its text, so `key`/`code`/text match. Control/Alt/Meta
  suppress text exactly as in the reference.
- **Text:** a single `QInputMethodEvent` commit is the same renderer insertion as
  CDP `Input.insertText`: trusted `beforeinput`/`input` with `insertText`, no
  per-character key events, Unicode/surrogates intact.
- **Wheel:** WebEngine converts a phase-less wheel to CSS pixels as
  `angle/120 × wheelScrollLines × 20` and caches the line count per process; the
  adapter inverts that, caching the same style hint. deltaY is exact at that
  resolution (0.5 CSS px with the default 3 lines); deltaX 0, deltaMode 0, at the
  visual viewport centre. The page scrolls by exactly deltaY in the tests.
- **Focus emulation:** from a guest's first automated step (reference `attach`),
  its input item's `FocusOut` is withheld and, if needed, a `FocusIn` is sent to
  it: `Emulation.setFocusEmulationEnabled`. Qt keyboard focus still moves
  (BrowserFocus, blur, giveBack), but the page sees no blur/change and keeps
  `document.hasFocus()`. IME commits require page focus, so this is also what
  lets ref-less type work without taking the app's keyboard.
- **giveBack/lending:** the focused item at receipt is recorded. After every
  step (finish or cancellation), unless the user has control, a keyboard the
  step moved into a guest returns there and the page is recorded as lent. Press
  and ref-less type focus a lent page first, as `browser-panel.js::run` does.

### Sequencing and barriers (`BrowserTools`)

- `act`'s order is ported: input needs a truthy pageId; ref point (scroll into
  view, left-centre, clamp, hit test) → `element_covered` before any click;
  pointer event + 420 ms when the open panel shows the guest; re-point →
  `stale_target`; move, press/release pairs; type's focus click, 80 ms,
  Control+A, Delete (empty text), insert, 60 ms + Enter; select's `choose`;
  press repetitions; scroll metrics + wheel; 250 ms; settle; SS.
- Direct-call coercions follow JavaScript: `Number()`, `String()`, truthiness,
  `?? ''`, `clear !== false`, loose `ref != null` on click's recheck, and
  `Math.round` for times.
- Every dispatch is checked before and after its completion (owner, deadline,
  tab, crash, lease, page). Between dispatches a `Probe` query round-trips to the
  admitted document; input queries never install a ref map and refuse a changed
  pageId, replaced document or a document-initiated navigation seen by the
  isolated world's Navigation API `navigate` listener. Only the final dispatch is
  navigation-permissive; it is followed by its release and settle/observation
  of the replacement document. Partial input marks the observation dirty, so
  cancellation, timeout or refusal after any input revises pageId and read state.
- The barrier replaces CDP's per-command acknowledgement. It is a bounded JS
  round trip plus native load/URL/document signals, not a renderer input ack.

### Recorded differences (tested, not hidden)

Public `QKeyEvent` lets WebEngine derive several DOM fields itself:

- Unmodified non-alphanumeric characters: keydown/keyup `keyCode` is 0. The
  reference sends `charCodeAt()` as the virtual key ('/' 47, '.' 46 = Delete).
- Control/Alt/Meta letter chords use Qt's case: `Control+A` → `a`, `Alt+X` → `x`,
  `Control+Shift+x` → `X` (reference: literal `A`, `X`, `x`). This is the only
  difference in `browser_type` (its Control+A keydown).
- Chorded punctuation carries a layout code/keyCode (`ctrl+/` → `Slash`/191;
  reference `''`/47); chorded non-ASCII keeps `code` empty but keyCode 0 (é: 201).
- Enter with Control/Alt/Meta also produces WebEngine's `\r` keypress (so e.g.
  a field's change/implicit Enter behavior); the reference's raw key down has none.

Sending a Qt key whose virtual key equals the reference value would change
`code`/`key` and trigger that key's editing command, and many reference values
have no Qt key; this was rejected rather than substituting a closer-looking
fake. Fixing these needs an engine API that sets DOM key fields; CDP would do it
but is not justified for these fields alone.

### Validation

Build directories outside the worktree, Release, Qt 6.11.2/GCC 16.2.1:

```sh
cmake -S . -B ~/.cache/og-mutating-on -DCMAKE_BUILD_TYPE=Release -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build ~/.cache/og-mutating-on --target openghost-cpp native_browser_test \
  native_browser_operations_test native_browser_automation_test native_contract_test
ctest --test-dir ~/.cache/og-mutating-on \
  -R '^native_browser(_operations|_automation)?_test$' --output-on-failure
~/.cache/og-mutating-on/native_contract_test
cmake -S . -B ~/.cache/og-mutating-off -DCMAKE_BUILD_TYPE=Release \
  -DOPENGHOST_BUILD_SMOKE_TEST=ON -DOPENGHOST_BROWSER=OFF
cmake --build ~/.cache/og-mutating-off --target openghost-cpp native_browser_test \
  native_browser_operations_test native_contract_test
ctest --test-dir ~/.cache/og-mutating-off -R '^native_browser(_operations)?_test$'
~/.cache/og-mutating-off/native_contract_test
ldd ~/.cache/og-mutating-off/openghost-cpp | grep -i webengine   # none
node --test tests/browser-tools/reference.test.cjs
node --test reference/openghost/test/browser-lifecycle.test.js
git diff --check
```

Results, each ON/OFF where built: foundation `native_browser_test` **12 passes**,
owner `native_browser_operations_test` **13 passes** (4 new input-sequencing
cases), whole `native_contract_test` **55 passes**; ON-only real guest
`native_browser_automation_test` **20 passes** (18 functions plus init/cleanup),
run four times without failure after the final change. Node oracle **27 passes**,
reference lifecycle **16 passes**. No skips. `ldd`: no WebEngine in the OFF
binary (two WebEngine libraries in ON). GCC's pre-existing Qt metatype
`-Wmaybe-uninitialized` and the OFF build's unused `browser_smoke.cpp` helper
warnings remain; nothing in the changed files warns.

New real-guest functions: `exactPreparedSchemasAndInputPageRequirement`,
`clickTrustedRefCoordinatesDoubleAndFocusReturn`,
`clickCoveredMovedAndFinalNavigation`, `typeInsertsTextClearsAppendsAndSubmits`,
`typeAndPressAtPageFocusWithoutAppKeyboard`,
`typeFieldMovedRefusesAndCancelSendsNothing`,
`selectMatchesTextValueAndDispatchesInputChange`,
`pressKeysModifiersRepeatAndNavigationBarrier`,
`scrollWheelExactDirectionAndDelta`, `inputRendererLossAndTakeControl`. New
owner functions: `inputSequenceStopsOnCancelAndLateAcknowledgement`,
`navigationBetweenDoubleClickPressesStopsTheSecond`,
`finalInputMayNavigateThenObservesTheReplacement`,
`inputDeadlineSuppressesLateAcknowledgement`. The two prototypes
(`nativeWheelDeliveryPrototype`, `trustedInputPrototypeNotToolImplementation`)
were replaced by these tool tests. No UI smoke or visual parity run.

## Non-input browser landing (based exactly on `4b9218c`)

Separate worktree/branch `feat/browser-noninput-gaps` from
`4b9218c8ff04e4f8415f579823330ab0fc03850a`. Schemas, the input tools, the
reference tree and the browser-input worktree are unchanged. No CDP, debug
port, Rust, RPC or FFI. The `BrowserAutomation` boundary is kept; it only gains
`observeDownloads`, the engine's exactly-once download start/end report.

| Gap | Implementation (public Qt only) | Focused tests |
| --- | --- | --- |
| Tall / full-page screenshot | `QtBrowserAutomation::capture`: reference region (CSS viewport width × min(content, 4 viewports) from the origin), `pageHeight` = capture height, `truncated`, JPEG 82 ≤1280. A tall or scrolled page: freeze the slot with a non-live `ShaderEffectSource` copy, scroll to the origin (isolated world), give the guest the capture height (`BrowserGuest.stretchCapture`), wait until the renderer reports that size and two animation plus two window frames, grab, then unstretch, restore the scroll in the same document only, and uncover | `fullPageFromTheTopWithoutDisturbingThePanel` (band pixels, visible panel sampled mid-capture, size/scroll/cover restored) |
| Off-viewport capture | The grab renders the guest item alone, so pixels beyond the viewport and guests under app chrome (a closed panel under the chat card) are captured; nothing above the guest is | `coveredGuestCapturesOnlyItsOwnPixels` |
| Cancellation / page change / renderer loss | `cancel` restores (not just drops completion); every step is document-guarded and answer-bounded (2 s); a destroyed guest finishes the capture; the owner's 12 s capture deadline cancels | `fullPageCancellationPageChangeAndRendererLossRestore` (cancel before and after stretch, navigation mid-capture → `stale_page`, renderer kill) |
| Bounds | 16384 px per edge (device-independent) and 64 Mi CSS pixels, else `unavailable`; output ≤1280 wide | — |
| Popups / new windows | `BrowserGuest.onNewWindowRequested`: popups → `BrowserPopup.qml` (520×700, `openIn`, same profile); tabs → `Browser::openFrom` next to the opener (background kept in background); other schemes refused | `guestOpenedTabsFollowTheirOpener`, `pageMenuPopupsAndGuestOpenedTabs` |
| Page context menu | `Browser::menu` + Qt Quick Controls `Menu`; actions via engine web actions; Inspect opens in-process DevTools for the user only | `pageMenuMatchesTheReference`, `pageMenuPopupsAndGuestOpenedTabs` |
| Sign-in observation | preload check in ApplicationWorld, reporting through a `WebChannel` limited to ApplicationWorld; host-only, ≤30, saved, unverified | `signInHintsAreHostOnlyBoundedAndSaved`, `signInHintsComeOnlyFromTheIsolatedObserver` |
| Downloads + attribution | profile `downloadRequested` → engine report → `Browser` picks a unique file, attributes it to `BrowserTools::running(tab)`'s operation, records completed ones per guest, toasts; turn end releases ownership | `downloadsBelongToTheStepRunningAtTheirStart`, `downloadsSavedUniquelyAndAttributedToTheirStep` |
| Cursor / ripple overlay | **Not done:** the reference drives it only from click/type pointer events, i.e. the input tools | — |

Page-observable difference left in T08: the temporary resize and the scroll to
the origin and back fire resize/scroll events and briefly change viewport
units. `browser_navigate` to a URL that becomes a download is not specially
mapped (native settles on the unchanged page; reference `loadURL` rejection
behavior was not re-qualified here).

Validation for this landing (build directories under `~/.cache`, not the worktree):

```sh
cmake -S . -B ~/.cache/og-noninput/on -DCMAKE_BUILD_TYPE=Release -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build ~/.cache/og-noninput/on --target openghost-cpp native_browser_test \
  native_browser_operations_test native_browser_automation_test native_contract_test -j8
ctest --test-dir ~/.cache/og-noninput/on \
  -R '^native_browser(_operations|_automation)?_test$' --output-on-failure
~/.cache/og-noninput/on/native_contract_test hostToolsRoutedAndReleased \
  hostHandBackRejectsAlreadyQueuedMessage browserToolFixtures browserResultFixtures
cmake -S . -B ~/.cache/og-noninput/off -DCMAKE_BUILD_TYPE=Release \
  -DOPENGHOST_BUILD_SMOKE_TEST=ON -DOPENGHOST_BROWSER=OFF
cmake --build ~/.cache/og-noninput/off --target openghost-cpp native_browser_test \
  native_browser_operations_test native_contract_test -j6
ldd ~/.cache/og-noninput/off/openghost-cpp   # no WebEngine
git diff --check
```

Results (QtTest passes, including init/cleanup): ON `native_browser_test` 15,
`native_browser_operations_test` 10 and `native_browser_automation_test` 19 (the full-page and full real-guest
suites were repeated 3–5 times without failure); 25 selected contract cases
pass in each of ON and OFF; OFF browser/owner tests 15 and 10 pass with no
WebEngine linked. The UI smokes (run, not the visual parity suite) fail the same
five browser checks as an untouched build of `4b9218c` (see the audit's
non-input section: `data:` pages never become DOM-ready); no new smoke failure.
Unchanged Node oracles: `tests/browser-tools/reference.test.cjs` 27 and
`reference/openghost/test/browser-lifecycle.test.js` 16 pass.

## Native automation landing (based exactly on `ce4cbce`)

Historical record of `4b9218c`; its input rows are superseded above.

Current implementation supersedes the **historical preparation baseline** below.
The separate `feat/native-browser-automation` worktree started at
`ce4cbcec65636679949668be3f5a0e50aabab63c`. Only this document, the frozen fixtures,
and the contract-test additions were reused from `prep/browser-tools-port`
(`0c41d69`); its older production files were not merged. No reference files,
plugin work, Rust/RPC/FFI or visual-parity fixtures were changed.

### Reachable tools

Seven unchanged prepared schemas are published when the real adapter is composed;
`host.browser.available` is true. Four input tools remain unpublished. This is a
native host implementation, **not a connected agent/backend qualification**.

| Tool | Current support | Gate |
| --- | --- | --- |
| `browser_snapshot` | Working viewport/full DOM heuristic, persistent same-page refs, masks, same-origin frames, open shadow roots and bounds | Real isolated-world guest tests |
| `browser_tabs` | Working list/new/switch/close, stable handles, capacity refusal; switch SS and new+URL SS+tabs | Real guest + owner tests |
| `browser_navigate` | Working URL/local/search normalization, native history/reload, load/failure/stop, settled SS | Local files and loopback failure tests |
| `browser_wait` | Working timed/text/open-shadow waits, quiet settling and `wait_timeout` | Real guest + late-callback tests |
| `browser_read` | Working capped frozen HTML-derived text, exact reference converter, UTF-16 slices, read identity/expiry | Real DOM parser, 4 Mi cap, surrogate and frozen-read tests |
| `browser_screenshot` | **PARTIAL:** real guest-only viewport JPEG (82), width ≤1280, scale/size metadata; `full_page` also works when the page fits at the top | Tall/scrolled full-page requests explicitly return `unavailable`; never resize/scroll/stitch and claim reference capture. *Superseded by the non-input landing above.* |
| `browser_scroll` | **PARTIAL:** reference `ref` reveal with required pageId, stale-ref refusal, settle and dirty identity | Direction/amount native wheel is explicitly `unavailable`; no `scrollBy` or synthetic wheel fallback |
| `browser_click`, `browser_press`, `browser_type`, `browser_select` | Not implemented or advertised | Target/occlusion/focus/sequence qualification remains; no fake DOM input |

### Ownership and least-powerful mechanisms

- `src/frontend/browser_automation.h`: narrow browser-owned engine seam. Its
  operations are observation, native navigation, capture and wheel—not CDP
  commands. `BrowserTools` owns serialization, receipt-time targets, active-tab
  leases, page/document/ref/read versions, cancellation and results in QtCore.
- `src/browser/qt_browser_automation.*`: public Qt implementation only. The QML
  guest uses native URL/history/reload/stop/lifecycle APIs; `runJavaScript` and
  document-creation scripts use **ApplicationWorld (1)**. No WebChannel/app
  object is exposed to page scripts. Qt world IDs differ from Electron's 1077.
- `src/browser/observe.js`: adapted directly from the frozen reference's
  observation and HTML conversion functions, not a new schema or `innerText`
  approximation of read. Removed unimplemented point/choose dispatch. Qt does
  not await JavaScript promises; mutation quiet is polled from the isolated
  observer (300 ms quiet, 2 s cap).
- DOM readiness is distinct from full loading. A per-document isolated marker
  is installed at creation; polling reads `document.readyState` through Qt,
  including a test with a deliberately unfinished image response. Guest
  incarnation, native navigation/commit/history revisions and this document
  marker all guard completion. A replacement document cannot execute a delayed
  ref reveal. Screenshots revalidate the document after the native grab too.
- Bounds: 90 s receipt/queue/readiness budget, 75 s page-operation budget after
  readiness, 12 s JS/capture call, 30 s navigation, 15 s readiness/loading settle;
  waits cap at 60 s. Host scheduling uses elapsed time; dispatched JS additionally
  checks an absolute expiry. These are cancellation barriers, not renderer CPU
  quotas or a promise to undo an already-issued navigation/reveal.
- Turn end, reverse cancellation, tab loss, renderer loss, changed targets and
  deadlines prevent further dispatch and suppress late completion. A cancelled
  queued job cannot free its predecessor. Take Control holds all affected calls;
  Hand Back substitutes current-page `{}` snapshots, never the old operation.
  A queued user message releases only its session's held calls with empty
  cancelled content and `reason: message`. Turn-end/abort is empty cancellation.
- Chat retains spent tool-call IDs for the turn and answers reverse cancellation
  once. **Early calls before accepted turn identity are still refused**, rather
  than held as the reference does (audit H03 remains PARTIAL). No new transport.
- Frozen text/ref/page/operation state is ephemeral and never serialized into
  browser layout or chat display. Download production/attribution and sign-in
  hints remain absent (B14/B17/H09); snapshots truthfully return no downloads.
  *(Since landed: see the non-input landing above.)*
- `OPENGHOST_BROWSER=OFF` composes `NoHost` and links no WebEngine. The QtCore
  abstraction/owner tests still build; engine/resource implementation is ON-only.

### CDP decision and focused public-input prototype

**No CDP endpoint, remote-debugging flag, client or port is enabled. A requirement
for CDP has not been established.** `trustedInputPrototypeNotToolImplementation`
shows public Qt window mouse/key delivery produces trusted click, keydown and
input events. JavaScript `.click()` produces untrusted events. This proves a
public path exists, not that click/type/press safety and Unicode insertion are
finished. Reference select itself uses isolated DOM assignment plus untrusted
input/change events; it is not evidence that select needs CDP.

`nativeWheelDeliveryPrototype` records the narrower current limitation: direct
Quick-item pixel-wheel delivery does not reach the renderer; window-level mouse
wheel delivery is trusted but hits the app shield when covered, and this Qt
6.11.2 route converts an angle delta of 120 to 60 CSS pixels despite a supplied
480-pixel delta. Arbitrary hidden/covered guest targeting and exact deltas are
not qualified. This is **not proof all public Qt input paths are impossible**.

`QQuickItem::grabToImage` produces verified viewport pixels, not off-viewport
content. A tall full-page capture from the top without scrolling/resizing remains
unavailable. *(The non-input landing above implements it with a temporary,
restored resize under a frozen panel copy; no CDP.)* Scroll stitching, printing, or resizing would change fixed/sticky
layout, scroll callbacks or print styles. No such substitute is labelled parity.
If future work proves CDP necessary for this gap, it must remain an internal
`BrowserAutomation` implementation; this landing does not open a debug endpoint.

### Focused validation for this landing

Build directories are outside the worktree. Release application builds were
performed with both ON and OFF; `ldd` confirms no WebEngine in the OFF binary.
Only browser-focused tests run, not UI smoke/visual parity or live providers:

```sh
cmake -S . -B /tmp/og-native-automation-on -DCMAKE_BUILD_TYPE=Release \
  -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build /tmp/og-native-automation-on --target openghost-cpp \
  native_browser_test native_browser_operations_test native_browser_automation_test native_contract_test -j3
ctest --test-dir /tmp/og-native-automation-on \
  -R '^native_browser(_operations|_automation)?_test$' --output-on-failure
/tmp/og-native-automation-on/native_contract_test \
  hostToolsRoutedAndReleased hostHandBackRejectsAlreadyQueuedMessage \
  browserToolFixtures browserResultFixtures
cmake -S . -B /tmp/og-native-automation-off -DCMAKE_BUILD_TYPE=Release \
  -DOPENGHOST_BUILD_SMOKE_TEST=ON -DOPENGHOST_BROWSER=OFF
cmake --build /tmp/og-native-automation-off --target openghost-cpp \
  native_browser_test native_browser_operations_test native_contract_test -j3
ctest --test-dir /tmp/og-native-automation-off \
  -R '^native_browser(_operations)?_test$' --output-on-failure
/tmp/og-native-automation-off/native_contract_test \
  hostToolsRoutedAndReleased hostHandBackRejectsAlreadyQueuedMessage \
  browserToolFixtures browserResultFixtures
ldd /tmp/og-native-automation-off/openghost-cpp
node --test tests/browser-tools/reference.test.cjs
node --test reference/openghost/test/browser-lifecycle.test.js
git diff --check
```

Final results: foundation **12 passes**, owner **9 passes**, real guest **13
passes**, selected browser contract cases **25 passes** in each ON/OFF build;
Node oracle **27 passes**, reference lifecycle **16 passes**. No skips in these
selected runs. Native suites cover real local guest behavior plus adversarial
delayed adapter callbacks. The original eleven placeholder skips were removed in favor of real
engine/owner assertions; unpublished tools are explicitly refused, not skipped
into a success count. GCC's two pre-existing Qt metatype warnings remain;
offscreen Vulkan/fontconfig/profile diagnostics are not suppressed. Intermediate
prototype failures informed the explicit wheel/full-page refusals above.

## Historical preparation: scope and landing boundary

Prepared from `cpp-native-extraction` HEAD
`fffa6ee231df6a12a98dfb69e0ba124c2f19601d`, in the separate
`prep/browser-tools-port` worktree. **Only `reference/openghost/` is the behavioral
source.** Native files are inspected solely to establish present contract coverage.
This map does not use another implementation as a specification.

No production browser host, panel, tool registration, Rust, RPC, FFI, engine,
network service or production dependency is added. The reference is unchanged.
The new fixtures are **test inputs**, not advertised native capabilities. All
11 native tools remain unimplemented. Do not turn the existing `NoHost` into a
mock success path or publish these schemas until the corresponding host exists.

This is a source-level implementation map, not real-browser qualification.
Reference implementation details win over summary prose, including the
asymmetries called out below. A behavior change needs a separate decision, not a
silent “cleanup” during the port.

### Source index

Reference paths in this document are relative to `reference/openghost/`.
Line numbers refer to the frozen tree at the base revision; symbols are the
stable lookup keys.

| Key | Exact source and responsibility |
| --- | --- |
| S | `host-tools.js:8–66`: `PAGE_CHARS`, `INPUT`, `fn`, `SCHEMAS`; all eleven advertised schemas |
| F | `host-tools.js:68–156`: `clean/code/flatten/readable/read/result`; text conversion, pagination, metadata allowlist, content blocks |
| P | `browser-panel.js:73–108,222–361`: restore/open/tabs, `createView`, `ensure` at 555, readiness and target ownership |
| Q | `browser-panel.js:579–641`: `run/giveBack/cancel`, cross-chat queue, receipt-time pin, focus and cancellation |
| T | `browser-panel.js:566–570,643–670,673–684`: `tabsText/tabsTool/snapshot` |
| H | `browser-panel.js:509–553`: `drive/take/handBack/waitForAgent/release/sync`; `chat.js:1217–1312`: `claim/onHostTool/awaitHandBack`; `chat.js:1349–1386`: `end` |
| D | `desktop/browser.js:28–280`: isolated-world `install`, DOM snapshot/ref/point/choose/reveal/quiet/has |
| L | `desktop/browser.js:284–324,360–382,432–489,727–762`: identity invalidation, guard checks, deadlines, loading, operation queue, active-target lease |
| A | `desktop/browser.js:496–725`: `state/pointer/mouse/keyOf/press/navigate/screenshot/act`, every page operation |
| B | `desktop/preload.js:32–37`; `desktop/main.js:111–128,238–240`: guarded run/cancel/shown bridge; not a native wiring proposal |
| U | `desktop/browser.js:326–359`: session, downloads and guest isolation; `desktop/browser-preload.js:1–19`: hostname-only submission hint |
| R | `docs/browser-host-tools.md`; `docs/backend-interface.md:626–695`; `chat.js::reconcile/recoverTurn/applyEvent`; `library.js::displayMessages` |

Inspected all of `host-tools.js`, `browser-panel.js`, `desktop/browser.js`,
`desktop/browser-preload.js`, the browser lifecycle contract, and the focused
reference lifecycle/cancellation/turn-identity tests. Main/preload/chat/backend
contract callers above were followed through admission, cancellation and result
settlement. Relevant tests are mapped at the end.

## 1. Exact public input schemas

Every tool is `{name, description, parameters}`. `parameters` has exactly:

```json
{"type":"object","properties":{"tabId":{"type":"string","description":"Stable tabId from a result or host.browser; otherwise the active tab is pinned at receipt."},"pageId":{"type":"string","description":"Page identity from a snapshot. Required for input; a changed page fails without input. Re-snapshot after navigation."}},"required":[]}
```

Merge the following properties into that `properties` object and replace
`required` with the listed array, in its listed order. This table specifies the
entire validation structure, not a stricter proposed API. Verbatim tool/property
**descriptions and full unfactored schemas** are frozen in
[`tests/browser-tools/schemas.json`](../tests/browser-tools/schemas.json), checked
by execution of reference `HostTools.schemas`.

| Tool | Additional properties (exact type/enum) | Exact `required` |
| --- | --- | --- |
| `browser_navigate` | `url:{type:"string"}` | `["url"]` |
| `browser_snapshot` | `full:{type:"boolean"}` | `[]` |
| `browser_click` | `ref:{type:"integer"}`, `x:{type:"number"}`, `y:{type:"number"}`, `double:{type:"boolean"}` | `["pageId"]` |
| `browser_type` | `ref:{type:"integer"}`, `text:{type:"string"}`, `submit:{type:"boolean"}`, `clear:{type:"boolean"}` | `["text","pageId"]` |
| `browser_select` | `ref:{type:"integer"}`, `option:{type:"string"}` | `["ref","option","pageId"]` |
| `browser_press` | `key:{type:"string"}`, `times:{type:"integer"}` | `["key","pageId"]` |
| `browser_scroll` | `direction:{type:"string",enum:["down","up"]}`, `amount:{type:"number"}`, `ref:{type:"integer"}` | `["pageId"]` |
| `browser_screenshot` | `full_page:{type:"boolean"}` | `[]` |
| `browser_read` | `start:{type:"integer"}`, `readId:{type:"string"}` | `[]` |
| `browser_wait` | `text:{type:"string"}`, `seconds:{type:"number"}` | `[]` |
| `browser_tabs` | `action:{type:"string",enum:["list","new","switch","close"]}`, `url:{type:"string"}`, `tab:{type:"integer"}` | `["action"]` |

There is **no** `additionalProperties:false`, `oneOf`, numeric minimum/maximum,
string length limit, schema default, or conditional required field. Descriptions
mention defaults; execution applies them separately. In particular:

- Click's ref-or-coordinate rule is semantic, not schema `oneOf`.
- Tabs switch/close require stable `tabId` **at execution**, not in `required`.
  `tab` is display position, never public executable targeting.
- Read continuation requires `readId` at execution when `Number(start)>0` or a
  truthy `readId` was supplied. `start=0` without it refreshes.
- Public `tabId` is not internal `args.tab` (numeric guest ID). The panel supplies
  internal `tab` and `operationId` after admission; do not expose that authority.
- The reference does not run a general schema validator inside these handlers.
  Below, explicit coercions/defaults describe direct-call behavior; they do not
  change the advertised types or authorize widening them in the native port.

## 2. Exact result families

### 2.1 Host result envelope and failure layers

`HostTools.result(args, answer)` returns the formatted body. `Chat.onHostTool`
adds `status`. The final semantic shape is:

```ts
{
  content: ({type: 'text', text: string} |
            {type: 'image', dataUrl: string, label?: string})[],
  isError?: boolean,
  status: 'ok' | 'error' | 'cancelled' | 'handed-back',
  reason?: 'message',
  data?: object
}
```

Normal formatter bodies always include `data:{...}`, even if empty; successful
ones omit `isError`, rather than explicitly setting false. Native `bool isError`
can carry the semantic value, but not absence. There is no native wire serializer
in this work; do not claim byte-exact wire coverage from the typed tests.

The **complete copied-key allowlist**, in order, is:
`code, tabId, pageId, readId, refs, tabs, truncated, coverage, scroll, width,
height, scale, pageWidth, pageHeight, downloads`. A key is copied whenever it is
not `undefined` (false, zero, empty collections and null are not dropped).
`text/image/html/url/title/stopped/taken/sourceTruncated` are not directly copied
into data. Read pagination adds `sourceTruncated` separately.

Branch order is significant:

1. Absent/falsy answer or truthy `answer.error` →
   `{isError:true, data:{...copied,code:answer.code||'browser_error'},
   content:[{type:'text',text:'Error: '+(answer.error||'the browser did not answer')}]}`.
2. Truthy `answer.image` → text block (`answer.text||''`), **then** image block
   `{type:'image',dataUrl:answer.image,label:'Screenshot of the built-in browser'}`.
3. `answer.html !== undefined` → one readable-text block and pagination data.
4. Otherwise → one text block (`answer.text||''`) and copied data. An empty object
   therefore produces empty text, not “browser did not answer.”

Status is `handed-back` if the handoff path was used, **even if the snapshot is an
error**; otherwise `error` iff `isError`, else `ok`. Stop, no live turn, request
abort or turn replacement bypass the formatter: `{status:'cancelled',content:[]}`.
A message releasing hand-back adds `reason:'message'`. `stopped:true` in a raw
bridge answer does not itself choose cancelled status.

Unknown tool/absent panel is an outer `unsupported` error. Unowned session is
`unknown_session`; mismatched/reused turn step is `stale_turn`. Browser-service
failures instead remain tool results. Typical `data.code` values: `unavailable`,
`timeout`, `wait_timeout`, `navigation_failed`, `tab_gone`, `guest_crashed`,
`stale_tab`, `stale_page`, `stale_ref`, `stale_target`, `element_covered`,
`stale_read`, `tab_limit`, `invalid_request`, `cancelled`, `browser_error`.
These are not all interchangeable, nor an exhaustive new enum.

**Error identities are conditional:** the panel adds `tabId` to a returned bridge
answer, including bridge errors. A failure thrown earlier in readiness/queue/tab
management need not have any ID. The main-process catch generally has no
`pageId`. Never fabricate current IDs to decorate an error.

### 2.2 Snapshot family (SS)

Success is one text block; data is:

```ts
{
  tabId: string, pageId: string,
  refs: {[decimalRef: string]: string},
  truncated: boolean,
  coverage: 'viewport-dom-heuristic' | 'full-dom-heuristic',
  scroll: {top:number,height:number,vh:number,vw:number,below:number},
  downloads: {file:string,at:number,operationId:string|null}[]
}
```

`tabId` comes from the panel and `pageId` from main `run`; `state` makes the rest.
Exact text assembly in `desktop/browser.js::state`:

- Optional note line (select/wait below).
- `Page: TITLE` (fallback `(no title)`), then `URL: URL`.
- If still loading: `The page is still loading.`
- If `height/max(1,vh)>1.05`:
  `Viewport VW×VH, scrolled P% of a page S screens tall.` (`S.toFixed(1)`;
  `P=round(top/(height-vh)*100)` only when `height-vh>4`, otherwise zero).
  Else: `Viewport VW×VH, the whole page fits on screen.`
- One `Downloaded: FILE` line per matching completed download.
- Blank line, then snapshot lines joined by newline, or
  `(nothing readable on screen)`.
- If `skipped>0`: `[… N more lines not shown. Call browser_snapshot with full true or scroll.]`
  for viewport; full uses `Scroll to them and take a snapshot` instead.
  Otherwise viewport with `below>8` appends `[More content below: scroll down to see it.]`.

No image, raw HTML, separate URL/title fields, or screenshot frame is implicit in
SS. Most action tools return SS, not a separate action success receipt.

### 2.3 Screenshot family (IMG)

Exactly two blocks as §2.1; data:
`{tabId,pageId,width,height,scale,pageWidth,pageHeight,truncated}`.
No refs, scroll, downloads or coverage are manufactured by this operation.

Text is `Screenshot of the viewport: TITLE_OR_URL, W×H; SCALE_NOTE.` or
`Screenshot of the page from the top: ...`. `SCALE_NOTE` is
`one screenshot pixel is one page pixel, so x and y for browser_click can be read from it`
if `abs(scale-1)<0.01`; otherwise
`to click by coordinates divide screenshot pixels by SCALE` (`toFixed(3)`).
Image label is exactly `Screenshot of the built-in browser`.

### 2.4 Read family (READ)

One text block; data:

```ts
{
  tabId:string, pageId:string, readId:string,
  start:number,end:number,total:number,hasMore:boolean,offsetUnit:'utf16',
  sourceTruncated:boolean,truncated:boolean,
  coverage:'html-derived; excludes form controls, shadow roots and iframe content; may include hidden text'
}
```

`from=max(0,floor(Number(args.start)||0))`, `part=text.slice(from,from+40000)`,
`end=from+part.length`, `total=text.length`, `hasMore=end<total`,
`truncated=sourceTruncated||hasMore`. These count **UTF-16 code units**, including
surrogates. Offsets beyond total are not clamped down; a boundary can split a
surrogate pair. Qt `QString` indexing is suitable; UTF-8 byte slicing is not.

Text is `URL + '\n\n' + (part || '(empty page)')`, followed, if `hasMore`, by:

```text


[Characters FROM–END of TOTAL. Call browser_read with tabId=JSON_STRING_TAB, readId=JSON_STRING_READ, start=END to read further.]
```

Then, if source was truncated, append
`\n\n[Source HTML truncated at 4 Mi UTF-16 code units.]`.
No image, refs, scroll, downloads or separate title/URL are returned.

### 2.5 Tab family (TAB)

A tab entry is **exactly**
`{n,tabId,state,loading,revision,title,url,active}`: one-based display `n`, stable
string handle, state string, loading bool, numeric renderer revision, title/URL
strings (blank URL becomes `''`), active bool. This is not the guest's pageId.

`tabsText()` is `No tabs are open.` when empty; otherwise one line per tab:
`N. TITLE_OR_NEW_TAB_OR_HOST` + optional ` (URL)` + optional ` active`.
No IDs are printed in these lines; they are in `data`.

| Action | Exact text / body family | Exact data additions |
| --- | --- | --- |
| list | `Tabs:\n` + tabsText | `{tabs}` only |
| new without truthy url | `Opened a new empty tab.\n\nTabs:\n` + tabsText | `{tabs,tabId}`; no pageId |
| new with truthy url | navigation SS | SS data **plus tabs** |
| switch | snapshot SS | SS data; **no tabs list** |
| close | `Closed. Tabs:\n` + tabsText | `{tabs}` only; no closed tabId/pageId |

The formatter can preserve tabs on an error returned by new+URL, but does not
synthesize a tab list for exceptions thrown before that response.

## 3. Shared lifecycle, frames, hand-back and persistence

These rules apply to **each of the eleven sections below**, with explicit tab-tool
exceptions. They are dependencies to implement/test, not production additions in
this change.

### C — Admission, queue, page lifetime and cancellation

1. Publish schemas only when browser bridge/host exists. Reference browser context
   is explicit even when panel closed or empty; null means **no panel**. Context:
   `{available,status,control,open,tabs,signedIn,signedInVerified:false}`.
2. `Chat.claim` waits for recovery/accepted turn identity, checks the named live
   turn and remembers `tool call TOOLCALLID` for the whole turn. The main and mini
   chat share the browser but have distinct step IDs. Approval is decided before
   the host call by the backend; no new frontend approval policy belongs here.
3. For page tools, panel `run` captures explicit tabId, else the active tab,
   **at receipt**. For `browser_tabs`, only an explicit tabId pins a target. It
   captures the target's renderer revision. Serialize tools/tab mutations across chats.
   A closed target → `tab_gone`; revision change while queued → `stale_page`;
   implicit active-target change → `stale_tab`. Explicit page tools select their
   pinned tab before running. They do not silently retarget a different guest.
   With no active tab, page-tool `ensure` can create a blank tab, not just navigate.
4. First DOM-ready: 15 s, failure/close/crash/abort reject. Failed guests are
   removed/recreated on a later readiness attempt; crash sets `gone`. Lazy tabs
   create a guest only when required. Readiness is not full document loading.
   After ensure, membership and active target are rechecked. Do not strengthen
   the source's comments into an additional revision check after readiness that
   the code does not perform; main page preconditions provide another barrier.
5. Renderer operation budget 90 s including queue; main guest operation budget
   75 s including its queue; CDP/JS calls 12 s; URL load 30 s; loading settle 15 s.
   Settle sleeps 120 ms (history/reload path uses 250 ms), waits for loading if
   necessary, then mutation quiet: 300 ms idle with a 2 s cap. Not network idle,
   DOM atomicity or proof a dynamic page is finished.
6. Main operation captures guest pageId and active-target lease; checks before
   dispatch and after awaits. Navigation start/commit/in-page main navigation
   revises pageId and clears frozen read. Input requires a truthy matching pageId;
   other **page** operations check it when supplied. `tabsTool` does not apply the
   optional pageId at all, even for switch/new+URL.
7. Input dispatch and point/choose/reveal JS mark the operation dirty. Returning
   state revises identity before snapshot; partial/cancelled dirty input revises
   in `finally`. This includes a ref point that only scrolled before refusing a
   covered element. A final input may navigate before acknowledgement; its
   release and observation may follow, not another action on the replacement
   document. Intermediate navigation must stop the remaining sequence.
8. Cancellation/deadline/crash/close releases waits and blocks late continuations
   from further dispatch. A cancelled queued entry retains its predecessor's
   queue barrier. `browser_navigate` failure/abort calls stop. Already-issued input, JS,
   navigation or download effects cannot be undone or certified “not started.”
   No tool has an automatic retry here.

### V — Screenshot versus iframe/frame behavior

There is no `frameId`, frame selector, frame attach API, video stream, or screenshot
on every tool result. All operations target one top-level guest. Input coordinates
are guest viewport CSS/page pixels as used by CDP, **not desktop/QML device pixels**.
Full-page screenshot coordinates start at page top; capture does not itself
scroll the viewport. The reference gives scale guidance but has no general
full-capture-to-scrolled-viewport coordinate remapping layer: do not invent one.

DOM snapshots descend accessible same-origin iframe bodies and open shadow
roots; inaccessible iframes get a `(content not readable)` line. Rectangles of
iframe elements are translated through frame offsets. Ref hit testing descends
open shadow roots and has an iframe-owner exception; this is a heuristic, not
complete cross-frame occlusion proof. Read excludes iframe/shadow contents.
Wait searches body text and nested open shadow roots, **not iframe documents**.
Screenshots capture painted guest pixels (including painted frame contents), not
structured cross-origin frame access. Every per-tool “no image” below means no
screenshot content block, not that a real page could not contain images.

### H — Shared hand-back behavior, all eleven tools

`drive` tracks the conversations using the browser. Take Control marks user
control, cancels running **and queued** panel jobs, and focuses the guest. Calls
already under user control do not execute their original operation. They wait
without a user-wait deadline. Hand Back releases waiters and lends focus back;
the interrupted call runs **`browser_snapshot` with `{}`**, on the then-current
active tab. Neither original tabId/pageId nor original arguments are retained for
that replacement. Even read/screenshot/tabs requests therefore return SS, not
their usual family, with `status:'handed-back'`.

- New user message while waiting → empty cancelled result with `reason:'message'`.
- Stop, reverse cancellation, turn completion/replacement → empty cancelled
  result; release every waiter, drop late result, no old action replay.
- A bridge result already delivered before Take Control keeps that real result;
  do not erase completed work just because cancellation follows.
- The snapshot after hand-back can itself fail, still `handed-back` plus isError.
- When the last driver ends, reset to agent and release waiters. Merely hiding the
  panel is not Take Control. Keyboard lending/giveBack is separate from ownership.

### P — Shared persistence and recovery, all eleven tools

Reference `browser-panel.js::save` writes `openghost.browser` with
`{open,width,tabs:[{url,title}],active}`. Only nonblank tabs are stored; active is
an index into that filtered list, clamped to zero. Restore at most 12 tabs lazily
with **fresh** UUID handles. No guest history stack, ref map, pageId/readId,
operation queue, pending action, hand-back wait or screenshot is persisted.
Writes catch storage errors; this is best-effort browser convenience state, not
a durable tool-effect journal.

`persist:browser` independently owns cookies/site storage. Account hints in
`openghost.browser.accounts` are hostname/time, most recent first, max 30,
`www.` removed; `signedInVerified` is always false. A password-form submission
hint is neither credentials nor proof of login. Never conflate it with provider
authentication or chat encryption.

Chat display recovery replays presentation/events, **not host-tool calls**.
`library.js::displayMessages` does not persist executable tool histories. The
frontend has no operation journal, exactly-once browser guarantee, crash-time
rollback, or resumable frozen read. A backend is responsible for its own durable
history and uncertain tool outcomes; this map introduces no backend replay policy.
Renderer/app reload creates fresh tab handles. A lost/recreated guest invalidates
page/read/ref state but can retain its renderer tab's handle; never rebind old
page/read/ref values to a replacement document. Merely hiding and reopening the
panel, or a backend handshake without guest replacement, does not itself clear
these identities in the reference. Always recheck live targets; never auto-resend
an interrupted mutation. Completed downloads remain host filesystem effects,
not automatically deletable on turn failure. Browser persistence failure must
not be called tool rollback.

## 4. Per-tool implementation cards

Each card supplies the exact input row in §1, output family in §2, source symbols,
C/V/H/P lifecycle applicability, current native coverage, and concrete missing
work. “N” below refers to the fully enumerated native contract coverage in §5;
it means representation/routing only, **not an implementation**.

### 1 — `browser_navigate`

- **Input:** §1 navigate: url required; optional tabId/pageId. Trim string URL;
  case-insensitive exact `back`, `forward`, `reload` select history actions.
  Otherwise preserve http/https/file/about/data; POSIX absolute path → `file://`;
  Windows drive path → `file:///` with slashes. Host normalizer recognizes
  localhost, 127.0.0.1, `[::1]`, IPv4 with optional port as http; dotted hostname
  without spaces as https; other words become Google `search?q=encodeURIComponent`.
  Empty URL is `browser_error`. Panel address-bar normalizer differs on `[::1]`;
  port the host path for this tool, not the toolbar helper by analogy.
- **Result:** viewport SS, no special navigation receipt or image. Back/forward
  with no history → `browser_error` (`There is no page to go ... to`). Rejected
  load/non-aborted failure → `navigation_failed`; deadline stays `timeout`.
- **Lifecycle/loading/cancel:** C; attach → navigate → settle → check recorded
  main-frame load failure → adopt new page identity → SS. History/reload use the
  loading settle path. Catch stops guest loading. No automatic retry.
- **Tabs/frames/screenshots:** V; absent active target can create a blank guest;
  explicit target selected, optional supplied pageId checked before navigation.
  Navigation revision invalidates old refs/readId; no cross-frame navigation arg.
- **Hand-back:** H, returns current-tab SS without replaying URL/history action.
- **Persistence/recovery:** P; URL/title can be saved by panel loading events;
  redirects, site effects and downloads may already have happened on failure.
  History stack/unfinished navigation is not durable/replayed.
- **Source:** S `fn('browser_navigate')`; A `act:606`, `navigate:560`, L
  `normalize:315/adopt`; P `ensure/run`; reference navigation failure tests.
- **Native coverage/missing:** N envelope/args/SS metadata fit unchanged. Missing
  normalization parity, real history/load/stop, load failure observation, deadlines,
  identity changes and SS observation. Implement after snapshot and tab owner.

### 2 — `browser_snapshot`

- **Input:** §1 snapshot; optional full boolean, tabId/pageId. `!!full` selects
  full DOM heuristic versus viewport; it does **not** request a screenshot.
- **Result:** SS with `full-dom-heuristic` or `viewport-dom-heuristic`. Nominal
  line-text budgets 40,000/9,000 UTF-16 units; 20,000 visited-element ceiling.
  `push` tests text length then counts newline too; not an exact serialized-result
  byte bound. Refs are inserted **before** a line may be dropped for budget, so
  refs need not be a one-to-one subset of printed lines. Truncated is skipped>0
  or visited>20,000, not proof that all other truncation was detected.
- **Lifecycle/loading/cancel:** C; attach and direct state snapshot, no preliminary
  settle. A loading page can yield a successful observation with loading text.
  Main navigation commit invalidates an in-load observation. Snapshot is not a
  transaction against concurrent page scripts.
- **DOM/frames/screenshots:** V; visible rectangle, display/visibility/opacity
  (>0.02), hidden/aria-hidden and skip-element heuristics. Roles/names/checked,
  expanded/current/disabled/focused and select options are described; password
  field value shown as `••••` if nonempty. Labels/text are individually cut;
  at most 15 select options shown. Same-page refs live in weak maps and are not
  reset just by another snapshot; navigation/dirty input installs a new map.
  No screenshot or accessibility-tree completeness guarantee.
- **Hand-back:** H, same tool but fresh `{}` viewport snapshot, not retained full
  flag/target. Can observe a different user-selected tab.
- **Persistence/recovery:** P; refs/page identity never saved/rebound. Reobserve
  after reopen or stale page; do not recover refs by matching label strings.
- **Source:** S snapshot; D `snapshot:153/roleOf/nameOf/describe/idOf`; A `state:496`,
  `act:622`; L `revise/world`.
- **Native coverage/missing:** N carries refs/scroll/coverage unaltered. Missing
  isolated observation, ref ownership, masking/label/text formatting, frame and
  shadow traversal, bounds and exact SS formatter. This is the first page tool.

### 3 — `browser_click`

- **Input:** §1 click; pageId required; ref or x/y semantic choice, double optional.
  A ref other than undefined/null/empty string wins over coordinates. Otherwise
  x/y are converted with `Number` and must be finite; no bounds/clamp for supplied
  coordinates. Invalid target message is `browser_error`, not invented validation.
- **Result:** viewport SS after input/settle. Initial covered ref →
  `element_covered` (`No click was sent.`); after pointer delay covered **or moved**
  → `stale_target`. Missing/disconnected ref → `stale_ref`.
- **Lifecycle/loading/cancel:** C; ref point scrolls to center/instant and chooses
  x near left-center (half-width capped 40), y center, clamps to viewport; covered
  check, pointer event, 420 ms delay only when shown guest matches open panel,
  check, re-point exact-coordinate/coverage check, mouseMoved, press/release pair(s).
  Double click uses count 1 then 2. Only final press is navigation-permissive;
  navigation on the first of a double click must not dispatch the second.
- **Tabs/frames/screenshots:** V; paired pageId/ref mandatory for safe input.
  Coordinate clicks do not run ref occlusion/movement validation. Pixels from IMG
  need scale conversion. Returns no image or new coordinate receipt.
- **Hand-back:** H, no old click/double-click replay.
- **Persistence/recovery:** P; click or ref's preliminary scrolling may have had
  effects before failure. No “exactly once” or automatic click retry.
- **Source:** S click; D `point:219/element/rectOf`; A `act:624`, `pointer/mouse`;
  reference covered/moved and final-navigation regressions.
- **Native coverage/missing:** N preserves ref/x/y/pageId/double JSON and SS. Missing
  hit testing, pointer delay/cancel rechecks, actual input/release, dirty identity
  handling and result observation. Needs snapshot before input implementation.

### 4 — `browser_type`

- **Input:** §1 type; text + pageId required, ref/submit/clear optional.
  Execution `String(text??'')`; clear defaults on unless **exactly false**.
- **Result:** viewport SS, not a separate inserted-text receipt (the snapshot can
  show non-password field values). Ref movement/coverage at second point →
  `stale_target` (unlike click's initial `element_covered` branch).
- **Lifecycle/loading/cancel:** C; optional ref point → pointer → re-point → mouse
  focus click → 80 ms sleep. No “must be input element” check here. Without ref,
  type at guest focus. If clear, Control+A, and Delete only for empty text. If
  nonempty, `Input.insertText`; if submit, 60 ms then Enter. Final insertion or
  delete (without submit), or submit Enter, may navigate; earlier changes stop
  subsequent dispatch. It uses insertText, not a per-character typing simulation.
- **Tabs/frames/screenshots:** V; no image. For ref-less input the panel restores
  lent guest focus before running; afterwards it returns focus to prior connected
  non-guest element when user does not own browser. Clearing is not platform-
  switched to Meta+A in this reference.
- **Hand-back:** H; must not reinsert secret/text or submit on hand-back.
- **Persistence/recovery:** P; field changes/submission may be partial. Do not save
  type arguments as browser restore state; site cookies/storage are separate.
- **Source:** S type; A `act:644`, `press:553`, L `command/world`; Q `keys/giveBack`.
- **Native coverage/missing:** N can carry Unicode/empty text and flags. Missing
  focus/ref handling, clear/append/delete/submit sequencing, intermediate barriers,
  dirty identity/SS. Fixture verifies Unicode survives the seam, not actual input.

### 5 — `browser_select`

- **Input:** §1 select: ref, option, pageId required, optional tabId.
- **Result:** viewport SS with first note `Chose "CHOSEN_TEXT".`. No special chosen
  value data field. Non-SELECT/no match → `browser_error` with explanation/options
  (up to 40 option texts); stale ref remains `stale_ref`.
- **Lifecycle/loading/cancel:** C; isolated `choose` checks connected ref and
  SELECT tag. Normalize desired text with whitespace collapse + lowercase;
  first find exact cleaned text or lowercased value, else text substring; first
  match wins. Focus, set value, set option.selected, dispatch bubbling input then
  change, return cleaned option text. Whole choose JS is navigation-permissive,
  then settle/SS. No disabled-option exclusion, multi-select-set API, separate
  pointer animation or rollback is present.
- **Tabs/frames/screenshots:** V; ref can belong to observed accessible iframe or
  open shadow root. No screenshot; custom menus must use clicks, not choose.
- **Hand-back:** H, no repeated input/change events or old option selection.
- **Persistence/recovery:** P; JS already issued can finish despite cancellation;
  selected state may have triggered site requests. Never replay from cache.
- **Source:** S select; D `choose:235/element`; A `act:667`, `afterInput`.
- **Native coverage/missing:** N fits string/ref and SS note. Missing DOM select
  operation/matching/errors and isolation/cancellation/identity. No new chosen DTO.

### 6 — `browser_press`

- **Input:** §1 press; key + pageId required, times optional. Execution count
  `min(20,max(1,round(Number(times)||1)))`. Schema is integer; direct calls round.
- **Result:** viewport SS. Empty/unknown key or modifier → `browser_error`.
- **Lifecycle/loading/cancel:** C; split `+`, trim/drop empty pieces; all but final
  are modifiers: alt=1, control/ctrl=2, meta/cmd/win=4, shift=8. Key aliases:
  enter/return, tab, escape/esc, backspace, delete, space, arrowup/up,
  arrowdown/down, arrowleft/left, arrowright/right, pageup/pagedown/home/end;
  otherwise one JS UTF-16 character only. Use table's key/code/virtual-key/text,
  not an OS-dependent guessed mapping. Control/Alt/Meta suppress text, Shift does
  not. Send keyDown if text, rawKeyDown otherwise, then keyUp. Check before each
  repeat; only last down is navigation-permissive. No F-key catch-all support.
- **Tabs/frames/screenshots:** V; focused guest, restores lent focus like ref-less
  type. No image/ref target. Stop during dispatch is not a promise that no down
  occurred or that every aborted sequence sent a release.
- **Hand-back:** H; no remaining repetitions on user's page.
- **Persistence/recovery:** P; shortcut effects may be partial; no key sequence
  is resumed after restart.
- **Source:** S press; `desktop/browser.js:17–25` KEYS/MODIFIERS; A
  `keyOf:533/press:553/act:672`; Q focus lending.
- **Native coverage/missing:** N fits key/count/SS. Missing exact mapping,
  dispatch/release and navigation-safe repeat boundaries. Implement the shared
  key primitive before type's Control+A/Delete/Enter sequence.

### 7 — `browser_scroll`

- **Input:** §1 scroll; pageId required. Ref (not undefined/null/empty string)
  wins. Otherwise amount=`min(10,max(0.1,Number(amount)||0.8))`; lowercase direction
  equal to up gives minus, everything else down (schema still restricts enum).
- **Result:** viewport SS with new scroll/ref observation, no separate wheel
  receipt or image.
- **Lifecycle/loading/cancel:** C; ref path runs `scrollIntoView` center/nearest/
  instant. Wheel path reads visual viewport metrics and sends mouseWheel at its
  center with deltaX=0 and deltaY=±clientHeight*amount. Sleep 250 ms then settle
  and state. Both paths dirty/invalidate the old page observation, even without
  navigation. Stale-ref/page/tab/timeout/cancel must block later dispatch.
- **Tabs/frames/screenshots:** V; top-level viewport wheel, not a frame argument
  or “scroll arbitrary container” selector. Ref reveal can act on observed nested
  element. No automatic screenshot of the destination.
- **Hand-back:** H; no remaining wheel/reveal after user intervention.
- **Persistence/recovery:** P; no saved scroll coordinate or ref restoration.
- **Source:** S scroll; D `reveal:251`; A `act:678`; L dirty state.
- **Native coverage/missing:** N fits optional selector/direction/amount and SS.
  Missing metrics, reveal/wheel host actions, bounds/settle/identity. Good first
  bounded input tool once snapshot is reliable.

### 8 — `browser_screenshot`

- **Input:** §1 screenshot; optional full_page/tabId/pageId, `!!full_page`.
- **Result:** IMG, not SS. Layout metrics give rounded viewport width, rounded
  viewport height or `min(content.height,viewportHeight*4)`. Full capture clips
  x=0,y=0,width=viewport width,height=capped height,scale=1 and enables beyond-
  viewport capture. It does not capture full content width. Capture PNG internally;
  resize to min(viewportWidth,1280) if needed; encode JPEG quality 82. Report
  actual encoded dimensions, scale=encodedWidth/viewportWidth, pageWidth=viewport
  width, pageHeight=**capture height**, not entire document height. Truncated only
  if full and content.height>capture height; viewport screenshot sets false.
- **Lifecycle/loading/cancel:** C; attach → metrics → capture → resize/encode;
  no pre-capture settle and no screenshot stream. Page precondition/lease/call
  deadlines still apply; no fake image on blank/destroyed capture failure.
- **Tabs/frames/screenshots:** V; pixels of guest only, not app chrome/Take Control
  overlay. No frame metadata. Image is a host-result block, **not an automatic
  chat screenshot card**, saved attachment or generic image-preview API.
- **Hand-back:** H; returns viewport SS instead of image, not a second screenshot.
- **Persistence/recovery:** P; no image cache/artifact or re-capture on recovery.
- **Source:** S screenshot; `desktop/browser.js:12` SHOT; A `screenshot:580/act:691`;
  F image result branch. Reference tests verify dimensions/truncation metadata.
- **Native coverage/missing:** N `HostToolResult::Image` + JSON metadata suffice.
  Missing real capture/crop/encoding/scale; typed fixture payload intentionally
  is not a decodable JPEG. Real pixels need later engine tests.

### 9 — `browser_read`

- **Input:** §1 read; optional start/readId/tabId/pageId. Fresh capture requires no
  truthy readId and no positive start (normally absent/zero). Positive start **or any truthy readId** invokes
  continuation: exact readId and matching current page required, else `stale_read`.
  A readId with start=0 rereads the old frozen snapshot; it does not refresh it.
- **Result:** READ. Fresh capture reads top-level `document.documentElement.outerHTML`
  (empty when absent), slices at 4*1024*1024 UTF-16 units, records URL/title/pageId,
  fresh UUID readId and sourceTruncated. Only one frozen read per guest. Subsequent
  slices reuse that HTML even if DOM changed; readable conversion runs again on
  the frozen HTML. Refresh, navigation/dirty input, crash/lost guest invalidate it.
- **Text conversion:** parse HTML inertly; remove script/style/noscript/template,
  svg/canvas/iframe/object/embed/link/meta/button/input/select/textarea. Prefer
  first article/main/[role=main], but fall back to body if cleaned text<200 units.
  Only on body fallback remove nav/footer/aside/[role=navigation]/[aria-hidden=true].
  Prefix cleaned document title as `# TITLE\n\n`. Flatten headings with # levels,
  list items with `-`, table cells with ` | `, block boundaries/newlines, pre/code
  fences (including line-class highlighters), image alts as `[image: ALT]`.
  HTTP(S) links use Markdown if nonempty label<90, not same-document fragment,
  and label!=URL; relative links resolve against captured URL. Final cleaning
  collapses spaces/tabs/NBSP and excess newlines, including in the assembled code
  text. This is not Markdown sanitization beyond the reference's rules.
- **Lifecycle/loading/cancel:** C; attach then bounded HTML JS or stored read;
  no pre-read settle, no guarantee computed visibility. Source truncation may
  cut markup mid-element. Parser behavior must be qualified separately; don't
  replace it with live innerText or form values for convenience.
- **Tabs/frames/screenshots:** V; no image, no refs; hidden text may be present,
  iframe/shadow contents and form controls excluded. `title` captured by host
  is not separately returned; converter uses HTML document title.
- **Hand-back:** H; returns fresh SS, not old readId continuation or READ fields.
- **Persistence/recovery:** P; frozen read is memory-only, not durable attachment
  or conversation snapshot. Restore must fail old read identities, not silently
  recapture under the same readId or continue at old offsets on different text.
- **Source:** S read; A `act:696`; F `code:72/flatten:88/readable:115/read:127`;
  L `revise`; reference frozen-source and UTF-16 regressions.
- **Native coverage/missing:** N generic data handles pagination and QString text.
  Missing bounded HTML capture, read ownership/invalidation, HTML parser and
  exact converter/pagination. New tests exercise slicing against a supplied
  text-only DOM fixture; they do not qualify a native parser.

### 10 — `browser_wait`

- **Input:** §1 wait; optional text/seconds/tabId/pageId. Seconds=
  `min(60,max(0.5,Number(seconds)||(text?15:2)))`: zero uses default, negative clamps
  to 0.5; truthy text selects text wait. No selector/URL/load-event wait schema.
- **Result:** viewport SS, with note `"TEXT" is on the page.` for found text;
  otherwise, on expiry, `wait_timeout` with
  `"TEXT" did not appear within SECONDS s.`. Timed sleep success has no note.
- **Lifecycle/loading/cancel:** C; text search lowercases string and checks body
  innerText, then children of open shadow roots including nested roots. Poll every
  400 ms until deadline; no-text sleeps in at-most-250 ms chunks. After success,
  settle then snapshot. Overall/JS timeout differs from text-not-found expiry.
  Page changes during waiting fail barriers rather than rebinding the wait to
  another document. No “wait timeout succeeded” result.
- **Tabs/frames/screenshots:** V; no iframe traversal in `has`, no screenshot.
- **Hand-back:** H; not a resumed countdown/text search; current SS is returned.
- **Persistence/recovery:** P; deadlines/waiters are ephemeral, not restarted on
  recovery. Observations may be stale immediately after result.
- **Source:** S wait; D `has:267`; A `act:705`; L `sleep/timed/settle`.
- **Native coverage/missing:** N text/seconds and SS/error fit. Missing cancellable
  scheduler, text/open-shadow observation, expiry codes and settle. Test early,
  since it exercises ownership without requiring mutating input primitives.

### 11 — `browser_tabs`

- **Input:** §1 tabs; action required in schema, execution defaults falsy action
  to list. Unknown action → `invalid_request`. switch/close require tabId; numeric
  public tab alone → `invalid_request`. A truthy unknown tabId fails `tab_gone`
  even on list/new because panel run resolves explicit targets before tabsTool.
- **Result:** exact TAB action table §2.5. Crucial: empty new/list/close have no
  pageId/refs/image; switch returns SS **without tabs**, new+URL SS **with tabs**.
- **Lifecycle/loading/cancel:** C queue but panel-owned, not a main browser `act`
  case. list needs no guest. new at 12 → `tab_limit`, no eviction; below capacity
  create blank active tab. Without url return immediately, often still lazy.
  With url ensure blank guest, recheck active/cancel, invoke navigate. switch
  selects pinned target, ensures readiness, rechecks, invokes snapshot. close
  removes pinned target; if active, select nearest remaining index or none.
  A cancelled new+URL may leave a newly created tab but must never navigate after
  late readiness. Do not mistake cancelled for “no tab created.”
- **Tabs/frames/screenshots:** V; tab handles distinct from display index and guest
  ID. Optional pageId ignored here and not forwarded by new/switch. Listing is
  tab state/context, not a page/frame inventory. Tool creation refuses capacity;
  **manual** `addTab` can evict an older non-active tab above 12—different policy.
- **Hand-back:** H; even list/new/close becomes a current-page SS; old mutation is
  not retried. Hand-back observation can itself create a blank tab via ensure.
- **Persistence/recovery:** P; panel saves URL/title/selection for nonblank tabs.
  Empty tabs omitted, handles regenerated, closed tab cannot be recovered by n.
  Tab creation/closure/selection isn't a journaled transaction or crash rollback.
- **Source:** S tabs; T `tabsTool/tabsText/snapshot`; P `addTab/newTab/select/close`;
  Q queue, not `desktop/browser.js::act`.
- **Native coverage/missing:** N tab context and JSON TAB results suffice. Missing
  real shared tab owner/list/limits/select/close/readiness. Land list/new-empty/
  close ownership first, then switch/new+URL after snapshot/navigation dependencies.

## 5. Historical preparation: native contract coverage and missing shared pieces

Verified at the base revision (none of these production files changed here).

| Native source | Already supports | Not covered / needed next |
| --- | --- | --- |
| `src/backend/types.h:134–163` `BrowserState/HostContext/HostToolSchema` | All context statuses, tab IDs/revisions, unverified sign-in hints, optional absent browser, arbitrary schema JSON | Actual context producer, tab model, schema implementation/availability selection, browser persistence |
| `src/backend/types.h:396–414` `HostToolRequest/HostToolResult` | All eleven input objects, text/image content, status/reason, optional data for every result family | No formatter, validation, parser, input executor, deadlines, queue, screenshot, download/read state; bool does not retain absent isError |
| `src/frontend/host.h` `HostServices` | Asynchronous run/finished, cancel, tools, browser and browserChanged seam; no boundary type change required for mapped envelopes | Native host, local tab/control/focus/driver/readiness/download/pointer APIs; cancellation exceptions to exactly-once completion need an explicit owner, not detached continuations |
| `src/frontend/chat_service.cpp:272–299,343,866,1470,1803–1834` | Published schemas forwarded; start/steer context forwarding; live session/turn and in-flight duplicates checked; result passed through once; late callback dropped | Early host requests are refused, not held for start/recovery like reference; finished/cancelled IDs are removed, so completed toolCallId duplicates are not retained for whole turn; no hand-back orchestration |
| `src/frontend/chat_service.cpp:1427–1443` `cancelHost`, constructor reverseCancelled/closed handlers | Cancels outstanding injected work on stop/turn end/disconnect; suppresses late callback | End result currently isError + text `Cancelled: the turn ended.` instead of reference empty content. reverseCancelled drops without response (reference handler settles cancelled while connected). Missing/unowned/stopped cases in reverse currently collapse to stale_turn, unlike reference distinction. These are explicit future orchestration tasks, not fixed in this prep |
| `src/window.cpp`, `src/main.cpp`, `NoHost` | Shipped composition publishes no browser tools; null browser honestly means absent | No real engine/panel host. FakeBackend is not browser implementation and refuses browser context. Do not wire fixtures into either composition |
| `tests/contract.cpp::hostToolsRoutedAndReleased` plus new fixture tests | Injected-only admission/result forwarding/cancel/late-result checks; all eleven exact schemas and representative content now traverse native semantic seam | No tool execution, refs, page safety, persistence or browser effects qualified. Generic data capacity isn't parity |
| `src/frontend/library.cpp::displayMessages`, ChatService recovery | Display reconstruction does not dispatch cached browser calls | Browser restore storage absent; do not add tool replay to “complete recovery” |

Suggested **local** responsibilities once the production host exists (not new
protocols or classes mandated by this map):

1. Host composition/availability and stable tab owner + persistent browser profile.
2. Operation owner: cross-chat queue, receipt-time targets, page/ref/read leases,
   cancellation/deadlines and late-continuation barriers; all work stays owned.
3. Observation and result formatting library, engine-independent where feasible.
4. Input primitives and engine observation/capture behind that owner.
5. UI ownership/focus/hand-back and ChatService admission fixes, jointly tested.
6. Browser convenience persistence/context reports/download notifications, never
   a substitute for backend durable history. Only completed downloads from the
   initiating guest **and operation at download start** appear in snapshot data;
   late completion may still toast, but a later tool must not claim that download.

Guest isolation/ownership and permission behavior are host prerequisites (U):
reference guests have separate `persist:browser`, app-sender checks, context
isolation, sandbox, no Node/subframe Node, web security, no insecure mixed content,
and isolated observation world 1077. Allowed site permissions are only
clipboard-sanitized-write/fullscreen/pointerLock. This document does not select a
native engine or weaken the current production dependency policy.

## 6. Historical preparation: test scaffolding and qualification gates

### Added without an engine

| File | What it proves / what it does not |
| --- | --- |
| `tests/browser-tools/schemas.json` | Full exact reference schemas including descriptions/order; not registered in production |
| `tests/browser-tools/results.json` | Literal raw-answer → formatter expectations: SS metadata, IMG order/scale, TAB variants, errors/absence, UTF-16 READ; deliberately opaque non-JPEG image marker |
| `tests/browser-tools/cases.json` | One named schema-valid sample per tool and concrete missing-host assertion checklist. Shared SS examples prove envelopes, not each action's exact DOM output; full snapshot/select have explicit caveats |
| `tests/browser-tools/reference.test.cjs` | Executes unchanged reference schema/formatter and chat hand-back for all 11 tools. Asserts no old arguments/target/action after hand-back; abort/message dispatch nothing. Read stub supplies text-only DOM to qualify UTF-16/pagination, not parsing, isolation or engine behavior |
| `tests/browser-tools/fixtures.h`, `tests/contract.cpp::browserToolFixtures/browserResultFixtures` | Exact schema publication and request fields through existing ChatService to passive injected host; exact result metadata/Unicode/images/status forwarded, duplicate completion dropped. Status matrix is envelope capacity, not synthetic production outcomes |
| `tests/contract.cpp::productionBrowserToolsPending` | Eleven **explicit skipped** qualification rows, each naming unavailable host behavior from cases.json. No expected-failure blanket, fake browser fallback or passing execution claim. Replace each skip with real host assertions when it lands; skips do not auto-detect future host availability |

The Node oracle tests are development-only; they neither install dependencies nor
launch Electron. Native CMake/build dependencies are unchanged. No production
source or root CMake edits were needed.

### Existing reference tests to port, not mislabel as native tests

| Reference test | Required native regression gate |
| --- | --- |
| `test/browser-lifecycle.test.js` discovery/readiness/recreation | absent vs empty context; close/initial failure/crash/timeout/cancel; fresh guest after crash |
| Same: serialized calls/tab mutations | target pinned before queue; switch/close/unknown handle/positional tab refusal; cancelled queue retains predecessor; limit 12 no eviction |
| Same: stale page/covered/moved/final ack | zero further input on stale/covered/moved/navigation-delay target; final input may navigate; repeated press cannot target replacement page |
| Same: timeouts/reads/downloads | late timed-out CDP can't dispatch next action; stop navigation; wait_timeout; frozen HTML expiry/cap; guest+operation-scoped download metadata |
| `test/cancellation.test.js` | Stop all running/queued calls; reverse cancel just one; main/mini unique IDs; Take Control before/during action; already-delivered result retained; all hand-back waiters released |
| `test/turn-identity.test.js` | wrong/stopped/unowned turn; early identity wait; duplicate step lifetime; terminal cancellation drops late result |
| `test/untrusted-text.test.js` | URL/title/error labels remain text in native chrome; do not interpolate page text into QML markup |
| `test/e2e/browser-lifecycle.mjs` | Later real host: local pages only; actual ref click/no wrong click, frozen slices, navigation invalidation, occlusion, pixel metadata, tab switch during pointer, navigation/wait failures. Not run by this preparation |

Additional per-tool host gates are in cases.json and the eleven cards. Prioritize
negative/partial-effect cases alongside happy paths: no rollback claim after a
new tab, focus click, insertText, select JS or navigation already occurred.
Read/parser fixtures should later add headings/lists/code/tables/link/title,
hidden content, excluded controls, same/cross-origin iframe and open/closed shadow
root cases with a real DOM oracle. Pixel capture needs a real engine; a dummy
base64 block cannot qualify it. File persistence/recovery tests must restore fresh
handles and refuse old page/read/ref IDs without action replay.

### Commands and run boundary

From this worktree's root:

```sh
node --test tests/browser-tools/reference.test.cjs
node --test reference/openghost/test/browser-lifecycle.test.js \
  reference/openghost/test/cancellation.test.js \
  reference/openghost/test/turn-identity.test.js \
  reference/openghost/test/untrusted-text.test.js

cmake -S . -B /tmp/og-browser-tools-build -DCMAKE_BUILD_TYPE=Release \
  -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build /tmp/og-browser-tools-build --target native_contract_test -j2
ctest --test-dir /tmp/og-browser-tools-build -R '^native_contract_test$' --output-on-failure
# Optional existing UI regression checks (not browser qualification):
cmake --build /tmp/og-browser-tools-build --target openghost-cpp -j2
ctest --test-dir /tmp/og-browser-tools-build -R '^native_(fake_)?ui_smoke$' --output-on-failure
# Run the binary directly to see each of the 11 QSKIP reasons.
/tmp/og-browser-tools-build/native_contract_test productionBrowserToolsPending

git diff --check
```

New browser tests use in-memory stores/injected fakes; the existing full native
suite also uses temporary file stores, never real profiles, cookies, providers
or external sites. Node/reference success is an oracle check; native fixture
success is contract plumbing. Neither changes the **0/11 native implementations**
status. Skipped qualification is not passing qualification.

### Validation recorded for this preparation

- New reference oracle: **27 passed**; four unchanged focused reference suites:
  **38 passed**. No Electron/real guest launched.
- Release `native_contract_test`: **54 passed, 0 failed, 11 explicitly skipped**.
  Build used Qt 6.11.2/GCC 16.2.1; it emitted the two already documented
  `moc_backend.cpp`/Qt metatype `-Wmaybe-uninitialized` warnings. No suppression.
- Release application built. `native_ui_smoke` passed. The first combined CTest
  run had a **segfault in `native_fake_ui_smoke`**; a focused rerun passed.
  Production/UI/smoke sources are unchanged from the base. The intermittent
  failure is **not diagnosed or fixed here**; do not report a clean full-suite
  first run or infer real browser/backend qualification from the rerun.
- `git diff --check`, local Markdown links/fences, eleven per-tool cards and JSON
  parsing checked. Source scope excludes `src/`, `qml/`, root CMake and the entire
  reference tree.

## 7. Preparation's implementation order (retained as the roadmap)

Before enabling any tool, land the real host's isolation, tab owner, shared queue,
identity/cancellation/deadline barriers and H hand-back lifecycle. Observation
results and errors must be truthful from the first enabled capability.

1. **browser_snapshot** — SS formatting/ref identity is the foundation for all
   input and every hand-back result.
2. **browser_tabs** — shared target owner; list/new-empty/close can precede step 1,
   but switch requires snapshot. Finish new+URL with step 3.
3. **browser_navigate** — load/history/stop/failure/commit identity and downloads.
4. **browser_wait** — qualify polling, deadlines and cancellation without input.
5. **browser_read** — independent frozen HTML/text/UTF-16 service after navigation
   invalidation exists; converter work can proceed in parallel without an engine.
6. **browser_screenshot** — exact crop/resize/coordinate scale before coordinate
   clicking is advertised.
7. **browser_scroll** — first bounded input, dirty observation/settle/ref reveal.
8. **browser_click** — ref/coordinate input, coverage/movement/pointer delays and
   final-navigation handling.
9. **browser_press** — key/release/modifier/count semantics; shared key primitive
   can be developed alongside click and is required by type.
10. **browser_type** — combines focus/click, keys, text insertion, submit and
    multi-stage cancellation; highest partial-input risk.
11. **browser_select** — DOM-mutating choice/matching/events, explicit independent
    tests; may be developed in parallel once refs/identity exist.

Order is dependency/risk guidance, not a claim of extra reference capabilities.
For each landing: replace its pending skip with real assertions, retain the exact
schema/result oracle, test H/C/P failure paths, and only then advertise the tool.
Do not solve remaining host gaps by connecting a backend or implementing RPC/FFI.
