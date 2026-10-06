/**
 * OpenGhost's approval card as the plugin's human authority (OpenGhost fork).
 *
 * The first block ports the donor's `requestPermissionDecisionFromUi` tests
 * (test/authority/permission-dialog.test.ts upstream): the same decisions, the
 * same widths and scopes, now made on the card. The rest covers the card's
 * request, its answer, and how it is taken back.
 */
import { describe, expect, it, vi } from "vitest";
import type { PromptModelConfig } from "#src/authority/permission-prompt-decision";
import { DEFAULT_DIALOG_KEYS } from "#src/config/dialog-keys";
import {
  APPROVAL_TITLE,
  ApprovalCards,
  CARD_TEXT_LIMIT,
  approvalStatusKey,
  cardActions,
  decideFromCard,
  WITHDRAWN_REASON,
} from "#src/openghost/approval-card";
import { ToolInputLedger } from "#src/openghost/tool-input-ledger";
import { makePromptPayload } from "#test/helpers/prompt-details-fixtures";
import { makePromptPreferences } from "#test/helpers/prompt-view-fixtures";

function config(overrides: Partial<PromptModelConfig> = {}): PromptModelConfig {
  return {
    doublePressToConfirm: true,
    keys: DEFAULT_DIALOG_KEYS,
    sessionLabel: "Yes, for this session",
    ...overrides,
  };
}

describe("card decisions (ported from requestPermissionDecisionFromUi)", () => {
  it("returns approved for Allow", () => {
    expect(decideFromCard(config(), true, undefined)).toEqual({
      approved: true,
      state: "approved",
    });
    expect(decideFromCard(config(), true, { action: "approve" })).toEqual({
      approved: true,
      state: "approved",
    });
  });

  it("returns approved_for_session for the session option", () => {
    expect(decideFromCard(config(), true, { action: "approveSession" })).toEqual({
      approved: true,
      state: "approved_for_session",
    });
  });

  it("returns denied for Deny, and for a dismissed card", () => {
    expect(decideFromCard(config(), false, { action: "deny" })).toEqual({
      approved: false,
      state: "denied",
    });
    expect(decideFromCard(config(), false, undefined)).toEqual({
      approved: false,
      state: "denied",
    });
  });

  it("returns denied_with_reason when a reason is given", () => {
    expect(
      decideFromCard(config(), false, { action: "denyWithReason", reason: " not now " }),
    ).toEqual({ approved: false, state: "denied_with_reason", denialReason: "not now" });
  });

  it("returns denied when deny-with-reason has an empty reason", () => {
    expect(
      decideFromCard(config(), false, { action: "denyWithReason", reason: "  " }),
    ).toEqual({ approved: false, state: "denied" });
  });

  it("offers four decisions, the session one under its label", () => {
    const actions = cardActions(config({ sessionLabel: 'Yes, allow "git *" for this session' }));
    expect(actions.map((a) => a.id)).toEqual([
      "approve",
      "approveSession",
      "deny",
      "denyWithReason",
    ]);
    expect(actions[1]?.detail).toBe('Yes, allow "git *" for this session');
    expect(actions.map((a) => a.key)).toEqual(["y", "s", "n", "r"]);
  });

  describe("both-directions session grant (#813)", () => {
    const widthLabel = 'Yes, allow reads and writes to "/tmp/*" for this session';

    it("offers the width option after the session option", () => {
      expect(cardActions(config({ widthLabel })).map((a) => a.id)).toEqual([
        "approve",
        "approveSession",
        "approveSessionBoth",
        "deny",
        "denyWithReason",
      ]);
    });

    it("returns the family width when the width option is chosen", () => {
      expect(
        decideFromCard(config({ widthLabel }), true, { action: "approveSessionBoth" }),
      ).toEqual({
        approved: true,
        state: "approved_for_session",
        sessionGrantWidth: "family",
      });
    });

    it("leaves the plain session option at the proven width", () => {
      expect(
        decideFromCard(config({ widthLabel }), true, { action: "approveSession" }),
      ).toEqual({ approved: true, state: "approved_for_session" });
    });

    it("continues into the scope from the width option", () => {
      expect(
        decideFromCard(
          config({
            widthLabel,
            sessionScope: {
              subagentLabel: "This subagent only",
              servingSessionLabel: "The whole session",
            },
          }),
          true,
          { action: "approveSessionBoth", scope: "session" },
        ),
      ).toEqual({
        approved: true,
        state: "approved_for_serving_session",
        sessionGrantWidth: "family",
      });
    });
  });

  describe("sessionScope (forwarded asks)", () => {
    const sessionScope = {
      subagentLabel: "This subagent only",
      servingSessionLabel: "The whole session",
    };

    it("maps the subagent scope to approved_for_session", () => {
      expect(
        decideFromCard(config({ sessionScope }), true, {
          action: "approveSession",
          scope: "subagent",
        }),
      ).toEqual({ approved: true, state: "approved_for_session" });
    });

    it("maps the whole-session scope to approved_for_serving_session", () => {
      expect(
        decideFromCard(config({ sessionScope }), true, {
          action: "approveSession",
          scope: "session",
        }),
      ).toEqual({ approved: true, state: "approved_for_serving_session" });
    });

    it("defaults to the least-privilege subagent scope when none is given", () => {
      expect(
        decideFromCard(config({ sessionScope }), true, { action: "approveSession" }),
      ).toEqual({ approved: true, state: "approved_for_session" });
    });

    it("ignores the scope for plain Allow", () => {
      expect(
        decideFromCard(config({ sessionScope }), true, { action: "approve", scope: "session" }),
      ).toEqual({ approved: true, state: "approved" });
    });
  });

  describe("without double-press", () => {
    it("decides the same", () => {
      const single = config({ doublePressToConfirm: false });
      expect(decideFromCard(single, true, { action: "approveSession" })).toEqual({
        approved: true,
        state: "approved_for_session",
      });
      expect(
        decideFromCard(single, false, { action: "denyWithReason", reason: "no" }),
      ).toEqual({ approved: false, state: "denied_with_reason", denialReason: "no" });
    });
  });
});

describe("the confirm's answer is authoritative", () => {
  it("never allows on an approving choice the confirm denied", () => {
    expect(decideFromCard(config(), false, { action: "approveSession" })).toEqual({
      approved: false,
      state: "denied",
    });
  });

  it("denies when the confirm allowed but the choice was a deny", () => {
    expect(decideFromCard(config(), true, { action: "deny" })).toEqual({
      approved: false,
      state: "denied",
    });
    expect(
      decideFromCard(config(), true, { action: "denyWithReason", reason: "x" }),
    ).toEqual({ approved: false, state: "denied" });
  });
});

/** A card UI whose confirm the test resolves, recording each request. */
function cardUi() {
  const requests: { title: string; message: Record<string, unknown>; signal?: AbortSignal }[] = [];
  const answers: ((value: boolean) => void)[] = [];
  return {
    requests,
    answer(index: number, value: boolean): void {
      answers[index]?.(value);
    },
    ui: {
      confirm: vi.fn(
        (title: string, message: string, options?: { signal?: AbortSignal }) =>
          new Promise<boolean>((resolve) => {
            requests.push({ title, message: JSON.parse(message), signal: options?.signal });
            answers.push(resolve);
            // As Pi does: an aborted dialog resolves with the default.
            options?.signal?.addEventListener("abort", () => resolve(false), { once: true });
          }),
      ),
      setStatus: vi.fn(),
    },
  };
}

function makeCards() {
  const toolInputs = new ToolInputLedger();
  toolInputs.record("call-1", { command: "touch a.txt" });
  return { cards: new ApprovalCards({ toolInputs, cwd: () => "/w" }), toolInputs };
}

const flush = (): Promise<void> => new Promise((resolve) => setTimeout(resolve, 0));

describe("ApprovalCards", () => {
  it("asks on OpenGhost's approval card, with the call and its decisions", async () => {
    const { cards } = makeCards();
    const { ui, requests, answer } = cardUi();
    const decision = cards.request(
      { ui, ...makePromptPreferences() },
      "Permission Required",
      makePromptPayload(),
      { sessionLabel: 'Yes, allow bash "touch *" for this session' },
      { toolCallId: "call-1", toolName: "bash" },
    );
    await flush();
    expect(requests[0]?.title).toBe(APPROVAL_TITLE);
    const request = requests[0]?.message ?? {};
    expect(request.approvalId).toMatch(/^pp-/);
    expect(request.toolCallId).toBe("call-1");
    expect(request.tool).toBe("bash");
    expect(request.args).toEqual({ command: "touch a.txt" });
    expect(request.presentation).toMatchObject({ kind: "command", code: "touch a.txt" });
    expect(request.doublePressToConfirm).toBe(true);
    expect((request.actions as { id: string }[]).map((a) => a.id)).toContain("approveSession");
    expect(Array.isArray(request.facts)).toBe(true);
    answer(0, true);
    expect(await decision).toEqual({
      approved: true,
      state: "approved",
      decidedBy: { kind: "user", via: "approval_card" },
    });
  });

  it("applies a choice relayed for its open card, and only then", async () => {
    const { cards } = makeCards();
    const { ui, requests, answer } = cardUi();
    const decision = cards.request(
      { ui, ...makePromptPreferences() },
      "Permission Required",
      makePromptPayload(),
      undefined,
      { toolCallId: "call-1" },
    );
    await flush();
    const id = requests[0]?.message.approvalId as string;
    expect(cards.choose("pp-unknown", { action: "approveSession" })).toBe(false);
    expect(cards.choose(id, { action: "approveSessionBoth" })).toBe(false); // not offered
    expect(cards.choose(id, { action: "bogus" })).toBe(false);
    expect(cards.choose(id, "approveSession")).toBe(false);
    expect(cards.choose(id, { action: "approveSession" })).toBe(true);
    answer(0, true);
    expect((await decision).state).toBe("approved_for_session");
    expect(cards.choose(id, { action: "approve" })).toBe(false); // answered: gone
  });

  it("takes the card back unanswered when the turn stops", async () => {
    const { cards } = makeCards();
    const { ui, requests } = cardUi();
    const controller = new AbortController();
    const decision = cards.request(
      { ui, signal: controller.signal, ...makePromptPreferences() },
      "Permission Required",
      makePromptPayload(),
    );
    await flush();
    const id = requests[0]?.message.approvalId as string;
    controller.abort();
    expect(await decision).toEqual({
      approved: false,
      state: "denied",
      confirmationUnavailable: true,
      denialReason: WITHDRAWN_REASON,
      decidedBy: { kind: "unavailable", reason: WITHDRAWN_REASON },
    });
    expect(ui.setStatus).toHaveBeenCalledWith(
      approvalStatusKey(id),
      JSON.stringify({ decision: null }),
    );
  });

  it("never opens a card for a turn already stopped", async () => {
    const { cards } = makeCards();
    const { ui } = cardUi();
    const controller = new AbortController();
    controller.abort();
    const decision = await cards.request(
      { ui, signal: controller.signal, ...makePromptPreferences() },
      "Permission Required",
      makePromptPayload(),
    );
    expect(ui.confirm).not.toHaveBeenCalled();
    expect(decision.approved).toBe(false);
  });

  it("allows and takes back every open card when Full arrives", async () => {
    const { cards } = makeCards();
    const { ui, requests } = cardUi();
    const decision = cards.request(
      { ui, ...makePromptPreferences() },
      "Permission Required",
      makePromptPayload(),
    );
    await flush();
    const id = requests[0]?.message.approvalId as string;
    cards.releaseForFullAccess();
    expect(await decision).toMatchObject({
      approved: true,
      state: "approved",
      decidedBy: { kind: "full_access" },
    });
    expect(ui.setStatus).toHaveBeenCalledWith(
      approvalStatusKey(id),
      JSON.stringify({ decision: "allow" }),
    );
  });

  it("withdraws every open card when the session ends", async () => {
    const { cards } = makeCards();
    const { ui } = cardUi();
    const decision = cards.request(
      { ui, ...makePromptPreferences() },
      "Permission Required",
      makePromptPayload(),
    );
    await flush();
    cards.withdrawAll();
    expect((await decision).confirmationUnavailable).toBe(true);
  });

  it("needs no terminal: only the confirm and the status line are used", async () => {
    const { cards } = makeCards();
    const { ui, answer } = cardUi();
    const strict = { confirm: ui.confirm, setStatus: ui.setStatus };
    const decision = cards.request(
      { ui: strict, ...makePromptPreferences() },
      "Permission Required",
      makePromptPayload(),
    );
    await flush();
    answer(0, false);
    expect((await decision).state).toBe("denied");
  });

  it("opens no card once Full holds: the ask is allowed as Full allows it", async () => {
    const toolInputs = new ToolInputLedger();
    const cards = new ApprovalCards({ toolInputs, cwd: () => undefined, isFullAccess: () => true });
    const { ui } = cardUi();
    const decision = await cards.request(
      { ui, ...makePromptPreferences() },
      "Permission Required",
      makePromptPayload(),
    );
    expect(ui.confirm).not.toHaveBeenCalled();
    expect(decision).toMatchObject({ approved: true, decidedBy: { kind: "full_access" } });
  });

  it("caps what a huge call puts on the card, never the call itself", async () => {
    const toolInputs = new ToolInputLedger();
    const content = "x".repeat(CARD_TEXT_LIMIT * 4);
    toolInputs.record("big", { path: "a.txt", content });
    const cards = new ApprovalCards({ toolInputs, cwd: () => "/w" });
    const { ui, requests, answer } = cardUi();
    const decision = cards.request(
      { ui, ...makePromptPreferences() },
      "Permission Required",
      makePromptPayload(),
      undefined,
      { toolCallId: "big", toolName: "write" },
    );
    await flush();
    const request = requests[0]?.message ?? {};
    const shownArgs = request.args as { content: string };
    const shown = request.presentation as { added: string };
    expect(shownArgs.content.length).toBeLessThan(CARD_TEXT_LIMIT + 100);
    expect(shown.added.length).toBeLessThan(CARD_TEXT_LIMIT + 100);
    expect(toolInputs.get("big")?.content).toBe(content);
    answer(0, false);
    expect((await decision).approved).toBe(false);
  });
});
