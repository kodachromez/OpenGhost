# Frontend-only OpenGhost

Branch `frontend-only-rust-backend`, from `main` at `42f0dc8` (OpenGhost 1.3.0). The backend-removal audit
(`docs/backend-removal-audit.md`) is the source of truth for what counted as backend; this records what was done with it.

```text
OpenGhost UI  →  backend-client.js (ABP v0)  →  desktop/backend-host.js  →  future external backend (Rust)
```

The interface itself is in `docs/backend-interface.md`.

## Removed

| Removed | What it was |
|---|---|
| `desktop/llm.js` | provider run registry, `llm:*` and `auth:*` IPC |
| `desktop/openai.js` | OpenAI Responses API and the ChatGPT/Codex backend client |
| `desktop/anthropic.js` | Anthropic Messages API client (`@anthropic-ai/sdk`) |
| `desktop/chatgpt.js` | ChatGPT OAuth (PKCE, loopback :1455), token refresh, plan limits |
| `desktop/keys.js` | provider API key vault (`safeStorage`, `keys:*` IPC) |
| `desktop/tools.js` | the agent's shell, file, git, `fetch_url` and video tools |
| `desktop/media.js` | image downscale and video frames for those tools |
| `deepseek.js` | DeepSeek client called from the page |
| `providers.js` | the renderer's provider facade, error mapping, usage hook |
| `agent-prompt.js` | the agent's system prompt, environment and permission-mode text |
| `agent-tools.js` | tool schemas, web search and media scraping, result formatting, approval policy (`needsApproval`) and its shell/git effect analyzers |
| from `chat.js` | the request/tool loop (`loop`, `request`, `useTool`, `approve`), system prompt and notes (`system`, `notes`, `FORMAT_GUIDE` as a prompt, `VISUAL_CHECK`, `TOOL_NOTES`), cache seams, history and transcript building, attachment→prompt XML, token estimation, compaction (`compact`, `compactIfNeeded`, `COMPACT` prompts), title generation (`name`, `TITLE_PROMPT`), the mini chat's prompt and history (`SIDE`, `snapshot`, `history`, `seam`) |
| from `user-context.js` | formatting the instructions and files into the system prompt (`prompt`, `block`, `pictures`) |
| from `usage.js` | the provider allowlist and provider-specific usage shapes |
| from `settings.js` | hard-coded providers, key storage and migration, provider-specific model loading and `models-stale` |
| from `settings-usage.js` | ChatGPT limits through `auth:limits` and the DeepSeek balance call |
| from `media-embed.js` | YouTube titles through the agent's `fetch_url` tool |
| `package.json` | the `@anthropic-ai/sdk` dependency (and its lockfile entries) |
| `i18n.js` | provider-specific strings (ChatGPT/OpenAI/Anthropic/DeepSeek key and sign-in texts, DeepSeek HTTP errors) and the titles of the removed tools' approval cards |
| `index.html` CSP | `connect-src https://api.deepseek.com` → `connect-src 'none'` |

No backend tests existed on `main` to remove (it had no tests at all).

## Added

| File | Role |
|---|---|
| `backend-client.js` | `window.Backend`: the UI's only way to an agent. JSON-RPC 2.0 peer: handshake, requests, events, reverse requests, `$/cancelRequest`, error wording |
| `desktop/backend-host.js` | starts the configured backend process and relays JSON lines; reports `none` / `running` / `exited` / `error` |
| `render-guide.js` | `FORMAT_GUIDE` minus its backend policy lines, handed to the backend at `initialize` (audit §3.6, §13.9) |
| `host-tools.js` | the built-in browser's tool schemas and result shaping, for `host.tool` (audit §7.5) |
| `docs/backend-interface.md` | the interface for an external backend |
| `test/` | unit tests (`npm test`), a headless end-to-end smoke test (`npm run test:e2e`), and a scripted ABP backend fixture used only by those tests |

## Kept, and rewired to the boundary

- `chat.js`: all rendering, scrolling, streaming, stats cards, locks and restore are unchanged; a turn is now
  `turn.start` plus the backend's events, Stop is `turn.cancel`, Retry is `turn.retry`, sending while busy is
  `turn.steer`, Compact is `session.compact`, a model switch is `session.configure`, titles come from `session.updated`.
  Approval cards come from `approval.request`; the browser's steps from `host.tool`.
- `settings.js`: the Providers page draws the same rows from `auth.providers`; the model picker and effort levels come
  from `models.list`; keys and sign-ins go to `auth.*`.
- `settings-usage.js`: the ledger is fed by `usage` events; plan limits and balances come from `account.limits`.
- `desktop/main.js`: window, theme, store, folders and PDF text as before; the chats-folder service moved here from
  `tools.js`; new IPC `backend:*`, `browser:run`/`browser:cancel` (host tools), `media:video-info` (YouTube oEmbed only).
- `desktop/browser.js`, `desktop/pdf.js`, `desktop/browser-preload.js`: unchanged.

Saved chats now hold a display copy only (text, attachments with their preview, model, usage, compaction markers, stats
cards). Chats saved by 1.3.0 still open: their model-facing parts are ignored, and pictures are still found in them.

## What can still reach the network

- The renderer: nothing (`connect-src 'none'`, no `fetch`; enforced by `test/boundary.test.js`).
- The main process: `https://www.youtube.com/oembed` for video card titles (a fixed host and an 11-character video id),
  and the built-in browser's webviews, which load what the user or the backend's `host.tool` calls open.
- The external backend, which is the only thing that talks to a model provider.

## Still coupled to the old backend

- **Approval presentation.** 1.3.0 worked out a command's effect (delete, install, …) in the app as a safety net. That
  analysis was tied to the removed tools' names (`run_bash`, `write_file`, …) and was removed with them; a backend must
  send `presentation` or the card falls back to the tool name and its JSON arguments.
- **Storage keys.** The effort preference is still stored under `deepseek.effort`, and saved chats from 1.3.0 still
  contain their old model-facing history, which nothing reads now.
- **Provider colors.** Settings → Usage keeps 1.3.0's colors for the provider ids it knew (`chatgpt`, `openai`,
  `anthropic`, `deepseek`, plus `openai-codex`); other providers take the palette in turn.
- **Frontend-held policy that a backend may want.** The chat folder for chats without a project
  (`~/OpenGhost/Chats/<name>`) is still chosen by the frontend and sent as `cwd`. Ask/Auto/Full are still the
  frontend's three modes (`settings.js` `MODES`).
- **Display-only diagram edits.** Editing a diagram changes the frontend's copy; the backend isn't told (no
  `session.editMessage` yet).
- **Text that describes the agent.** README sections and UI strings still describe what the agent does (find pictures,
  watch videos, three permission modes); whether that holds is up to the backend.
- **License.** `LICENSE` reserves the name, logo, animations and visual design; see the audit §13.1 before publishing builds.
