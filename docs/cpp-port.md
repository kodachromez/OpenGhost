# C++ / Qt frontend extraction

## Target and first-pass result

**Clone the dissected OpenGhost 1.3 frontend.** The existing native Qt port
supplies reusable code, not a new visual specification. OpenGhost's own motion
stays; the source port's custom appearance and extra features do not.

The standalone C++17/Qt 6.11/CMake executable builds and opens without any Rust
backend, backend executable, workspace grant, credentials or Node installation.
The copied QML window, sidebar, composer, settings, splash and native renderers
are used directly. A small disconnected `WindowController` supplies their
existing bindings; it is not an RPC client or a simulated agent.

The clean fork was made on branch `cpp-native-extraction` at
`/home/brian/openghost-native-qt`, because the supplied Rust-backend worktree and
existing `og-wt-native-cpp` worktree contained unrelated changes. Neither was
modified. This repository contains no Rust backend sources.

## Reference and provenance

The entire tracked dissected frontend at commit
`177ba954d0ddc7b47dd0526b11f98b517c1735ff` was moved, unchanged, under
[`reference/openghost/`](../reference/openghost/). Its `index.html`, `styles.css`,
`splash.js`, `splash-mist.js`, `settings*.js`, chat/composer/sidebar modules,
assets and boundary documentation remain available. Its old Electron launcher,
package files and tests are **reference only**: no CMake target, resource,
install rule or native runtime reaches them.

Most native files are whole-file imports from Ghosty's `native-ghosty/` at
`3b0e7a51` (the full hash and per-file source hashes are in
[`native-import.json`](native-import.json)). Committed source was used rather
than sweeping up unrelated local edits. Selective historical copies restore
OpenGhost 1.3 work that Ghosty later customized:

- `c4143066^`: original **1.3 spiral/mist splash and app reveal**, procedural
  ghost blink, theme splash palette and native theme implementation. The later
  return to 1.2's splash is not the target.
- `29b295c1^`: 1.3 backdrop, gradients and row colors, before Ghosty's glossy
  black/1.2 palette preferences.
- `31c8e38c^`: appearance page/store and welcome ghost without the owner's
  custom mascot splash selector/handoff.
- `b7caaae6^`: General page without Ghosty's execution-backend choices.
- `3222049f`: whole sidebar and list-model files with the already-implemented
  **1.3 folderless Chats group**. Its search-shell colors are restored to the
  earlier 1.3 parity implementation; none of its service-switching code is used.

The original 1.3 parity work is recorded upstream in Ghosty's
`docs/reviews/2026-10-02-openghost-1-3-visual-parity.md` (commits `608f05e6` and
`81212f65`), and the approvals/mode-picker port in `b7caaae6`. These are provenance,
not a claim that this extraction repeated the full visual qualification.

## What was copied

| Area | Native files retained |
| --- | --- |
| Native window and layout | `qml/Main.qml`, `Backdrop.qml`, sidebar and chat delegates; the useful clipboard/selection/network-denial helpers from `window.cpp` |
| Sidebar and chat | `Sidebar.qml`, `ChatEntry.qml`, `LiveText.qml`, `model.*`, folderless Chats grouping, hover/glide/fade/selection components |
| Composer | Existing composer inside `Main.qml`, model/effort controls and stages, mode picker/dock, file-card presentation, tooltips |
| Settings | `SettingsDialog.qml`, General, Providers, Usage and Appearance; `settings.*` presentation model; `appearance.*` local theme persistence |
| Markdown and code | `markdown.*`, `highlight.*`, `rich.*`, `cssfont.*`, Markdown/Block/InlineText/CodeBlock/TableBlock QML |
| TeX and diagrams | `tex.*`, the full `diagram*.cpp/.h` family, `diagramview.*`, editable diagram view and its native drawing/selection code (1.3's 43-kind engine) |
| OpenGhost artwork and motion | Procedural `ghost.*`, original 1.3 `Splash.qml` and `mist.frag`, welcome/working ghost, `motion.*`, `wave.*`, `reveal.*`, effects, springs, entrance/glide components and shader resources |
| Other presentation | Icons, file kinds, shadows, exposure tracking, approval cards, metrics and selection/copy machinery |
| Assets | Original OpenGhost icon; all applicable source/artwork terms and attribution retained |

`presentation.h` extracts only the display-value declarations needed by the
copied list models and settings from `client.h` and `transcript.h`.
No implementations of those clients, projections or protocol parsers were
imported. `presentation.cpp` retains two display-truncation marker strings used
by the rich renderer.

Most imported files are unchanged apart from the QML namespace/resource and
frontend-binding names. Changes to large QML files remove additions or reconnect
existing components; layout, drawing algorithms and OpenGhost animation curves
were not reimplemented. The wordmark and retained code names are **OpenGhost**.
Historical names remain only in source provenance and attribution records.

### Explicit exclusions

- All Rust/backend/agent/tool code; `client.*`, `rpc.*`, `launch.*`, backend
  transcript/activity projection implementations, service discovery and backend
  process/signal supervision.
- The source port's file-upload/storage bridge, audio/voice implementation, notification
  delivery/policy, installer/updater scripts and RPC tests.
- The owner's custom mascot splash/image, Nunito font, custom splash
  mist/aura shaders, Ripple splash/motion assets and happy-motion variants.
- The source port's splash selector, custom Model and Notifications settings tabs and
  bell/toggle animations, execution-mode settings, voice/transcription controls,
  slash-command helper and added Stop button. OpenGhost stops with Escape.
- Tool/subagent result panels, child activity DTOs, transcript thinking disclosure,
  startup workspace/service picker and Recover/Abandon & Delete banner. These
  unused source-port paths and their model/controller bindings have been removed.
  The reference's `chat.js::applyEvent` tracks tool status and shows the working
  ghost; it does not create tool/subagent/thinking transcript cards. Approval
  cards, model thinking-level selection and the original working ghost remain.
- No Electron, Chromium, WebEngine, Node or React dependency in the native
  project. Qt QML's own JavaScript expressions are not Node or a browser.

## Remaining presentation contracts

The visible shell now follows the reference's four settings sections and 1.3
appearance. Some **presentation interfaces**, not backend implementations, still
reflect the source port:

- `Entry`, `Attachment`, `Session`, `Selection`, `Account` and model roles; the
  account prompt/provider-row fields and permission identifiers are not ABP DTOs.
- Existing QML action names and acknowledgement/draft bookkeeping. These must be
  mapped deliberately to the dissected frontend's ABP semantics, not assumed to
  match because method names look similar.
- Attachment count/preview assumptions, title limits, provider colors and fallback
  names, plus the native renderer's pre-existing bounded-rendering limits.

These contracts still need deliberate ABP integration; renaming retained code to
OpenGhost does not make it a wire-compatible backend adapter.

This is a buildable extraction, **not yet a complete functional or pixel-exact
1.3 clone**. The inherited native port still lacks parts of the reference,
including persistent mini-chat, chat locks, browser/media integration, plus-menu
operations, and the visual diagram form editor. They have not been invented in
this first pass. Existing text/antialiasing, shadow and drawing differences also
remain; this task did not rerun the original full pixel-comparison suite.

## What still depends on the old backend

**No backend is needed to build or launch.** The following UI paths are
explicitly disconnected:

- Catalog/auth/defaults; send/cancel/steer; session listing/history/rename/delete,
  approvals and mode changes.
- Attachment upload, stored-image retrieval and per-run usage/metrics.

`src/window.*` returns no accepted submission, does not append chat messages,
starts no subprocess, reads no credentials and sends no network requests.
Unavailable operations cannot clear a draft or claim a saved result. The only
working OS-facing helpers are native window behavior, text clipboard/selection,
allowlisted user-clicked external links, and theme preferences.

The original General/Usage local stores were also **not** brought over: they
share the source port's POSIX/file-transfer helpers. Their QML is present, but General is
read-only and Usage has no connected data. This does not mean these frontend
preferences should become agent logic: their eventual ownership should match the
[dissected frontend contract](../reference/openghost/docs/backend-interface.md).
Appearance alone persists through Qt's app-specific path, separate from previous
native and Electron profiles. No old profile migration is attempted.

## Platform boundaries and remaining Windows/macOS work

`src/platform/` is selected by CMake:

- `desktop.cpp`: Qt external-link dispatch and the explicit
  `OPENGHOST_REDUCED_MOTION=0/1` override.
- `linux.cpp`: copied Linux render-loop/portal setup, desktop identity and KDE
  reduced-motion preference lookup. These assumptions no longer live inside the
  shared renderer.
- `portable.cpp`: Qt-native window/dialog/clipboard path for Windows/macOS,
  with no Linux environment, DBus or POSIX dependency. Native reduced-motion
  discovery there is still a follow-up, not a claimed implementation.

CMake has MSVC UTF-8/warning options, a Windows GUI target and a macOS bundle;
shaders are compiled through Qt ShaderTools. Still to qualify: platform SDK/Qt
builds, framework/plugin deployment, bundle/EXE icons and signing, native dialogs
and link behavior, accessibility/reduced-motion discovery, DPI/font metrics,
IME/shortcuts, graphics backends, and the appearance store's permissions/ACL
behavior. No Linux-only backend process/descriptor code was copied as a supposed
cross-platform implementation.

## Focused checks

On Linux with Qt 6.11.2 and GCC 16.2.1:

- Release configure and build succeeded, using C++/Qt only.
- One `native_ui_smoke` check: independent offscreen/software launch, 1.3 splash
  reveal, both themes, all four settings pages, no source-port-only controls,
  disconnected-send/attachment refusal, Markdown/TeX/diagram rendering, native
  close signal and no QML load/binding warnings.
- The same smoke launched on the real **Wayland/OpenGL** Qt scene graph and
  exited successfully. The uninstalled development executable produced a
  nonfatal portal app-ID registration warning; the Linux install includes its
  desktop entry.

The cleanup pass repeated the Release build and offscreen smoke, adding checks
for the removed panels/helpers, all three retained transcript delegates and
message-state updates. A case-insensitive search found no former product names
in native source, QML, tests, shaders, resources or CMake. The frozen reference
and original license notices were not changed.

Windows/macOS and full visual/behavior parity were not tested. No Rust, Electron,
Node, provider login or live model tests were run. Commands are in the root
[README](../README.md).

## Next smallest migration step

Keep the copied UI/renderers fixed. Add a narrow **read-only ABP adapter** for
`initialize`, `auth.providers` and `models.list`, tested against a local scripted
peer using the dissected frontend's contract. Translate those answers into the
existing settings presentation model, replacing its unavailable state. Do not
copy the source port's RPC, add an agent, or enable sending until durable turn/session
identity and acknowledgement handling have been migrated explicitly.
