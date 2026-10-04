# Testing OpenGhost-Frontend

Use focused tests during normal development. The test files, [package scripts](../package.json), and [build workflow](../.github/workflows/build.yml) define the available checks; there is no separate lint, typecheck, or coverage script.

All commands below run from the repository root. Use Node 22, as CI does. The standalone Electron scripts depend on Node's built-in `fetch` and `WebSocket`. Run `npm ci` when the locked Electron/build dependencies need installing; it is setup, not a test.

## Quick commands

```sh
# One relevant Node test file
node --test test/session-recovery.test.js

# Related files for a boundary change
node --test test/backend-client.test.js test/backend-envelope.test.js

# One named regression while iterating
node --test --test-name-pattern='failed durable checkpoint' test/session-recovery.test.js

# All top-level Node test files, not the Electron scripts
npm test

# Real Electron smoke test with no-backend and scripted-backend cases
npm run test:e2e

# Separate real-webview checks; not included in either command above
node test/e2e/browser-lifecycle.mjs
```

The name filter above matches an existing test. After a narrowly filtered run, run its containing file to check neighboring behavior. Choose other files from the code-area map below rather than defaulting to the full suite.

## Structure and test kinds

| Location | Purpose |
| --- | --- |
| [`test/*.test.js`](../test/) | Node's built-in test runner and `node:assert/strict`: unit tests, state-machine regressions, static boundary guards, and some real subprocess checks. `npm test` expands to `node --test test/*.test.js`. |
| [`test/helpers.js`](../test/helpers.js) | `renderer()` loads real classic-script renderer modules in a fresh VM context; it loads `backend-protocol.js` before `backend-client.js`. `fakeTransport()` records outbound messages and lets tests deliver replies/status. `tick()` advances asynchronous work via `setImmediate`. |
| [`test/fixtures/`](../test/fixtures/) | Deterministic subprocess peers for ABP and process-tree cleanup. These are test inputs, not independently discovered test suites. |
| [`test/e2e/smoke.mjs`](../test/e2e/smoke.mjs) | Real Electron app, driven through the Chrome DevTools Protocol (CDP), with a synthetic backend or no backend. |
| [`test/e2e/browser-lifecycle.mjs`](../test/e2e/browser-lifecycle.mjs) | Real Electron webviews on controlled local pages, without a backend. |
| [`package.json`](../package.json), [`.github/workflows/build.yml`](../.github/workflows/build.yml) | Packaging configuration and build automation. Output goes to `dist/`; producing an artifact is not a behavioral test. |

Keep three kinds of evidence distinct:

- **Synthetic backend/provider tests** supply invented catalogs, auth states, ABP replies, and events. They check frontend handling of the contract, not AI quality, provider connectivity, OAuth implementation, or an external backend's correctness.
- **Real frontend behavior tests** execute application code. Node tests commonly replace DOM, storage, transport, or Electron objects with small stubs; they do not validate actual layout or Chromium behavior. Electron tests exercise the real renderer, preload/main-process integration, and, where used, real guests. Their backend can still be synthetic.
- **Packaging/build checks** validate artifact creation and startup. The packaged smoke mode adds behavioral assertions against an executable; a successful build or CI screenshot alone does not.

## Unit tests and code-area map

Start with the files for the code you changed. All test files listed here are under `test/` and can be passed directly to `node --test`. Several cross-cutting regressions belong to more than one area; combine files when a change crosses those boundaries.

### Backend boundary and protocol

| Code area | Focused tests | Coverage |
| --- | --- | --- |
| `backend-client.js` | `backend-client.test.js` | Availability, initialize metadata, request results/errors, notifications, reverse handlers, aborts, deadlines, late replies, and crash handling. |
| `backend-protocol.js`, envelope handling in client/host | `backend-envelope.test.js` | JSON-RPC validation, malformed matching responses, numeric versus string IDs, unknown/late IDs, reverse-request validation/cancellation, protocol-version negotiation, and error mapping. |
| `desktop/preload.js`, client lifecycle, unavailable-state UI | `backend-lifecycle.test.js` | IPC subscription stripping/unsubscribe, useful configuration/spawn/exit diagnostics, disposal, re-evaluation, and preventing late work from reviving a disposed client. |
| `index.html`, preload surface, dependency and frontend/backend ownership boundaries | `boundary.test.js` | Script existence/order, CSP, source scans for provider clients and renderer connection APIs, process-spawn ownership, package exclusions, and named i18n keys. |

A simulated later `running` status in a client test is not evidence of automatic backend restart. These tests drive statuses explicitly. Use the [ABP contract](backend-interface.md) alongside the tests when changing message semantics.

### Recovery and sessions

| Code area | Focused tests | Coverage |
| --- | --- | --- |
| `Chat.reconcile`, checkpoints, `Library` persistence, main/mini recovery | `session-recovery.test.js` | Fresh VM contexts sharing only a display store and synthetic backend state; uncertain starts, replay/live races, session incarnations, missing sessions, failed writes, queued inputs, separate mini caches, and connection-scoped RPC IDs. |
| Retry and steering in `chat.js`, delayed attachment preparation | `retry-steering.test.js` | Exact failed-turn targeting, recovery before uncertain retry, explicit no-retry errors, serialized inputs despite reversed preparation order, and inputs combined after a quiet operation. |
| Stream/event handling, compaction and usage correlation in `chat.js` | `event-ordering.test.js` | Sequence gates, bounded early events, interleaved message buffers, finalization, terminal behavior, accepted inputs, replay without charging usage twice, and compaction operation identity. |
| Approval and host-tool ownership in `chat.js` | `turn-identity.test.js` | Wrong/stale turns, requests arriving before start acknowledgement, duplicate calls, Stop, and turn-end cancellation. |
| Stop and browser handoff across main/mini chats | `cancellation.test.js` | Running/queued browser work, individual reverse-request cancellation, distinct call IDs, Take Control/Hand Back, and no delayed input after cancellation. |
| `Chat.remove`, folder deletion, `chat-list.js`, `library.js` | `deletion.test.js` | Backend acknowledgements before local removal, main/mini failures, partial folder failures, overlapping confirmations, session-ID collisions, new work during deletion, and workspace release. Includes reduced-motion variants. |
| Display projection, saved-cache reading, local sealing in `library.js` and `chat-lock.js` | `legacy-display.test.js` | Text/previews/markers/stats preservation, malformed cache data, exclusion of model-only fields, recovery-ID retention, main/mini saves, and sealed/unsealed projection using real Web Crypto. |
| Local lock presentation in `lock-ui.js` and `i18n.js` | `lock-ui.test.js` | Persistent cache-only protection disclosures in lock setup, management, locked screens, and labels. |

Recovery tests execute real chat/session and persistence logic, but use in-memory stores and a synthetic session peer. They do not prove an external backend's durable journal or real Electron reload behavior. The display cache must not become model history, and an uncertain input must not be silently resent.

### Provider, auth, model, and settings presentation

| Code area | Focused tests | Coverage |
| --- | --- | --- |
| Provider/auth UI and refresh logic in `settings.js` | `provider-auth.test.js` | Backend-supplied placeholders/status, overlapping key saves, input debounce, login/logout/cancel races, authoritative refreshes, stale events, backend closure, opaque provider IDs, escaped markup, and malformed/unsupported auth methods. |
| Catalog/model selection in `settings.js`, `model-stage.js`, `effort-slider.js`, chat configuration and stats | `model-capabilities.test.js` | Unknown versus advertised thinking/vision/context metadata, defaults versus saved preferences, canonical configuration responses, custom effort levels, unavailable selections, and catalog-cache validation. |
| `settings-general.js`, `user-context.js`, `settings-usage.js` | `settings-presentation.test.js` | Exact pinned-text character counts, local file/text caps and replacement, local-limit wording, and provider-neutral usage colors/totals. Attachment extraction is stubbed here. |
| `usage.js`, usage chart buckets | `usage.test.js` | ABP usage increments, arbitrary/reserved provider IDs, and omission of events with no model or no counted usage. |

These tests supply provider metadata and auth responses themselves. A passing login-race test means the UI rejects stale state; it does not mean any provider login was performed. For real settings DOM/startup integration, add the Electron smoke test when relevant, not a live provider dependency.

### Browser tests

For `browser-panel.js`, `host-tools.js`, and `desktop/browser.js`, start with `browser-lifecycle.test.js`. Its controlled renderer/Electron objects exercise:

- unavailable versus empty browser state, tool discovery, guest readiness/failure/recreation;
- serialized calls, receipt-time tab targeting, stable handles, capacity, and queued cancellation;
- page/ref identity, covered or moved targets, navigation races, stalled CDP calls, timeouts, and guest destruction;
- frozen read continuation, truncation/result metadata, and operation-scoped downloads.

Add `cancellation.test.js` and `turn-identity.test.js` when the change touches chat ownership, Stop, or Take Control/Hand Back. Use the separate real-webview script for changes that depend on actual DOM observation, navigation, input dispatch, screenshots, or guest lifecycle. The [browser host-tool contract](browser-host-tools.md) describes the boundary those tests exercise.

### Transport and process host

| Code area | Focused tests | Coverage |
| --- | --- | --- |
| Command configuration and startup in `desktop/backend-host.js` / `desktop/main.js` | `backend-config.test.js` | Environment/file precedence, invalid configuration diagnostics, whole executable strings versus literal argv, no shell expansion, inherited environment, home-directory startup, relative paths, and PATH lookup. Runs real local Node probes; the startup function is evaluated without booting Electron. |
| Subprocess lifecycle and JSON lines in `desktop/backend-host.js` | `backend-host.test.js` | Real scripted subprocess traffic, reinitialize/recovery and deduplication, split/non-JSON lines, malformed envelopes reaching the client, spawn failure, crash status, graceful shutdown, and descendant cleanup. |
| Framing and outbound flow control in `desktop/backend-host.js` | `backend-transport.test.js` | The 64 MiB UTF-8 byte boundary, split multibyte characters, oversized-line discard/resynchronization, outbound limits, bounded backpressure, and FIFO drain using a controlled writable stream. |

Transport limit tests allocate large buffers; they are not necessary for a label or layout edit. Process-tree assertions read Linux `/proc` and skip on other platforms. The POSIX symlink/path-resolution case in `backend-config.test.js` skips on Windows; parser tests still cover Windows-shaped paths.

### Security and regression checks

Security assertions are spread across the focused files rather than a separate security command:

- `untrusted-text.test.js` loads browser address-bar, approval-card, and chat error presentation with a hostile-text DOM harness. It checks that URLs, tool names/arguments/presentation, errors, and provider names remain text.
- `provider-auth.test.js` and `usage.test.js` cover opaque/reserved provider IDs without prototype collisions; provider tests also check markup/DOM-ID safety.
- `backend-envelope.test.js`, `backend-transport.test.js`, `turn-identity.test.js`, and `cancellation.test.js` protect malformed-input, resource-limit, stale-owner, and cancellation boundaries.
- `legacy-display.test.js`, `session-recovery.test.js`, `deletion.test.js`, and `lock-ui.test.js` protect display-only storage, fail-closed recovery/deletion, and accurate local-encryption scope.
- `boundary.test.js` guards the renderer CSP and frontend/backend separation. Its source scans are regression guards, not a network-isolation proof or a complete security audit.

## Scripted backend fixtures

[`test/fixtures/scripted-backend.js`](../test/fixtures/scripted-backend.js) is a deterministic JSON-lines ABP peer launched with the current Node executable by host tests and the Electron smoke test. It advertises protocol version `0.1`, a test provider/model, capabilities, canned usage/limits, and session operations. It has no model or provider SDK behind it.

Its turn behavior is selected by words in the input:

| Input word | Scripted behavior |
| --- | --- |
| `approve` | Sends an `approval.request` and includes the decision in the reply. The displayed shell command is presentation data; no shell tool is executed. |
| `browser` | Sends a `host.tool` request for `browser_tabs` with `action: 'list'`. |
| `hang` | Leaves the turn unfinished so Stop can be exercised. |
| `fail` | Completes with a synthetic auth error. |
| `crash` | Exits mid-turn with code 3. |

Ordinary turns stream fixed reply pieces and emit usage/completion events. The fixture remembers sessions, client-turn identities, and replay events **in memory** across `initialize` calls in the same process; it is not a durable backend. `test.hello` echoes initialize metadata for assertions and is fixture-only.

[`test/fixtures/tree-backend.js`](../test/fixtures/tree-backend.js) creates shell/sleep descendants in shared and separate process groups. Its `graceful`, `stubborn`, and `crash` modes test host cleanup, not agent tool execution. Let `backend-host.test.js` manage its lifetime rather than launching it as an application backend.

Both fixtures are excluded from packaged files by `!test/**`. The app does not automatically select a fixture or fall back to one when no backend is configured.

## Electron and E2E tests

### Application smoke test

`npm run test:e2e` runs only `test/e2e/smoke.mjs`. It starts the real app headlessly, connects over local CDP, drives composer/keyboard/button behavior, and asserts DOM and application state. It checks:

- startup and useful UI/settings errors with no backend;
- initialize host tools/render-guide metadata and backend-supplied provider/model presentation;
- streamed replies, approval interaction, the browser tab-list host tool, usage, and chat title;
- Escape to Stop, synthetic auth-error actions, manual compaction, saved display-data shape, backend crash reporting, and absence of captured page errors.

The saved-display assertion reads the saved conversation; it does **not** reload the renderer or relaunch the app. Detailed reload/replay scenarios currently live in the Node tests.

### Real browser guests

`node test/e2e/browser-lifecycle.mjs` is separate from both `npm test` and `npm run test:e2e`. It runs the real browser panel, host adapter, and webviews against data URLs, `about:blank`, and a deliberately nonexistent local file. Checks include stable tab/page IDs, a real ref click, frozen UTF-16 read pagination, stale/covered refs, navigation after clicking, screenshot metadata, tab switching during pointer delay, and explicit wait/navigation errors.

This tests actual frontend browser behavior without an agent choosing actions or a public website providing the page. Screenshots are checked for metadata, not compared against visual baselines.

### Launch and isolation

Both launchers use `--ozone-platform=headless`, `--disable-gpu`, and a local remote-debugging port. They set temporary `HOME` and `XDG_CONFIG_HOME`, clear the inherited `OPENGHOST_BACKEND`, and terminate their child app and remove temporary homes on normal cleanup. The smoke test explicitly configures its scripted subprocess for the backend case. Its scratch parent is `~/.cache/openghost-e2e`; the browser script uses the OS temporary directory.

The current headless/profile setup is Linux-oriented. The smoke script's source-Electron path lookup handles `electron`/`electron.exe`, not the macOS app-bundle executable layout. Do not assume these scripts establish cross-platform profile isolation or E2E coverage. Use a disposable environment when validating another platform, never real credentials or a normal user profile. `npm start` launches the interactive app with normal configuration; it is not an isolated test command.

## Avoiding live AI and provider calls

The Node tests use fake transports/backends, local in-memory data, controlled streams, or explicitly selected local subprocess fixtures. Provider names, keys, catalogs, and responses in tests are synthetic. The Electron smoke test supplies its own fixture command; the browser E2E script leaves the backend unconfigured and supplies local pages. No provider account, API key, installed production agent, or model download is required by these tests.

Keep this separation when adding tests: synthesize the boundary response instead of adding a provider client, real secret, sign-in flow, or paid request. For race bugs, control reply/event timing locally.

This is **not** a claim that the application has no networking. CDP uses local HTTP/WebSocket connections; browser guests and frontend media/icon services can intentionally network. The app renderer's `connect-src 'none'` and the static boundary checks do not forbid every image request or webview connection.

## Packaged-app tests and build checks

The existing package scripts are:

```sh
npm run dist        # Windows NSIS installer
npm run dist:mac    # Universal macOS DMG
npm run dist:linux  # Linux tar.gz
```

Each uses `electron-builder` with `--publish never`. Application files are packaged with ASAR; tests and docs are excluded. Use the appropriate build environment for the target, rather than expecting every target to build on every host.

The smoke test can target an **already built executable** instead of source Electron. For the Linux unpacked output:

```sh
OPENGHOST_E2E_APP="$PWD/dist/linux-unpacked/openghost" npm run test:e2e
```

This reuses the same no-backend/scripted-backend assertions. The executable must exist first; the command does not build it. The test runner, Node executable, and fixture still come from the checkout, not the package. This mode does not test installation/uninstallation, signing, or distribution delivery. The separate browser E2E script has no `OPENGHOST_E2E_APP` override.

The current [Build workflow](../.github/workflows/build.yml):

- runs on pushes to `release-*` branches or manual dispatch, not every development-branch push or pull request;
- uses Node 22 and `npm ci` in both jobs;
- runs `npm test` and `npm run dist:linux` on Ubuntu;
- runs `npm run dist:mac` on macOS without a Node test step, with signing identity auto-discovery disabled;
- uploads DMG/tar.gz artifacts and attempts screenshots with `continue-on-error: true`;
- does not run either E2E script, packaged smoke assertions, or a Windows job.

The screenshots are build/startup aids, not asserted UI tests. Workflow artifacts are not automatically published releases.

## When to broaden the checks

- **Normal focused change:** run the relevant Node file(s) from the map. Add neighboring ownership/lifecycle tests if the change crosses modules. Do not run transport stress tests or package the app merely to edit presentation text.
- **DOM, preload/IPC, startup, or webview integration:** add the relevant real-Electron script when mocks cannot establish the behavior. A pure browser change may need the browser script without the application smoke test; a catalog/startup change may need smoke without browser lifecycle checks.
- **Shared/cross-cutting change:** use `npm test` when touching common client/event/storage contracts, script loading, shared helpers, dependencies, or several subsystems, and for broader release validation. This is the full Node suite, not all available Electron checks.
- **Packaging/runtime/release change:** build when changing Electron/build versions, packaging files/entry points, icons/platform configuration, or preparing/verifying a distributable. Use packaged smoke when validating packaged behavior. Ordinary renderer logic, CSS, and documentation work does not inherently require a build.
- **Documentation only:** review Markdown, local paths/links, and `git diff --check`. Do not launch Electron, execute test suites, or build packages just for prose.

For visual behavior not covered by assertions, inspect the affected UI in the relevant themes, main/mini chats, keyboard/focus paths, long/streamed content, and reduced motion. Manual inspection supplements focused tests; it is not an existing automated visual suite.

## Adding a focused regression test

1. **Identify the violated frontend invariant.** For example: a failed durable checkpoint must dispatch no `turn.start`; a stale approval must not attach to a new turn; an older auth response must not overwrite a newer state.
2. **Choose the smallest existing harness.** Use a nearby `test/*.test.js` file and its real module loader. Use `fakeTransport()` or a local backend stub for protocol timing, a store stub for persistence failure, and controlled guest objects for browser state. Extend a subprocess fixture only when process/stdio behavior matters. Add an Electron case when the bug depends on real DOM/IPC/webview behavior.
3. **Make the failure deterministic.** Supply the minimal input, IDs, capability/catalog data, and event sequence. Use deferred promises, controlled timers, or `tick()` as existing tests do instead of arbitrary long sleeps. Keep backend/provider responses synthetic and browser pages local.
4. **Assert outcomes and forbidden side effects.** Check rendered/persisted state or outgoing messages, plus what must not happen: no duplicate dispatch, no input after Stop, no lost cache, no secret retained, or no stale state revival. Clean up subscriptions, timers, streams, temporary files, and children.
5. **Run the focused regression and its containing file.** Confirm it fails for the bug and passes with the fix. The name-filter command in Quick commands shows the existing Node runner syntax. A new top-level `test/*.test.js` file is picked up by `npm test`; a new fixture or E2E script is not. Broaden only for the affected boundaries described above.

Name tests after observable behavior, not an implementation detail. Keep the production implementation under test real; stub its external dependencies rather than replacing the method containing the bug.

## Coverage limits

- **Backend/provider conformance:** there is no live AI/provider suite or real external-backend integration suite here. Synthetic recovery peers retain memory, not a durable journal across backend process restarts. Backend implementation and provider behavior require their own tests.
- **Real reload/auth workflows:** recovery, retry, deletion, and auth races have focused Node coverage, but the Electron smoke script does not exercise a renderer reload, an app relaunch with saved data, or OAuth login/logout/cancel interactions.
- **Rendering and attachments:** there is no dedicated visual-baseline suite or comprehensive real image/text/Office/PDF/video extraction suite. Display projection, pinned-file metadata/limits, delayed preparation, and smoke rendering cover narrower behavior; they do not validate every renderer/extractor.
- **Platform/process lifecycle:** descendant cleanup is asserted only on Linux, and one configuration path case skips Windows. The build workflow has no Windows job and no automated E2E execution on any platform.
- **Packaging/security:** packaged smoke is opt-in, not an installer/signing test or a CI gate. Source/CSP and hostile-text regressions do not replace runtime trust-boundary testing or a security audit.
