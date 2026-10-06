/**
 * How plugin-permissions meets OpenGhost's bridge (OpenGhost fork).
 *
 * Two channels, both OpenGhost's existing ones:
 *
 * - The access mode arrives on pi.events `openghost:mode` (`{mode, seq}`),
 *   applied to the process's {@link AccessModeState}. A switch to Full allows
 *   and takes back every card this instance has open.
 * - The bridge reaches the plugin through a process-global handle
 *   (`Symbol.for("openghost:plugin-permissions")`): to relay a card choice
 *   ahead of its confirm answer, and to tell OpenGhost an enforcer is loaded
 *   and which mode it holds. The handle counts as an enforcer only while a
 *   connected instance (connected once its gate is registered) remains; with
 *   none, OpenGhost says permission enforcement is unavailable rather than
 *   enforcing anything itself.
 *
 * In-process subagents load their own plugin instance; each registers its
 * cards here, so a choice reaches whichever instance opened the card.
 */
import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { type AccessModeState, MODE_CHANNEL, type ModeSnapshot } from "./access-mode";
import type { ApprovalCards } from "./approval-card";

export const PLUGIN_NAME = "plugin-permissions";
export const PLUGIN_VERSION = "1.0.0";

const HANDLE_KEY = Symbol.for("openghost:plugin-permissions");

/** What the bridge sees of the plugin. */
export interface PluginPermissionsHandle {
  readonly name: string;
  readonly version: string;
  /** The mode the plugin enforces now. */
  mode(): ModeSnapshot;
  /** Relay a card choice; whether an open card took it. */
  choose(approvalId: string, choice: unknown): boolean;
  /** Each live instance's open cards. */
  readonly cards: Set<ApprovalCards>;
  /** Whether any instance is connected, its gate in place. */
  enforcing(): boolean;
}

function handle(mode: AccessModeState): PluginPermissionsHandle {
  const slot = globalThis as Record<symbol, unknown>;
  const existing = slot[HANDLE_KEY] as PluginPermissionsHandle | undefined;
  if (existing?.cards instanceof Set && typeof existing.choose === "function")
    return existing;
  const cards = new Set<ApprovalCards>();
  const created: PluginPermissionsHandle = {
    name: PLUGIN_NAME,
    version: PLUGIN_VERSION,
    mode: () => mode.current(),
    choose: (approvalId, choice) => {
      for (const instance of cards)
        if (instance.has(approvalId)) return instance.choose(approvalId, choice);
      return false;
    },
    cards,
    enforcing: () => cards.size > 0,
  };
  slot[HANDLE_KEY] = created;
  return created;
}

/** Connect one plugin instance; the returned function disconnects it. */
export function connectOpenGhost(
  pi: Pick<ExtensionAPI, "events">,
  mode: AccessModeState,
  cards: ApprovalCards,
): () => void {
  const shared = handle(mode);
  shared.cards.add(cards);
  const unsubscribeMode = pi.events.on(MODE_CHANNEL, (data) => {
    mode.apply(data);
  });
  const unsubscribeChange = mode.onChange((next) => {
    if (next.mode === "full") cards.releaseForFullAccess();
  });
  return () => {
    unsubscribeMode();
    unsubscribeChange();
    shared.cards.delete(cards);
  };
}
