# OpenGhost Native — frontend extraction

Standalone C++17 / Qt Quick frontend targeting the **dissected OpenGhost 1.3
frontend**. It reuses the existing native Qt implementation without the source
port's custom appearance or extra features; this is not a UI rewrite.
See [NOTICE.md](NOTICE.md) for attribution.

Default launch is **disconnected**. Use `--fake-backend` to exercise chats,
streaming, Escape Stop and model switching against an explicitly labelled,
in-memory C++ fixture. It runs no model, tools, credentials, network or process.
General instructions, model/effort/mode preferences and appearance save locally.
There is no Rust/backend transport integration yet.

## Build and launch

Requires CMake 3.21+, a C++17 compiler and **Qt 6.11+** development packages:
Core, Gui, Network, Qml, Quick, QuickControls2, QuickDialogs2, ShaderTools and
the Qt Quick Layouts, Shapes and Effects QML modules. Qt Test is optional.
No Cargo, Node, Electron, Chromium, WebEngine or React is used by the native build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./build/openghost-native                 # disconnected
./build/openghost-native --fake-backend  # explicit in-memory demo
```

On multi-configuration generators use `--config Release`; on macOS the target
is an app bundle. Windows/macOS build wiring exists but is not yet qualified.
Qt must be discoverable through its normal CMake prefix/toolchain configuration.

```sh
# Focused contract tests plus disconnected/fake UI smoke checks.
cmake -S . -B build -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build build --parallel 4
ctest --test-dir build -R '^(native_contract_test|native_ui_smoke|native_fake_ui_smoke)$' --output-on-failure
```

Smoke checks isolate appearance and preferences in temporary directories. The
normal app keeps `appearance.json` and `preferences.json` under Qt's
`AppConfigLocation` for `openghost-native`; it reads no previous profile. Fake
chats disappear on exit. Unimplemented capabilities explicitly refuse; the fake
is not a durable or production-conforming backend.

## Layout

- `qml/`, `src/`, `shaders/`, `resources/`: copied native presentation and its
  small presentation adapter.
- `src/backend/`: typed semantic contract, explicit fake, and transport interface
  (no wire/process implementation).
- `src/frontend/`: frontend-owned chat state and portable local preferences.
- `src/platform/`: OS integration, selected by CMake.
- [`reference/openghost/`](reference/openghost/): the **unchanged dissected 1.3
  frontend**, retained for visual/behavior comparison only. Its Electron/JS
  files are never built, loaded or installed by the Qt application.
- [`docs/cpp-port.md`](docs/cpp-port.md): copy inventory, exclusions, limitations,
  platform work and next migration step.
- [`docs/native-import.json`](docs/native-import.json): source commits and hashes.

This is an independent local port, not official OpenGhost or a completed
feature-for-feature clone. Keep the original frontend as the authority for
remaining work. Upstream restricts its name, artwork, animations and visual
design separately from source code: see [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).
