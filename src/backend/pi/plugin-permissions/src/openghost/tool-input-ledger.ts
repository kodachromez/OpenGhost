/**
 * The raw input of each recent tool call, by tool-call id (OpenGhost fork).
 *
 * The donor's gates hand an ask its facts, not the call's arguments; OpenGhost's
 * approval card shows the command or the changes themselves, so the plugin
 * notes each call's input as the call arrives. Read-only to every decision:
 * nothing here is consulted to allow or deny. Bounded, oldest first out.
 */
export class ToolInputLedger {
  private readonly inputs = new Map<string, Record<string, unknown>>();

  constructor(private readonly capacity = 256) {}

  record(toolCallId: string, input: unknown): void {
    if (!toolCallId) return;
    this.inputs.delete(toolCallId);
    this.inputs.set(
      toolCallId,
      input !== null && typeof input === "object" && !Array.isArray(input)
        ? (input as Record<string, unknown>)
        : {},
    );
    while (this.inputs.size > this.capacity) {
      const oldest = this.inputs.keys().next().value;
      if (oldest === undefined) break;
      this.inputs.delete(oldest);
    }
  }

  get(toolCallId: string): Record<string, unknown> | undefined {
    return this.inputs.get(toolCallId);
  }
}
