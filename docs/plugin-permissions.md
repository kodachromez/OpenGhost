# plugin-permissions

`plugin-permissions` is OpenGhost's Pi permission plugin, in
[`src/backend/pi/plugin-permissions/`](../src/backend/pi/plugin-permissions/).
It runs inside each chat's Pi process, beside OpenGhost's bridge, and is **the
only place an OpenGhost chat's tool calls are permitted or refused**.

**OpenGhost owns** the Ask / Auto / Full selection and its persistence, the
approval card, its keyboard handling, and the transport and lifecycle of
requests and answers. **plugin-permissions owns** permission policy and
enforcement: every `allow`, `deny` and `ask`.

```text
OpenGhost UI ── Ask / Auto / Full ──▶ bridge ── openghost:mode ──▶ plugin-permissions
                                                                       │ rules + mode layer
Pi tool call ──▶ plugin-permissions ──▶ allow ─────────────────────────┤──▶ runs
                                    ──▶ deny ──────────────────────────┤──▶ refused, reason to the agent
                                    ──▶ ask ── openghost:approval ──▶ OpenGhost ApprovalCard
                                                                       │ user decision
                         plugin-permissions ◀── choice + confirm ◀─────┘ ──▶ runs / refused
```

It is derived from [`@gotgenes/pi-permission-system`](https://github.com/gotgenes/pi-packages/tree/191011f3585f96796b44a29dd20c70bfac49febe/packages/pi-permission-system)
40.0.0 (MIT, Copyright (c) 2026 MasuRii and Christopher D. Lasher), whose
enforcement engine it keeps. Its terminal UI and control layer were removed and
OpenGhost's mode and card took their place:
[PROVENANCE.md](../src/backend/pi/plugin-permissions/PROVENANCE.md).

## What it enforces

The donor's engine, unchanged: `allow` / `ask` / `deny` rules per surface with
last-match-wins wildcard patterns.

- **bash.** The command is parsed with tree-sitter into units. Chains, pipelines,
  substitutions and subshells are each gated, the most restrictive wins, and
  heredoc and redirect edge cases are handled. Indirection wrappers (`bash -c`,
  `eval`, `sudo`, `env`, `xargs`, `find -exec`, …) and unparseable commands ask
  rather than pass (fail closed). The paths a command names are extracted and
  gated as reads or writes.
- **Files.** File tools are gated on the path Pi will actually open (`@`, `~`, file
  URLs, Unicode spaces, read fallbacks), normalized and symlink-resolved, so an
  alias cannot evade a rule. Reaching outside the working directory goes through
  the `external_directory` boundary; the cross-cutting `path` surface protects
  sensitive files from every tool.
- **Other tools.** MCP tools (Pi's own and the `mcp` proxy), skills and extension
  tools, with registered access extractors and formatters.
- **Failures.** A gate error blocks the call (fail closed), and an invalid project
  config clamps every `allow` to `ask`.
- **Subagents.** In-process and out-of-process subagent asks are forwarded to the
  serving session and answered on its card.
- **Visibility.** Tools a policy fully denies are hidden from the agent.
- **Records.** Decisions are broadcast on `permissions:decision` and recorded in a
  review log (`~/.pi/agent/extensions/plugin-permissions/logs/`).
- **Authorizer chain.** An opt-in chain of decision links (`authorizerChain`) is
  consulted before the card.

## Ask / Auto / Full

OpenGhost relays the chat's mode on pi.events `openghost:mode` as `{mode, seq}`.
The plugin takes it as authoritative and holds no other mode. There is no config
key, command or status that sets one. Until OpenGhost says a mode the plugin
enforces **Ask**. An update numbered below the last applied is ignored, and a
malformed one is read as Ask. The state is process-wide, so an in-process
subagent enforces the same mode.

A mode is a **rule layer** handed to the engine, composed after the defaults and
before the operator's config:

- **Ask.** Asks before running any command, changing a file, looking outside the
  chat's folder, and any other tool. Looking inside the folder is silent.
- **Auto.** Works in the folder on its own (file changes, ordinary commands). It
  asks before risky commands (deleting, privilege, stopping processes,
  services, system or global installs, permissions and firewalls, git
  operations that rewrite history or reach a remote, a bare shell), anything
  outside the folder, MCP and other tools.
- **Full.** Never asks. Every resulting `ask` becomes `allow` through the donor's
  composition-stage rewrite (formerly `yoloMode`), including the wrapper and
  unparseable sentinels. **A `deny` still denies**: an operator's deny rule
  (through every alias, symlink and chain), and the fail-closed clamp. Full is
  the plugin's behavior; OpenGhost never allows a call because the mode is
  Full.

Switching to Full while a card waits allows that call: the plugin takes the
request back with `{decision: "allow"}`. Switching between Ask and Auto applies
from the next call; a waiting card keeps waiting for an answer.

The exact rules are in
[configuration.md](../src/backend/pi/plugin-permissions/docs/configuration.md#how-a-mode-and-the-files-combine).

## Decisions on the card

A request offers the donor's decisions, each with a shortcut key:

| Decision | Card | Key | Effect |
| --- | --- | --- | --- |
| `approve` | **Allow** | `y` | This call only. |
| `approveSession` | **Allow for session** | `s` | A session rule for the suggested pattern (the tooltip names it, e.g. `Yes, allow bash "npm test*" for this session`). |
| `approveSessionBoth` | **Allow both for session** (link) | `b` | Offered for a file ask that proved one direction: reads and writes to the target. |
| `deny` | **Deny** | `n` | Refused. |
| `denyWithReason` | **Deny with reason** (link) | `r` | Opens a reason field with Cancel and Deny. The agent is told the reason. An empty reason cannot be sent. |

- **Subagent scope.** For an ask forwarded from a subagent, a session grant then
  asks for its reach: **This subagent** (the default, least privilege) or
  **Whole session**.
- **Shortcuts.** They work while the card (or one of its buttons) has keyboard
  focus. With `doublePressToConfirm` (the default) the first press arms ("Press
  s again to allow for this session.") and the second commits; Escape disarms,
  or leaves the reason step. Clicking a button commits at once.
  `permissionDialogKeys` remaps the keys.
- **Requests without actions.** These (another backend, the fake fixture's
  `/fake approval`) show OpenGhost 1.3's Allow and Deny only.
- **No durable approvals.** There is no "always allow" decision; upstream parks
  durable approvals (issue #799). Persistent policy is the config files.

What a decision means is the plugin's. The donor's own decision model
(`permission-prompt-decision.ts`, `reducePrompt`) turns the card choice into
the decision, exactly as the same keys did in the donor's terminal dialog.

## Configuration

| Scope | Path |
| --- | --- |
| Global | `~/.pi/agent/extensions/plugin-permissions/config.json` |
| Project (trusted projects only) | `<project>/.pi/extensions/plugin-permissions/config.json` |
| Per agent | `permission:` frontmatter in Pi agent definitions |

Operator rules refine the mode. For example, `"bash": {"npm test": "allow"}`
stops Ask asking, and a `deny` holds in every mode. An explicit deny always
wins: a session approval suppresses later asks but never overrides a deny rule,
in Ask, Auto or Full. Session approvals live in
the chat's Pi process and end with it. The donor's own config locations and
legacy files are not read and not migrated. Reference:
[configuration.md](../src/backend/pi/plugin-permissions/docs/configuration.md).

## The OpenGhost integration

All of it uses OpenGhost's existing channels:

| Channel | Direction | Carries |
| --- | --- | --- |
| bridge op `{op: "mode", mode, seq}`, then pi.events `openghost:mode` `{mode, seq}` | OpenGhost → plugin | The chat's mode. The reply names the enforcer and the mode it holds: `enforcer: {name, version, mode, seq}`, or `null` when no instance is connected with its gate in place. OpenGhost counts the mode as held only when the enforcer reports that mode. |
| Pi `confirm` titled `openghost:approval`, message `{approvalId, toolCallId, tool, args, presentation, actions, doublePressToConfirm, scopes?, title, facts}` | plugin → OpenGhost | A request, shown as the ApprovalCard. |
| bridge op `{op: "choice", approvalId, choice: {action, reason?, scope?}}` | OpenGhost → plugin | The card decision beyond yes/no, sent first. The plugin keeps it only for a card it has open and only as a refinement: a choice that disagrees with the confirm is dropped, and the safer answer holds. |
| the confirm's answer | OpenGhost → plugin | Allow or deny: authoritative. Sent once, only to the dialog that asked, and Allow only while that turn runs. |
| status `openghost:<approvalId>:approval` `{decision: "allow" \| null}` | plugin → OpenGhost | A request taken back: `null` for Stop or session end, `allow` when Full released it. |
| `globalThis[Symbol.for("openghost:plugin-permissions")]` | bridge → plugin | `choose(approvalId, choice)`, `mode()` and `enforcing()`: how the bridge relays a choice and reports the enforcer. |

OpenGhost's lifecycle is unchanged (`PiBackend`, `ChatService`):

- **Ownership.** A request becomes a card only for the chat's own Pi and its
  accepted, running turn. Early, stale and duplicate requests are handled, and
  answers are taken once.
- **Withdrawal.** Stop, the turn's end, Pi exiting or retiring, and deleting the
  chat withdraw the card, declining the request if Pi still waits on it.
- **Mode updates.** Numbered, so an older one never wins.

The plugin adds its own guards. It never opens a card for a turn already
stopped or once Full holds, it takes back every open card on Stop and when the
session ends, and it caps each text a card request carries (64 KiB) so a huge
call cannot make an undeliverable RPC record; the decision is made on the call
itself. A shortcut key held down never counts as its second press.

## Turning it off

Settings → Plugins → *Permissions* (`openghost.plugin-permissions`, on by
default) is the switch for the plugin and OpenGhost's permission UI.

- **Off.** The Ask / Auto / Full picker, the approval card's decisions (Allow
  for session, the links, the reason step) and the `y` / `s` / `b` / `n` / `r`
  shortcuts are gone. A request already waiting is declined to the Pi that
  asked and its card withdrawn, so Pi is never left waiting. Chats' Pi then
  start without plugin-permissions: an idle chat's Pi restarts before its next
  run, and a request a Pi still running with the plugin asks is declined at
  once. Pi's tools run without asking, and OpenGhost says permissions are not
  enforced.
- **On again.** The picker, decisions and shortcuts return, and each chat's Pi
  restarts with the plugin before its next run. A turn already running without
  the plugin finishes unenforced.

## The boundary

| OpenGhost (C++/QML, `openghost-bridge.js`) | plugin-permissions |
| --- | --- |
| Mode picker, per-chat mode, saved preference | The rules each mode stands for |
| Relaying the mode, numbered | Applying it; Full's rewrite |
| The approval card, its keys and its steps | The decisions a request offers, and what each means |
| Delivering the answer once, to the right dialog | Turning it into allow / deny / a session rule |
| Request lifecycle: Stop, turn end, exit, retirement, deletion | Taking a request back when it decides to |
| Saying when nothing enforces permissions | Every allow, deny and ask |

**OpenGhost decides no tool call.** It has no shell, path, read/write,
symlink, containment or mode policy. When plugin-permissions is not loaded
(Permissions turned off, or another Pi setup),
Pi's tools run unasked. OpenGhost then says *"Permissions are not enforced: Pi's
permission plugin (plugin-permissions) is not loaded, so Pi's tools run without
asking, whatever the access mode."* It does not enforce anything in the plugin's
place.

## Packaging

The plugin ships inside OpenGhost. Its committed bundle `dist/plugin-permissions.js`
(the sources with zod and web-tree-sitter inlined; Pi's own packages are
provided by Pi) and the two WASM grammars are Qt resources. `PiBackend`
unpacks them beside the bridge and starts every chat's Pi with
`-e openghost-bridge.js -e plugin-permissions.js`. Nothing is installed from npm
at run time, and building OpenGhost needs no Node. `native_plugin_permissions_bundle_check`
proves the committed bundle is what the sources build to.

## Tests

The suites:

- **`native_plugin_permissions_test`.** The plugin's vitest suite (5,500+
  tests), skipped without `npm ci`.
  - The donor's retained tests:
    - bash parsing (chains, substitutions, heredocs, redirects, wrappers,
      `sed`/`awk` scripts, variable expansion, MSYS tokens, arity, salvage of
      partial parses);
    - path extraction, normalization, canonicalization and symlinks
      (Windows and POSIX flavors);
    - external-directory and directional surfaces, sensitive `path` rules and
      MCP targets;
    - extension tools, extractors and formatters;
    - subagent forwarding, liveness and scopes;
    - the authorizer chain;
    - config validation, project trust and the fail-closed clamp;
    - session approvals and grant widths, and the review log;
    - the composition-root integration tests (82).
  - Upstream's regression tests for fixed bypasses (the `#NNN` cases) are kept
    as they were.
  - `test/openghost/` adds:
    - Ask, Auto and Full end to end through the factory, covering chains,
      substitution, wrappers, `eval`, redirects, traversal, symlinks,
      unparseable commands, unknown tools and hard denies under Full;
    - mode ordering and malformed modes;
    - no config or command mode;
    - session-approval scope, and config precedence: a session approval never
      overrides a config deny, in Ask, Auto or Full;
    - the card's decisions (the donor's fallback-dialog tests, ported), and the
      card's withdrawal on Stop, on Full and at session end.
- **`native_pi_real_test`.** The shipped bundle in real Pi (faux model, no
  network):
  - Ask: a card; Deny refuses and Allow runs.
  - Auto: the folder is silent; a risky command, a path outside the folder and
    a deny rule are each enforced.
  - Full: risky and outside calls are allowed and a hard deny holds.
  - The mode reaches the plugin, and an older update never wins.
  - Full releases a waiting card.
  - Malformed arguments never run.
  - Allow for session holds for this chat only.
  - Deny with a reason reaches the agent.
  - Stop, a stale answer and a duplicate answer run nothing.
  - Chat deletion and Pi exit close the card.
  - Without the plugin, OpenGhost enforces nothing and says so.
  - A session approval never overrides an operator's deny, in Ask, Auto or Full.
- **`native_pi_test`.** A card choice reaches the asker before its confirm; only
  offered decisions are taken. Turning Permissions off declines and withdraws
  the waiting card, declines later requests at once and restarts the chat's Pi
  without the plugin; on again, with it. Plus the existing lifecycle tests: Stop,
  deletion, staleness, answer-once and mode relay.
- **`native_fake_ui_smoke`.** The card's shortcuts (arming, Escape, double
  press), Allow for session, and the reason step's Cancel and Send. Permissions
  off withdraws a waiting card and hides the mode picker, and a card then has
  only Allow and Deny with no shortcuts; on again, all of it returns.

Pi validates and coerces a call's arguments against the tool's schema before
the `tool_call` hook runs, and hands the plugin those same arguments: the
plugin judges exactly what would run, and a malformed call never reaches it or
runs (`native_pi_real_test::malformedArgumentsNeverRun`).
