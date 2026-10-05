# OpenGhost-Frontend

OpenGhost Frontend is an Electron desktop interface for external AI agent backends, with packaging targets for Windows, macOS and Linux. It combines streaming chat, rich visual responses, local conversation management and a shared browser in a plain JavaScript, HTML and CSS interface.

**This repository is frontend-only.** It does not include an agent runtime or model-provider clients. A separately installed, compatible backend supplies the agent loop, provider integrations, authentication, model-facing session history and non-browser agent tools. The app launches without a backend, but generating responses requires one.

## Features

- **Rich conversations:** streaming Markdown, syntax-highlighted code, math, image galleries and YouTube preview cards.
- **Interactive visuals:** charts and diagrams with in-place editors. Edits affect the frontend's display copy, not backend history.
- **Organized chats:** standalone conversations, project-folder chats and saved mini chats for side questions.
- **Attachments:** images, videos, PDFs and other files, with previews and local text extraction where supported. Interpretation depends on the backend and model.
- **Shared browser:** a tabbed browser panel with backend-driven navigation and interaction, plus controls to take over and hand control back.
- **Backend-driven controls:** provider connection settings, model and effort selection, Ask/Auto/Full access modes and approval cards. The backend defines permission behavior and decides when approval is required.
- **Usage visibility:** token, cache and context statistics from backend reports, account limits when available, and manual compaction when supported.
- **Personalization:** shared instructions and reference files, light/dark/system themes, and password locks for local chat display caches. Locks do not protect backend history or other files.

## Architecture

```text
Renderer UI and backend-client.js
        ↕ window.openghost.backend
Preload bridge (desktop/preload.js)
        ↕ Electron IPC
Main process and desktop/backend-host.js
        ↕ JSON Lines over stdin/stdout
External agent backend
```

| Layer | Responsibilities |
| --- | --- |
| Renderer | Chat and settings UI, rich response rendering, attachment preparation, local display state, and the JSON-RPC client. |
| Preload | Exposes the `window.openghost` API across Electron's context-isolated boundary for backend messages, storage, folders, browser operations and other desktop services. |
| Main process | Creates the application window, handles native services and local storage, manages browser guests, and starts, relays messages to and stops the configured backend process. |
| External backend | Owns agent execution, provider API traffic, credentials, durable sessions, compaction and approval policy. It can request the frontend's browser host tools. |

### Backend protocol

The connection uses **JSON-RPC 2.0 over JSON Lines**: one UTF-8 JSON object per line on the backend process's stdin/stdout. The Agent Backend Protocol (ABP v0, protocol version `0.1`) begins with an `initialize` handshake that exchanges capabilities, browser-tool schemas and the renderer's format guide. Backend logs belong on stderr, not stdout.

The frontend sends turn, session, model and authentication requests. The backend streams response and usage events and can make reverse requests for approvals (`approval.request`) or browser operations (`host.tool`). Chat execution requires the backend's `sessions.recovery` contract; local display caches are not sent back as model history.

Approval requests appear as cards. Ordinary tool calls show a working indicator, not tool cards, and reasoning events are not rendered. See the [backend interface](docs/backend-interface.md) for the complete contract.

### Data and networking

The frontend stores its chat index, display caches and preferences locally; the backend owns its session history and credential storage. Local chat locks encrypt only the frontend display cache.

Frontend-only does not mean network-isolated. The renderer can load remote images, thumbnails and favicons; the main process fetches YouTube preview metadata; and the built-in browser loads web pages. AI/provider API requests belong to the external backend.

## Development

### Install and run

Use Node.js **22.12 or newer** and npm. From the repository root:

```sh
npm ci
npm start
```

`npm start` launches Electron directly from source; there is no separate renderer build step. Configure a backend as described below to use the chat. Without one, the interface opens and reports that no backend is configured.

### Build packages

Run the command on the corresponding operating system. Packages are written to `dist/` and do not include an agent backend.

| Platform | Command | Output |
| --- | --- | --- |
| Windows | `npm run dist` | `dist/OpenGhost-<version>-Setup.exe` |
| macOS | `npm run dist:mac` | `dist/OpenGhost-<version>-mac.dmg` |
| Linux | `npm run dist:linux` | `dist/OpenGhost-<version>-linux.tar.gz` |

### Tests

- `npm test` — unit and frontend/backend boundary tests.
- `npm run test:e2e` — headless Electron smoke test using the scripted backend fixture, without a model provider.

The fixture in `test/fixtures/` is for testing only and is not a usable AI backend or part of packaged builds.

## Backend configuration

Install a backend that implements the [backend interface](docs/backend-interface.md), then configure its executable using either:

1. **`OPENGHOST_BACKEND`** — an executable path or a JSON array containing the executable and its arguments. A nonblank value takes precedence over file configuration.
2. **`backend.json`** — a file in Electron's application user-data directory with a `command` field in the same form.

For example, in a POSIX shell:

```sh
OPENGHOST_BACKEND='["/absolute/path/to/backend"]' npm start
```

Or in `backend.json`:

```json
{
  "command": ["/absolute/path/to/backend"]
}
```

Replace the placeholder with your backend executable and append any arguments it requires as separate array elements. These settings select a local process, not an HTTP endpoint or a shell command line. Prefer absolute paths; the host does not perform shell expansion.

Typical user-data locations are `~/.config/OpenGhost/` on Linux, `~/Library/Application Support/OpenGhost/` on macOS and `%APPDATA%\OpenGhost\` on Windows. See the [configuration reference](docs/backend-interface.md#connecting-a-backend) for syntax, precedence and platform details.

**Only configure a trusted backend.** It runs with your user privileges, inherits the app's environment and starts in your home directory; it is not sandboxed. Provider credentials and sign-in behavior are managed by that backend. Settings displays the connection methods and models it advertises.

After changing configuration or resolving a backend failure, relaunch the app. There is no automatic backend restart; Retry does not start a backend process.

## Project structure

| Path | Purpose |
| --- | --- |
| `index.html`, `styles.css`, `script.js` | Application shell, styling and UI wiring. |
| `chat.js`, `mini-chat.js`, `library.js`, `chat-store.js`, `chat-lock.js` | Conversations, display persistence and local chat locks. |
| `markdown.js`, `highlight.js`, `tex.js`, `diagram.js`, `media-embed.js`, `stream-view.js` | Rich response rendering. |
| `settings*.js`, `attachments.js`, `attachment-reader.js`, `user-context.js` | Settings, attachment preparation and shared user context. |
| `backend-client.js`, `backend-protocol.js`, `render-guide.js` | Backend RPC client, envelope validation and supported rendering formats. |
| `browser-panel.js`, `host-tools.js` | Browser UI and backend-facing browser-tool integration. |
| `desktop/` | Electron main process, preload bridges, backend process host, browser and PDF services. |
| `test/` | Unit tests, backend fixtures and Electron smoke tests. |
| `docs/` | Protocol documentation, browser lifecycle details and architecture records. |
| `.github/workflows/`, `package.json` | Build automation, scripts and packaging configuration. |

## Documentation

- [Backend interface](docs/backend-interface.md) — configuration, RPC methods, events, capabilities and session recovery.
- [Browser host-tool lifecycle](docs/browser-host-tools.md) — browser targeting, user control, cancellation and result metadata.
- [Architecture](docs/architecture.md) — current component ownership, persistence and lifecycle.
- [Development](docs/development.md) — setup, platform limits and contributor workflow.
- [Testing](docs/testing.md) — available checks and coverage limits.
- [Security](docs/security.md) — trust boundaries, protection scope and remaining limits.
- [Frontend separation notes](docs/frontend-only.md), [backend separation audit](docs/backend-removal-audit.md) and [boundary review](docs/reviews/2026-10-04-frontend-only-backend-boundary-audit.md) — historical migration/design/review context, not current implementation or protocol specifications.

## License

The source code is MIT-licensed. The OpenGhost name, logo, animations and visual design are excluded and subject to separate restrictions. See [LICENSE](LICENSE) before distributing builds or using those materials.
