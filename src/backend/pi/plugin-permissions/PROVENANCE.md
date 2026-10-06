# Provenance of plugin-permissions

plugin-permissions is derived from **`@gotgenes/pi-permission-system` 40.0.0**,
maintained in [gotgenes/pi-packages](https://github.com/gotgenes/pi-packages)
(`packages/pi-permission-system`), at tag `pi-permission-system-v40.0.0`, commit
**`191011f3585f96796b44a29dd20c70bfac49febe`** (2026-10-06, "chore(release):
pi-permission-system 40.0.0"). That package is itself a fork of
[MasuRii/pi-permission-system](https://github.com/MasuRii/pi-permission-system).

License: MIT, **Copyright (c) 2026 MasuRii and Christopher D. Lasher**. The
license is kept verbatim in [LICENSE](LICENSE) (and in the repository's
`licenses/plugin-permissions-LICENSE`); it covers this directory's code derived
from the donor. The donor's changelog is kept as [CHANGELOG.md](CHANGELOG.md).
The bundle also contains zod, web-tree-sitter and tree-sitter-bash: see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

OpenGhost maintains this fork as its own component. Source comments still cite
the donor's issue numbers (`#NNN`) and ADRs, which refer to gotgenes/pi-packages
at the commit above.

## What was taken

`src/`, `test/`, `schemas/permissions.schema.json`, `config/config.example.json`,
`LICENSE`, `CHANGELOG.md`, `tsconfig.json`/`tsconfig.base.json` and
`vitest.config.ts`. The donor's `docs/` (including its development retros,
plans and ADRs) and `scripts/` were not copied; [docs/configuration.md](docs/configuration.md)
links the upstream reference at the commit above.

Of 168 donor source files, 123 are byte-for-byte unchanged, 40 were adapted and
5 removed. Of 198 donor test files, 155 are unchanged, 37 adapted and 6 removed
(their decision semantics ported where they still apply).

## Removed (the donor's terminal UI and control layer)

| Donor file | What it was | Why |
| --- | --- | --- |
| `src/authority/permission-prompt-component.ts` | The inline `ctx.ui.custom` terminal dialog, and the `select`/`input` fallback dispatch | OpenGhost's approval card is the human surface (`src/openghost/approval-card.ts`). Its key semantics live on in the kept `permission-prompt-decision.ts` and `config/dialog-keys.ts`. |
| `src/authority/bracketed-paste.ts` | Terminal paste handling for the reason editor | Terminal only. |
| `src/presentation/prompt-notification.ts` | Terminal bell / OSC 9 / OSC 777 notifications | Terminal only. |
| `src/config/config-modal.ts` | The `/permission-system` command and its settings modal (yolo toggle, logging, reset) | A second mode authority and a donor control UI. |
| `src/config/status.ts` | The status-bar yolo indicator | OpenGhost shows the mode. |
| `requestPermissionDecisionFromUi` in `src/authority/permission-dialog.ts` | The `select`/`input` fallback dialog | Replaced by the card; its semantics are ported to `test/openghost/approval-card.test.ts`. |
| `ConfigStore.save` | Wrote runtime knobs for the modal | Only the modal used it. |
| Legacy config loading in `config-loader.ts` / `config-paths.ts` | Read and merged `pi-permissions.jsonc` (global and project) and a `config.json` beside the extension | No silent reading of donor configuration; no migration. |
| `yoloMode`, `promptNotifications` config keys | A second mode authority; terminal notifications | See above. A file setting them is now invalid. |

## Adapted (integration, not policy)

- `src/index.ts`, the composition root: the mode from `openghost:mode` drives the
  donor's yolo rewrite (as Full) and a new mode rule layer; asks go to the
  approval card; no command registered; the tool call's input is noted for the
  card inside the fail-closed boundary.
- `src/policy/permission-manager.ts`: an optional `getModeLayer` composed after the
  synthesized defaults and before config, keyed into the resolved-rules cache.
  An explicit deny always wins: `check` asks the policy without session rules
  first, and a deny it reaches from a config or built-in rule holds, so a
  session approval never overrides it (upstream composed session rules last,
  letting a grant whose pattern covered a denied call allow it). The upstream
  tests asserting that override (`test/policy/permission-manager-unified.test.ts`)
  now assert the deny; Ask, Auto and Full are covered in
  `test/openghost/modes.test.ts`.
- `src/policy/rule.ts`: origin `mode`; the yolo origin and rewrite renamed
  `full_access` / `rewriteAsksForFullAccess`.
- `src/authority/local-user-authorizer.ts`, `src/authority/authorizer.ts`: the
  human authority is the card (Pi's `confirm` plus the turn's abort signal), not
  the terminal dialog.
- `src/authority/decision-source.ts`: user decision surface `approval_card`.
- `src/access-intent/bash/parser.ts`: the two WASM grammars are found beside the
  bundle before `node_modules`.
- `src/config/*`: knobs and legacy loading removed as above; descriptions name
  the card.
- Everything else: names only. `pi-permission-system` → `plugin-permissions` in
  paths, log names, process-global `Symbol.for` keys
  (`openghost:plugin-permissions:*`) and messages; "yolo" → "full access" in
  identifiers, origins and the decision kind.

## Added (OpenGhost)

`src/openghost/`: `access-mode.ts` (the mode as OpenGhost says it),
`mode-policy.ts` (Ask and Auto as rules), `approval-card.ts` (the card as the
human authority), `presentation.ts` (the card's wording, OpenGhost 1.3's),
`tool-input-ledger.ts`, `connection.ts` (the bridge's handle). Tests in
`test/openghost/` and `test/helpers/approval-card-ui.ts`; build scripts in
`scripts/`.

## Retained upstream behavior worth knowing

- Durable "always allow" is not offered: upstream parks durable approvals on
  issue #799. Persistent policy is the config files.
