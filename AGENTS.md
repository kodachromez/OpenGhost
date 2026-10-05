# Native frontend fork

- Clone the dissected OpenGhost 1.3 frontend in `reference/openghost/`; it is the
  visual/behavior authority. Do not edit that frozen reference as part of a port.
- Reuse existing whole Qt/QML files from Ghosty wherever possible. Do not redesign
  the UI or import Ghosty's custom animations, splash choices or extra features.
- No Rust agent/backend, Ghosty RPC/client, provider logic, tools, Electron,
  Chromium, WebEngine, Node or React in the native target. The legacy web files
  under `reference/` are reference material only.
- Keep backend integration separate from rendering. No fabricated acceptance,
  results, credentials or session state when disconnected.
- Platform-specific work belongs in `src/platform/`, selected by CMake.
- Read `docs/cpp-port.md` before migrating dependencies. Preserve attribution.
- Focused checks: build, optional `native_ui_smoke`, and `git diff --check`.
