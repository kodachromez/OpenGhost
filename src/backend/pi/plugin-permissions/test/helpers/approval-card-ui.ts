/**
 * Drive OpenGhost's approval card from a donor-style `select`/`input` script
 * (OpenGhost fork).
 *
 * The donor's tests played the human through Pi's `select`/`input` dialogs:
 * "Yes", the session label, the width label, "No", "No, provide reason", then a
 * reason input or a scope select. plugin-permissions asks on OpenGhost's card
 * instead (a `confirm` titled `openghost:approval`). This adapter reads the
 * card request, offers the same choices under the same labels to the test's
 * script, then answers the way OpenGhost does: the card choice through the
 * plugin's handle (as the bridge relays it), and the confirm with the answer.
 * So every donor test that exercises a decision keeps its meaning.
 */

type Select = (title: string, options: string[]) => Promise<string | undefined>;
type Input = (title: string, placeholder?: string) => Promise<string | undefined>;

interface CardRequest {
  approvalId: string;
  title?: string;
  facts: string[];
  actions: { id: string; label: string; detail?: string; key: string }[];
  scopes?: { subagent: string; session: string };
}

const HANDLE = Symbol.for("openghost:plugin-permissions");

const DONOR_LABELS: Record<string, string> = {
  approve: "Yes",
  deny: "No",
  denyWithReason: "No, provide reason",
};

/** The donor's option label for a card action. */
function labelOf(action: CardRequest["actions"][number]): string {
  return DONOR_LABELS[action.id] ?? action.detail ?? action.label;
}

export function cardConfirm(
  select: Select,
  input: Input = async () => undefined,
): (title: string, message: string) => Promise<boolean> {
  return async (title, message) => {
    if (title !== "openghost:approval") return false;
    const request = JSON.parse(message) as CardRequest;
    const heading = request.title ?? "Permission Required";
    const labels = request.actions.map(labelOf);
    const picked = await select(
      `${heading}\n${request.facts.join("\n")}`,
      labels,
    );
    const action = request.actions[labels.indexOf(picked ?? "")];
    if (!action) return false;
    const choice: Record<string, string> = { action: action.id };
    if (action.id === "denyWithReason") {
      choice.reason =
        (await input(
          `${heading}\nShare why this request was denied (optional).`,
          "Reason shown back to the agent",
        )) ?? "";
    }
    if (request.scopes && action.id.startsWith("approveSession")) {
      const scope = await select(`${heading}\nApply this session grant to:`, [
        request.scopes.subagent,
        request.scopes.session,
      ]);
      choice.scope = scope === request.scopes.session ? "session" : "subagent";
    }
    const handle = (globalThis as Record<symbol, unknown>)[HANDLE] as
      | { choose(id: string, choice: unknown): boolean }
      | undefined;
    if (action.id !== "approve" && action.id !== "deny")
      handle?.choose(request.approvalId, choice);
    return action.id.startsWith("approve");
  };
}
