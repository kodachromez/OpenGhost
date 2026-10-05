# Fresh visual parity audit

2026-10-05 · `cpp-native-extraction` · before backend integration

## Verdict

**Not pixel-exact; not visually qualified for release.** The native port is close
in many retained text/diagram views, but has substantial presentation gaps and
unported reference views. Passing the contract/UI smokes never established parity.
This audit measures those differences rather than accepting them under a tolerance.
No product feature, Rust backend, RPC transport or FFI integration was added.

The frozen `reference/openghost/` was not edited. Both sides were freshly rendered;
no old screenshot was used as a baseline. The audited source starts at
`37c5da5937d37408b6154a5fc6874d0f48d456be`, with the changes in this audit commit.
Reference tree SHA-256 (relative paths plus bytes, as computed by the runner):
`5fa789f929814738e7de7d9bea3473350ac66ce54f03b021df4e717fad5b1e6f`.

Final run:

| Result | Count |
| --- | ---: |
| Inventory | 162 |
| Fresh paired headless captures | 156 |
| Exact full-window RGB matches | **0** |
| Differing full-window pairs | **156** |
| Manual/visible cases explicitly not run | 6 |
| Headless effort captures with an additional GPU qualification limitation | 4 |
| Fixtures with any nonzero repeat difference on either renderer | 143 |
| Fixtures with a repeat difference exceeding 8 in any channel | 13 |

The last two rows are diagnostics, **not alternative pass thresholds**. Of 156
repeat pairs per renderer, 132 reference pairs and 14 native pairs were exact.
Much native repeat variation is only 1–2 channel levels in gradients/effect edges.
The 13 larger repeat cases include startup/idle motion, effort-high, scroll-middle
and several reference settings/auth transitions. Animation phase is not locked.
These results do not certify animation timing, GPU equivalence or input behavior.

## Desktop-safe reproduction

From the repository root:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build build-release --parallel 4
python3 tests/parity/run.py
```

That last command is the **default full parity command**. It writes
`build-release/visual-parity/`; it does not open the report or a desktop window.
For the recorded audit:

```sh
python3 tests/parity/run.py --strict --output build-release/visual-parity-final
```

Exit 0 without `--strict` means capture/measurement completed, **not parity**.
Capture failure exits nonzero. `--strict` exits 2 for any differing pair, unstable
repeat, limited capture or outstanding manual case. The final strict run completed
all captures and exited **2**, as it should. No comparison tolerance was loosened.

Prerequisites, all test-only:

- The Release smoke-enabled binary; Qt 6.11 with its offscreen platform and OpenGL.
- Python with Pillow and NumPy; `fontconfig`/`fc-match`, Noto Sans and Noto Sans Mono.
  Native generic monospace must resolve to Noto Sans Mono; the capture refuses a
  mismatched UI/monospace family. Font paths and SHA-256s are recorded.
- Node with built-in `WebSocket` (tested with 26.10.0), and a Chromium-family
  browser supporting modern headless/CDP. `PARITY_BROWSER` selects its executable;
  the tested default is `/opt/brave-bin/brave`.
- Xvfb with GLX. Use installed `Xvfb`, or `PARITY_XVFB=/absolute/path/to/Xvfb`.
  A local `build-release/parity-tools/usr/bin/Xvfb` is also recognized. This audit
  used a locally extracted CachyOS `xorg-server-xvfb-21.1.24-1.1-x86_64_v4` package;
  no system installation is performed by the harness. Missing support is a hard
  failure, not permission to launch on the desktop.

Safety/ownership details:

1. One strictly headless browser, one reused tab, fresh navigation for each case.
   `--headless=new` and `--ozone-platform=headless` are unconditional. Both desktop
   display variables are cleared for the browser. There is no Electron launch.
2. Qt always uses `QT_QPA_PLATFORM=offscreen`, never xcb/Wayland windows. This Qt
   plugin could not create a true surfaceless OpenGL context, so the runner owns
   one private, memory-only Xvfb (`-displayfd`, `-nolisten tcp`, no window manager).
   It never connects to the user's display. OpenGL is asserted: a shaderless Qt
   software-scenegraph fallback would invalidate these captures and is refused.
3. Browser capture finishes before Qt starts. Qt reuses one existing application
   window for each DPR group (1×, then 2×), not one process/window per fixture.
   Renderer commands have deadlines and owned process groups; interruption/error
   terminates the group. The private display is reaped in `finally`.
4. Temporary HOME/config/data/cache/runtime/browser profiles; no real credential,
   library or provider state. The native network-denying factory stays in place.
   Reference HTTP(S) requests are intercepted and refused, except one synthetic
   image URL fulfilled directly from fixture PNG bytes. No real image/model request
   is sent. No file chooser, external browser, clipboard write or approval answer
   is invoked by the parity snapshots.
5. Smoke-only native parity flags reject visible platforms and Qt platform
   overrides **before** constructing the application. No automatic visible retry.
   Neither browser tooling nor fixture adapters are part of a normal native build.

## Method and artifacts

Sources: [`tests/parity/fixtures.py`](../tests/parity/fixtures.py),
[`reference-fixture.js`](../tests/parity/reference-fixture.js),
[`reference.mjs`](../tests/parity/reference.mjs),
[`native.cpp`](../tests/parity/native.cpp),
[`run.py`](../tests/parity/run.py),
[`metrics.py`](../tests/parity/metrics.py), and
[`triage.py`](../tests/parity/triage.py).

One generated JSON manifest supplies source text, rows, attachment metadata/bytes,
model/provider choices, usage counts, approval objects, dimensions and theme to
both adapters. It exercises the real reference renderers and real native
components, not screenshots drawn by substitute renderers. Injection is explicitly
presentation-only: it does not prove backend acceptance, persistence or actions.
Error snapshots enable the existing native Retry affordance without dispatching
it. Reference-only states intentionally capture native absence/refusal, not a
fabricated implementation.

- Default logical client size: **1280×840**, DPR 1. Also 760×540, 900×640,
  1600×1000; welcome and rich-text 900×640 at DPR 2. Both PNG dimensions must match
  the requested physical pixels. No screenshot registration or resizing in metrics.
- Noto Sans/Noto Sans Mono on both sides, with native serif/math/CJK/emoji fallback
  retained. UTC and `C.UTF-8`; usage dates are current-day data, not a permanently
  frozen calendar. Font/rasterization differences are still possible.
- Dark/light and system-dark/system-light are explicit fixture choices. Most
  captures request reduced motion and settle before capture. Full-motion splash
  phases/idle motion are separate. Browser random palette generation is seeded;
  restored native palettes use the reference source-text seed.
- Two complete passes per renderer quantify repeatability. Native transcript,
  selection, popup, search/rename/delete and usage-ledger state is reset per case.
  Settings use their actual navigation method; its highlight position is asserted.
  Catalog presence and absence of leaked effort popups are asserted. Both diagram
  compilers must accept every valid diagram fixture. Loaded synthetic galleries
  must contain the expected decoded image. QML/JS exceptions fail capture.
- The offscreen GLX backing surface is initially allocated at 2048×1400 logical
  pixels before resizing; otherwise larger captures had black unallocated regions.
  This is test-only initialization, not a production window-size change.

For RGB bytes `A` and `B`, the full-window measurements are:

- changed pixels: any channel differs, threshold **0**;
- `over8_percent`: percentage whose maximum channel difference exceeds 8;
- MAE: mean absolute error across all RGB channels, in 0–255 units;
- RMSE: root mean squared channel error; maximum absolute channel error.

No blur, masks, alignment compensation or approved-pixel exclusions. A separate
fixed reading-area measurement is diagnostic only and never replaces the full
window. Classification never alters a measurement or makes a difference pass.

Local final artifacts are in `build-release/visual-parity-final/` (about 185 MiB):

- `input.json`, per-DPR manifests and `fixtures.json`: exact evaluated inputs;
- `reference/` and `native/`: first/repeat PNGs plus geometry JSON;
- `diff/`: raw absolute RGB differences and an explicitly labelled ×8 display;
- `results.json`: every metric, both self-diffs, notes, limitations and summary;
- `measurements.csv`, `index.html`: per-fixture table and linked paired image report;
- environment JSONs, native/reference/Xvfb/browser logs, reference exception list.

The final per-fixture measurements are also committed as
[`visual-parity-results.csv`](visual-parity-results.csv), including blank numeric
fields for manual cases—never misleading zero-error values. PNGs/generated HTML
are local build artifacts, not committed reference baselines. Earlier pilot/control
runs are development evidence only; changed adapters make their totals unsuitable
as a claimed before/after percentage improvement.

## Coverage inventory

The manifest and CSV are the exhaustive list for this run; this is broad snapshot
coverage, not every possible data combination, animation frame or interaction.

| Surface | Fresh fixtures |
| --- | --- |
| Shell/welcome | Dark/light/system themes, empty/disconnected states, minimum/compact/wide sizes, 2× scaling, sidebar hidden |
| Splash/motion | Flight, word, handoff, finished, normal idle motion; phase uncertainty explicitly measured |
| Sidebar | Pinned/home/folder conversations, populated list, focused search, empty result, rename, delete confirmation |
| Composer/pickers | Empty, draft, multiline, overflow, quoted draft; catalog present/absent; dark/light model stage; four effort levels; mode dock |
| Conversation | User/assistant, user quotes, stopped/error/auth-error, partial prose and incomplete code in both live parsers, thinking/tool activity ghost, completed operation, timing |
| Long conversation/selection | Top/middle/bottom, jump/veil geometry, selected answer/menu; no synthesis of hidden tool or reasoning panels |
| Settings/auth | All four pages in dark/light; small/wide sheet; disconnected/connected/waiting/error/key prompt provider states; empty/populated/multi-provider usage |
| Markdown | Headings/emphasis, nested/task/number lists, quotes/callout, table alignment, links/rule, literal HTML, Unicode/RTL/emoji, dark/light/2× |
| Code/math | C++, Python, JavaScript, JSON, diff, long code, column arithmetic; inline/display TeX and matrix |
| Media/files | Held/loaded image, image stack, blocked video thumbnail, sent image metadata, single/multiple sent/composer cards, attachment-note and drop presentation |
| Approval | Command, file-diff and web cards, collapsed and expanded; identical display objects, no actual operation/answer |
| Diagram | All 43 dispatch families, plus light, invalid fallback and editor |
| Unported reference views | Add dock, attachment-note editor, drop art, stats card, compact notice, mini-chat, lock and password field; measured as gaps |
| OS/host | Six named manual cases below; never silently attempted by the default command |

The 43 diagram families are flow, state, sequence, pie, xy, candles, timeline,
gantt, mindmap, quadrant, radar, er, class, wireframe, files, metrics, bars, ranges,
plan, steps, journey, waterfall, funnel, sankey, heatmap, scatter, treemap, git,
array, bracket, nutrition, facts, checklist, changes, outline, matches, words,
gloss, forms, recipe, parts, settings and route. Each has real sample input, not
just a parser dispatch assertion. This does not certify every diagram grammar or
editing interaction.

## Quantitative results and classification

Final comparison covers **168,271,200 pixels**; **40,090,942** differ. Pixel-weighted
MAE is **1.389766 / 255**. Across fixtures, changed-pixel percentage ranges from
**5.36226% to 98.89844%** (median **18.675735%**); median MAE is **0.636889**.
Low average error can coexist with a conspicuous missing component: large flat
backgrounds dilute it. It is not a usability or parity score.

Selected full-window rows (all rows are in the CSV):

| Fixture | Changed % | >8 % | MAE | RMSE |
| --- | ---: | ---: | ---: | ---: |
| welcome-dark | 18.81269 | 0.64053 | 0.470363 | 5.381673 |
| chat-basic | 17.74442 | 0.82868 | 0.536112 | 5.956822 |
| settings-general-dark | 26.12565 | 2.51060 | 1.196439 | 8.488472 |
| settings-providers-dark | 24.32887 | 1.16536 | 0.778675 | 7.696948 |
| usage-populated | 29.10026 | 3.67336 | 2.534141 | 16.329587 |
| diagram-flow | 17.79120 | 0.71670 | 0.458921 | 5.147396 |
| diagram-sankey | 27.37974 | 0.79827 | 0.570493 | 6.155698 |
| markdown-tex-matrix | 17.59849 | 0.70582 | 0.572879 | 7.146024 |
| markdown-image | 26.39304 | 7.32645 | 7.618786 | 31.170429 |
| approval-file-expanded | 31.08519 | 6.46922 | 2.679672 | 16.925956 |
| mini-chat | 93.22442 | 85.53981 | 12.225322 | 23.141892 |

Primary triage labels across the inventory:

| Category | Fixtures | Meaning |
| --- | ---: | --- |
| Genuine native parity bug | 43 | Visible layout/formatting discrepancy requiring further work |
| Intentional difference | 38 | Disconnected safety/product-scope difference or knowingly unported view; still a parity gap |
| Font/platform rasterization noise | 66 | Principal retained content is close; glyph/curve/gradient rasterization is a likely contributor, not proof every changed pixel is noise |
| Test nondeterminism | 5 | Full-motion snapshots with unlocked phase; no animation-equivalence claim |
| Fixture requiring visible/manual validation | 10 | Six excluded manual cases plus four measured but GPU-limited effort cases |

Labels describe the primary fixture issue, **not mutually exclusive pixel causes**.
Every normal chat/shell comparison also contains the intentional native
"Backend not connected" notice, missing reference folder pill/browser toggle,
and rasterization differences. The 66 noise-labelled fixtures are not 66 passes.
Repeat metrics can additionally flag any category as unstable.

Remaining visible findings:

- Sidebar grouping/row spacing, folder/draft presentation and empty-search text.
- Model-stage backdrop/veil and label placement; effort glass/oil cannot be
  certified on this GLX path. Unknown effort labels are now correct, not invented.
- Thinking/tool ghost vertical position; quoted user-message splitting; error
  colour/action presentation; long-transcript scroll positioning.
- Settings lead text and auth structure (reference inline API-key/account sections
  versus native prompt-based flow); usage copy, chart/date/empty-state spacing;
  appearance preview geometry. General-page file copy intentionally does not
  advertise unsupported native preparation/limits just to resemble the reference.
- Approval card spacing/sizing; quote attribution/callout layout, display math
  height/spacing and RTL alignment. Column arithmetic is a reference worksheet
  but still a native code block.
- Selection/veil/menu differences, including absent mini-chat action; sent file
  preview refusals and vertical rows rather than reference cards/thumbnails.
- Add dock, attachment notes, drop art, stats/compact/mini/lock/browser views are
  absent, not secretly implemented by the fixture injection. Media network denial
  remains intact. These require separate product-scope decisions, not pixel hacks.

## Safe fixes made

Regression assertions were added to [`tests/smoke.cpp`](../tests/smoke.cpp).

1. `qml/EffortControl.qml`: no blank toolbar slot without advertised levels;
   close the popup if levels disappear, matching reference `setEfforts`.
2. `qml/EffortStage.qml`: `none` is "Instant"; an unrecognized `off` falls back to
   "Off" with no invented hint, matching reference i18n. No backend ID translation.
3. `qml/ChatEntry.qml`: restored Markdown palette seeded from source text like
   `StreamView.render`, not opaque row IDs. Live replies keep a stable initial
   palette through growth/completion.
4. `qml/ChatEntry.qml`: remove the six-pixel phantom caret allowance in displayed
   user bubbles; retain the reference's 16-pixel side padding.
5. `src/rich.cpp`, `qml/Markdown.qml`, `qml/ChatEntry.qml`: retain wide diagrams'
   6-pixel first top margin and 22-pixel last bottom margin, including collapse
   with following metrics/copy toolbar. The later reference CSS rule overrides
   `:first-child`/`:last-child`; it is not an arbitrary spacing adjustment.
6. `qml/UsagePage.qml`: supplied provider order/names and reference cyclic
   turquoise/lilac/orange/blue, not legacy hard-coded provider branding. Handle a
   removed provider delegate while totals/repeater updates settle.

The harness also caught and removed false differences: a disconnected-service
callback erasing injected models, an effort popup leaking into later fixtures,
settings page assignment bypassing its navigation highlight, stale usage counts,
undersized offscreen backing buffers, and an unclicked reference image-consent
control. These were fixture defects, not reasons to change the frozen reference.

## Explicit manual/visible follow-up — NOT RUN

These names are included in the manifest with `manual: true`. The default command
never launches them. No visible fallback or desktop smoke was used for this audit.
An operator must explicitly arrange an isolated interactive desktop/profile and
approve each session. There is deliberately no automatic manual-suite launcher.

| Case | Required evidence / limitation |
| --- | --- |
| `manual-visible-effort-hdr` | Capture the same effort states on a supported real GPU; record Qt/backend/driver and RGBA16F support, glass/oil/blur and repeat results. Offscreen logs `QSGRhiLayer: Attempted to set unsupported texture format 8`; the four headless pictures are measured, not qualified. |
| `manual-visible-file-dialog` | Native platform/portal file chooser, cancellation/focus return, light/dark host theme. Only private synthetic files; no user documents. |
| `manual-visible-folder-picker` | Reference host folder chooser. Native entry point is absent; record the gap rather than inventing a picker. |
| `manual-visible-window-chrome` | Decorations, snap/activation/minimize/maximize, monitor migration and fractional desktop scaling; content-only PNGs cannot certify these. |
| `manual-visible-ime-accessibility` | Real IME candidates, keyboard/focus routing, screen-reader naming/navigation. Unicode screenshots are not accessibility validation. |
| `manual-visible-browser-host` | Reference Electron webview host; native view absent. Requires separate isolated host setup, no real login or remote site. |

Startup captures also need phase-locked instrumentation before claiming matching
animation trajectories. Simply showing two visible windows would not solve that
measurement problem.

## Validation actually performed

- Release configure/build, including a clean rebuild and final incremental build,
  `OPENGHOST_BUILD_SMOKE_TEST=ON`, four build jobs: passed. The clean build showed
  the two previously documented GCC metatype/variant warnings, not new failures.
- `native_contract_test`, `native_ui_smoke`, `native_fake_ui_smoke`: **3/3 passed**,
  serial CTest; UI tests use their existing offscreen/software environments.
- `python3 tests/parity/test_harness.py`: **4/4 passed** (zero-tolerance metrics,
  dimensions, fixture inventory and pre-application visible-platform refusal).
- Full final strict parity run: **162 inventoried, 156 captured twice per side,
  six excluded manual, zero exact**; exit 2. Reference exception list empty; no
  QML binding/load failure. RGBA16F warnings retained as a limitation, not hidden.
- Python/Node syntax checks and `git diff --check`: passed. Reference unchanged.
- No visible application, live provider, Rust backend, RPC or FFI test was run.

Environment: Linux/CachyOS, Qt **6.11.2**, Mesa **26.2.4-arch3.1**, llvmpipe
(LLVM **23.1.1**, OpenGL compatibility **4.6**), Chromium **154.0.8037.98** through
Brave/headless SwiftShader, Node **26.10.0**, Python **3.14.7**, NumPy **2.5.3**,
Pillow **12.3.0**. The captured native/test source-tree hash is
`8979bbccfdf5337a32ffb17a284f4a0cf4dfca5359de7d5cd9ab5568d2bf7181`;
`environment.json` also pins the built binary and font files.

This audit is a measured starting point for parity work, **not clearance to begin
backend integration under a claim of completed visual parity**.
