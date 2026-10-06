# plugin-permissions configuration

plugin-permissions is OpenGhost's Pi permission plugin. The overview, the
Ask / Auto / Full semantics and the OpenGhost boundary are in the repository's
[docs/plugin-permissions.md](../../../../../docs/plugin-permissions.md); this
file is the operator's reference for the rule files.

The rule engine is the one plugin-permissions was derived from
(`@gotgenes/pi-permission-system` 40.0.0). Its full upstream reference,
[configuration.md at 191011f](https://github.com/gotgenes/pi-packages/blob/191011f3585f96796b44a29dd20c70bfac49febe/packages/pi-permission-system/docs/configuration.md),
still describes every surface, pattern rule and recipe exactly, with the
differences listed under [Removed](#removed-from-the-donor) below.

## Files

| Scope   | Path                                                      |
| ------- | --------------------------------------------------------- |
| Global  | `~/.pi/agent/extensions/plugin-permissions/config.json`   |
| Project | `<project>/.pi/extensions/plugin-permissions/config.json` |

`~/.pi/agent` is Pi's agent directory (`PI_CODING_AGENT_DIR` when set). The
project file is read only once Pi trusts the project, so an untrusted
repository cannot loosen the global policy. A project file that fails
validation clamps every `allow` to `ask` (fail closed). Per-agent YAML
frontmatter (`permission:`) on Pi agent definitions overrides both.

The files are validated strictly against
[schemas/permissions.schema.json](../schemas/permissions.schema.json): an
unknown key is an error, and an invalid project file fails closed. A
`config.json` beside the plugin, the donor's legacy `pi-permissions.jsonc`
files and the donor package's own
`~/.pi/agent/extensions/pi-permission-system/` directory are **not** read:
there is no migration from the donor's configuration.

## How a mode and the files combine

For each check the plugin composes, lowest precedence first:

1. the universal fallback (`"*"` in the files; `ask` when absent);
2. the **access mode's rules** (OpenGhost's Ask or Auto layer, below);
3. the global, project and per-agent rules, in that order;
4. the session's approvals.

Within a surface the last matching rule wins, with one exception: **an explicit
deny always wins.** A call the files (or the universal fallback) deny is denied
even when a session approval's pattern covers it; session approvals only take
the place of asks and the mode's defaults. So an operator rule refines the
mode (`"bash": {"npm test": "allow"}` stops Ask asking about `npm test`), and a
`deny` in the files holds in every mode, Full included, and after any session
approval. Under **Full** every
resulting `ask` becomes `allow` (the donor's composition-stage rewrite); a
`deny` and the fail-closed clamp are untouched.

The mode layers, in the same flat form as the files:

| Surface                     | Ask     | Auto                                   |
| --------------------------- | ------- | -------------------------------------- |
| `read`, `grep`, `find`, `ls` | `allow` | `allow`                                |
| `write`, `edit`             | `ask`   | `allow`                                |
| `bash`                      | `ask`   | `allow`, except the risky commands below: `ask` |
| `mcp`                       | `ask`   | `ask`                                  |
| `skill`                     | `allow` | `allow`                                |
| `path`                      | `allow` | `allow`                                |
| `external_directory`        | `ask`   | `ask`                                  |
| any other tool              | `ask` (the universal fallback) | `ask`             |

Auto's risky commands (`AUTO_RISKY_COMMANDS` in `src/openghost/mode-policy.ts`),
each also matched path-qualified (`/bin/rm`): `rm`, `rmdir`, `shred`, `mkfs*`,
`dd`, `shutdown`, `reboot`, `halt`, `poweroff`, `sudo`, `su`, `doas`, `kill`,
`killall`, `pkill`, `systemctl`, `service`, `chmod`, `chown`, `ufw`, `iptables`,
`nft`, the system package managers (`apt`, `apt-get`, `dnf`, `yum`, `pacman`,
`zypper`), `brew install|uninstall|upgrade|remove`, global `npm`/`pnpm`/`yarn`
installs, `pip --user`, `git push|pull|clean|rebase|restore|filter-branch`,
`git reset --hard` and `git checkout --|-f|.` (also after git's own options, as
in `git -C dir push`), `find … -delete`, inline interpreter code (`python -c`,
`node -e`, `perl -e`, `ruby -e`, `php`, `deno eval`, …), `command`, `builtin`,
`exec` and an escaped `\cmd`, and a bare shell (`sh`, `bash`, `zsh`, `dash`,
`ksh`, `fish`) reading its standard input. The list errs toward asking; it is a
rule list, not a complete risk model. The engine's own floors apply on top
in every mode: an indirection wrapper (`bash -c`, `eval`, `sudo`, `env`,
`xargs`, `find -exec`, …) or an unparseable command asks, and a command reaching
outside the folder asks on `external_directory`.

## Runtime settings

| Key                         | Default | Meaning |
| --------------------------- | ------- | ------- |
| `debugLog`                  | `false` | Verbose diagnostics in `logs/plugin-permissions-debug.jsonl` beside the global config. |
| `permissionReviewLog`       | `true`  | Request and decision audit events in `logs/plugin-permissions-permission-review.jsonl`. |
| `doublePressToConfirm`      | `true`  | The approval card's shortcuts arm on the first press and commit on the second. |
| `permissionDialogKeys`      | `y` `s` `b` `n` `r` | The character each card decision answers to: `approve`, `approveSession`, `approveSessionBoth`, `deny`, `denyWithReason`. `j`/`k` are reserved; a refused binding keeps its default and is reported. |
| `promptMaxRows`, `promptFieldMaxWidth` | `24`, `400` | Bounds of the request facts the card carries. |
| `reviewLogFieldMaxWidth`    | `1000`  | Longest value the review log writes. |
| `forwardingTimeoutMs`       | `600000` | How long a subagent waits for its parent to answer a forwarded ask. |
| `piInfrastructureReadPaths` | `[]`    | Extra directories read as Pi infrastructure (outside the `external_directory` gate). |
| `authorizerChain`           | `[]`    | Names of registered decision links to consult before the card (opt-in). |
| `shellTools`                | none    | Non-`bash` tools that carry shell semantics, gated as `bash`. |
| `permission`                | none    | The rules. |

## Removed from the donor

- `yoloMode`: the access mode is OpenGhost's alone. A file that still sets it is
  rejected as invalid (global: its other settings are ignored and reported;
  project: fail closed).
- `promptNotifications`: terminal bells and OSC notifications belonged to the
  terminal dialog.
- The `/permission-system` command and its settings modal, the status-bar
  indicator, the inline terminal dialog and its `select`/`input` fallback.

## The pure-reader command core

A small, frozen set of command words is read-only for any arguments, in any
implementation. A path token owned by one of them consults the `_read` surface
alone:

<!-- BEGIN PURE_READER_CORE -->

`awk`, `basename`, `cat`, `cd`, `diff`, `dirname`, `echo`, `egrep`, `fd`, `fgrep`, `find`, `grep`, `head`, `ls`, `pwd`, `realpath`, `rg`, `sed`, `sort`, `stat`, `tail`, `wc`, `which`

<!-- END PURE_READER_CORE -->

Admission is structural: implementation-independent read-only-ness across GNU
and BSD alike, no option that redirects output to a file, and effects that do
not depend on argument content.
