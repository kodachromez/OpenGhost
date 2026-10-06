import {
  buildDirectionalSessionLabels,
  buildForwardedScopeLabels,
  buildPathAccessSessionLabel,
  describeGrantTarget,
} from "#src/presentation/pattern-suggest";
import {
  emitUiPromptEvent,
  type PermissionEventBus,
} from "#src/service/permission-events";
import { buildUiPrompt } from "#src/service/permission-ui-prompt";
import { provenDirectionOf } from "#src/session/approval-grant";
import type { AskDialogAdmission } from "./ask-dialog-queue";
import type { TerminalAuthorizer } from "./authorizer";
import type {
  ApprovalCardUi,
  ApprovalCards,
  PromptPreferences,
} from "#src/openghost/approval-card";
import type {
  PermissionPromptDecision,
  RequestPermissionOptions,
} from "./permission-dialog";
import type { PromptPermissionDetails } from "./permission-prompter";

/** Dependencies required by {@link LocalUserAuthorizer}. */
export interface LocalUserAuthorizerDeps {
  /** The active session's UI surface: the confirm OpenGhost shows as its card. */
  ui: ApprovalCardUi;
  /** The running turn's abort signal, read when each card opens. */
  signal: () => AbortSignal | undefined;
  /** Event bus used for the `permissions:ui_prompt` broadcast. */
  events: PermissionEventBus;
  /** Serializes this session's dialogs so no ask replaces another (#965). */
  dialogs: AskDialogAdmission;
  /** Read live at prompt time so a settings-modal toggle takes effect on the next prompt. */
  getPromptPreferences: () => PromptPreferences;
  /** OpenGhost's approval card (OpenGhost fork; the donor's terminal dialog). */
  requestPermissionDecision: ApprovalCards["request"];
}

/**
 * Authorizer for a session with an active UI: prompt the human here.
 *
 * Emits the `permissions:ui_prompt` broadcast (moved here from
 * `PermissionPrompter`'s `ctx.hasUI` arm) before showing the dialog, so
 * observers know a decision is imminent. This is the single emit site: a
 * forwarded ask carries its provenance on `details.forwarding`, which this
 * class renders (populated `forwarding` context + "(Subagent)" title) so the
 * broadcast stays non-degraded (#292) without a second emission path.
 *
 * Every ask goes through the session's `AskDialogAdmission`, because the host
 * holds one inline dialog slot: a second presentation mounts over the first and
 * strands its promise (#965).
 */
export class LocalUserAuthorizer implements TerminalAuthorizer {
  constructor(private readonly deps: LocalUserAuthorizerDeps) {}

  authorize(
    details: PromptPermissionDetails,
  ): Promise<PermissionPromptDecision> {
    return this.deps.dialogs.run(
      () => this.present(details),
      unansweredDecision,
    );
  }

  /**
   * Announce the imminent prompt, then show it.
   *
   * Both live inside the queued region: `permissions:ui_prompt` is documented
   * as firing immediately before the user-facing UI is invoked, so an emit at
   * admission would alert a notification consumer for a dialog that is still
   * minutes of deliberation away.
   */
  private present(
    details: PromptPermissionDetails,
  ): Promise<PermissionPromptDecision> {
    emitUiPromptEvent(this.deps.events, buildUiPrompt(details));
    const title = details.forwarding
      ? "Permission Required (Subagent)"
      : "Permission Required";
    return this.deps.requestPermissionDecision(
      {
        ui: this.deps.ui,
        signal: this.deps.signal(),
        ...this.deps.getPromptPreferences(),
      },
      title,
      details.payload,
      buildRequestOptions(details),
      { toolCallId: details.toolCallId, toolName: details.toolName },
    );
  }
}

/**
 * The answer an ask gets when the session released it before a human ruled.
 *
 * Mirrors `ParentAuthorizer`'s abandonment: `confirmationUnavailable` keeps it
 * out of the "User denied" family, since a user who was never asked denied
 * nothing (#719), and the agent-facing reason and the provenance record reuse
 * one string so what the model is told and what the log attributes cannot
 * drift (#726).
 */
function unansweredDecision(reason: string): PermissionPromptDecision {
  return {
    approved: false,
    state: "denied",
    confirmationUnavailable: true,
    denialReason: reason,
    decidedBy: { kind: "unavailable", reason },
  };
}

/**
 * The dialog options this ask offers, composed from three independent groups.
 *
 * The label names what the session grant covers: the proven direction and
 * target for a directional path ask, else a gate-supplied label, else the
 * target alone for a path ask that proves no direction. An ask whose grants
 * all prove the same direction additionally offers the both-directions width
 * (#813). A forwarded ask additionally offers the scope choice (subagent vs
 * whole session).
 *
 * They compose rather than exclude: a forwarded path ask offers all three, and
 * an ask that qualifies for none passes `undefined` so the dialog keeps its
 * defaults.
 */
function buildRequestOptions(
  details: PromptPermissionDetails,
): RequestPermissionOptions | undefined {
  const grants = details.sessionApproval?.grants ?? [];
  const direction = provenDirectionOf(grants);
  const widths = direction
    ? buildDirectionalSessionLabels(direction, describeGrantTarget(grants))
    : null;
  const sessionLabel =
    widths?.sessionLabel ??
    details.sessionLabel ??
    buildPathAccessSessionLabel(grants) ??
    undefined;

  const options: RequestPermissionOptions = {
    ...(sessionLabel ? { sessionLabel } : {}),
    ...(widths ? { sessionWidth: { label: widths.widenedLabel } } : {}),
    ...(details.forwarding && grants.length > 0
      ? {
          sessionScope: buildForwardedScopeLabels(
            details.forwarding.requesterAgentName,
            grants,
          ),
        }
      : {}),
  };
  return Object.keys(options).length > 0 ? options : undefined;
}
