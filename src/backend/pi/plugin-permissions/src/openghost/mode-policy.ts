/**
 * The rules each OpenGhost access mode stands for (OpenGhost fork).
 *
 * A mode is not a second permission engine: it is a rule layer handed to the
 * donor engine, composed after the synthesized defaults and before every config
 * scope (global → project → agent), so the engine evaluates it like any rule —
 * bash parsing, the cross-cutting `path` surface, the `external_directory`
 * boundary, fail-closed sentinels and session approvals all still apply.
 *
 * The meanings are OpenGhost 1.3's:
 *
 * - **Ask** asks before running commands, changing files, or looking outside
 *   the chat's folder. Looking inside the folder is silent.
 * - **Auto** works in the chat's folder on its own; it asks before risky
 *   commands, anything outside the folder, and other tools (an extension's or
 *   an MCP server's).
 * - **Full** never asks: every `ask`, these rules' and the operator's alike,
 *   becomes `allow` through the donor's composition-stage rewrite. A `deny`
 *   (an operator rule, or a fail-closed clamp) still denies. Full's layer is
 *   Auto's; the rewrite does the rest.
 *
 * Operator config sits after this layer, so a rule there refines a mode: an
 * `allow` for `npm test` stops Ask asking about it, a `deny` for `.env` holds in
 * every mode. Universal `"*"` stays the operator's (default `ask`).
 */
import { expandDirectionalSugar, normalizeFlatConfig } from "#src/policy/normalize";
import type { Rule, Ruleset } from "#src/policy/rule";
import type { FlatPermissionConfig } from "#src/types";
import type { AccessMode } from "./access-mode";

/** Pi's tools that only look: silent in the folder in every mode. */
const LOOKING_TOOLS = ["read", "grep", "find", "ls"] as const;

/** Pi's tools that change files. */
const CHANGING_TOOLS = ["write", "edit"] as const;

/**
 * Commands Auto asks before, as 1.3 did: deleting, wiping, privilege,
 * stopping processes or the machine, services, installing system or global
 * packages, permissions and firewalls, and git operations that rewrite or
 * reach a remote. A bare shell asks too (`curl … | sh` runs whatever it reads);
 * the engine's own wrapper floor already asks before `bash -c`, `eval`, `sudo`
 * and other indirection.
 */
export const AUTO_RISKY_COMMANDS: readonly string[] = [
  ...[
    "rm",
    "rmdir",
    "shred",
    "dd",
    "shutdown",
    "reboot",
    "halt",
    "poweroff",
    "sudo",
    "su",
    "doas",
    "kill",
    "killall",
    "pkill",
    "systemctl",
    "service",
    "chmod",
    "chown",
    "ufw",
    "iptables",
    "nft",
    "apt",
    "apt-get",
    "dnf",
    "yum",
    "pacman",
    "zypper",
  ].flatMap(anySpelling),
  "mkfs*",
  "*/mkfs*",
  ...["install", "uninstall", "upgrade", "remove"].map((v) => `brew ${v}*`),
  ...["npm", "pnpm", "yarn"].flatMap((pm) => [
    `${pm} * -g`,
    `${pm} * -g *`,
    `${pm} * --global`,
    `${pm} * --global *`,
  ]),
  "pip * --user*",
  "pip3 * --user*",
  // A git subcommand, with or without git's own options before it
  // (`git -C dir push`, `git --no-pager push`).
  ...[
    "push",
    "pull",
    "clean",
    "rebase",
    "reset --hard",
    "checkout --",
    "checkout -f",
    "checkout .",
    "restore",
    "filter-branch",
  ].flatMap((sub) => [`git ${sub}*`, `git * ${sub}*`]),
  // Deleting through find.
  "find * -delete*",
  // A program given as inline code.
  ...[
    "python",
    "python2",
    "python3",
    "node",
    "deno",
    "bun",
    "perl",
    "ruby",
    "php",
  ].flatMap((interpreter) => [
    `${interpreter} -c*`,
    `${interpreter} -e*`,
    `${interpreter} -p*`,
    `${interpreter} --eval*`,
    `${interpreter} --print*`,
    `${interpreter} * -c *`,
    `${interpreter} * -e *`,
  ]),
  "deno eval*",
  // Indirection the engine's wrapper floor does not name: `command rm`, an
  // escaped `\\rm`, `exec`.
  "command *",
  "builtin *",
  "exec *",
  "\\*",
  // A bare shell reading its standard input (`curl … | sh`).
  ...["sh", "bash", "zsh", "dash", "ksh", "fish"].flatMap((shell) => [
    shell,
    `*/${shell}`,
  ]),
];

/** A command word as typed or path-qualified (`/bin/rm`), with or without arguments. */
function anySpelling(command: string): string[] {
  return [`${command} *`, `*/${command} *`];
}

const tools = (
  names: readonly string[],
  action: "allow" | "ask",
): FlatPermissionConfig =>
  Object.fromEntries(names.map((name) => [name, action]));

/** The rules of Ask, in the donor's flat config form. */
export const ASK_POLICY: FlatPermissionConfig = {
  ...tools(LOOKING_TOOLS, "allow"),
  ...tools(CHANGING_TOOLS, "ask"),
  bash: "ask",
  mcp: "ask",
  skill: "allow",
  path: "allow",
  external_directory: "ask",
};

/** The rules of Auto (and, under its rewrite, Full). */
export const AUTO_POLICY: FlatPermissionConfig = {
  ...tools(LOOKING_TOOLS, "allow"),
  ...tools(CHANGING_TOOLS, "allow"),
  bash: {
    "*": "allow",
    ...Object.fromEntries(AUTO_RISKY_COMMANDS.map((c) => [c, "ask"])),
  },
  mcp: "ask",
  skill: "allow",
  path: "allow",
  external_directory: "ask",
};

export function modePolicy(mode: AccessMode): FlatPermissionConfig {
  return mode === "ask" ? ASK_POLICY : AUTO_POLICY;
}

const compiled = new Map<AccessMode, Ruleset>();

/** The mode's rules, tagged as the mode's (layer `baseline`, origin `mode`). */
export function modeRules(mode: AccessMode): Ruleset {
  const cached = compiled.get(mode);
  if (cached) return cached;
  const rules = normalizeFlatConfig(
    expandDirectionalSugar(modePolicy(mode)),
  ).map((rule): Rule => ({ ...rule, layer: "baseline", origin: "mode" }));
  compiled.set(mode, rules);
  return rules;
}
