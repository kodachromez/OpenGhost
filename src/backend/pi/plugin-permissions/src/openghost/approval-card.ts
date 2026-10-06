/**
 * OpenGhost's approval card as the plugin's human authority (OpenGhost fork).
 *
 * Replaces the donor's terminal dialog (`permission-prompt-component.ts`, the
 * inline `ctx.ui.custom` component and its `select`/`input` fallback). An ask
 * that reaches a human is OpenGhost's existing request contract: a Pi `confirm`
 * titled `openghost:approval` whose message is the request as JSON,
 *
 *   {approvalId, toolCallId, tool, args, presentation, actions,
 *    doublePressToConfirm, scopes?, title, facts}
 *
 * which OpenGhost shows as its approval card. `actions` is the donor's decision
 * roster (allow once, allow for the session, allow both directions for the
 * session, deny, deny with a reason) with each decision's shortcut key, so the
 * card offers exactly the decisions this ask supports; OpenGhost's native UI
 * handles the keys. Nothing here reads a terminal.
 *
 * The confirm's answer is authoritative for allow versus deny. A card choice
 * beyond the plain answer (which session grant, the reason) arrives first,
 * through OpenGhost's bridge ({@link ApprovalCards.choose}), and only refines
 * it: a choice that disagrees with the answer is dropped and the safer of the
 * two holds, so nothing but the user's own confirm can allow a call. The
 * choice is turned into a decision by the donor's decision model
 * (`reducePrompt`), so a card decision means exactly what the same keys meant
 * in the donor's dialog.
 *
 * A request taken back unanswered is said with the status
 * `openghost:<approvalId>:approval` `{decision: "allow" | null}`: `null` when
 * the turn was stopped or the session released it, `"allow"` when OpenGhost
 * switched the chat to Full while it waited (Full allows every ask).
 */
import { randomUUID } from "node:crypto";
import type { ExtensionUIContext } from "@earendil-works/pi-coding-agent";
import type {
  PermissionPromptDecision,
  RequestPermissionOptions,
  UnattributedDecision,
} from "#src/authority/permission-dialog";
import { createDeniedPermissionDecision } from "#src/authority/permission-dialog";
import {
  initialPromptState,
  type PromptModelConfig,
  type PromptOutcome,
  reducePrompt,
  visibleActions,
} from "#src/authority/permission-prompt-decision";
import type { DialogKeyBindings, PromptAction } from "#src/config/dialog-keys";
import {
  type RenderBudget,
  renderPromptDialog,
} from "#src/presentation/dialog-renderer";
import type { PromptPayload } from "#src/presentation/prompt-payload";
import { type ApprovalPresentation, presentAsk } from "./presentation";
import type { ToolInputLedger } from "./tool-input-ledger";

/** The confirm title OpenGhost shows as an approval card. */
export const APPROVAL_TITLE = "openghost:approval";

/** The status that says a request was taken back unanswered. */
export function approvalStatusKey(approvalId: string): string {
  return `openghost:${approvalId}:approval`;
}

/** What a request said when it was taken back unanswered. */
export const WITHDRAWN_REASON =
  "The permission request was withdrawn before anyone answered it.";

/** Live prompt preferences, read when each card opens. */
export interface PromptPreferences {
  /** A shortcut arms its decision first and commits on a second press. */
  doublePressToConfirm: boolean;
  /** How much of the request's facts the card's `facts` lines carry. */
  budget: RenderBudget;
  /** The character bound to each decision. */
  dialogKeys: DialogKeyBindings;
}

/** The slice of Pi's UI the card needs: the confirm, and the status line. */
export type ApprovalCardUi = Pick<ExtensionUIContext, "confirm" | "setStatus">;

export interface ApprovalCardView extends PromptPreferences {
  ui: ApprovalCardUi;
  /** The running turn's abort signal: Stop takes the card back. */
  signal?: AbortSignal;
}

/** The tool call an ask is about, when it is about one. */
export interface ApprovalCardSubject {
  toolCallId?: string;
  toolName?: string;
}

/** One decision the card offers. */
export interface CardAction {
  id: PromptAction;
  /** Short button wording. */
  label: string;
  /** The donor's full wording for what a session grant covers. */
  detail?: string;
  /** The shortcut that chooses it (`permissionDialogKeys`). */
  key: string;
}

/** What the user chose on the card, beyond the confirm's yes or no. */
export interface ApprovalCardChoice {
  action: PromptAction;
  /** `denyWithReason`: the reason the agent is told. */
  reason?: string;
  /** A forwarded session grant: the subagent alone, or the whole session. */
  scope?: "subagent" | "session";
}

const ACTION_LABELS: Record<PromptAction, string> = {
  approve: "Allow",
  approveSession: "Allow for session",
  approveSessionBoth: "Allow both for session",
  deny: "Deny",
  denyWithReason: "Deny with reason",
};

const DEFAULT_SESSION_LABEL = "Yes, for this session";

const APPROVING: ReadonlySet<PromptAction> = new Set([
  "approve",
  "approveSession",
  "approveSessionBoth",
]);

interface OpenCard {
  readonly controller: AbortController;
  readonly config: PromptModelConfig;
  choice?: ApprovalCardChoice;
  /** Set when Full access released the card: it is allowed, not withdrawn. */
  allowedByFullAccess?: boolean;
}

export interface ApprovalCardsDeps {
  /** The raw input of each recent tool call, for the card's presentation. */
  toolInputs: ToolInputLedger;
  /** The session's working directory, for the card's places. */
  cwd: () => string | undefined;
  /**
   * Whether OpenGhost's mode is Full now. An ask raised under Ask or Auto that
   * reaches its card after the switch (it waited in the dialog queue) is then
   * allowed as Full allows it, without a card. Defaults to never.
   */
  isFullAccess?: () => boolean;
}

/**
 * The cards this plugin instance has open, and the one function that opens
 * one ({@link ApprovalCards.request}, injected where the donor injected its
 * terminal dialog).
 */
export class ApprovalCards {
  private readonly open = new Map<string, OpenCard>();

  constructor(private readonly deps: ApprovalCardsDeps) {}

  /** Whether `approvalId` is a card this instance has open. */
  has(approvalId: string): boolean {
    return this.open.has(approvalId);
  }

  /**
   * Record the card choice OpenGhost relays before answering the confirm.
   * Returns whether the card is open here and the choice is one it offers.
   */
  choose(approvalId: string, choice: unknown): boolean {
    const card = this.open.get(approvalId);
    const parsed = parseChoice(choice);
    if (!card || !parsed) return false;
    if (!visibleActions(card.config).includes(parsed.action)) return false;
    card.choice = parsed;
    return true;
  }

  /** OpenGhost switched to Full: every open card is allowed and taken back. */
  releaseForFullAccess(): void {
    for (const card of this.open.values()) {
      card.allowedByFullAccess = true;
      card.controller.abort();
    }
  }

  /** The session is ending: every open card is taken back unanswered. */
  withdrawAll(): void {
    for (const card of this.open.values()) card.controller.abort();
  }

  /** Ask OpenGhost's user, on an approval card. */
  readonly request = async (
    view: ApprovalCardView,
    title: string,
    payload: PromptPayload,
    options?: RequestPermissionOptions,
    subject: ApprovalCardSubject = {},
  ): Promise<PermissionPromptDecision> => {
    const config: PromptModelConfig = {
      doublePressToConfirm: view.doublePressToConfirm,
      keys: view.dialogKeys,
      sessionLabel: options?.sessionLabel ?? DEFAULT_SESSION_LABEL,
      widthLabel: options?.sessionWidth?.label,
      sessionScope: options?.sessionScope,
    };
    if (this.deps.isFullAccess?.() === true) {
      return {
        approved: true,
        state: "approved",
        decidedBy: { kind: "full_access", pattern: payload.request.matchedPattern },
      };
    }
    const approvalId = `pp-${randomUUID()}`;
    const card: OpenCard = { controller: new AbortController(), config };
    const signal = view.signal
      ? AbortSignal.any([view.signal, card.controller.signal])
      : card.controller.signal;
    const toolName = subject.toolName ?? payload.request.toolName ?? "";
    const args = capStrings(
      subject.toolCallId
        ? (this.deps.toolInputs.get(subject.toolCallId) ?? {})
        : {},
    ) as Record<string, unknown>;
    const message = {
      approvalId,
      toolCallId: subject.toolCallId ?? "",
      tool: toolName,
      args,
      presentation: capStrings(
        presentAsk(payload, title, toolName, args, this.deps.cwd()),
      ) as ApprovalPresentation,
      actions: cardActions(config),
      doublePressToConfirm: config.doublePressToConfirm,
      ...(config.sessionScope
        ? {
            scopes: {
              subagent: config.sessionScope.subagentLabel,
              session: config.sessionScope.servingSessionLabel,
            },
          }
        : {}),
      title,
      facts: renderPromptDialog(payload, {
        ...view.budget,
        width: Number.MAX_SAFE_INTEGER,
      }).lines,
    } satisfies { presentation: ApprovalPresentation } & Record<
      string,
      unknown
    >;

    this.open.set(approvalId, card);
    let confirmed = false;
    try {
      if (!signal.aborted) {
        confirmed = await view.ui.confirm(
          APPROVAL_TITLE,
          JSON.stringify(message),
          { signal },
        );
      }
    } finally {
      this.open.delete(approvalId);
    }

    if (signal.aborted) {
      const allowed = card.allowedByFullAccess === true;
      view.ui.setStatus(
        approvalStatusKey(approvalId),
        JSON.stringify({ decision: allowed ? "allow" : null }),
      );
      return allowed
        ? {
            approved: true,
            state: "approved",
            decidedBy: {
              kind: "full_access",
              pattern: payload.request.matchedPattern,
            },
          }
        : {
            approved: false,
            state: "denied",
            confirmationUnavailable: true,
            denialReason: WITHDRAWN_REASON,
            decidedBy: { kind: "unavailable", reason: WITHDRAWN_REASON },
          };
    }
    return {
      ...decideFromCard(config, confirmed, card.choice),
      decidedBy: { kind: "user", via: "approval_card" },
    };
  };
}

/** The decisions a card with `config` offers, in the donor's order. */
export function cardActions(config: PromptModelConfig): CardAction[] {
  return visibleActions(config).map((id) => {
    const detail =
      id === "approveSession"
        ? config.sessionLabel
        : id === "approveSessionBoth"
          ? config.widthLabel
          : undefined;
    return {
      id,
      label: ACTION_LABELS[id],
      ...(detail ? { detail } : {}),
      key: config.keys[id],
    };
  });
}

/**
 * The decision the user's answer and card choice make together.
 *
 * `confirmed` (Pi's confirm) decides allow versus deny. The choice refines it
 * only when it agrees: an approving choice on a denied confirm, or a denying
 * choice on an allowed one, is dropped in favor of a plain deny.
 */
export function decideFromCard(
  config: PromptModelConfig,
  confirmed: boolean,
  choice: ApprovalCardChoice | undefined,
): UnattributedDecision {
  const approving = choice ? APPROVING.has(choice.action) : confirmed;
  if (!confirmed || !approving) {
    if (!confirmed && choice?.action === "denyWithReason") {
      const decided = runModel(config, choice);
      if (decided) return decided;
    }
    return createDeniedPermissionDecision();
  }
  return (
    runModel(config, choice ?? { action: "approve" }) ?? {
      approved: true,
      state: "approved",
    }
  );
}

/**
 * Drive the donor's decision model the way its dialog's keys did: the
 * shortcut (twice when double-press applies), then the reason or scope step
 * the decision opens. `undefined` when the model does not commit (an empty
 * reason), so the caller falls back to the plain answer.
 */
function runModel(
  config: PromptModelConfig,
  choice: ApprovalCardChoice,
): UnattributedDecision | undefined {
  const { action } = choice;
  let outcome: PromptOutcome = reducePrompt(
    config,
    initialPromptState(config),
    { type: "hotkey", action },
  );
  if (outcome.kind === "render" && outcome.state.armedAction === action) {
    outcome = reducePrompt(config, outcome.state, { type: "hotkey", action });
  }
  if (outcome.kind === "render" && outcome.state.step === "reason") {
    outcome = reducePrompt(config, outcome.state, {
      type: "submitReason",
      draft: choice.reason ?? "",
    });
  }
  if (outcome.kind === "render" && outcome.state.step === "scope") {
    let { state } = outcome;
    if (choice.scope === "session") {
      const moved = reducePrompt(config, state, {
        type: "nav",
        direction: "down",
      });
      if (moved.kind === "render") state = moved.state;
    }
    outcome = reducePrompt(config, state, { type: "confirm" });
  }
  return outcome.kind === "decision" ? outcome.decision : undefined;
}

/**
 * The longest text any one field of a card request carries. A card shows a
 * few hundred lines at most, and the request crosses Pi's RPC (JSON in JSON),
 * so a huge write must not make it a record too large to deliver. Display
 * only: the decision is made on the call as it is, never on this copy.
 */
export const CARD_TEXT_LIMIT = 64 * 1024;

function capStrings(value: unknown, depth = 0): unknown {
  if (typeof value === "string")
    return value.length > CARD_TEXT_LIMIT
      ? `${value.slice(0, CARD_TEXT_LIMIT)}\n… (${value.length - CARD_TEXT_LIMIT} more characters)`
      : value;
  if (depth > 8 || value === null || typeof value !== "object") return value;
  if (Array.isArray(value)) return value.slice(0, 256).map((v) => capStrings(v, depth + 1));
  return Object.fromEntries(
    Object.entries(value).map(([key, v]) => [key, capStrings(v, depth + 1)]),
  );
}

const PROMPT_ACTIONS: ReadonlySet<string> = new Set([
  "approve",
  "approveSession",
  "approveSessionBoth",
  "deny",
  "denyWithReason",
]);

function parseChoice(value: unknown): ApprovalCardChoice | undefined {
  if (value === null || typeof value !== "object") return undefined;
  const record = value as Record<string, unknown>;
  if (typeof record.action !== "string" || !PROMPT_ACTIONS.has(record.action))
    return undefined;
  const choice: ApprovalCardChoice = { action: record.action as PromptAction };
  if (typeof record.reason === "string") choice.reason = record.reason;
  if (record.scope === "subagent" || record.scope === "session")
    choice.scope = record.scope;
  return choice;
}
