# Backend-removal audit: OpenGhost 1.3.0 as a frontend-only desktop client

- **Repository:** `kodachromez/OpenGhost` (fork of `ANDRETRIPOL/OpenGhost`), audited at `42f0dc8` (`package.json` version `1.3.0`)
- **Date:** 2026-10-03
- **Status:** This is an audit only. No code was removed, moved or changed, and runtime behavior is the same as before. The only file this commit adds is this document.
- **Out of scope:** the untracked `.qualification/`, `read-speeds.png` and `test/` in the working tree aren't part of the repository. They come up only where they matter (see §12, §13).

The goal is the same one used throughout this document:

```text
OpenGhost UI  →  clean backend interface  →  replaceable backend (Pi, Ghosty/Rust, Tau/Python, new TS/JS/Python)
```

The frontend must not depend on whichever backend is chosen.

**Historical proposal, not the current interface:** restart/adapter/mock architecture below describes a proposed
baseline, not shipped behavior. This branch has no automatic backend restart or reconnect command; relaunch the app
after a crash or configuration change. See [backend-interface.md](backend-interface.md) for the implemented contract.

---

## Contents

1. [Executive summary](#1-executive-summary)
2. [Architecture of OpenGhost 1.3.0 today](#2-architecture-of-openghost-130-today)
3. [File-by-file classification](#3-file-by-file-classification)
4. [Findings by topic](#4-findings-by-topic)
5. [What the frontend must keep locally](#5-what-the-frontend-must-keep-locally)
6. [Proposed boundary and transport](#6-proposed-boundary-and-transport)
7. [Proposed request/event schema (ABP v0)](#7-proposed-requestevent-schema-abp-v0)
8. [How each 1.3.0 UI surface is fed through the contract](#8-how-each-130-ui-surface-is-fed-through-the-contract)
9. [Backend adapter notes](#9-backend-adapter-notes)
10. [Exact files to delete, refactor, keep](#10-exact-files-to-delete-refactor-keep)
11. [Safest implementation order](#11-safest-implementation-order)
12. [Tests that prove the UI survives](#12-tests-that-prove-the-ui-survives)
13. [Risks and hidden backend coupling](#13-risks-and-hidden-backend-coupling)
14. [Recommended frontend-only baseline](#14-recommended-frontend-only-baseline)
- [Appendix A: IPC inventory](#appendix-a-ipc-inventory)
- [Appendix B: persisted data inventory](#appendix-b-persisted-data-inventory)

---

## 1. Executive summary

1. **The agent loop lives in the renderer, inside the chat view.** `chat.js` (1,746 lines) holds the DOM rendering for the thread *and* the whole agent: the system prompt (`FORMAT_GUIDE`, `VISUAL_CHECK`), the request/tool loop (`Chat.loop`, `chat.js:946`), tool dispatch and approvals (`useTool`/`approve`, `chat.js:1077`/`1124`), compaction (`compact`, `chat.js:1234`), title generation (`name`, `chat.js:1366`), token estimation, cache-mark placement (`seam`), app "notes" injected into the history (`notes`, `chat.js:1001`) and the mini-chat prompt (`SideChat`, `chat.js:1617`). Splitting `chat.js` is most of the work.
2. **Model calls are split across two processes.** DeepSeek is called straight from the page (`deepseek.js`, with CSP `connect-src https://api.deepseek.com`). OpenAI, the ChatGPT/Codex sign-in and Anthropic run in the main process (`desktop/llm.js`, `openai.js`, `chatgpt.js`, `anthropic.js`) and stream back over `llm:*` IPC. `providers.js` is the renderer facade that unifies both paths.
3. **Tools run in the main process. Approval policy runs in the renderer.** `desktop/tools.js` spawns PowerShell/zsh/bash and git, reads and writes files, fetches URLs, and dispatches `browser_*` to `desktop/browser.js`. `agent-tools.js` (988 lines) holds the tool schemas, web search and media scraping (Bing/DDG/YouTube HTML), result formatting, the approval *policy* (`needsApproval`) and the approval *presentation* (`describe`, with a shell-effect analyzer for Windows and POSIX).
4. **The renderer persists everything through a generic store.** That covers the chat index, transcripts, mini chats, usage and user context (`store:*` IPC → `userData/store/*.json`). Persisted transcripts are **model-facing histories in Chat Completions shape**, with provider-native blocks (Anthropic signed thinking, OpenAI encrypted reasoning), cache marks, injected app notes and image data URLs. The UI renders *from* that model format (`promptOf`, `chat.js:1410`).
5. **1.3.0 has no tool cards and no visible reasoning.** The UI shows a "ghost thinking" status while the agent works, approval cards, a compaction notice, stats cards and the browser panel's live cursor and drive state. Tool calls and thinking are deliberately hidden (agent prompt: "The user sees only your messages, never the tool calls"). The contract should still carry tool and reasoning events so tool cards can come later. To preserve 1.3.0 exactly, the frontend should ignore them at first.
6. **Browser automation can't leave the Electron process.** The `browser_*` tools drive `<webview>` guests through `webContents.debugger` (CDP) in the main process, coordinated with the renderer's `BrowserPanel` (tabs, take control / hand back, cursor animation). An out-of-process backend can't reach them, so the browser has to be exposed **as a host-provided tool through a reverse request** (backend → frontend). That one requirement shapes the protocol.
7. **Recommended boundary:**
   - The renderer talks only to a typed `window.openghost.backend` bridge (Electron IPC).
   - The main process is a thin **host**: lifecycle, relay, OS services, browser host tools. It has no agent logic.
   - The backend is a child process that speaks **JSON-RPC 2.0 framed as JSONL over stdio**. The same messages can later run over a Unix socket, named pipe or WebSocket for an attached or remote backend.
   - Backends with their own protocol (for example Pi's RPC mode) get a small adapter that translates to this contract.
8. **Licensing is the biggest non-technical risk.** `LICENSE` (tightened in `a93e65b`) keeps the name, logo, **animations** and **visual design** "all rights reserved". It forbids publishing, distributing or hosting a *modified version* that keeps any of them, and it allows a GitHub fork only "in order to propose changes to this repository". This plan keeps exactly those assets in a modified app. Get written permission from the author before anything is published or distributed, or plan a rebrand (own name, logo, animations and design). See §13.1.
9. **There are no automated tests.** `npm test` doesn't exist. The untracked `test/bench-reads.js` requires a `./harness` that isn't in the tree. Phase 0 of the plan is a characterization harness that records 1.3.0's behavior *before* anything moves.

---

## 2. Architecture of OpenGhost 1.3.0 today

### 2.1 Processes and layers

```text
┌──────────────────────────── Electron main process (desktop/) ───────────────────────────────┐
│ main.js      window, theme, titlebar, single-instance, store:* (JSON files), folder:*,     │
│              pdf:read, tool:*, browser:shown; registers llm.js + keys.js                   │
│ llm.js       llm:start/abort/models, auth:* ──► openai.js (Responses API, Codex backend)   │
│                                              ──► anthropic.js (@anthropic-ai/sdk)          │
│                                              ──► chatgpt.js (OAuth PKCE, :1455, limits)    │
│ keys.js      API keys (openai/anthropic/deepseek) in safeStorage-encrypted keys.bin        │
│ tools.js     run_<shell>, read/write/edit/list files, git, fetch_url, video_frames         │
│              ──► media.js (hidden window: image downscale, video frames)                   │
│              ──► pdf.js   (hidden window + Chromium PDF viewer → text)                     │
│              ──► browser.js (webview guests: CDP snapshot/click/type/screenshot…)          │
│ preload.js   contextBridge window.openghost { store, tools, llm, keys, auth, browser, … }  │
│ browser-preload.js  in each webview: tells the host which site got a password submitted     │
└───────────────▲──────────────────────────────────────────────────────────▲─────────────────┘
                │ ipcRenderer.invoke / send / sendSync                     │ llm:event, browser:event
┌───────────────┴──────────────────── Renderer (index.html, one page) ─────┴─────────────────┐
│ 69 classic <script> files sharing window globals, loaded in a fixed order (index.html).    │
│                                                                                            │
│ AGENT (backend in the page)          GLUE                        UI (keep)                 │
│  chat.js  loop/prompt/compact/…      settings.js  providers UI   markdown/tex/highlight/   │
│  agent-tools.js schemas/policy/…     usage.js     token ledger   diagram/stream-view/      │
│  agent-prompt.js system prompt       user-context.js instr+files approval-card/stats-card/ │
│  providers.js  call facade           library.js   chat index     chat-list/lock-ui/        │
│  deepseek.js   DeepSeek over fetch   chat-store.js store facade  model-stage/effort-*/     │
│                                      attachment-reader.js        mode-picker/dock/…        │
│                                      browser-panel.js            splash/theme/i18n/…       │
└──────────────────────────────────────────┬─────────────────────────────────────────────────┘
                                           │ fetch (renderer, CSP-allowed)
                                   https://api.deepseek.com
```

### 2.2 Renderer module wiring

- **Load order (`index.html`):** primitives and custom elements first, then `deepseek.js` and `providers.js`, then renderers (`tex`, `highlight`, `markdown`, `diagram`, `stream-view`), then attachments and settings, then `chat-store`, `usage`, `settings-usage`, `user-context`, `chat-lock`, `library`, `agent-tools`, `agent-prompt`, `approval-card`, `stats-card`, `chat`, and finally `script.js` (bootstrap) and `splash.js`.
- **Globals that cross modules:** `I18n`, `Glyphs`, `Markdown`, `StreamView`, `FileKinds`, `AttachmentReader`, `Attachments`, `Settings`, `Usage`, `UserContext`, `ChatStore`, `ChatLock`, `Library`, `AgentTools`, `AgentPrompt`, `Providers`, `DeepSeek`, `ApprovalCard`, `StatsCard`, `Chat`, `SideChat`, `MiniChat`, `BrowserPanel`/`window.browserPanel`.
- **Bootstrap (`script.js`):**
  - Constructs `Settings`, `GeneralSettings`, `AppearanceSettings`, `UsageSettings`, `Library`, `Chat`, `LockScreen`/`LockCard`, `FolderPill`, `ChatList`, `Attachments`, `SelectionMenu`, `AddMenu`, `ModelStage` and `EffortSlider`.
  - Shows the mode picker and browser toggle only when `AgentTools.available`, which means the desktop preload exists.

### 2.3 One turn today (sequence)

```text
script.send()
 └ Chat.send(text, attachments)                           chat.js:706
    ├ Library.create()/update()  (first message makes the chat record + "space" folder name)
    ├ userMessage() bubble → DOM
    └ run() → begin() turn {controller, queue, approvals} → drive() → loop()           :892/:934/:946
        loop:
         compactIfNeeded()  (estimate tokens vs settings.windowOf(model) × 0.9)         :1219
         request():                                                                    :1038
           system()  = AgentPrompt.build + FORMAT_GUIDE | env + UserContext.prompt + VISUAL_CHECK
           history() + withPictures(UserContext.pictures) + seam() cache mark + notes()
           Providers.stream(config, {messages, tools: AgentTools.schemas, onContent})
             ├ deepseek → DeepSeek.streamChat (renderer fetch, SSE)
             └ other    → window.openghost.llm.start → llm:event deltas → done/error
           Usage.record(); spend(entry); conv.tokens = usage.total_tokens
         for each tool call: useTool()                                                 :1077
           AgentTools.needsApproval(mode, cwd) → approve() → new ApprovalCard(describe()) :1124
           browser_* → BrowserPanel.run → tools.run IPC → desktop/browser.js (CDP)
           others    → AgentTools.run → tools.run IPC → desktop/tools.js
           result text (+ images → synthetic user "imageStep")
         save() → Library.saveMessages → store:write chats/<id>.json
         queued user messages (sent mid-turn) → takeQueue() → continue
     end(): notes (stopped/finish/empty), error box + Retry/Open settings, toolbar,  :1171
            name() title via Providers.complete, unread flag, seal if locked
```

### 2.4 Data

See Appendix B for the full list.

- **Files** live in `userData/store/` (`index.json`, `chats/<id>.json`, `mini/<id>.json`, `usage.json`, `context.json`, `context/<fileId>.json`), `userData/store/auth/{keys.bin,chatgpt.bin}` and `userData/theme.json`.
- **Chat folders:** each chat without a project folder gets one under `~/OpenGhost/Chats/<space>/`.
- **localStorage** holds model, effort, mode, the model catalog, approval-details state, the YouTube info cache, browser tabs and signed-in sites, and legacy API key names.

---

## 3. File-by-file classification

Legend: **K** = Frontend, keep · **R** = Backend/agent implementation, remove · **S** = Shared glue, refactor behind the backend interface · **U** = Unclear (reason given).

### 3.1 Desktop shell (`desktop/`)

| File | Lines | Class | What it is / why |
|---|---:|---|---|
| `desktop/main.js` | 231 | **S** | Window, theme, titlebar overlay, macOS menu, single-instance, Windows `.lnk` shortcut, devtools/reload keys and the generic JSON store are all **K**. The `tool:*` handlers, `LLM.register`, `Keys.register`, `Tools.CHATS`/`release`, and `Tools.cancelAll`/`LLM.cancelAll` on close and quit are backend wiring. Refactor: replace them with a `BackendHost` (spawn/relay/shutdown) and host services. |
| `desktop/preload.js` | 53 | **S** | `desktop`, `platform`, `pathOf`, `readPdf`, `pickFolder`/`revealFolder`/`chatsFolder`/`releaseFolder`, `setTitleBar`, `setTheme`, `store`, `browser` are **K**. `tools`, `llm`, `keys`, `auth` are backend-shaped. Replace them with one `backend` bridge (`request`, `notify`, `onMessage`, `respond`) plus a narrow `host` API. |
| `desktop/llm.js` | 74 | **R** | Provider run registry, `llm:*` and `auth:*` IPC. |
| `desktop/anthropic.js` | 230 | **R** | Anthropic Messages API through `@anthropic-ai/sdk`: model discovery, thinking/effort mapping, cache marks, server-side fallback beta, usage mapping. |
| `desktop/openai.js` | 209 | **R** | OpenAI Responses API and Codex backend (catalog, SSE, encrypted reasoning items, `prompt_cache_key`). |
| `desktop/chatgpt.js` | 218 | **R** | ChatGPT OAuth (PKCE, loopback port 1455, Codex client id), token refresh, plan limits (`wham/usage`). |
| `desktop/keys.js` | 65 | **R** (U) | API key vault (safeStorage). It's removed if the backend owns credentials (recommended). It's *unclear* only because a backend with no secure storage of its own might want the host to hold secrets. If so, keep it as a generic `host.secrets` service, not provider-keyed. |
| `desktop/tools.js` | 434 | **R** | The agent's shell/fs/git/fetch/video tools. **Keep these pieces out of it:** `CHATS` + `release()` (chat-folder lifecycle, see §5) and `environment()`, which `media-embed.js` uses indirectly. |
| `desktop/media.js` | 200 | **R** | Only `tools.js` uses it (image downscale for `read_file`, `video_frames`). Attachment previews are made in the renderer, not here. |
| `desktop/pdf.js`, `desktop/pdf.html` | 151 + 3 | **K** (S) | Used by `tools.js` (remove that use) **and** by attachments through `pdf:read` (keep). It's a host utility that extracts PDF text for attachments. |
| `desktop/browser.js` | 630 | **S** | `setup`/`guard`/`adopt` (session partition, permissions, downloads, popups, context menu, keys) are needed by the browser **panel** whatever the backend: **K**. `run`/`act`/`state`/`screenshot` plus the injected `install()` script are the agent's browser tools. Keep them too, but re-expose them as **host tools** behind the contract instead of through `tools.js`. |
| `desktop/browser-preload.js` | 19 | **K** | Detects password submits and tells the panel which site the user signed in to. |
| `desktop/icon.*`, `cursor.png` | — | **K** | Brand assets (see the license, §13.1). `cursor.png` is the agent cursor in the browser panel. |

### 3.2 Agent and provider code in the renderer

| File | Lines | Class | What it is / why |
|---|---:|---|---|
| `chat.js` | 1746 | **S** (largest) | See §3.6 for the line-level split. View code (rendering, scrolling, follow spring, bubbles, ghost status, notes and errors, copy, compaction notice DOM, stats posting, restore) is **K**. The loop, prompts, compaction, naming, token estimation, notes, cache seams, tool dispatch and approval policy are **R**. Turn/steer/approval orchestration is **S**. |
| `agent-tools.js` | 988 | **R** + **U** | Tool schemas, web search (Bing/DDG HTML scraping), `find_media`, `fetch_url` page reader, result formatting and `needsApproval` policy are **R**. `describe()` and the shell/git effect analyzers that feed the approval card are **U**: they're presentation, but also an app-side *safety net* ("worked out by the app, so a harmless-sounding sentence can't hide a deletion", `approval-card.js:5`). Recommendation: move `describe`/effect analysis into a frontend `approval-presenter.js` as a fallback when the backend sends no presentation (§7.6). |
| `agent-prompt.js` | 97 | **R** | Agent system prompt, environment block, permission-mode text. |
| `providers.js` | 107 | **R** | Provider facade, error→i18n mapping, `Usage.record`, `models-stale` event. Keep the error *mapping idea* in the frontend (§7.8). |
| `deepseek.js` | 162 | **R** | DeepSeek client (fetch from the page), model list, balance. |
| `user-context.js` | 162 | **S** | Settings → General: standing instructions plus pinned files. Storage, limits, add/remove and reading through `AttachmentReader` are **K**. `prompt()`/`block()`/`pictures()` (prompt formatting) are **R**. Send structured context to the backend instead (§7.3 `session.configure.userContext`). |
| `usage.js` | 149 | **S** | Local token ledger by day×provider×model: **K**. `parts()` normalizes provider-specific usage shapes (DeepSeek `prompt_cache_hit_tokens` and so on), which is **R**. Generalize the hard-coded `PROVIDERS` filter, which silently drops unknown providers. Feed it from `usage` events. |
| `settings.js` | 514 | **S** | Dialog pager and animations, key fields, eye toggle, account row and status lines are **K**. Hard-coded providers (`ORDER`, `KEYS`, `LINKS`, `FIRST_PROVIDER='deepseek'`), key storage and migration, `Providers.models`, `auth.*`, `configFor` (builds the provider request config) and `windowOf` default `1_000_000` are backend-shaped. Make them data-driven from `auth.providers` and `models.list`. |
| `settings-usage.js` | 451 | **S** | Charts and tables are **K**. Hard-coded `ORDER`/`TONES`/`PLANS`, ChatGPT limits through `auth.limits` and DeepSeek balance through `DeepSeek.balance` change to `account.limits` (generic windows and balances) and provider ids from the backend. |
| `library.js` | 365 | **S** | Chat index, folders, pins, titles, locking and the per-chat write queue are **K**. `cwdOf`/`space`/`home` (the agent's working folder for chats without a project) are workspace policy: keep them in the frontend and pass `cwd` to the backend. `conversation`/`saveMessages`/`side` store **model-facing** histories; they change to a display transcript cache plus backend session ids (§5, §13.2). |
| `chat-store.js` | 19 | **K** | Generic store facade (desktop IPC or localStorage). |
| `chat-lock.js` | 41 | **K** (U) | PBKDF2 + AES-GCM sealing of chats. It's a frontend feature, but it only protects what the frontend stores. **Unclear** because backend-owned sessions would sit unencrypted (§13.3). |
| `attachment-reader.js` | 294 | **K** (S) | Reads dropped files: image downscale, video poster and metadata, text, Office XML, PDF via `readPdf`, and `pathOf`. No backend calls. It produces payloads the backend consumes. The `<file …>` XML wrapping lives in `chat.js` (`fileBlock`/`videoBlock`) and is **R**. |
| `media-embed.js` | 253 | **S** | Renders image stacks and YouTube cards (**K**). It looks up video titles by calling the agent tool `fetch_url` (`media-embed.js:82-86`) with `AgentTools.environment().home` as cwd: hidden coupling. Replace that with a host `media.videoInfo` (oEmbed) service. |
| `browser-panel.js` | 573 | **S** | Panel UI, tabs, address bar, resize, take control / hand back, cursor animation, downloads toast and sign-in list are **K**. `run()`/`tabsTool()`/`context()`/`tabsLine()` are the agent integration. They become the **host tool executor** and the host context provider (§7.5). |
| `mini-chat.js` | 248 | **S** | Mini-chat dialog UI (**K**). It instantiates `SideChat` (agent logic in `chat.js`) and gates its mode picker on `AgentTools.available`. Change to capability `sessions.side`. |
| `model-stage.js` | 531 | **S** | Model picker UI (**K**). It depends on `settings.models`/`find`/`freshen`, on hard-coded `GROUPS` names and on `chat.switchModel`, which triggers compaction by the old model. Change to `models.list` provider names and `session.configure({model})`. |
| `effort-slider.js` | 391 | **S** (minor) | UI (**K**). Default `EFFORTS=['none','low','high','max']`; levels come from the model's `thinkingLevels`. |
| `mode-picker.js` | 87 | **S** (minor) | UI (**K**). Hard-coded `ask/auto/full`. Drive it from capability `permissions.modes`. |
| `add-menu.js` | 67 | **S** (minor) | UI (**K**). "Compact" and "Stats" items gate on `chat.canCompact`/`canStats`/`fill`. Change to capabilities and backend context fill. |
| `script.js` | 201 | **S** (minor) | Bootstrap. Gates on `AgentTools.available` and flushes `Usage`/`UserContext` on `pagehide`. |
| `index.html` | 213 | **S** (minor) | Script list (drop removed files) and CSP (drop `connect-src https://api.deepseek.com` → `'none'`). |
| `i18n.js` | 518 | **S** (minor) | English only. Provider-specific strings (`settings.chatgpt.*`, `settings.<provider>.*`, `error.signin`/`error.key` and so on) become generic or templated. `approve.*`, `compact.*`, `finish.*`, `mini.*` and `usage.*` are kept. |

### 3.3 Pure frontend (keep unchanged)

| File | Lines | Notes |
|---|---:|---|
| `markdown.js`, `tex.js`, `highlight.js` | 974, 268, 96 | Markdown, TeX and code highlighting. |
| `diagram.js` | 7996 | Mermaid-style renderer plus the custom kinds (metrics, bars, files, wireframe, nutrition…). It fires a `diagram-edit` event that `chat.js` handles (see §13.6). |
| `stream-view.js` | 306 | Streaming text reveal (pace, wave). Fed by `push(text)`/`finish()`. |
| `approval-card.js` | 157 | Renders a presentation object (`kind`, `title`, `effect`, `places`, `code`, `removed`/`added`, `quote`, `reveal`) and resolves `allow`/`deny`. Backend-agnostic already. |
| `stats-card.js` | 288 | Renders a stats snapshot `{models, turns, mini, context, uncounted}`. |
| `ghost-thinking.js`, `welcome-ghost.js`, `splash.js`, `splash-mist.js` | 133, 106, 325, 131 | Brand animations (see §13.1). |
| `chat-list.js`, `lock-ui.js`, `folder-pill.js`, `search-field.js`, `row-glide.js` | 595, 975, 129, 160, 61 | Sidebar, lock screens, folder pill, search. They talk to `Library`/`Chat` only. |
| `composer-text.js`, `attachments.js`, `media-slider.js`, `file-kinds.js`, `link-chip.js`, `smooth-height.js`, `scrollbar.js` | 589, 405, 355, 178, 128, 60, 126 | Composer, attachment tray, media stack, icons, link chips. |
| `dock.js`, `effort-button.js`, `effort-morph.js`, `effort-paint.js`, `effort-stage.js`, `model-button.js` | 794, 54, 151, 236, 233, 81 | Menus and effort and model controls. |
| `selection-focus.js`, `selection-menu.js` | 172, 173 | Text selection → quote / "ask in mini chat". |
| `settings-general.js`, `settings-appearance.js`, `settings-button.js` | 186, 98, 42 | Settings pages (General is fed by `UserContext`). |
| `theme.js`, `desktop.js`, `liquid-glass.js`, `glyphs.js`, `icon-button.js` | 117, 10, 124, 43, 77 | Theme, platform classes, effects, icons. |
| Custom-element buttons: `add-button`, `browser-toggle`, `clear-button`, `close-button`, `scroll-button`, `search-button`, `send-button`, `sidebar-toggle` | 25–40 each | Pure UI. |
| `styles.css` | 12239 | All UI styling, including approval, browser, stats, compaction, mini chat. Nothing in it is backend-specific. Keep every rule. |

### 3.4 Build, repository and documentation

| File | Class | Notes |
|---|---|---|
| `package.json` | **S** | Remove the `@anthropic-ai/sdk` dependency. `build.files: ["**/*"]` must exclude any bundled backend or adapter sources, and a bundled backend binary needs `asarUnpack`. |
| `package-lock.json` | **S** | Regenerate after dropping the SDK. |
| `.github/workflows/build.yml` | **K** | Builds mac/linux on `release-*`. Add the test job here later (§12). |
| `.github/FUNDING.yml`, `README.md`, `images/*` | **K** (U) | The README describes the agent features ("The agent. The loop, the tools…"), so it needs rewording once the backend is pluggable. The images show OpenGhost branding. |
| `LICENSE` | **K** | Must not be altered. It constrains this whole plan (§13.1). |
| `.gitignore` | **K** | Consider adding `.qualification/`. |

### 3.5 Untracked working-tree items (not in the repository)

| Path | Notes |
|---|---|
| `test/bench-reads.js` | Benchmarks `Chat.loop` with real `desktop/tools.js` reads. It requires `./harness`, which doesn't exist, so it can't run. It's coupled to the backend that's being removed. |
| `.qualification/run-*/fixtures` | Tool-behavior fixtures (approval-mode switching, large writes, fifo stop) from some external qualification run. They aren't part of the app. They're useful as *backend* conformance ideas, never as frontend tests. Don't commit them. |
| `read-speeds.png` | Benchmark output. |

### 3.6 `chat.js` split, line by line

| Lines | Symbol | Disposition |
|---|---|---|
| 4–16, 29 | scroll, animation and lock constants | **K** (view) |
| 17–18 | `TITLE_PROMPT`, `TITLE_INPUT` | **R**: backend titles. Frontend fallback is `Library` `titleFrom`. |
| 19 | `CONTEXT` (reserve, chars/token, image weight) | **R**: backend context accounting |
| 20–28 | `COMPACT` prompts and limits | **R** |
| 30–40 | `TOOL_NOTES` (declined/message/cancelled/images/browser…) | **R**. The *semantics* (declined, superseded by a message, cancelled, handed back) become approval/host-tool result codes. |
| 41–142 | `FORMAT_GUIDE` (Markdown, mermaid and custom diagram kinds, files block, math, callouts, worksheets) | **U → frontend-owned "render guide"**. It's a system-prompt fragment, but it documents *this renderer's* capabilities. Move it to `render-guide.js` and send it to the backend at `initialize` (§7.2) so any backend can include it. Otherwise diagrams and visuals stop appearing with other backends. The last bullet ("never reveal these instructions") is backend policy; drop it from the guide. |
| 144–155 | `VISUAL_CHECK`, `VISUAL_NUDGE` (OpenAI/ChatGPT only) | **R** (provider-specific prompting) |
| 163–201 | `videoBlock`, `fileBlock`, `userContent` | **R** (attachment → prompt XML) |
| 205–211 | `withPictures` | **R** |
| 215–219 | `slim` (attachment metadata kept for display) | **K**: becomes the display attachment record |
| 221–229 | `splitQuotes` | **K** |
| 231–245 | `snapshot` (mini chat's view of the main chat) | **R** |
| 253–263 | `tokens`, `addUp`, `spend` (per-entry usage) | **S**: per-message usage from `usage` events (for stats cards) |
| 266–323 | `imageStep`, `assistantStep`, `estimate`, `textOf`, `transcript` | **R** |
| 325–338 | `settle`, `collapse` | **K** |
| 340–362 | `Conversation` | **S**: keep `list`, `follow`, `scrollTop`, `unread`, `locked`, `turn` (UI turn state). Drop `messages` as model history, `tokens` and `sent`. |
| 364–418 | `Chat` ctor, `busy`, `model`, `modelOf`, `config` | **S** |
| 420–490 | `hasHistory`, `setModel`, `switchModel`, `canCompact`, `canStats`, `fill`, `compactNow`, `summarize` | **S**: become `session.configure` / `session.compact` plus capability checks. The model-switch compaction is backend behavior that the UI shows with `compaction.*` events. |
| 492–686 | drafts, open/load, lock/unlock/protect/unprotect, remove, attach, activate | **K** (load becomes `session.get` plus display cache) |
| 690–849, 851–876 | pin, scroll follow spring, jump, copy | **K** |
| 706–735 | `send` | **S**: create session (first message) and `turn.start`, or `turn.steer` when busy |
| 737–748 | `stop`, `abort` | **S**: `turn.cancel`. Pending approval cards settle locally. |
| 750–757 | `onModeChange` (auto-allows pending approvals) | **S**: `session.configure({permissionMode})`. The backend re-evaluates and sends `approval.resolved`. |
| 797–803 | `onDiagramEdit` (rewrites stored assistant text) | **U**: needs `session.editMessage` or a frontend overlay (§13.6) |
| 878–885 | `cwd`, `agent` (tools enabled iff absolute cwd and desktop) | **S** → capability `tools` plus host cwd |
| 887–944 | `begin`, `run`, `compose`, `attachedVideos`, `resume`, `interject`, `drive` | **S**: turn lifecycle stays in the UI. `compose` (prompt XML), `attachedVideos` (approval policy) and the drive loop go to the backend. |
| 946–982 | `loop` | **R** |
| 988–1075 | `system`, `notes`, `seam`, `history`, `request` | **R**. The `onContent` → `StreamView.push` wiring is **K** (driven by `message.delta`). |
| 1077–1135 | `useTool`, `approve` | **R** (dispatch/policy). Approval card mounting is **K**. Browser take-control wait moves into the host-tool executor (**S**). |
| 1137–1169 | `takeQueue`, `openPart`, `closePart` | **S**: "parts" map to assistant message ids from events, and queued bubbles are placed on `input.accepted` |
| 1171–1217 | `end` | **K** (UI wrap-up) with **R** bits: `name()`, `save()` of model history, `turn.switch` model write |
| 1219–1294 | `compactIfNeeded`, `switchLabels`, `compact`, `compactNotice`, `finishNotice` | **R** (compaction). The notice DOM and labels are **K**, driven by `compaction.started`/`completed`. |
| 1296–1364 | `save`, `stats`, `postStats`, `onStatsRemove` | **S**: stats are built from per-message usage in the display transcript. Stats cards are frontend-only annotations. |
| 1366–1382 | `name` | **R** (`session.updated{title}`) |
| 1384–1583 | `restore`, `entryView`, `promptOf`, `restoredMessage`, toolbar, ghost, `fail`, `retry`, `note`, `action`, `userMessage`, `attachmentViews`, `fileCard`, `assistantMessage` | **K**. `promptOf` must stop reading image URLs out of model content (§13.2). `retry` becomes `turn.retry`. |
| 1586–1742 | `SIDE`, `movedNotice`, `SideChat` | **S**: dialog behavior and the "moved" notice are **K**. History and prompt construction are **R**. Change to capability `sessions.side`. |

---

## 4. Findings by topic

| Topic | Where today | Class | Disposition |
|---|---|---|---|
| **Agent loop** | `chat.js` `drive`/`loop`/`request`/`useTool`/`takeQueue`/`end` | R | Backend. The UI keeps only turn state for rendering. |
| **Model/provider code** | `desktop/llm.js`, `openai.js`, `anthropic.js`, `chatgpt.js`, `deepseek.js`, `providers.js`, `settings.configFor` | R | Backend. The UI gets `models.list` and `auth.providers`. |
| **Anthropic/API-specific code** | `desktop/anthropic.js` (SDK, `cache_control`, `server-side-fallback-2026-07-01` beta, adaptive/budget thinking, `refuses` learning), `chat.js` `seam()` cache marks, `providers.complete` min 2048 tokens for Anthropic, `settings.js` `LINKS.anthropic`, the `@anthropic-ai/sdk` dependency | R | Delete. The SDK dependency goes with it. |
| **System prompts** | `agent-prompt.js` (agent and environment and state), `chat.js` `FORMAT_GUIDE`, `VISUAL_CHECK`, `COMPACT.*`, `TITLE_PROMPT`, `SIDE.*`, `TOOL_NOTES.*`, `user-context.js` `PROMPT.*` | R / U | Backend, **except** `FORMAT_GUIDE`, which becomes a frontend-published render guide (§7.2). |
| **Tools (definitions)** | `agent-tools.js` `SCHEMAS` (shell per OS, read/write/edit/list, video_frames, git, web_search, fetch_url, find_media, browser_*) | R | Backend. `browser_*` schemas become **host tools** published by the frontend. |
| **Tool execution** | `desktop/tools.js`, `desktop/media.js`, `desktop/pdf.js` (`read_file` PDFs), `agent-tools.js` search/media scraping and formatting | R | Backend. Browser execution stays in the host (§7.5). |
| **Permission/approval logic** | Policy: `agent-tools.js` `needsApproval`, `riskyShell`/`riskyGit`/`RISKY_CLICK`/`inside`, and `chat.js` `onModeChange`, `attachedVideos`. Presentation: `agent-tools.js` `describe` + effect analyzers, `approval-card.js` | R / U / K | Policy goes to the backend. Presentation stays in the frontend as a fallback presenter. The card is kept as is. |
| **Filesystem/process/browser execution** | `desktop/tools.js` (spawn, taskkill, PowerShell detection, encodings, CRLF/BOM handling), `desktop/browser.js` (CDP) | R / S | Process and fs go to the backend. Browser stays a host service. |
| **Session persistence** | `library.js` (`index`, `chats/<id>`, `mini/<id>`), `chat-store.js`, `main.js` store IPC | S | The backend owns model-facing sessions. The frontend owns the index and organization plus a display-transcript cache (§5.4). |
| **Context/compaction** | `chat.js` `CONTEXT`, `estimate`, `compactIfNeeded`, `compact`, `transcript`, `switchModel`; `settings.windowOf`; `SideChat.compactIfNeeded` (5% share) | R | Backend. The UI shows `compaction.*` events and the context fill from `usage.context`. |
| **Usage/token accounting** | `providers.counted` → `usage.js` ledger; `chat.js` `spend`/`stats`; `settings-usage.js`; `chatgpt.limits`; `deepseek.balance` | S | Backend emits normalized `usage` events. The frontend keeps the ledger, stats and usage page. Limits and balances come through `account.limits`. |
| **Model/provider auth** | `desktop/keys.js`, `desktop/chatgpt.js`, `settings.js` (key fields, migration from localStorage, check-by-listing-models), `auth:*` IPC | S | The backend owns credentials. The frontend renders `auth.providers` (API-key rows, OAuth rows) and calls `auth.setKey`/`login`/`cancel`/`logout`. |
| **Attachments** | Reading: `attachment-reader.js`, `attachments.js`, `pdf:read`, `pathOf` (K). Prompting: `chat.js` `userContent`/`fileBlock`/`videoBlock`/`withPictures` (R). Display: `slim`, `promptOf`, `attachmentViews`, `fileCard`, `MediaSlider` (K). | S | The frontend sends a structured `Attachment[]`. The backend decides how they reach the model. |
| **Tool cards / streaming UI** | No tool cards exist. Streaming is `StreamView` (K), ghost status `showGhost`/`dismissGhost` (K), approval cards (K), browser cursor and drive state (K). | K | Drive them from `message.delta`, `tool.started`/`completed` (ghost on/off) and `approval.request`. Ignore tool and reasoning events at first. |
| **Browser UI vs browser backend** | UI: `browser-panel.js` panel parts, `browser-toggle.js`, the `guard`/`adopt`/`setup` part of `desktop/browser.js`, `browser-preload.js`. Agent: `desktop/browser.js` `run`/`act`, `browser-panel.js` `run`/`tabsTool`/`context`, `agent-tools.js` browser schemas, `RISKY_CLICK`, `refs`. | K / S | The browser is a **host tool provider**. See §7.5. |
| **Frontend vs backend settings** | Frontend: theme, sidebar, approval details state, effort/mode/model *selection*, browser tabs and accounts, media cache, General instructions and files (stored). Backend: keys and sign-ins, model catalog source, permission-mode semantics, context windows, compaction thresholds. | S | §5.6 |
| **Desktop/Electron IPC boundary** | 25 channels (Appendix A), `fromApp` sender check, `contextIsolation` + `sandbox` | S | It shrinks to a backend bridge plus host services. Keep `fromApp` checks on every channel. |
| **Platform-specific code** | §4.1 below | mixed | Keep the window, theme, menu and path-case code. The shell and process code goes with the backend. |
| **Other coupling** | `media-embed.js` → `fetch_url`; `models-stale` event; `diagram-edit`; stats and moved entries inside histories; mini-chat cache seam; CSP; `keys:read` sendSync at startup; title generation | — | §13 |

### 4.1 Platform-specific code

| Location | Platform behavior | Fate |
|---|---|---|
| `desktop/main.js` | Windows `.ico`, others `.png`. Windows and macOS: hidden titlebar plus `titleBarOverlay`. Linux: native frame. macOS: app/edit/window menu (Cmd shortcuts). Windows: `--create-shortcut` `.lnk`. `setAppUserModelId`. | **K** |
| `desktop.js`, `styles.css` | `is-desktop`, `is-mac`, `is-linux` classes | **K** |
| `library.js` | `pathKey` treats paths as case-insensitive except on Linux. Windows reserved names (`con`, `prn`…) in chat folder names. | **K**, but it must use the **backend host's** platform when the backend runs elsewhere (§13.10) |
| `desktop/tools.js` | PowerShell 7 detection (avoids the Store alias), Windows PowerShell 5.1 fallback, `taskkill /T /F` vs process-group `SIGKILL`, `windowsHide`/`detached`, UTF-8 prelude and epilogue, BOM/CRLF preservation, `windows-1251` fallback decoding | **R** |
| `agent-tools.js`, `agent-prompt.js` | Tool name `run_powershell`/`run_zsh`/`run_bash` and OS-specific prompt text, Windows vs POSIX command parsers for approvals | **R**, except the presenter, which needs the backend's OS (§7.2 `backend.platform`) |
| `desktop/media.js` | ffmpeg install hint by OS | **R** |
| `desktop/keys.js` | Windows rename retry (`EPERM`/`EBUSY` after antivirus), `safeStorage` (Keychain, DPAPI, libsecret) | **R** (or a generic host secrets service) |
| `desktop/browser.js` | UA string per OS (fixed Windows UA in the browser partition) | **K** |
| `.github/workflows/build.yml`, `package.json` `build` | NSIS (Windows), DMG universal (macOS), tar.gz (Linux) | **K**. Add per-OS backend binaries if one is bundled. |

---

## 5. What the frontend must keep locally

Some of what the current backend does is genuinely client work and must survive the removal:

1. **Workspace organization.** The chat index, folders, pins, collapsed state, manual renames (`renamed`), unread and busy flags, search, and the "chat without a project gets its own folder" policy (`~/OpenGhost/Chats/<space>`, created lazily and released when empty: `tools.js` `CHATS`/`inChats`/`release`, `library.js` `space`/`cwdOf`). That policy moves into a small host `workspace` service. The backend receives a ready `cwd`.
2. **Chat locking** (`chat-lock.js`, `lock-ui.js`, `library.protect`/`unlock`/`seal`). It's frontend-only crypto, so it only covers what the frontend stores. See §13.3.
3. **Attachment intake.** File kinds and icons, image downscaling, video posters and metadata, text and Office extraction, PDF text through the Electron viewer (`desktop/pdf.js`), and `pathOf` for local paths. The backend gets structured attachments, with bytes when it can't read local paths.
4. **Display transcript cache.** Per session: user text plus *display* attachment records (with preview URLs), assistant final text per part, per-message model and usage, compaction markers, plus **frontend-only annotations** (stats cards, mini-chat "moved" markers, diagram edits). This keeps instant reopen, offline rendering, locking and stats possible even when the backend's history is opaque or lives somewhere else.
5. **Usage ledger.** `usage.js`, fed by `usage` events, so Settings → Usage keeps working for any backend that reports tokens.
6. **UI preferences:**
   - Theme
   - Selected model, effort and mode, validated against backend lists
   - Model catalog cache
   - Approval details open state
   - YouTube info cache
   - Browser tabs and signed-in sites
   - General instructions and files, kept in the frontend and sent to the backend
7. **Browser panel and browser host tools.** Webview hosting and hardening (`guard`/`adopt`/`setup`), downloads, sign-in detection, the agent cursor and ripple, take control / hand back, the per-tab action queue, and the browser context summary.
8. **Render guide.** `FORMAT_GUIDE` describes what *this* renderer can draw, so the frontend owns it and hands it to the backend.
9. **Approval presentation fallback.** The effect and places analysis as a safety net when a backend sends no presentation, or a presentation the frontend's own analysis disagrees with (§7.6).
10. **YouTube oEmbed lookup** for video cards (today it goes through `fetch_url`). It becomes a host `media.videoInfo` service restricted to `youtube.com/oembed`.
11. **Error → i18n mapping** (from `providers.explain`), keyed on the contract's error codes.
12. **Turn UI mechanics:**
    - Escape to stop
    - Bubbles for messages sent mid-turn, placed before the next assistant part
    - Retry and Open-settings actions
    - Finish notes (`length`, `content_filter`, `insufficient_system_resource`), stopped and empty notes
    - Effort slider and model picker locked while busy
13. **Shell services.** Window, titlebar color, theme, single instance, macOS menu, devtools and reload keys, Windows shortcut, flushing the store before quit.

---

## 6. Proposed boundary and transport

### 6.1 Options compared

| Transport | Pros | Cons | Verdict |
|---|---|---|---|
| **Electron IPC only** (backend inside main) | Already in place, no framing | Backend must be Node inside Electron; ties the backend's language and lifecycle to the app; can't host Rust or Python directly | Use it **only for the inner hop** (renderer ↔ main). |
| **JSON-RPC 2.0 as JSONL over stdio** (child process) | Language-neutral (Node, Rust, Python), no ports or auth, child lifetime tied to the app, trivial framing, natural for streaming notifications, Pi already has a JSONL-over-stdio RPC mode, easy to record and replay for tests | One process per app (fine); stdout must be protocol-only, so logs go to stderr | **Recommended primary.** |
| **Local socket / named pipe / WebSocket** | Attach to a long-running daemon or a remote machine; several frontends | Needs a token, port or path management and reconnection. If opened from the renderer it widens the CSP. | **Secondary**, same messages. Implement in main, never in the renderer. |
| **HTTP + SSE** | Remote and firewall-friendly | Request/response mismatch for reverse requests (approvals, host tools); more machinery | Only via an adapter if a backend requires it. |
| **In-process JS module** | Fastest | Couples language and crash domain; the "replaceable" goal fails | No. |

### 6.2 Recommendation

```text
Renderer (UI only)
  │  window.openghost.backend.{request, notify, respond, onMessage}   ← one generic bridge, no provider names
  │  window.openghost.host.{workspace, pdf, media, browser, store, theme, …}
  ▼
Main process = Host (no agent logic)
  ├─ BackendHost: spawn(command, args, env, cwd) | connect(socket/ws) → JSON-RPC peer
  │     relays client↔backend messages, enforces size limits, restarts, kills on quit
  ├─ Host services: store, workspace folders, pdf text, YouTube oEmbed, theme/titlebar, browser host tools (CDP)
  ▼
Backend process (any language), or an adapter in front of one (e.g. Pi RPC → ABP)
```

Principles:

- **One protocol, many transports.** ABP messages are transport-agnostic JSON-RPC 2.0. Framing is one JSON object per line (`\n`, UTF-8, no embedded newlines). Stdio is the default.
- **Model/provider API traffic belongs to the external backend.** The app renderer's CSP becomes `connect-src 'none'`, blocking fetch/XHR/WebSocket-style connections, not HTTP(S) image loads or the separate browser webviews' networking. This is not a provider-hostname deny policy; see `docs/frontend-only.md` for the implemented boundary.
- **The main process stays dumb.** It relays opaque JSON plus host services. Validation, versioning and capability negotiation happen in a renderer-side `backend-client.js` module, which keeps the UI testable with a fake transport.
- **Reverse requests are first-class.** Approvals and host tools are JSON-RPC *requests from the backend*, so the backend's loop simply awaits them.
- **Backend config** lives in a settings entry (`backend.command`, `args`, `env`, `transport`), with a built-in **mock backend** for the frontend-only baseline and for tests.

---

## 7. Proposed request/event schema (ABP v0)

"ABP" (Agent Backend Protocol) is a working name. All payloads are JSON. The types below are documentation (TypeScript notation), not code to add yet.

### 7.1 Envelope

```ts
// JSON-RPC 2.0. Requests have id; notifications don't. Both sides may send requests.
type Request      = { jsonrpc: '2.0', id: number | string, method: string, params?: object }
type Response     = { jsonrpc: '2.0', id: number | string, result?: any, error?: RpcError }
type Notification = { jsonrpc: '2.0', method: string, params?: object }
type RpcError     = { code: number, message: string, data?: AbpError }
// Cancellation of any in-flight request (either direction):
//   notification "$/cancelRequest" { id }
```

### 7.2 Handshake and capabilities

```ts
// client → backend
initialize(params: {
  protocolVersion: '0.1',
  client: { name: string, version: string, platform: 'win32'|'darwin'|'linux', locale: string },
  host: {
    tools: ToolSchema[],              // host-provided tools (browser_*); backend may expose them to its model
    renderGuide?: string,             // frontend's FORMAT_GUIDE, for the backend to put in its system prompt
    attachments: { localPaths: boolean },   // false when backend is remote
  }
}): {
  protocolVersion: '0.1',
  backend: { name: string, version: string, platform: 'win32'|'darwin'|'linux'|'other', remote?: boolean },
  capabilities: Capabilities
}

type Capabilities = {
  turns:       { steer: boolean, followUp: boolean, cancel: boolean, retry: boolean },
  thinking:    { visible: boolean },                      // emits reasoning.delta
  tools:       { events: boolean, hostTools: boolean },   // emits tool.*; will call host tools
  approvals:   { modes: string[] } | null,                // e.g. ['ask','auto','full']; null = no approvals
  sessions:    { list: boolean, delete: boolean, rename: boolean, side: boolean, edit: boolean, encrypted: boolean },
  compaction:  { manual: boolean, auto: boolean, onModelSwitch: boolean },
  usage:       { tokens: boolean, cost: boolean, context: boolean, limits: boolean },
  auth:        { providers: boolean },
  attachments: { image: boolean, text: boolean, pdf: boolean, video: boolean, paths: boolean, maxBytes: number },
  titles:      boolean,                                    // backend names sessions
  userContext: boolean                                     // accepts instructions/files
}
shutdown(): null          // backend flushes; host kills after a grace period
```

Each capability turns UI on or off (§8). The frontend never assumes a feature it wasn't told about.

### 7.3 Client → backend requests

```ts
// Auth and providers
auth.providers(): Provider[]
auth.setKey({ provider, key: string|null }): ProviderStatus
auth.login({ provider }): ProviderStatus                   // may take minutes; cancellable
auth.cancel({ provider }): ProviderStatus
auth.logout({ provider }): ProviderStatus
account.limits({ provider }): AccountLimits | null

// Models
models.list({ provider?: string, refresh?: boolean }): Model[]

// Sessions
session.create({ cwd, model, thinking, permissionMode, title?, userContext? }): { sessionId }
session.get({ sessionId }): { items: TranscriptItem[], model, thinking, permissionMode, context?: ContextFill }
session.list(): SessionInfo[]                              // if capabilities.sessions.list
session.delete({ sessionId }) / session.rename({ sessionId, title })
session.configure({ sessionId, model?, thinking?, permissionMode?, userContext? }): { model, thinking, permissionMode }
session.compact({ sessionId }): { ok: boolean }
session.side({ sessionId }): { sessionId }                 // mini chat: side session that reads the parent live
session.editMessage({ sessionId, messageId, text })        // if capabilities.sessions.edit (diagram edits)

// Turns
turn.start({ sessionId, clientTurnId, input: Input }): { turnId }
turn.steer({ sessionId, turnId, clientInputId, input: Input }): { accepted: boolean }   // between steps
turn.followUp({ sessionId, clientInputId, input: Input })                               // after the turn
turn.cancel({ sessionId, turnId })
turn.retry({ sessionId }): { turnId }                      // re-run from history, no new input

// Host context (replaces the browser "notes" chat.js injects)
context.update({ sessionId?: string, host: { browser?: { open: boolean, tabs: Tab[], signedIn: SignedIn[] } } })

type Input = { text: string, attachments: Attachment[], quotes?: string[] }
type Attachment = {
  id: string, name: string, mime: string, size: number,
  kind: 'image'|'text'|'pdf'|'video'|'office'|'file',
  path?: string,                    // only if capabilities.attachments.paths && host.attachments.localPaths
  dataUrl?: string,                 // images (downscaled like attachment-reader does today)
  text?: string, truncated?: boolean,         // extracted text (text, office, pdf)
  video?: { duration: number, width: number, height: number, audio?: boolean },
  note?: string                     // the user's note on this attachment
}
type UserContext = { instructions: string, files: Attachment[] }
```

### 7.4 Backend → client notifications (events)

Every event carries `{ sessionId, seq }`, and turn-scoped events also carry `turnId`. `seq` is monotonic per session, so the client can detect gaps and re-sync with `session.get`.

```ts
turn.started      { turnId, clientTurnId?, model }
input.accepted    { turnId, clientInputId, messageId }     // a steer/follow-up became part of history (place the bubble)
message.started   { turnId, messageId, role: 'assistant', model }
message.delta     { messageId, text }                      // append-only text
reasoning.delta   { messageId, text }                      // only if capabilities.thinking.visible
message.completed { messageId, text, finishReason?: FinishReason }
tool.started      { turnId, toolCallId, name, title?, args?, host?: boolean }
tool.progress     { toolCallId, text?: string, percent?: number }
tool.completed    { toolCallId, status: 'ok'|'error'|'denied'|'cancelled', summary?: string }
approval.resolved { approvalId, decision: 'allow'|'deny'|'superseded'|'cancelled' }   // backend-side resolution
compaction.started   { turnId?, reason: 'auto'|'manual'|'model-switch', from?: string, to?: string }
compaction.completed { ok: boolean }
usage             { turnId?, messageId?, provider, model, modelName?, input, cached, written, output,
                    requests: number, cost?: { amount: number, currency: string },
                    context?: ContextFill }
session.updated   { title?, model?, thinking?, permissionMode? }
turn.completed    { turnId, status: 'done'|'cancelled'|'error', finishReason?: FinishReason, error?: AbpError }
models.changed    { provider }                             // replaces the 'models-stale' window event
auth.changed      { provider, status: ProviderStatus }
log               { level: 'debug'|'info'|'warn'|'error', message }   // goes to devtools, never to the chat

type FinishReason = 'stop'|'length'|'content_filter'|'tool_calls'|'insufficient_system_resource'|'other'
type ContextFill  = { used: number, window: number }
```

### 7.5 Backend → client requests (reverse)

```ts
// Approval: the backend's loop awaits the answer.
approval.request({
  sessionId, turnId, approvalId, toolCallId,
  tool: string, args: object,
  presentation?: {                     // what ApprovalCard renders today
    kind: 'command'|'file'|'web', title: string,
    effect?: 'read'|'change'|'delete'|'install'|'system'|'online'|'record'|'run',
    badge?: boolean, places?: { kind: 'file'|'folder'|'site', label: string, title: string }[],
    code?: string, removed?: string, added?: string, quote?: string,
    reveal?: 'command'|'content'|'changes'
  }
}): { decision: 'allow'|'deny' }
// If the user sends a message instead, the client answers { decision: 'deny', reason: 'superseded' } and
// then sends turn.steer. (Today: TOOL_NOTES.message.)

// Host tool call: browser_* (and any future host tool).
host.tool({ sessionId, turnId, toolCallId, name: string, args: object }):
  { content: ({ type: 'text', text: string } | { type: 'image', dataUrl: string, label?: string })[],
    isError?: boolean, status?: 'ok'|'cancelled'|'handed-back' }
// Cancellation arrives as $/cancelRequest. The executor handles take control / hand back internally
// (today chat.js useTool lines 1092–1106) and reports 'handed-back' with a fresh snapshot.
```

### 7.6 Approval presentation rule

- If `presentation` is present, render it.
- If it's absent, the frontend's `ApprovalPresenter` builds one from `tool` and `args` for known shapes (shell code, file write and edit, git, URL, browser actions). Otherwise it uses a generic card (`title = tool`, `code = JSON args`), which is what `describe()`'s `default` branch does today.
- **Safety net:** for shell-like tools the presenter also runs its own effect analysis, using `backend.platform` to choose the Windows or POSIX parser. When its `effect` is more severe than the backend's, the card shows the more severe one.

### 7.7 Provider, model and session types

```ts
type Provider = {
  id: string, name: string, group?: string,           // group = model-picker heading (e.g. "OpenAI API")
  methods: ({ type: 'apiKey', label: string, hint?: string, url?: string, placeholder?: string }
          | { type: 'oauth', label: string, hint?: string })[],
  status: ProviderStatus, color?: string               // usage-chart tone hint
}
type ProviderStatus = { connected: boolean, checking?: boolean, waiting?: boolean,
                        account?: { email?: string, plan?: string }, error?: AbpError, keySaved?: boolean }
type Model = {
  id: string, provider: string, name: string,
  contextWindow?: number, maxOutput?: number, vision: boolean,
  thinkingLevels: string[],           // e.g. ['none','low','medium','high','xhigh','max']
  defaultThinking?: string
}
type AccountLimits = {
  plan?: string, reached?: boolean,
  windows: { seconds: number, used: number /*0–100*/, resets: number /*epoch ms*/ }[],
  models?: { name: string, windows: AccountLimits['windows'] }[],
  balances?: { currency: string, total: number }[], credits?: { unlimited?: boolean, balance?: string }
}
type SessionInfo = { sessionId, title, cwd, created, updated, model }
type TranscriptItem =
  | { type: 'user', id, text, attachments: { id, name, mime, size, kind, previewUrl?, video?, note? }[], at }
  | { type: 'assistant', id, turnId, model, text, finishReason?, usage?: Usage, at }
  | { type: 'tool', id, turnId, name, title?, status }        // ignored by the 1.3.0 view
  | { type: 'compaction', id, ok: boolean, reason?: string, at }
```

### 7.8 Errors

```ts
type AbpError = {
  code: 'auth'|'quota'|'rate_limit'|'network'|'server'|'context_overflow'|'invalid_request'
      | 'model_unavailable'|'cancelled'|'backend_crashed'|'unsupported'|'unknown',
  message: string,            // human-readable, already in English
  provider?: string, status?: number, retryable?: boolean,
  action?: 'open-settings'|'retry'|'none'
}
```

The frontend maps `code` to the existing i18n keys:

| Code | i18n key |
|---|---|
| `auth` | `error.key` / `error.signin` |
| `quota` | `error.quota` |
| `rate_limit` | `error.rate` |
| `server` | `error.server` |
| `network` | `error.connect` |
| `context_overflow` | new key |
| anything else | `message` |

`action: 'open-settings'` reproduces the 401 "Open settings" button. `backend_crashed` is raised by the host when the child exits mid-turn.

### 7.9 Example stream (stdio, abbreviated)

```jsonl
{"jsonrpc":"2.0","id":7,"method":"turn.start","params":{"sessionId":"s1","clientTurnId":"c9","input":{"text":"Clean up Downloads","attachments":[]}}}
{"jsonrpc":"2.0","id":7,"result":{"turnId":"t3"}}
{"jsonrpc":"2.0","method":"turn.started","params":{"sessionId":"s1","seq":41,"turnId":"t3","clientTurnId":"c9","model":"m-a"}}
{"jsonrpc":"2.0","method":"message.started","params":{"sessionId":"s1","seq":42,"turnId":"t3","messageId":"a1","role":"assistant","model":"m-a"}}
{"jsonrpc":"2.0","method":"message.delta","params":{"sessionId":"s1","seq":43,"messageId":"a1","text":"I will see what is in Downloads."}}
{"jsonrpc":"2.0","method":"tool.started","params":{"sessionId":"s1","seq":44,"turnId":"t3","toolCallId":"k1","name":"shell","title":"List Downloads"}}
{"jsonrpc":"2.0","id":"b5","method":"approval.request","params":{"sessionId":"s1","turnId":"t3","approvalId":"p1","toolCallId":"k2","tool":"shell","args":{"command":"rm -r ~/Downloads/old"},"presentation":{"kind":"command","title":"Delete the old folder","effect":"delete","badge":true,"code":"rm -r ~/Downloads/old","reveal":"command"}}}
{"jsonrpc":"2.0","id":"b5","result":{"decision":"allow"}}
{"jsonrpc":"2.0","method":"usage","params":{"sessionId":"s1","seq":51,"turnId":"t3","messageId":"a1","provider":"p","model":"m-a","input":5120,"cached":4096,"written":0,"output":210,"requests":1,"context":{"used":5330,"window":200000}}}
{"jsonrpc":"2.0","method":"turn.completed","params":{"sessionId":"s1","seq":60,"turnId":"t3","status":"done","finishReason":"stop"}}
```

---

## 8. How each 1.3.0 UI surface is fed through the contract

| UI surface | Today | Through ABP | Capability gate |
|---|---|---|---|
| Streaming assistant text (`StreamView`) | `onContent` → `view.stream.push(entry.content)` | `message.delta` accumulates; `push(fullText)` | — |
| Multi-part replies (parts between tool steps) | `openPart`/`closePart` per request | One view per `message.started`; empty parts collapse on `message.completed` | — |
| Ghost "thinking" status | `showGhost` before tools, `dismissGhost` on text/approval | Shown on `turn.started` and `tool.started`, dismissed on first `message.delta` or `approval.request` | — |
| Reasoning | not shown | `reasoning.delta` is ignored at first (optional future disclosure) | `thinking.visible` |
| Tool cards | not shown | `tool.*` is ignored at first (future cards) | `tools.events` |
| Approval card | `ApprovalCard(AgentTools.describe(...))` | `approval.request` → `ApprovalCard(presentation ?? presenter(tool,args))` | `approvals != null` |
| Mode picker (Ask/Auto/Full) | `AgentTools.available` | `session.configure({permissionMode})`; modes from capabilities | hidden if `approvals == null` |
| Stop / Escape | `abort()` | `turn.cancel`; pending cards settle `deny` locally | `turns.cancel` |
| Sending while busy | `interject` → queue → between steps | `turn.steer` (or `followUp` if steering isn't supported); bubble placed on `input.accepted` | `turns.steer` / `followUp` |
| Retry button | `retry()` → `resume()` | `turn.retry` | `turns.retry` |
| Error box + Open settings | `fail()` with `status===401` | `turn.completed{status:'error', error}` with `action` | — |
| Finish notes | `FINISH_NOTES` | `finishReason` | — |
| Compaction notice, "Compact" item | `compact()` | `session.compact`; `compaction.started`/`completed` | `compaction.manual`/`auto` |
| Model switch with summary | `switchModel()` | `session.configure({model})` → `compaction.*{reason:'model-switch'}` → `session.updated{model}` | `compaction.onModelSwitch` |
| Model picker | `settings.models` from `Providers.models` | `models.list` (cached), groups from `Provider.group`/`name` | — |
| Effort slider | `model.efforts` | `Model.thinkingLevels`; `session.configure({thinking})` | hidden if one level |
| Context fill (add menu) | `conv.tokens / windowOf()` | `usage.context` | `usage.context` |
| Stats card | `chat.stats()` from `entry.usage` | Per-message `usage` stored in the display cache; same `stats()` | `usage.tokens` |
| Settings → Providers | Hard-coded sections | `auth.providers` rows (API key, OAuth) | `auth.providers` |
| Settings → Usage | `Usage` ledger + ChatGPT limits + DeepSeek balance | Ledger from `usage` events; `account.limits` | `usage.*` |
| Settings → General | `UserContext.prompt()` into the system prompt | `session.configure({userContext})` on change and at create | `userContext` |
| Chat titles | `name()` → `Providers.complete` | `session.updated{title}`; fallback `titleFrom` | `titles` |
| Mini chat | `SideChat` | `session.side` + normal turn APIs; "moved" marker stays frontend | hidden if `!sessions.side` |
| Browser panel (user) | `desktop/browser.js` guard/adopt | unchanged host service | — |
| Browser panel (agent) | `browser_*` via `tool:run` | `host.tool` reverse request; drive state from `host.tool` start/end | `tools.hostTools` |
| Browser context notes | `chat.notes()` | `context.update` on panel change | — |
| Video cards' titles | `fetch_url` tool | host `media.videoInfo` | — |
| Attachments | `userContent()` XML | `Input.attachments` | `attachments.*` |
| Chat lock | frontend seal | unchanged for the display cache; warn if `!sessions.encrypted` | — |
| Delete chat / folder | `library.remove` + `releaseFolder` | + `session.delete` | `sessions.delete` |

---

## 9. Backend adapter notes

- **Legacy adapter (recommended first backend).** Extract the 1.3.0 agent (§10.1 files plus the R parts of `chat.js`) into a standalone Node process that speaks ABP. It proves the contract with **identical behavior**, because the same prompts, tools and providers are behind it. That makes it the safety net for the refactor. It can later live in its own repository. It needs Electron-only pieces replaced:
  - `safeStorage` → OS keychain via a library, or host secrets
  - `net.fetch` → `fetch`
  - Hidden `BrowserWindow`s for media and PDF → ffmpeg or `pdfjs-dist`, or host services. The simplest route is the host-service route: the backend calls a host request such as `host.pdfText`.
- **Pi.** To my knowledge Pi's coding agent has an RPC mode that speaks JSON lines over stdin/stdout:
  - Commands for prompting, steering, follow-up and abort
  - Model and thinking-level changes, compaction and session stats
  - Streamed message, tool-execution and compaction events

  That maps closely onto ABP `turn.*`, `message.*`, `tool.*`, `compaction.*` and `session.configure`. It's also why this contract keeps *steer* and *follow-up* as distinct operations. Pi favors running without permission prompts, so expect `approvals: null` (mode picker hidden) unless approvals are added through a Pi extension that asks the host for confirmation. Verify every command and event name against the Pi version you target before writing the adapter. Nothing here has been tested against Pi.
- **Ghosty/Rust, Tau/Python, new backends.** Implement ABP directly over stdio. The conformance suite (§12.4) is the spec. These backends advertise only what they support, and the UI hides the rest.
- **Adapter placement.** Adapters are separate executables, or a `backend` config entry pointing at them. They aren't renderer modules. The frontend repository may ship the **mock backend** only.

---

## 10. Exact files to delete, refactor, keep

### 10.1 Files that would eventually be deleted from the frontend

Move them into the legacy-backend package first (§11), then delete them here.

| File | Reason |
|---|---|
| `desktop/llm.js` | provider run registry |
| `desktop/openai.js` | OpenAI / Codex client |
| `desktop/anthropic.js` | Anthropic client |
| `desktop/chatgpt.js` | ChatGPT OAuth and limits |
| `desktop/keys.js` | provider key vault (unless kept as generic host secrets, §3.1) |
| `desktop/tools.js` | shell, fs, git, fetch, video tools |
| `desktop/media.js` | tool-only media helper |
| `deepseek.js` | DeepSeek client in the page |
| `providers.js` | provider facade |
| `agent-prompt.js` | agent system prompt |
| `agent-tools.js` | tool schemas, search scraping, policy (after extracting `approval-presenter.js`) |
| `test/bench-reads.js` (untracked) | benchmarks the removed loop and tools; depends on a missing harness |
| `package.json` → `dependencies["@anthropic-ai/sdk"]` and the matching `package-lock.json` entries | SDK used only by `desktop/anthropic.js` |

### 10.2 Files that would need refactoring

| File | Change |
|---|---|
| `chat.js` | Split per §3.6 into `chat.js` (view, turn UI state, rendering, restore, stats) + a `backend-client.js` consumer. Remove prompts, loop, compaction, notes, seams, estimation and naming. `promptOf` reads display attachments. `SideChat` keeps only its UI differences. |
| `settings.js` | Data-driven providers (`auth.providers`); drop `KEYS`/`LINKS`/`FIRST_PROVIDER`/key migration and storage; models from `models.list`; `configFor` → `{modelId, thinking}` only; `windowOf` from `Model.contextWindow`. |
| `settings-usage.js` | Providers, tones and plans from data; `account.limits` instead of `auth.limits` + `DeepSeek.balance`. |
| `usage.js` | Accept normalized `usage` events; drop the `PROVIDERS` allowlist and `parts()` provider shapes; optionally store `cost`. |
| `user-context.js` | Keep store and UI API; replace `prompt()`/`pictures()` with `toUserContext()` structured output. |
| `library.js` | Store `sessionId` per chat; `conversation`/`saveMessages`/`side` → display-cache read/write; keep lock, index, workspace; `pathKey` by backend platform. |
| `media-embed.js` | Use host `media.videoInfo` instead of `tools.run('fetch_url')` + `AgentTools.environment()`. |
| `browser-panel.js` | `run`/`tabsTool` become the `host.tool` executor (absorbing the take-control wait from `chat.js` `useTool`); `context()` feeds `context.update`. |
| `mini-chat.js` | Gate on `capabilities.sessions.side` and `approvals`; no `AgentTools`. |
| `model-stage.js` | Provider groups from data; `switchModel` → `session.configure`. |
| `effort-slider.js`, `mode-picker.js`, `add-menu.js` | Levels, modes and items from capabilities. |
| `script.js` | Construct `BackendClient`; gate UI on capabilities, not `AgentTools.available`. |
| `index.html` | Drop removed scripts; add `backend-client.js`, `approval-presenter.js`, `render-guide.js`; CSP `connect-src 'none'`. |
| `i18n.js` | Generic provider and auth strings; new `error.context_overflow`, `error.backend` strings. |
| `desktop/main.js` | Remove `tool:*`, `LLM`, `Keys`, `Tools`; add `BackendHost` and host services (`workspace`, `media.videoInfo`, `host.tool` → `Browser.run`); quit sequence flushes store, then `shutdown`s the backend. |
| `desktop/preload.js` | Remove `tools`/`llm`/`keys`/`auth`; add `backend` and `host` bridges. |
| `desktop/browser.js` | Keep; export `run` for the host-tool path; no `tools.js` import. |
| `desktop/pdf.js` | Keep; no longer imported by `tools.js`. |
| `package.json` | Drop the SDK; `build.files` excludes adapters and mock fixtures except the shipped mock; `asarUnpack` for any bundled backend binary. |
| `README.md` | Describe the pluggable backend (after the licensing question is settled). |

**New files (later):**

- `backend-client.js`: renderer JSON-RPC peer, capabilities, event dispatch, seq tracking
- `approval-presenter.js`: `describe` + effect analyzers, extracted from `agent-tools.js`
- `render-guide.js`: `FORMAT_GUIDE`
- `desktop/backend-host.js`: spawn, relay and restart
- `desktop/host-services.js`
- `desktop/mock-backend.js`: scripted ABP backend for the baseline and tests
- `test/…` (§12)

### 10.3 Kept unchanged

Every file in §3.3, plus `desktop/browser-preload.js`, `desktop/pdf.html`, the icons, `cursor.png`, `chat-store.js`, `chat-lock.js`, `attachment-reader.js`, `attachments.js`, `approval-card.js`, `stats-card.js`, `stream-view.js`, `styles.css`, `LICENSE` and `.github/`.

---

## 11. Safest implementation order

The approach is a strangler fig. Every phase ends with the app working and the visual baselines green.

| Phase | Work | Exit criteria |
|---|---|---|
| **0. Characterize** (no runtime change) | Add an Electron Playwright harness and capture baselines on unmodified 1.3.0. Stub the providers: intercept `https://api.deepseek.com` (renderer `fetch`, CSP-allowed) with canned SSE, and seed `userData` with fixture stores. Record screenshots and DOM snapshots for every state in §12.2. | Baselines committed; deterministic on CI (Linux, xvfb). |
| **1. Contract in-process** | Write `backend-client.js` and an **in-renderer LegacyBackend** that wraps today's loop *unchanged* behind ABP calls and events (same objects, no transport). `chat.js` view code calls only the client. | All baselines identical; legacy behavior unchanged. |
| **2. Split `chat.js`** | Move prompts, loop, compaction, naming and notes out of `chat.js` into the LegacyBackend module. Extract `approval-presenter.js` and `render-guide.js`. Introduce the display-transcript format with a **reader for 1.3.0 stored chats** (no write-back yet). | Baselines identical; old chats open identically. |
| **3. Host services** | Add `workspace`, `media.videoInfo` and `host.tool` (browser) services; move the take-control wait into the host-tool executor; `context.update`. | Browser-panel agent flows identical with the legacy backend. |
| **4. Out of process** | Move the LegacyBackend into a child process (`desktop/backend-host.js` + stdio JSONL). Replace Electron-only helpers per §9. Remove `llm:*`, `tool:*`, `keys:*`, `auth:*` IPC. | Same baselines; `backend_crashed` path tested; quit flushes. |
| **5. Data-driven settings** | Providers, models, usage and limits from the contract; capability gating everywhere. | Settings baselines identical with the legacy backend; mock backend with fewer capabilities hides the right controls. |
| **6. Frontend-only baseline** | Ship the **mock backend** as the default when none is configured. Delete §10.1 files from the frontend repository, and move the legacy backend to its own repository or package. Tighten the CSP. Drop the SDK. | `npm start` with no backend shows the full UI; conformance suite green for mock + legacy. |
| **7. New backends** | Pi adapter, Rust/Python backends against the conformance suite. | Per-backend conformance plus visual smoke. |

Order rationale:

- Phase 0 has to come first because there are no tests today.
- Phase 1 changes *call paths*, not behavior.
- Persistence migration (phase 2) is read-only until the new format is proven.
- Process extraction (phase 4) happens only after the in-process contract is stable.
- Deletion is the *last* step.

---

## 12. Tests that prove the UI survives

### 12.1 Harness

- Playwright's Electron support (`_electron.launch`) with an isolated `--user-data-dir` per test, `prefers-reduced-motion` both on and off, a fixed window size and the dark and light themes.
- A **scripted mock backend** that replays JSONL fixtures with timing control (instant, stepwise), including reverse requests. The same fixtures drive the in-renderer client in unit tests through a fake transport.
- **Phase-0 provider stubs:** intercept `api.deepseek.com/chat/completions` and `/models` with canned SSE, so baselines come from *unmodified* 1.3.0.
- Visual comparison: pixel diff with a small threshold. Animations are tested at reduced motion for the static diff and with `page.clock` for frame checks.

### 12.2 Visual and behavioral baselines (each in empty, short and long chats)

1. Welcome and empty state, splash (desktop), sidebar with folders, pins and search, collapsed sidebar.
2. Streaming: ghost appears → first delta dismisses it → text streams (StreamView) → toolbar appears; multi-part reply with a tool step between parts; follow-scroll spring and the scroll-to-bottom button.
3. Markdown, TeX, code highlighting and copy buttons; **every diagram kind** in `FORMAT_GUIDE` (one fixture message each); media stack and video card (with stubbed oEmbed); callouts; column-arithmetic worksheet.
4. Approval cards: command (with effect badge and places), file write, file edit (diff), web and browser; details reveal and its remembered state; allow, deny, superseded-by-message; mode switch auto-allowing a pending card.
5. Stop by Escape mid-stream and mid-approval; "stopped", "empty" and finish notes; error box with Retry and Open settings.
6. Sending while busy: the bubble lands before the next assistant part; steer accepted.
7. Compaction: manual from the plus menu, auto, and model switch (running → done / failed labels).
8. Stats card enter animation, removal, persistence across reopen.
9. Model picker (groups, staged animations, "needs key" path), effort slider (levels per model, locked while busy), mode picker.
10. Settings: General (instructions and files), Providers (API-key row, OAuth row waiting, connected, error), Usage (periods, month nav, limits windows, balance), Appearance (theme transition, titlebar color IPC).
11. Attachments: drop zone, tray, notes, image, video poster, PDF, Office, pasted text; sent bubble rendering; reopen rendering.
12. Mini chat: open from selection, quote, answer, close and reopen ("moved" notice), clear with confirmation.
13. Chat lock: protect → locked list row → lock screen → unlock → content; lock during a running reply.
14. Browser panel: open and resize, tabs, address bar; agent drive state, cursor ripple, take control → message → hand back; downloads toast; sign-in recorded.

### 12.3 Persistence and migration tests

- Golden 1.3.0 stores (`index.json`, `chats/*.json` with native blocks, image data URLs, stats and compaction entries, a sealed chat, `mini/*.json`, `usage.json`, `context.json`). Assert that the display-transcript reader renders them **pixel-identical** to 1.3.0 and that nothing is written back until the user acts.
- Lock round-trip (seal → unlock → content) on migrated data.

### 12.4 Contract conformance suite (backend-agnostic)

- Handshake and capability honesty: no event or request for a feature the backend didn't advertise.
- Turn lifecycle ordering (`turn.started` → messages → `turn.completed` exactly once), `seq` monotonic, cancel within N ms, steer acceptance, retry.
- Approval reverse-request semantics (deny, superseded, cancelled via `$/cancelRequest`), host-tool round trips, `handed-back`.
- Errors: every `AbpError.code` producible by the mock; `backend_crashed` when the child exits.
- Framing robustness: large messages, Unicode, partial lines, stderr noise ignored.
- Run against: mock (always), legacy (phases 4–6), Pi adapter and others (phase 7). The untracked `.qualification` fixtures (approval-mode switches, large writes, stopping a write) are good *ideas* for backend-side tests here.

### 12.5 Unit tests (renderer, without Electron)

`backend-client` (dispatch, seq gaps → `session.get`), `approval-presenter` (port today's shell and git effect cases for Windows and POSIX), `usage` ledger aggregation, `library` index and locking, the error → i18n mapping, and the `render-guide` export.

---

## 13. Risks and hidden backend coupling

### 13.1 License (highest risk)

`LICENSE`:

- MIT covers "the agent engine, the tools, and the rendering engine".
- The name, ghost logo, **animations** and **visual design** are excluded and "all rights reserved".
- Distributing, publishing or hosting a *modified version* that keeps any excluded material is forbidden.
- Commercial use, including "a fork, a rebrand", is forbidden.
- Keeping a fork on GitHub is allowed "in order to propose changes to this repository". Any other use needs written permission.

This plan **removes the MIT-licensed part and keeps the reserved part**, then modifies it. Even private use of a modified build isn't in the "may do" list. I'm not a lawyer. Before publishing builds, pushing a divergent fork beyond proposing changes upstream, or using this commercially, get written permission from the author, or plan a separate name, logo, animations and visual design. This doesn't block the audit. It decides what the final product can look like.

### 13.2 The persisted history is model-shaped and provider-specific

`chats/<id>.json` stores Chat Completions messages with:

- `steps` (assistant, tool and injected note messages)
- `native` blocks (Anthropic signed thinking, OpenAI encrypted reasoning)
- `cache` marks
- `reasoning_content`
- **image data URLs inside user `content`**

The UI renders user images by pulling `image_url` parts back out of that content (`promptOf`, `chat.js:1410`). New backends can't resume these histories. Migration is display-only (lossy for the model) unless the legacy backend is kept around to continue old chats. Decide this explicitly.

### 13.3 Chat lock vs backend-owned sessions

The lock encrypts the frontend's files. A backend that stores its own transcripts (Pi does) keeps plaintext copies, so "locked" would become a UI lock only. Options:

- Advertise `sessions.encrypted`.
- Disable locking for backends without it.
- Keep backends stateless, with the frontend as the store. That doesn't fit Pi.

The UI must not overstate the protection.

### 13.4 Steering semantics are subtle

A message sent mid-turn:

- settles any pending approval as "the user sent a new message instead" (`TOOL_NOTES.message`)
- is queued and injected **between tool steps**, not after the turn
- has its bubble placed before the next assistant part (`turn.next`)
- if the turn ends first, is stored as a user message without being answered (`end`, lines 1183–1189)

Backends differ here (steer vs follow-up). The contract needs `input.accepted` and both operations, and the UI must handle "not accepted".

### 13.5 Mode changes reach into running turns

`onModeChange` re-runs `needsApproval` for pending cards and auto-allows them. With backend-owned policy, the backend must re-evaluate on `session.configure` and send `approval.resolved`. Otherwise cards hang.

### 13.6 The frontend mutates history (`diagram-edit`)

Editing a diagram rewrites the stored assistant text, which the model sees in later turns. The contract needs `session.editMessage`, or the frontend has to keep edits as display-only overlays and accept that the model won't see them.

### 13.7 Display-only entries are mixed into the history

`{role:'stats'}` cards and mini-chat `{role:'moved'}` markers live in the message arrays. The backend's `history()` skips them only implicitly. They must become frontend annotations anchored to message ids.

### 13.8 Browser automation is host-bound

- It uses CDP on webview guests (`webContents.debugger`), a per-tab action queue, cursor `pointer` events and focus juggling (`giveBack`).
- `RISKY_CLICK` approval needs the last snapshot's `refs` map, which `agent-tools.js` holds.
- The take-control wait is in `chat.js` `useTool`.

All of it must become one host-tool executor. Any backend that wants the browser has to call the host for it.

### 13.9 Prompt coupling to the renderer

Without `FORMAT_GUIDE`, other backends won't produce `metrics`, `files`, `wireframe` and the other custom blocks, and the "visual app" UX quietly degrades. `find_media` and the video-link format feed `media-embed.js`. The render guide must ship with `initialize`. Backends decide whether to honor it, and the UI has no way to enforce it.

### 13.10 Platform assumptions

- The approval presenter, path chips (`place`, `distinct`), `pathKey` case rules and Windows reserved-name handling all assume **the frontend's OS is the backend's OS**. A remote backend breaks that. Use `backend.platform`.
- `pathOf` paths mean nothing to a remote backend, so attachments must carry bytes or text.

### 13.11 Other hidden coupling

- `media-embed.js` borrows the agent's `fetch_url` tool (with `home` as cwd) for YouTube titles. Removing tools silently drops video titles.
- `AgentTools.available` doubles as "is desktop", which gates the mode picker, browser toggle and mini-chat mode picker.
- `providers.js` dispatches the window event `models-stale` (Anthropic "quiet" refusal learning), and `settings.js` listens for it.
- `keys:read` is `sendSync` at startup. `settings.readKeys` migrates legacy `localStorage` keys (`openai.apiKey`, `anthropic.apiKey`, `deepseek.apiKey`) into the keychain. Removing it without a migration loses or strands users' keys. Moving keys to a backend means a one-time export (with consent) or re-entry.
- `settings.js` shows saved keys in the field, with an eye to reveal. Backend-owned keys make that "saved, hidden", which is a small UX change.
- `usage.js` silently drops unknown provider ids (`PROVIDERS` allowlist).
- `settings.windowOf` defaults to a **1,000,000-token** window for unknown models, so compaction never triggers on a bad catalog. The context fill comes from the same number.
- `effort` is stored under the key `deepseek.effort`. `'none'` is a synthetic level the app adds.
- Title generation spends tokens on the chat's model (`Providers.complete`). Backends should advertise `titles`.
- The CSP allows `connect-src https://api.deepseek.com`. Tighten it once DeepSeek is gone, and never widen it for a transport.
- `before-quit` waits for store writes and kills tools. The backend child now needs a `shutdown` handshake with a timeout, and a Windows process-tree kill.
- `build.files: ["**/*"]` would package adapters, fixtures and mock data unless excluded. A bundled native backend needs `asarUnpack` and per-OS builds in CI.
- The mini chat assumes the backend reads the *live* parent history on every request, and caches against the parent's prefix (`SideChat.seam`). `session.side` must define freshness.
- Several i18n strings name providers ("Sign in to ChatGPT in settings first" comes from `chatgpt.js`, plus `settings.chatgpt.*`).
- There's no test infrastructure at all. The untracked `test/bench-reads.js` is broken (missing `./harness`).

---

## 14. Recommended frontend-only baseline

### 14.1 Shape

```text
OpenGhost (frontend-only)                                    Backends (separate repos/packages)
┌──────────────────────────────────────────────┐            ┌───────────────────────────────┐
│ Renderer                                     │            │ legacy-openghost-backend (Node│
│  UI modules (§3.3, unchanged)                │            │   1.3.0 agent behind ABP)     │
│  chat.js (view + turn UI state)              │            │ pi-abp-adapter → pi (RPC)     │
│  backend-client.js (ABP peer, capabilities)  │            │ ghosty (Rust, ABP native)     │
│  approval-presenter.js  render-guide.js      │            │ tau (Python, ABP native)      │
│  library.js (index, workspace, display cache,│            └──────────────▲────────────────┘
│              lock)  usage.js (ledger)        │                           │ JSON-RPC 2.0 / JSONL
│  settings*.js (data-driven)                  │                           │ stdio (default) or
│  browser-panel.js (panel + host-tool exec)   │                           │ socket/pipe/ws
├──────── preload: backend / host bridges ─────┤                           │
│ Main (host)                                  │                           │
│  main.js window/theme/menu/quit              │                           │
│  backend-host.js spawn|connect, relay ───────┼───────────────────────────┘
│  host services: store, workspace, pdf,       │
│    media.videoInfo, browser (CDP) host tools │
│  mock-backend.js (default when unconfigured) │
└──────────────────────────────────────────────┘
```

### 14.2 Rules for the baseline

1. The frontend contains **no provider names, no model ids, no system prompts other than the render guide, no tool implementations other than host tools, and no approval policy**.
2. Everything it shows about models, providers, auth, usage, limits and permissions comes from the backend, gated by capabilities.
3. It owns:
   - workspace organization
   - the display cache and annotations
   - locking
   - attachment intake
   - the usage ledger
   - UI preferences
   - the browser panel
   - the approval safety-net presenter
4. With the mock backend it renders every 1.3.0 surface (§12.2), so visual regressions are caught without any real model.
5. A backend is a command plus args in settings. Swapping Pi, Rust, Python or anything new is a config change plus that backend passing the conformance suite. The frontend doesn't change.

---

## Appendix A: IPC inventory

| Channel | Kind | Direction | Handler | Fate |
|---|---|---|---|---|
| `folder:pick` | invoke | R→M | `main.js` | keep (host) |
| `folder:reveal` | invoke | R→M | `main.js` | keep |
| `folder:chats` | invoke | R→M | `main.js` (`Tools.CHATS`) | keep → host `workspace` |
| `folder:release` | invoke | R→M | `main.js` (`Tools.release`) | keep → host `workspace` |
| `store:read` / `store:write` / `store:remove` | invoke | R→M | `main.js` | keep |
| `window:titlebar` | send | R→M | `main.js` | keep |
| `theme:set` | invoke | R→M | `main.js` | keep |
| `tool:run` / `tool:cancel` / `tool:environment` | invoke | R→M | `main.js` → `tools.js`/`browser.js` | remove; browser → `host.tool` |
| `browser:shown` | send | R→M | `browser.setShown` | keep |
| `browser:event` | send | M→R | `browser.js` (open/key/pointer/download) | keep |
| `pdf:read` | invoke | R→M | `pdf.js` | keep |
| `llm:start` / `llm:abort` | send | R→M | `llm.js` | remove → backend bridge |
| `llm:models` | invoke | R→M | `llm.js` | remove → `models.list` |
| `llm:event` | send | M→R | `llm.js` | remove → backend messages |
| `keys:read` | sendSync | R→M | `keys.js` | remove (migration, §13.11) |
| `keys:write` | invoke | R→M | `keys.js` | remove → `auth.setKey` |
| `auth:login` / `cancel` / `logout` / `status` / `limits` | invoke | R→M | `llm.js` → `chatgpt.js` | remove → `auth.*`, `account.limits` |
| `signin` | `sendToHost` | webview→R | `browser-preload.js` | keep |

All renderer → main handlers check `fromApp(event)` (sender is the app window with a `file:` URL). The new `backend:*` and `host:*` channels must keep that check.

## Appendix B: persisted data inventory

| Location | Owner today | Content | Future owner |
|---|---|---|---|
| `userData/store/index.json` | `library.js` | folders, chats (id, title, folder/space, pinned, named, renamed, model, lock{salt,iterations,title}) | frontend (+ `sessionId`) |
| `userData/store/chats/<id>.json` | `library.js` | model-facing history `{messages, tokens}` or `{sealed}` | backend history + frontend display cache |
| `userData/store/mini/<id>.json` | `library.js` | mini-chat history + `seen` | backend side session + frontend display cache |
| `userData/store/usage.json` | `usage.js` | token ledger by day and model | frontend |
| `userData/store/context.json`, `context/<fileId>.json` | `user-context.js` | instructions, files, payloads | frontend (sent to backend) |
| `userData/store/auth/keys.bin` | `keys.js` | API keys (safeStorage) | backend (migrate, §13.11) |
| `userData/store/auth/chatgpt.bin` | `chatgpt.js` | OAuth tokens | backend |
| `userData/theme.json` | `main.js` | theme choice | frontend |
| `~/OpenGhost/Chats/<space>/` | `tools.js` / `library.js` | working folders of chats without a project | frontend workspace service |
| `localStorage` `openghost.model`, `deepseek.effort`, `openghost.mode`, `openghost.catalog` | `settings.js` | selection and catalog cache | frontend (rename effort key) |
| `localStorage` `openghost.approval.details` | `approval-card.js` | details open | frontend |
| `localStorage` `openghost.media.info` | `media-embed.js` | YouTube titles | frontend |
| `localStorage` `openghost.browser`, `openghost.browser.accounts` | `browser-panel.js` | tabs, signed-in sites | frontend |
| `localStorage` `openai.apiKey`, `anthropic.apiKey`, `deepseek.apiKey` | `settings.js` (legacy, migrated) | old plain keys | delete after migration |
| `localStorage` `openghost:*` | `chat-store.js` fallback (non-desktop) | store in a browser | frontend |
| webview partition `persist:browser` | `browser.js` | the browser's cookies and logins | frontend host |
