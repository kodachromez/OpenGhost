# Native frontend fork

- Clone the dissected OpenGhost 1.3 frontend in `reference/openghost/`; it is the
  visual/behavior authority. Do not edit that frozen reference as part of a port.
- Reuse the existing whole Qt/QML files wherever possible. Do not redesign the UI
  or import custom animations, splash choices or features absent from OpenGhost 1.3.
- Use OpenGhost names in code; retain historical names only in provenance and
  attribution records. Do not revive tool/subagent panels or source-port controls
  absent from the reference's chat path.
- No Rust agent/backend, source-port RPC/client, provider logic, tools, Electron,
  Chromium, WebEngine, Node or React in the native target. The legacy web files
  under `reference/` are reference material only.
- Keep backend integration separate from rendering. No fabricated acceptance,
  results, credentials or session state when disconnected.
- Platform-specific work belongs in `src/platform/`, selected by CMake.
- Read `docs/cpp-port.md` before migrating dependencies. Preserve attribution.
- Focused checks: build, optional `native_ui_smoke`, and `git diff --check`.
