/**
 * OpenGhost's access mode, as the plugin enforces it (OpenGhost fork).
 *
 * OpenGhost owns the Ask / Auto / Full selection: its bridge relays the chat's
 * choice on the `openghost:mode` channel as `{mode, seq}`, numbered so an older
 * update never lands last. This module is the plugin's only record of it. There
 * is no config knob, command or other channel that sets a mode, so the plugin
 * cannot hold a mode OpenGhost did not send.
 *
 * Until OpenGhost says otherwise the mode is `ask`, the most restrictive of the
 * three. A malformed update is read as `ask` too: a mode nobody can name is
 * never taken as permission to ask less.
 *
 * The state is process-wide (`Symbol.for` on `globalThis`, like the donor's
 * service registries): one Pi process serves one OpenGhost chat, and an
 * in-process subagent's own plugin instance must enforce the same mode as its
 * parent rather than a default of its own.
 */

export type AccessMode = "ask" | "auto" | "full";

export const ACCESS_MODES: readonly AccessMode[] = ["ask", "auto", "full"];

/** The pi.events channel OpenGhost's bridge says the mode on. */
export const MODE_CHANNEL = "openghost:mode";

export function isAccessMode(value: unknown): value is AccessMode {
  return value === "ask" || value === "auto" || value === "full";
}

export interface ModeSnapshot {
  readonly mode: AccessMode;
  /** The OpenGhost update number applied last; -1 before any numbered one. */
  readonly seq: number;
}

type ModeListener = (snapshot: ModeSnapshot, previous: ModeSnapshot) => void;

export class AccessModeState {
  private snapshot: ModeSnapshot = { mode: "ask", seq: -1 };
  private readonly listeners = new Set<ModeListener>();

  current(): ModeSnapshot {
    return this.snapshot;
  }

  mode(): AccessMode {
    return this.snapshot.mode;
  }

  /** Full: every `ask` is allowed; hard denies still deny. */
  isFullAccess(): boolean {
    return this.snapshot.mode === "full";
  }

  /**
   * Apply one `openghost:mode` update. Returns whether it was applied.
   *
   * An update numbered below the last applied one is stale and ignored. An
   * unnumbered update applies (OpenGhost numbers every one it sends; the
   * number is a guard, not a requirement).
   */
  apply(data: unknown): boolean {
    const record =
      data !== null && typeof data === "object"
        ? (data as Record<string, unknown>)
        : {};
    const seq = Number.isSafeInteger(record.seq)
      ? (record.seq as number)
      : undefined;
    if (seq !== undefined && seq < this.snapshot.seq) return false;
    const mode: AccessMode = isAccessMode(record.mode) ? record.mode : "ask";
    const previous = this.snapshot;
    this.snapshot = { mode, seq: seq ?? previous.seq };
    if (previous.mode !== mode) {
      for (const listener of this.listeners) listener(this.snapshot, previous);
    }
    return true;
  }

  onChange(listener: ModeListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }
}

const MODE_STATE_KEY = Symbol.for("openghost:plugin-permissions:access-mode");

/**
 * The process's mode state, created on first use.
 *
 * Read structurally, not by `instanceof`: each plugin instance (a subagent's
 * included) may load its own copy of this module, and every copy must share
 * the one state.
 */
export function getAccessModeState(): AccessModeState {
  const slot = globalThis as Record<symbol, unknown>;
  const existing = slot[MODE_STATE_KEY] as AccessModeState | undefined;
  if (
    typeof existing?.apply === "function" &&
    typeof existing.current === "function"
  )
    return existing;
  const created = new AccessModeState();
  slot[MODE_STATE_KEY] = created;
  return created;
}
