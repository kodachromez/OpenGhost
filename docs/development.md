# Development guide

OpenGhost-Frontend is an Electron app with a plain JavaScript renderer. This guide uses the current `package.json`, source files and tests as its reference. **Linux is the currently verified development and build target.** Other platform scripts exist, but their presence is not a claim of equivalent validation.

The frontend owns presentation, attachment preparation, local display caches and the built-in browser. A separately installed backend owns agent execution, provider networking, credentials, durable sessions, model-facing history, compaction and approval policy. A frontend change must preserve that boundary.

## Prerequisites

- Git and a checkout of this repository.
- Node.js **22.12 or newer**, with npm. The build workflow uses Node 22; locked build dependencies require at least 22.12. The Electron smoke harness also uses Node's built-in `fetch` and `WebSocket`.
- A Linux environment that can run Electron, including its system libraries and sandbox support. Interactive development needs a graphical desktop; the E2E scripts launch Electron headlessly. The build workflow uses Ubuntu, but this repository does not specify a complete distro-by-distro system-package installation recipe.
- Network access for `npm ci` and any packaging-tool downloads. Electron and electron-builder are development dependencies; there are no runtime npm dependencies.
- For actual agent responses, a trusted executable implementing [the backend interface](backend-interface.md), including `sessions.recovery`. No backend, provider account or API key is needed for the Node tests or scripted smoke tests.

There is no backend source/build toolchain to install from this repository. Consult the chosen backend's own documentation for installation, credentials and required executable arguments.

## Repository and worktree expectations

Run commands from the checkout root, where `package.json` and `index.html` live. Before editing or committing, check the actual checkout and branch:

```sh
pwd
git status --short --branch
git branch --show-current
git worktree list
```

- Work in the assigned contributor branch/worktree. Do not switch or modify another worktree, or commit directly to `main`.
- A linked worktree can have a `.git` file rather than a directory; leave Git's worktree metadata alone.
- Preserve unrelated local changes and untracked files. Stage only the files belonging to your change and review the staged diff before committing. Push only the intended contributor branch; inspect the configured remote rather than assuming its destination.
- Install dependencies in the checkout you are using. `node_modules/` and `dist/` are ignored output, not sources to hand-edit or commit. Commit `package-lock.json` alongside intentional dependency changes.
- Keep backend binaries, credentials and local `backend.json` configuration outside the repository. A backend is not an application runtime dependency or a bundled test fixture.
- Worktrees do **not** automatically isolate Electron profiles. Ordinary source and packaged launches may use the same local app data. The app also has a single-instance lock: a second launch can focus the existing window instead of starting your changed checkout. Fully quit the old app first.

## Install and run

From the checkout root:

```sh
npm ci
npm start
```

`npm ci` installs the lockfile's dependencies. `npm start` runs `electron .`, whose entry point is `desktop/main.js`. It loads the local `index.html`; there is no bundler, transpiler, development server, watch script or separate renderer build.

For renderer HTML/CSS/JavaScript edits, use **F5** or **Ctrl+R** in the app to reload. Use **F12** or **Ctrl+Shift+I** to toggle renderer DevTools. Restart the app after main-process/backend-host changes; relaunching is also the reliable check for preload and startup behavior.

A renderer reload sends `initialize` to the **same running backend process**, then reconciles opened sessions. It is not a process restart. Ordinary `npm start` uses your actual profile/configuration; use the isolated E2E harness for destructive storage, deletion or crash checks rather than real conversations and credentials.

Without a configured backend, the UI still opens and reports unavailability. Opening the page in a normal browser is not a substitute for the Electron/backend workflow: the desktop bridge and agent connection are absent.

## Configure a backend for local development

The process host reads configuration once at application startup:

1. A nonblank `OPENGHOST_BACKEND` environment variable takes precedence.
2. Otherwise, it reads `backend.json` under Electron's `app.getPath('userData')`, normally `~/.config/OpenGhost/` on Linux. The effective location can change with the environment, including `XDG_CONFIG_HOME`.

For a terminal launch:

```sh
OPENGHOST_BACKEND='["/absolute/path/to/backend"]' npm start
```

Or place this JSON in the user-data `backend.json`:

```json
{
  "command": ["/absolute/path/to/backend"]
}
```

Replace the placeholder with the actual executable. If it needs arguments, append each as a separate string in the array, using that backend's documented arguments. A plain executable string is also accepted, but it is **one complete executable path/name**, not a shell command line.

Important configuration behavior from `desktop/backend-host.js`:

- Unset, empty or whitespace-only environment values fall back to the file. An invalid nonblank override is an error, not a reason to fall back. An existing invalid/unreadable file is also an error.
- The host uses `spawn` with `shell: false`. It does not split arguments or expand `~`, `$HOME`, globs, pipes or redirections. Paths containing spaces belong in a single array element, without embedded shell quotes.
- Prefer absolute executable paths. Bare names use the app's inherited `PATH`; relative paths resolve from the **user's home directory**, not this checkout or the config-file directory. A desktop launcher can have a different environment from your terminal.
- The executable inherits the app's environment and user privileges. It is trusted local code, not a sandboxed plugin. A chat's workspace `cwd` is sent separately over the protocol and does not change the process's startup directory.
- This config selects one local stdio process, not an HTTP/WebSocket endpoint. The renderer cannot change the configured command.

**After changing configuration or fixing a stopped/crashed backend, fully quit and relaunch OpenGhost. There is no automatic backend restart.** Retry is a turn/session operation, not a backend launcher. On normal app quit, the host sends `shutdown`, closes stdin, waits up to two seconds, then terminates remaining work; Linux process-tree cleanup is covered by tests.

Provider sign-in and key handling are backend operations surfaced by Settings. Do not put credentials into source, display caches or test fixtures.

## Important npm scripts and checks

| Command | Current behavior |
| --- | --- |
| `npm start` | Launch Electron directly from source. |
| `npm test` | Run `node --test test/*.test.js`; does not include the E2E scripts. |
| `npm run test:e2e` | Run `node test/e2e/smoke.mjs` with real headless Electron and scripted/no-backend cases. |
| `npm run dist:linux` | Package a Linux `tar.gz` with electron-builder; `--publish never`. |
| `npm run dist` | Windows NSIS packaging, **not** a generic build command. |
| `npm run dist:mac` | Universal macOS DMG packaging. |
| `npm run shortcut` | Launch the `--create-shortcut` path, which creates a Windows `.lnk`; not a Linux setup step. |

There are no npm lint, format, typecheck or `dev` scripts. Do not substitute guessed commands.

For code changes, run focused regressions while iterating, then `npm test`. For example:

```sh
node --test test/session-recovery.test.js
node --test test/backend-client.test.js test/backend-envelope.test.js
```

The Node suite uses `node:test`, VM-loaded renderer modules and fake DOM/transport/Electron objects in `test/helpers.js` and individual tests. Process tests also launch fixture subprocesses; Linux descendant-cleanup tests read `/proc`. No live model provider is involved.

Use real Electron checks when changing DOM, IPC or guest-browser behavior that mocks cannot establish:

```sh
npm run test:e2e
node test/e2e/browser-lifecycle.mjs
```

The second command is a separate real-webview regression, **not included** in `test:e2e`. Both harnesses isolate HOME/config data and drive Electron over CDP. The smoke test covers no-backend startup, catalog/auth presentation, streaming, approvals, browser tools, usage, Stop, compaction, display saves and crash reporting. The browser harness uses local pages for lifecycle/input checks.

`test/fixtures/scripted-backend.js` is a deterministic ABP peer with canned responses, not an AI backend. Its sessions are in memory: do not use it to claim durable recovery across backend process restarts. Production recovery needs a backend with durable sessions/journals.

For documentation-only changes, review Markdown, local links/paths and `git diff --check`. Do not run the app, test suites or packaging solely to change prose.

## Linux build and package workflow

From the root of the Linux checkout:

```sh
npm ci
npm test
npm run dist:linux
```

The package configuration produces:

- `dist/OpenGhost-<version>-linux.tar.gz`, with the version from `package.json`.
- `dist/linux-unpacked/`, including the `openghost` executable for inspection/smoke testing.

To smoke-test that built executable with the existing isolated harness:

```sh
OPENGHOST_E2E_APP="$PWD/dist/linux-unpacked/openghost" npm run test:e2e
```

For an interactive packaged-app check, run `./dist/linux-unpacked/openghost` after quitting any other instance. This uses normal app configuration, just like the source launch; it does not supply a backend.

The build uses ASAR and excludes `dist/`, `images/`, `.github/`, Markdown, `docs/` and `test/` from application files. The archive contains the frontend, not an agent backend. There is no Linux installer script or publishing step in `dist:linux`.

`.github/workflows/build.yml` runs on manual dispatch and pushes matching `release-*`. Its Linux job uses Ubuntu/Node 22, runs `npm ci`, `npm test` and `npm run dist:linux`, and uploads archives as workflow artifacts. It does not run the E2E suite or automatically publish a release. Its best-effort screenshot step uses Xvfb; that is not the normal interactive development command. Do not adopt its `--no-sandbox` screenshot flag as a general sandbox workaround.

Windows/macOS packaging commands are listed for orientation only. Cross-building, signing/notarization and non-Linux runtime validation are not established by this guide. Check [LICENSE](../LICENSE) before distributing a build or reusing branding/design assets.

## Where code lives

There is no `src/` tree. Renderer scripts live at the repository root and are explicitly ordered in `index.html`.

| Area | Starting points |
| --- | --- |
| Renderer composition/layout | `index.html`, `script.js`, `styles.css`; `desktop.js` is renderer platform/splash setup, **not** the Electron entry point. |
| Chat, sidebar and mini chat | `chat.js` (including `SideChat`), `chat-list.js`, `mini-chat.js`, composer/control modules. |
| Rich output | `stream-view.js`, `markdown.js`, `highlight.js`, `tex.js`, `diagram.js`, `media-embed.js`, `media-slider.js`; `render-guide.js` describes supported syntax. |
| Settings/provider/model UI | `settings.js`, `settings-general.js`, `settings-appearance.js`, `settings-usage.js`, `model-stage.js`, `model-button.js`, `effort-*.js`, `mode-picker.js`. |
| Preload boundary | `desktop/preload.js` exposes the narrow `window.openghost` API. `desktop/browser-preload.js` is a separate guest preload. |
| Main process | `desktop/main.js`: window lifecycle, native services, IPC, disk store and backend startup/shutdown. `desktop/browser.js` and `desktop/pdf.js` implement browser/PDF services. |
| Backend RPC boundary | `backend-client.js`: handshake, requests, events, reverse requests and errors. `backend-protocol.js`: shared JSON-RPC envelope validation. `desktop/backend-host.js`: process hosting and JSON-line transport, not RPC method dispatch. |
| Attachments/context | `attachments.js`, `attachment-reader.js`, `file-kinds.js`, `user-context.js`; `chat.js` maps prepared input into ABP. |
| Browser host service | `browser-panel.js`, `host-tools.js`, `Chat.onHostTool`, browser IPC and `desktop/browser.js`. |
| Local persistence/protection | `library.js`, `chat-store.js`, `chat-lock.js`, `lock-ui.js`, `usage.js`, store handlers in `desktop/main.js`. |
| Regression/build inputs | `test/`, `package.json`, `package-lock.json`, `.github/workflows/build.yml`. |

The agent path is:

```text
renderer -> window.Backend -> window.openghost.backend -> backend:* IPC
         -> BackendHost -> external process stdin/stdout
```

Backend approvals and browser requests travel back through the same boundary to the owning chat. Do not add a second provider/agent connection in the renderer or main process.

## Making a normal frontend change

1. Find the owning module and the wiring in `script.js`/`index.html`. Read nearby tests before changing behavior.
2. Keep the change in the appropriate presentation/service layer. Add UI strings through `i18n.js`; use the existing CSS state/theme patterns. If adding a classic script, explicitly position it after its dependencies in `index.html`.
3. Add/update focused regressions. For a visual change, manually inspect long content, streaming and restored messages, main and mini chats, keyboard focus/Escape, scrolling, light/dark/system themes and reduced motion.
4. Check unavailable-backend behavior as well as the success path. Normal tool events drive working status, **not visible tool cards**; approval cards are a separate decision UI. Reasoning events are not rendered.
5. Run the relevant checks above, update affected docs/contracts, and review the diff for unrelated changes, secrets and generated output.

## Making a backend-boundary or protocol change

Start with [the ABP contract](backend-interface.md) and, for browser requests, [the browser host-tool contract](browser-host-tools.md). Trace the caller, renderer client, preload/IPC and host before editing.

- Specify request/result/event shapes, capabilities, errors, cancellation and compatibility behavior. The current handshake requires protocol version `0.1`; do not silently change what that version means for an existing backend.
- Use `Backend.request`, `notify`, `on`, `handle` and `can` rather than bypassing the client. `Backend.ready` resolves even on failure: await it **and check `Backend.available`**. Dispose an old client before replacing it so subscriptions and outstanding reverse requests are released.
- Keep method semantics in the client/caller and external backend. The process host frames JSON, bounds transport buffers and manages the child; it must not acquire provider APIs, tool policies or model-history logic.
- Preserve object params, exact string/safe-integer IDs, connection-scoped request correlation, deadlines and `$/cancelRequest`. JSON-RPC batches are unsupported. A malformed matching response must reject the right call, not dispatch another action.
- For chat work, preserve session incarnation (`sessionVersion`), client/accepted turn identities, message/input identities, `seq` ordering and terminal-event behavior. Route by owning session/turn, not whichever chat happens to be visible.
- Exercise absent capabilities, initialization failure, malformed/late/duplicate events, cancellation and reload. A timeout is not proof that the backend never accepted an operation; do not repair uncertainty by blind resending.
- Update the contract, relevant fixture behavior and regression tests together. Coordinate the external backend implementation separately; passing a scripted test does not establish real-provider compatibility.

Useful focused test groups (paths below are under `test/`):

| Change | Regressions to inspect/run |
| --- | --- |
| Layering, CSP, script order, dependencies | `boundary.test.js` |
| RPC, handshake, errors, cancellation/disposal | `backend-client.test.js`, `backend-envelope.test.js`, `backend-lifecycle.test.js` |
| Command config, framing, process lifecycle | `backend-config.test.js`, `backend-host.test.js`, `backend-transport.test.js` |
| Correlation, ordering, reverse-request ownership | `event-ordering.test.js`, `turn-identity.test.js`, `cancellation.test.js` |
| Recovery, retry, steering | `session-recovery.test.js`, `retry-steering.test.js` |
| Catalog/auth/model presentation | `provider-auth.test.js`, `model-capabilities.test.js`, `settings-presentation.test.js` |
| Display caches, locks, deletion | `legacy-display.test.js`, `lock-ui.test.js`, `deletion.test.js` |
| Browser lifecycle and untrusted output | `browser-lifecycle.test.js`, `untrusted-text.test.js`, plus the real-webview harness |
| Usage accounting | `usage.test.js` and recovery/ordering regressions |

## Provider and model presentation

A new backend-advertised provider/model should ordinarily appear without a frontend catalog edit. `auth.providers` supplies names, grouping, auth methods/status and limit metadata; `models.list` supplies model IDs, labels and capabilities.

For a presentation change:

- Work in `settings.js`, model/effort controls and, for account limits, `settings-usage.js`. Add varied catalog fixtures in the corresponding tests instead of a production provider-name switch.
- Keep provider IDs opaque and untrusted. Use `Map`/null-prototype dictionaries and escaped/text DOM assignment. Cover hostile IDs, unknown auth kinds, duplicate methods and disappearing models.
- Do not hardcode endpoints, auth URLs, key prefixes, model windows, vision support or thinking levels. Missing metadata stays unknown/unsupported; an unavailable saved selection must not silently select a different model.
- Keep backend defaults separate from saved user choices. Apply canonical fields returned by `session.configure`, including cleared thinking selections; do not infer new capabilities from a canonical selection.
- Auth actions use `auth.setKey`, `auth.login`, `auth.cancel` and `auth.logout`. Typed keys are transient; stored credentials stay in the backend. Preserve authoritative refreshes and freshness guards so stale auth/catalog responses cannot overwrite newer actions.
- If a genuinely new metadata field or auth interaction is needed, treat it as a protocol change and update the contract/tests. Do not implement provider behavior in a display adapter.

## Working on specific frontend areas

### Approvals

Start with `approval-card.js`, `Chat.claim`/`onApproval`/`onModeChange` in `chat.js`, and `mode-picker.js`.

Cards validate optional backend presentation and otherwise show the tool name/JSON arguments. Render commands, diffs, titles and errors as untrusted text. Preserve session/turn ownership and deduplication before showing a card. Test Allow/Deny, request cancellation, Stop, a superseding message, mode changes and late responses.

The backend decides whether approval is needed and what Ask/Auto/Full mean. Mode changes configure the backend; they do not let the frontend auto-allow a pending card. Browser `host.tool` execution must not introduce a separate frontend approval policy.

### Attachments and standing context

Follow `attachments.js` -> `attachment-reader.js`/`file-kinds.js` -> `chat.js` `inputOf`/`attachmentOf`. Keep preparation asynchronous and wait for each attachment's `ready` promise before dispatch. Paths come from the preload's `pathOf` bridge; never invent a usable filesystem path from a filename.

Test picker/drop/paste, delayed preparation, notes, failed reads and reload previews with image, text, PDF, video and unsupported files. Preserve bounded extraction, image resizing and cleanup. PDF reading lives in `desktop/pdf.js`/`desktop/pdf.html` and extracts selectable text, not OCR. Video preparation supplies metadata/preview and a local path when available, not frontend video interpretation.

Prepared backend input and saved display attachments are different shapes: `slim` in `chat.js` and `displayAttachments` in `library.js` retain previews, not complete inputs for later resend. If changing ABP attachment fields, update the contract too.

Standing instructions/pinned files use `settings-general.js` and `user-context.js`, reusing the reader. These are saved snapshots, not watched files. Local character/file limits are storage/payload guardrails, not model token/context estimates. `UserContext.forBackend()` supplies structured context; prompt assembly remains in the backend.

### Settings and usage

Keep general/context, appearance and usage changes in their respective `settings-*.js` modules. Appearance coordinates `theme.js` with native theme/title-bar handling in `desktop/main.js`; check startup as well as live switching.

`usage.js` maintains a local ledger from incremental backend reports, not provider billing or model history. Account limits come from `account.limits` when `usage.limits` is advertised. Preserve unknown values, per-provider isolation and replay deduplication; do not estimate missing token counts from characters. Test settings reopen/refresh and backend disconnect as well as a populated catalog.

### Browser-owned frontend services

Browser execution is intentionally frontend-owned because it uses the user's Electron webviews and browser session. Read `browser-panel.js`, `host-tools.js`, `Chat.onHostTool`, `desktop/browser.js` and the separate guest preload together.

Preserve serialized calls, receipt-time tab targeting, stable `tabId`, required `pageId` input preconditions and stale-target failures. Test tab switching/closure, guest crashes, delayed readiness, cancellation/timeouts and Take Control/Hand Back. Hand Back returns a fresh observation instead of replaying the interrupted action. A cancelled action may already have partial effects; do not automatically repeat it.

Keep host-tool schemas, result metadata and [the browser contract](browser-host-tools.md) aligned. Guest cookies/site state in `persist:browser` and password-submission hostname hints are not backend provider credentials or verified account state.

Do not confuse frontend-only with network-free: webviews browse sites; media/favicon images load remotely; the main process has a narrow YouTube oEmbed service. Preserve those intended services without weakening the app renderer's `connect-src 'none'` CSP or adding provider clients. Keep guest isolation, preload separation and IPC sender checks intact.

### Persistence, recovery, locks and deletion

The frontend stores a **display projection**, not model/session history. Begin with `library.js`'s display allowlist and write queues, `chat-store.js`, the disk-store handlers, and `Chat.reconcile`/`recoverTurn`/`checkpoint`/`end`.

On desktop, `ChatStore` keys map to `<userData>/store/<key>.json`:

| Location/key | Frontend data |
| --- | --- |
| `index` | Local chat/folder index. |
| `chats/<id>`, `mini/<id>` | Main/mini display caches and recovery markers, optionally sealed. |
| `context`, `context/<file id>` | Standing instructions and pinned-file snapshots. |
| `usage` | Locally recorded usage buckets. |
| Renderer `localStorage` | UI preferences, cached model catalog and other module-owned presentation state. |
| `<userData>/theme.json` | Native startup theme choice. |
| `~/OpenGhost/Chats/<space>` | Default chat **workspace**, not its display-cache location. |

Keep atomic disk writes and per-chat serialization. Required checkpoints must reach storage before start/retry/steer dispatch; do not swallow their failures as best-effort display saves.

Saved chats reconcile through `session.get` and `sessions.recovery` before continuation. Replay rebuilds the affected turn at the snapshot revision, without recharging usage or replaying browser actions. Preserve checkpoints and exact session incarnations; missing sessions/unknown pending turns stay display-only. Never import a display cache as model history, silently create replacement history, or resend uncertain input.

For recovery changes, test reload during a turn, events racing the snapshot, duplicate/late events, failed checkpoints, main/mini separation and terminal behavior. Test actual backend-restart durability against a compatible backend, not the in-memory fixture.

`chat-lock.js`/`lock-ui.js` protect local titles and display caches only. They do not protect backend history, credentials, workspaces, browser state or standing context. Keep that scope explicit. Parent-chat deletion requires backend acknowledgements for both main and mini session IDs before removing local records; folder failures retain retryable children. Mini-chat Clear has different best-effort backend deletion behavior—inspect that path rather than assuming the parent semantics.

## Debugging backend startup, crashes and configuration

Launch with `npm start` from a terminal so main-process and backend stderr are visible. Open renderer DevTools for UI/RPC errors. Useful read-only console inspections are:

```js
Backend.process
Backend.state
Backend.available
Backend.failure?.message
Backend.info
Backend.capabilities
```

| Symptom | Check next |
| --- | --- |
| No backend configured | Check the launched process's environment and effective user-data directory. An empty override still falls back to `backend.json`. Fully quit an existing instance before launching with new settings. |
| `OPENGHOST_BACKEND: ...` or `backend.json: ...` error | Check JSON syntax, the `command` field, nonempty executable and string arguments. Invalid configuration does not silently fall back. |
| Spawn error | Verify absolute path, executable permission, interpreter/runtime availability, inherited `PATH` and home-directory-relative paths. Read the actual OS error instead of replacing it with a generic connection message. |
| Process running, client unavailable/initializing | Inspect the `initialize` reply and protocol version `0.1`; check newline framing and stdout contamination. `Backend.ready` resolving alone does not prove availability. |
| Request timeout | The default deadline is 60 seconds; `auth.login` allows 15 minutes and `session.compact` 10 minutes. Check whether the backend answered/cancelled the exact RPC ID. Reconcile an uncertain turn rather than resending it. |
| Backend exited | Preserve the displayed exit code/signal and terminal stderr. Fix the external process/configuration and relaunch the app; reload or Retry cannot restart it. |
| Catalog present but saved chat cannot continue | Check `sessions.recovery`, `session.get`, the session incarnation and durable journal. A local display cache cannot restore missing backend history. |

Never paste secrets or complete private attachment payloads into diagnostics. The host does not redact backend logs. Report whether the failure was from a source launch or packaged app, Linux/Node versions, the relevant non-secret error/status and a minimal reproduction.

### External backend stdout/stderr contract

- **stdout is protocol-only:** one UTF-8 JSON-RPC 2.0 object per newline, in both directions over stdin/stdout. Do not print startup banners, debug text, progress bars or pretty-printed multi-line JSON there. Flush complete messages promptly.
- **stderr is for process logs.** The host prints chunks with a `[backend]` prefix to its console/stderr, without secret redaction. Backend stderr is not a chat response stream.
- Non-JSON stdout lines are logged and dropped. Parsed malformed envelopes reach the renderer validator so correlated errors can fail their requests. A stream with no terminating newline cannot deliver its message.
- Each line is capped at 64 MiB of UTF-8 bytes excluding LF. Oversized incoming lines are discarded through the next newline; outbound lines and the stdin write queue are bounded too. The host does not retain rejected sends for retry, and renderer IPC sends have no delivery acknowledgement.

When testing transport changes, use `backend-transport.test.js` and `backend-envelope.test.js`; do not add restart/retry loops to conceal framing or process failures.

## Coding and style expectations

Follow the file you are editing rather than imposing a new formatting system:

- Renderer modules generally use classic-script IIFEs, `'use strict'`, `window.*` exports and custom elements for small controls. Electron files use CommonJS; E2E scripts use `.mjs`. Preserve load order and global API names.
- Match nearby indentation, single-quoted strings, semicolons, naming and small helper patterns. Avoid whole-file formatting churn; there is no configured formatter/linter to enforce a different style.
- Keep comments focused on ownership, ordering and failure behavior. Prefer existing modules/patterns over adding a framework, bundler or provider SDK for an isolated UI task.
- Use `i18n.js` for UI copy, retain accessible labels/focus/keyboard behavior, and preserve reduced-motion and theme paths.
- Treat backend/provider/site/file text as untrusted. Use safe text assignment/escaping and existing URL validation. Preserve sandbox/context isolation, CSP and narrow preload/IPC surfaces; never expose raw Node/Electron objects to the renderer or browser guests.
- Keep asynchronous work cancellable and correlated to its owner. Release listeners/waiters, check cancellation after awaits, and preserve useful errors rather than silently treating failures as success.
- Add deterministic regression cases for behavior changes. Mocked tests cannot establish all real DOM, Electron or browser behavior; use the relevant smoke check when needed.

## When to update documentation and contracts

Include documentation in the same change when observable behavior or a contributor assumption changes:

- Update **this guide** for prerequisite, npm script, build output, configuration, debugging or worktree/setup changes.
- Update [backend-interface.md](backend-interface.md) for handshake/capability, method/event/error, provider/model metadata, attachment, recovery, process configuration or stdio/lifecycle changes. Keep corresponding tests/fixtures in sync.
- Update [browser-host-tools.md](browser-host-tools.md) and `host-tools.js` schemas for browser targeting, cancellation, preconditions, results or continuation changes.
- Update `render-guide.js` when supported response syntax changes; it describes rendering capabilities, not prompt policy or agent behavior.
- Update affected user-facing documentation when setup, ownership, privacy/protection scope or supported behavior changes. Do not promise backend behavior that only a frontend fixture implements.

Before handing off, review `git diff --check`, the changed-file list and the staged diff. State which checks actually ran, what remains unverified, and any separately required backend work.
