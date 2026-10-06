import type { SessionEntryView } from "#src/session/active-agent";

/**
 * Narrow context the forwarding subsystem reads: the UI gate (`hasUI`) and
 * the three session-manager readers `getSessionId`
 * and the `active-agent` helpers use.
 *
 * A full `ExtensionContext` satisfies this structurally, so production
 * callers pass `ctx` unchanged.
 */
export interface ForwarderContext {
  hasUI: boolean;
  /** The session's working directory, stamped onto a forwarded request as the requester cwd. */
  cwd: string;
  sessionManager: {
    getSessionId(): string;
    getSessionDir(): string;
    getEntries(): readonly SessionEntryView[];
  };
}

/** Reads the current session cwd off `ctx`. */
export function getCwd(ctx: ForwarderContext): string {
  return ctx.cwd;
}

/** Reads the current session id off `ctx`, falling back to `"unknown"`. */
export function getSessionId(ctx: ForwarderContext): string {
  try {
    const sessionId = ctx.sessionManager.getSessionId();
    if (typeof sessionId === "string" && sessionId.trim()) {
      return sessionId.trim();
    }
  } catch {}

  return "unknown";
}
