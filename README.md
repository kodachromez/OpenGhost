# OpenGhost C++

A standalone **native Qt 6 / QML / C++17 implementation of the OpenGhost
frontend**, targeting the dissected OpenGhost 1.3 presentation and behavior.
The window, chat, sidebar, settings, Markdown, code, TeX, diagrams, ghost and
animations are native Qt Quick/C++ — not an embedded website.

**No real backend is connected.** The normal application starts disconnected.
It cannot call a model, run agent tools, sign into a provider or resume a real
backend session. `--fake-backend` explicitly selects an in-memory development
fixture. The native target has no Rust/RPC/FFI integration, backend process
launcher or implemented transport.

This is an independent, incomplete port, **not official OpenGhost** and not a
claim of full functional or pixel-exact parity. Before publishing or distributing
it, read [Licensing and attribution](#licensing-and-attribution): the retained
name, artwork, animations and visual design are **not covered by the source-code
MIT grant**.

## Build and run

Requirements:

- CMake 3.21+ and a C++17 compiler.
- **Qt 6.11+** development packages: Core, Gui, Network, Qml, Quick,
  QuickControls2, QuickDialogs2 and ShaderTools; the Qt Quick Layouts, Shapes
  and Effects QML modules must also be available.
- Qt Test when enabling the optional tests below.
- Qt WebEngine (WebEngineQuick) for the built-in browser panel, unless configured
  with `-DOPENGHOST_BROWSER=OFF`.

Qt must be discoverable through CMake (for example,
`-DCMAKE_PREFIX_PATH=/path/to/Qt/6.11.x/gcc_64`). There is no Cargo, Node,
Electron, React or provider SDK build dependency. Qt WebEngine's Chromium renders
only the pages inside the built-in browser panel, as the reference's Chromium
webviews do; `-DOPENGHOST_BROWSER=OFF` builds without it and without the panel.
The JavaScript expressions in QML are part of Qt, not a browser/Node runtime.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./build/openghost-cpp                 # disconnected
./build/openghost-cpp --fake-backend  # explicit simulation
./build/openghost-cpp --help
```

For multi-configuration generators, build with `--config Release`; the executable
is in the configuration's output directory. macOS uses an `.app` bundle. Those
platforms are not yet qualified; see [Portability](#portability).

For a local install (not a self-contained Qt deployment):

```sh
cmake --install build --prefix "$HOME/.local"
```

CMake installs the executable, notices/licenses, icon and, on Linux, desktop
entry. It does not install the reference tree or bundle the Qt runtime/plugins.

### Focused tests and smokes

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build build-release --parallel 4
ctest --test-dir build-release \
  -R '^(native_contract_test|native_plugins_test|native_media_test|native_ui_smoke|native_fake_ui_smoke|native_splash_test|native_splash_test_60hz)$' \
  --output-on-failure
```

Use `ctest -C Release` with multi-configuration generators. The option defaults
to **OFF** and enables all four checks:

- `native_contract_test`: typed contract, identity/cancellation/recovery, store,
  checkpoint failures, mini-chat/lock state, usage, attachments, approvals/auth
  and host routing. No GUI or real backend.
- `native_plugins_test`: capability-gated plugin discovery, authoritative toggles,
  live/external changes, disabling, failures, connection-generation refreshes,
  rejection of stale completions and of events from a replaced connection, and
  no-op updates, through a scripted typed backend (not real RPC).
- `native_media_test`: reply picture/video parsing, trusted-place and redirect
  rules, real loopback byte/deadline bounds, QML network denial, injected video
  metadata/cache lifecycle, video link words, stack shape and consent/decode
  bounds and stale-load handling. No external network; transport tests use loopback.
- `native_ui_smoke`: disconnected QML launch, splash handoff, themes/settings,
  refusal paths and Markdown/TeX/diagram rendering, plus a reply's picture
  stack, click-to-load plate, lost links and video cards from fixture bytes.
- `native_fake_ui_smoke`: the same UI plus the composer's effort dropdown:
  only advertised levels, keyboard/pointer selection, busy locking and model changes;
  actual composer submission, streaming, Escape, steering, Retry,
  attachment/approval cards, deletion and fixture auth.
  Both UI smokes also drive the production Plugins page over a scripted backend:
  button dispatch, pending controls, failures, external changes and capability loss.
- `native_splash_test` (240 Hz) and `native_splash_test_60hz`: the 1.3 splash
  stepped on a fixed clock and checked frame by frame against `splash.js` (scene
  start, lean, rounded flight transform, word on the landing's frame, blink,
  opening, handoff); every splash alpha source stays in [0, 1] and no colour
  goes out of range over the whole timeline, nor as the browser button's globe
  spring settles closed (it dips just below 0 at 240 Hz). Run on a GPU
  scene graph (e.g. `MAXIMIZE=1 tests/parity/private-kwin.sh 3840 2160 1.45 out
  --splash-check`, a private headless KWin) it also checks the rendered frames:
  mist at .6 of the window's pixels, no Ghost afterimage, identical redraws.
- `native_tool_calls_test`: the Tool Calls frontend plugin without a window:
  its one hook through repeated On/Off (saved choice, no leftover hooks), its
  card texts and selection, and Ghosty's tool formatter with Pi's tool schemas.
  `native_pi_test` maps the scripted Pi's tool events to cards (order, snapshots,
  results, Stop, restart), and `native_fake_ui_smoke` draws the cards in the
  real transcript and switches the plugin Off/On twice while a call waits.
- Thinking and Settings → Appearance → Chat Settings: `native_pi_test` maps the
  scripted Pi's thinking (live, redacted, saved, restart) to thinking rows,
  `native_contract_test` their placement, bounds and display cache, and
  `native_frontend_plugins_test` saved plugin options. Both UI smokes click
  Show Tool Calls, Show Thinking and Start Collapsed in the real Settings sheet
  (live effect, greyed-out Start Collapsed, nothing repeated under Plugins,
  restart); `native_fake_ui_smoke` also streams thinking shut and open.
- `native_pi_real_test`: `PiBackend` against the installed `pi` (skipped without
  one) with Pi AI's faux model in a throwaway agent directory: Ask/Auto/Full
  relayed to Pi, approval cards for a test-only stand-in Pi permission plugin's
  requests, Stop, other extensions' dialogs and errors. No network or
  credentials. `native_sign_in_test` checks the sign-in form across
  cancel/sign-in-again races against the scripted Pi.

CTest uses offscreen/software rendering. Smokes isolate appearance, preferences
and the library in temporary directories (even the fake smoke uses a temporary
file-backed library); contract tests use temporary or memory stores. No real
credentials or live model calls are needed. They are focused regressions, not
full visual parity, real crash recovery or cross-platform qualification.

### Desktop-safe visual audit

After the test-enabled Release build above:

```sh
python3 tests/parity/run.py
```

This captures the unchanged JS reference headlessly and Qt offscreen, sequentially,
with a private memory-only display and no visible fallback. It never opens a
report window. See [visual parity](docs/visual-parity.md) for prerequisites,
quantitative results, classifications and `--strict`. The audit currently has
**0 exact matches out of 156 captured fixtures**, plus six excluded manual cases.

### Manual/visible GPU smoke — opt-in only

**Not part of the default suite; can open a window and take focus.** With an
explicitly approved Linux Wayland desktop session, the same test-enabled binary
can exercise the GPU path without CTest's offscreen override:

```sh
QT_QPA_PLATFORM=wayland QSG_RHI_BACKEND=opengl QT_QUICK_BACKEND=rhi \
  QT_FORCE_STDERR_LOGGING=1 OPENGHOST_REDUCED_MOTION=0 \
  ./build-release/openghost-cpp --smoke-test --fake-backend
```

An uninstalled Linux development binary can emit a nonfatal portal app-ID
registration warning. **Two GCC Release metatype warnings also remain**; see
[the warning analysis](docs/cpp-port.md#known-gcc-metatype-warnings). Do not
interpret a successful build as warning-free.

## What works, and what does not

“Implemented” below describes frontend behavior within the stated limits, not
successful integration with a real backend.

| Area | Current status |
| --- | --- |
| Native presentation | Implemented window/sidebar/composer, four base settings pages plus capability-gated Plugins, themes, splash/motion, selection/copy, Markdown/code/TeX/diagram rendering. Full visual parity is unqualified. |
| Local preferences | Implemented appearance, standing instructions, explicit model/effort and Ask/Auto/Full preference persistence, and pinned text files (General → Files). |
| Chat flow in the existing UI | Implemented draft/open/search/local rename, pins/collapse, streaming, Escape Stop, Retry, steering, deletion, approval cards and text attachment cards. Turn-dependent behavior is exercised only with the fake. |
| Local state/recovery | Implemented index/display-cache/checkpoint/reconciliation logic, tested with stores and scripted backends. Caches are not backend history; persistence caveats are below. |
| Mini chat, locks, folder creation | State/service APIs and tests exist. **No mini-chat dialog, lock/unlock screen or add-folder picker is exposed in the UI.** Production encryption is absent. Existing folder groups can be displayed/collapsed and their chats managed. |
| Models/auth/usage | Frontend selectors, status, login controls and usage views exist. Catalog, sign-in, approval effects and token counts are **simulated** in fake mode. No provider credentials are saved. |
| Runtime plugin management | Data-driven Settings page and connection-scoped state implemented at the typed backend seam; hidden without the runtime capability. Scripted contract/UI tests only; real plugin RPC needs the still-absent adapter/transport. |
| Real generation, tools, durable backend recovery | **Backend-dependent and unavailable.** No backend is bundled or contacted. |
| Built-in browser panel | Implemented in the default build on Qt WebEngine guests: globe toggle, resizable panel, tabs, address/search, back/forward/reload/stop, loading/failure/crash states, saved layout and tabs, persistent site profile, driving/Take control/Hand back overlays. Usable without a backend. |
| Host browser/media tools | Typed requests and tested dispatch/cancellation bookkeeping exist. The panel reports its snapshot but publishes **no browser tools** yet (`available: false`), so host calls refuse. No downloads, page context menu or media preparation. |
| Compaction/account limits | Contract types exist; fake refuses them, no operational compaction UI or live billing/limits. Saved compact/stats rows are retained as cache data, not rendered as cards. |
| Other intentionally absent UI | No tool/subagent/reasoning transcript panels, source-port service/workspace picker, voice controls, other extra settings tabs or visual diagram form editor. Reasoning/tool activity is not assistant text. |

The composer **+ file picker** handles text files and pictures; the reference's broader
plus-menu operations are not implemented. No new controls were invented to
expose the unported state.

## Using `--fake-backend`

The fake exists to develop/test frontend state and presentation without installing
an agent backend or risking provider charges. It uses Qt timers (deterministic
`advance()` in tests), two labelled models — **Fake Echo** and **Fake Brief** —
and process-local sessions. It uses no model, real tools, network requests,
credentials or subprocesses. Text attachment preparation is a separate, real
local file read by the frontend.

**Fake Echo** advertises the reference's full effort set: **Instant, Low, Medium,
High, Extra high, Max, Ultra**, defaulting to Medium. On a fresh profile Echo is
selected, so the Effort dropdown appears between the model and Send buttons.
It lists only the selected model's levels (the reference's slider is not shown
for now). A saved model preference is still respected; select Fake Echo in the
model picker to try all seven levels. **Fake Brief** retains only Low/High
(default Low), exercising model-dependent options and preference fallback.
Effort stays visible but dimmed and locked during a reply. Choices update the draft or configure the existing
fake session through the same frontend contract used by a future backend;
no real thinking/generation occurs.

Send a normal message, press **Escape** to stop, or Send again while the reply
runs to steer. Submit these exact whole-message fixture selectors:

| Message | Scenario |
| --- | --- |
| `/fake tools` | Simulated tool activity/working ghost; no host effect or tool transcript panel. |
| `/fake approval` | Approval card in every mode (the fixture stands in for a backend permission plugin; OpenGhost decides nothing). Allow/Deny only affects the simulation. |
| `/fake error` | Terminal error with Retry; retry targets that failed turn without another user bubble. |
| `/fake empty` | Empty final reply notice. |
| `/fake length` | Output-limit finish notice. |

These are not production slash commands. At the end of its scripted reply, the
fake emits fixed 100 input / 20 cached / 30 output tokens, not estimates or a bill. Providers →
Add API key accepts **only `fixture`**; Sign in is a cancellable timer. **Never
enter real credentials.**

Fake chats, sessions, pins and usage are in memory and disappear on exit.
However, **normal fake launches share the app's saved appearance and preferences**
(including instructions/model/mode) with disconnected launches. `--fake-backend`
is not an isolated profile flag; `--smoke-test` is isolated. The fake's recovery
capability applies only while that fake instance survives — it is not a durable
or production-conforming backend.

## Architecture and backend boundary

```text
QML (OpenGhost.Ui)
  -> WindowController / display models
     -> ChatService + Library / PreferencesStore / UsageStore
        -> Backend (typed asynchronous semantic interface)
           -> FakeBackend, only when explicitly selected
           -> future adapter -> ByteTransport implementation (both absent)
```

- [`src/backend/types.h`](src/backend/types.h) defines commands/replies, session
  and global events, errors, model/auth metadata, approvals, host-tool calls,
  usage and separate full-input vs display-preview values. These are **QtCore
  C++ values, not JSON-RPC serializers**. JSON objects are used for extensible
  tool arguments/schemas/details, not as the UI's protocol.
- [`Backend`](src/backend/backend.h) is an asynchronous, same-owner-thread
  interface. Local request IDs correlate replies; session/version/client-turn/
  remote-turn/message IDs have separate meanings. Replies never arrive inline;
  events can precede acceptance. Request cancellation is not turn Stop or rollback.
- [`ByteTransport`](src/backend/transport.h) only declares bounded ordered byte
  writes, received bytes and lifecycle/error signals. A queued write is **not**
  backend acceptance. No socket, process, framing, parser or reconnection exists.
- [`ChatService`](src/frontend/chat_service.h) owns frontend projection and
  lifecycle, not agent execution. [`presentation.h`](src/presentation.h), list
  models and `WindowController` adapt it for QML. `native_contract` links only
  QtCore; rendering and platform integrations are separate targets/layers.

The frontend is **backend-agnostic at this semantic seam**. A later adapter and
transport could connect the Rust harness, Pi, or another compatible backend.
None is currently integrated or claimed wire-compatible. An adapter must map
capabilities and preserve acceptance, identity, event ordering, recovery,
cancellation, approval and error semantics — or explicitly refuse unsupported
operations. Merely forwarding text is insufficient. The current service requires
session recovery during initialization. Details and the ABP-oriented future
wire responsibilities are in [the port notes](docs/cpp-port.md#typed-boundary-and-future-adapters).

### Runtime plugins

Settings adds **Plugins** only when an initialized backend advertises runtime
plugins. `ChatService` owns the connection-scoped `Plugins` projection; QML
observes display rows through `WindowController`. Every returned ID is shown,
without a built-in allowlist or inference from tool activity. State is not saved
as a local preference.

The typed commands correspond to `plugin.list`, `plugin.enable {pluginId}` and
`plugin.disable {pluginId}`; `PluginChanged` corresponds to global
`plugin.changed {plugin}`. The future wire adapter must map
`initialize.capabilities.plugins.runtime` to `Capabilities::runtimePlugins`,
preserve snapshots/notifications in stream order, and stamp each
`PluginChanged` with the `connectionId` of the connection that delivered it;
events from any other connection are ignored. **There is no JSON-RPC serialization or live
connection in this checkout yet.** The shipped disconnected and fake backends
therefore do not expose this page; the plugin tests inject an advertising backend.

Initialization (including reinitialization after connection loss) refreshes the
list. On/Off controls retain the backend's state while pending. A disabling
plugin shows how many running calls it is waiting for and can be turned back on.
Unavailable or unknown-state plugins cannot be toggled. Failures show inline without
changing chat state and reread the list; the error clears once a later read or
notification confirms the state, and an unconfirmed state stays non-interactive
with a Refresh action after a failed read. A change the backend keeps only in
memory (or could not confirm saving) says it may not survive a restart. External
notifications apply immediately, including during a turn, and update rows in
place so keyboard focus stays put. Stopping a chat never cancels plugin requests.
Neither a toggle nor its failure restarts anything.

## Local persistence and recovery

Qt selects the app-specific locations for application name `openghost-cpp`:

| Location | Contents |
| --- | --- |
| `AppConfigLocation/appearance.json` | Theme choice. |
| `AppConfigLocation/preferences.json` | Versioned instructions, preferred provider/model/effort and permission mode. |
| `AppDataLocation/library/index.json` | Chat/folder index, titles/rename metadata, pins/collapse, model selection, lock metadata. |
| `…/library/chats/<id>.json` | Allowlisted display messages, per-reply usage and pending-turn/steering checkpoints. |
| `…/library/mini/<parent-id>.json` | Separate mini-chat display cache, pending markers and parent `seen` timestamp. |
| `…/library/usage.json` | Local usage ledger, schema v2 (v1 keys upgraded on read). |

On Linux the defaults are normally `~/.config/openghost-cpp` and
`~/.local/share/openghost-cpp`, respecting Qt/XDG overrides. Windows/macOS use
Qt's platform locations. On first launch, a config, data or cache location left
under the pre-rename application name is moved once to the matching
`openghost-cpp` location if that location does not exist yet. No Electron profile
is read or migrated.
Normal disconnected mode selects the file-backed library, but cannot create a
backend chat; New Chat is only a draft until a first send is admitted.

The key store uses one JSON object per validated key (64 MiB maximum) and
`QSaveFile` replacement. Unknown/unreadable **index** makes the library read-only;
unreadable cache reads fail instead of becoming an empty chat. Preferences and
usage also refuse to overwrite unreadable/unknown-version state. Appearance has
a different, simpler policy: invalid input is ignored and a later theme choice
can replace it. These are not universal filesystem-safety or transaction guarantees.

### Sessions, checkpoints, Retry and steering

- Frontend chat identity/index/annotations are local; backend session incarnation,
  revision and canonical conversation history belong to the backend. A new start
  is create-only; a later start names the known incarnation.
- Start, Retry and steering must first save the index and display checkpoint.
  Failure means **nothing is dispatched**. Ordinary subsequent cache saves are
  best effort, not a durable backend journal or multi-file transaction.
- Reopening a saved chat loads display data, then uses `session.get`, naming the
  saved pending client turn when present. Matching journal events rebuild that
  turn; raced events are bounded/buffered and replay is not charged as new usage.
  Remote IDs/version/sequence are reacquired, not reconstructed from text.
- Missing sessions stay display-only; a missing pending turn is not resent.
  Full attachment input and unsent composer drafts are **not persisted** (the
  QML draft cache holds at most 32 inactive drafts in memory).
- A failed accepted turn uses `RetryTurn` with its exact failed remote ID and a
  new client ID, **no input**. In-process uncertain acceptance is reconciled
  first; only a positively missing new session with retained exact prepared
  input can restart that request. This is not an automatic crash/reconnect retry.
- Escape freezes local output immediately; remote cancellation is separate and
  does not undo effects. Local Stop alone does not clear pending checkpoints.
  Send while busy steers: queued, applied, notApplied and unconfirmed remain
  distinct; command acceptance alone is not application to the conversation.
- Deletion waits for backend acknowledgements for the main and `<id>:mini`
  sessions before removing local state. Folder deletion proceeds chat by chat,
  not atomically. Local cleanup failures still need better reporting.

### Mini chat and password locks

Mini-chat service methods maintain `<parent-id>:mini`, independent messages and
turn state, `side.parent`/`parentBusy`/`moved`, and a *Caught up with the main chat*
marker when the parent moves on. Close stops and retains it; Clear deletes only
the mini session/cache. **This is tested state, not a shipped mini-chat UI.**

The lock state machine supports protect, relock-on-leaving, unlock and unprotect
through an injected `ChatSealer`. **The application supplies no sealer and has no
password UI: it does not currently encrypt chats.** Existing protected records
remain locked and cannot be opened or overwritten as empty. Protect/unlock refuse
without a sealer. Tests use an HMAC-authenticated XOR fixture, **not production
cryptography**. The reference uses PBKDF2-SHA256 (600,000 iterations) and AES-GCM;
a vetted implementation and compatibility/security tests remain work to do.
Even that design protects local display bodies/titles, not backend history or all
index metadata (folder/space names remain visible).

### Usage, attachments, approvals and host state

Usage views aggregate correlated live usage by provider/model/day/month and keep
per-reply metrics. Duplicate/replayed events do not charge again; unknown timing
is not guessed. Saves are debounced by 800 ms, with pending changes flushed on
shutdown. Failed writes return false from `flush()` and emit the store's
`saveFailed` signal; counts remain pending for an explicit flush, a later usage
update or shutdown. There is no automatic retry loop or dedicated usage-error UI.
Unsaved counts can still be lost on a crash or persistent storage failure. See
[the persistence findings](docs/repository-readiness.md).

Attachments are local regular UTF-8 text files (256 KiB each) or pictures
(PNG/JPEG/GIF/WebP/BMP kept up to 6 MB and 2560 px, others re-encoded within
them): 20/message, 64 retained draft tokens, 48 MiB retained. Preparation is
synchronous and byte-bounded, not deadline-bounded; a failed selection adds nothing.
PDFs, office documents, media and other binary files are refused by name, as are
symlink and nonlocal inputs. With `--pi`, text files reach Pi in its `pi @file`
form and pictures as prompt images, only for a model that sees images. Tokens own
full prepared payloads separately from cached metadata; previews are never resent.
General → Files keeps pinned text files with the preferences (20 files, 200,000
characters together); Pi gets them as a system-prompt section on every run.

Approval requests are bound to session/turn/request identities, answered once,
and dismissed on resolution, Stop, cancellation or superseding steering. Auth
and model notifications trigger fresh reads. Pending auth/approvals are memory
state, not persisted consent or credentials.

OpenGhost no longer owns permission policy. Permission enforcement belongs to
Pi/the Pi permission plugin. OpenGhost only renders permission UI and relays
decisions. Ask/Auto/Full is the chat's mode, relayed to Pi (the bridge says it to
Pi's extensions on `pi.events` channel `openghost:mode`); OpenGhost never decides
from it whether a tool call runs. Without a Pi permission plugin, Pi's tools run
unasked in every mode. A plugin asks with a Pi extension UI `confirm` titled
`openghost:approval` (see `src/backend/pi/openghost-bridge.js`).

`HostServices` publishes tool schemas at initialization and routes only named,
live-turn calls, tracking duplicate in-flight calls, cancellation and turn-end
release. The desktop build's `Browser` host (`src/frontend/browser.*`) supplies
the panel's `host.browser` snapshot and change notifications and releases a
chat's hold on the browser when its turn ends, but publishes no tools yet; an
OFF build uses `NoHost` (null browser). Either way host calls return
`unsupported`. A scripted test host exercises routing, not browser operations.

### Built-in browser

The panel (`qml/BrowserPanel.qml`, `qml/BrowserGuest.qml`) draws the host's
state; each tab's page is a WebEngine guest in its own persistent profile
(`AppDataLocation/browser`), and the layout and tab URLs/titles are saved to
`browser.json` beside the preferences. Guests get no app objects or channel.
`native_browser_test` checks the host's lifecycle, navigation/error/crash states,
panel open/width, hand-back and turn-end release without an engine; both UI
smokes drive the real panel on local pages (`data:` URLs and a refused loopback
port; no network). An `-DOPENGHOST_BROWSER=OFF` smoke checks the panel's absence.
Status by feature: [browser/web audit](docs/browser-web-feature-audit.md).

## Repository map

| Path | Purpose |
| --- | --- |
| `src/main.cpp` | App identity, command-line selection, store paths, composition and QML engine. |
| `src/backend/` | Semantic types/interface, explicit fake and transport interface only. |
| `src/frontend/` | Chat lifecycle/projection, library/store, preferences, attachment preparation, usage and host-service seam. |
| `src/window.*`, `model.*`, `settings.*`, `presentation.*` | C++ → QML presentation adapters and display models. |
| Remaining `src/` | Native rich text/highlighting/TeX/diagram renderers, theme, ghost, motion and drawing helpers. |
| `src/platform/` | CMake-selected Linux or portable host integration and external-link dispatch. |
| `qml/` | Qt Quick window, sidebar, composer, settings, delegates and visual components. |
| `shaders/`, `resources/` | Qt-compiled shaders, icon and Linux desktop entry. |
| `tests/` | Qt contract suite, application UI smokes and test-only headless/offscreen visual parity harness. |
| `docs/cpp-port.md` | Engineering inventory, contracts, exclusions, warnings and portability detail. |
| `docs/native-import.json` | Historical import commits and source hashes, not hashes of today's edited files. |
| `LICENSE`, `NOTICE.md`, `licenses/` | Current terms and preserved historical attribution. |
| `reference/openghost/` | **Frozen dissected 1.3 reference**, including legacy JS/Electron files; never built, loaded or installed by the native target. Do not edit it. |

## Portability

**Linux is the exercised platform:** Qt 6.11.2 / GCC 16.2.1, Release build,
offscreen/software tests and Wayland/OpenGL smokes. Linux selects threaded
rendering/portal defaults and KDE reduced-motion discovery. The portable override
`OPENGHOST_REDUCED_MOTION=0/1` is available. QML networking is denied; explicit
HTTP(S) links (and allowed mail links) open through the OS, so this is not a
sandbox or a guarantee that external applications stay offline. Narrow frontend
resource services load reply pictures and video previews (`src/medialoader.*`, the
reference's media-embed.js rules): HTTPS from the reference's trusted preview
hosts loads by itself, any other http(s) picture only after a click on its
plate; bounded in time and size, re-checked on every redirect, no cookies or
credentials. A separate injectable frontend service (`src/videoinfo.*`) obtains
real YouTube titles/authors through bounded HTTPS oEmbed, retaining the original
link words or no title on failure. It requires no backend, RPC, FFI or browser.
See [reply media](docs/reply-media.md) for caching, native frosting, safety
differences and remaining boundaries, and run `python3 tests/parity/run.py --media`
for the media-only visual audit.

CMake has MSVC UTF-8/warning options, a Windows GUI executable and a macOS bundle,
but **Windows/macOS have not been built or run in this audit**. Still to validate:
Qt/SDK/compiler builds, runtime/QML plugin deployment, signing/icons, graphics
backends, fonts/DPI, native dialogs/clipboard/links, keyboard/IME/accessibility and
system reduced-motion discovery. Path comparison lowercases outside Linux;
case-sensitive macOS volumes, Windows case/path aliases and long paths need
review. Store replacement under antivirus/indexer locks and macOS sandbox paths
are unqualified.

Library/preferences storage has no explicit private-permission/ACL policy,
interprocess locking or multi-file transaction. Appearance separately requests
owner-only permissions. Use a private profile and one application instance; do
not treat these files as a secure vault. See [repository readiness](docs/repository-readiness.md)
for issues to carry into a standalone repository.

## Licensing and attribution

OpenGhost is by **Andrew, Copyright © 2026 Andrew**. This port retains the native
implementation's historical attribution and import record; historical product
names/paths in those records are provenance, not current build instructions.

Read [LICENSE](LICENSE), [NOTICE.md](NOTICE.md) and the retained [notices](licenses/).
The MIT source-code terms explicitly exclude the OpenGhost name, logo, animations
and visual design. Publishing/distributing this modified UI with those materials
requires resolving the upstream restrictions, not merely retaining notices.
Moving it into a private repo does not create additional rights or permit
commercial use. No license or artwork permission is changed by this documentation.
