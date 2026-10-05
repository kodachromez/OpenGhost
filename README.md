# OpenGhost Native — frontend extraction

Standalone C++17 / Qt Quick frontend targeting the **dissected OpenGhost 1.3
frontend**, not Ghosty's custom appearance or features. The existing native Qt
implementation was copied from Ghosty; this is not a UI rewrite.

This first pass launches independently, with **no backend connected**. It does
not start a process, authenticate, send messages, or fabricate responses.
Appearance settings work; backend-dependent controls refuse or remain disabled.

## Build and launch

Requires CMake 3.21+, a C++17 compiler and **Qt 6.11+** development packages:
Core, Gui, Network, Qml, Quick, QuickControls2, QuickDialogs2, ShaderTools and
the Qt Quick Layouts, Shapes and Effects QML modules. Qt Test is optional.
No Cargo, Node, Electron, Chromium, WebEngine or React is used by the native build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./build/openghost-native
```

On multi-configuration generators use `--config Release`; on macOS the target
is an app bundle. Windows/macOS build wiring exists but is not yet qualified.
Qt must be discoverable through its normal CMake prefix/toolchain configuration.

```sh
# One small, offline launch/settings/renderer smoke check (~6 seconds).
cmake -S . -B build -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

The smoke check uses a temporary appearance store. The normal app keeps only
`appearance.json` under Qt's `AppConfigLocation` for `openghost-native`; it does
not read Ghosty's credentials, conversations or preferences.

## Layout

- `qml/`, `src/`, `shaders/`, `resources/`: copied native presentation and its
  small standalone/disconnected integration layer.
- `src/platform/`: OS integration, selected by CMake.
- [`reference/openghost/`](reference/openghost/): the **unchanged dissected 1.3
  frontend**, retained for visual/behavior comparison only. Its Electron/JS
  files are never built, loaded or installed by the Qt application.
- [`docs/cpp-port.md`](docs/cpp-port.md): copy inventory, exclusions, limitations,
  platform work and next migration step.
- [`docs/ghosty-import.json`](docs/ghosty-import.json): source commits and hashes.

This is an independent local port, not official OpenGhost or a completed
feature-for-feature clone. Keep the original frontend as the authority for
remaining work. Upstream restricts its name, artwork, animations and visual
design separately from source code: see [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).
