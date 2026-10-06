// Test only: a stand-in for a Pi permission plugin, so OpenGhost's relay of the
// access mode and its approval cards run in real Pi. OpenGhost ships no such
// plugin and decides nothing itself. This one asks before every `bash` call, in
// every mode, and lets every other tool run. `/og-mode` says the last mode
// OpenGhost relayed. Once the mode turns "full" it allows a waiting call itself
// and takes the request back, as a plugin may.
export default function (pi) {
  let mode = "unset";
  const waiting = new Map(); // approvalId -> {controller, allowed}
  let next = 0;
  pi.events.on("openghost:mode", (data) => {
    mode = data?.mode ?? "unset";
    if (mode === "full")
      for (const entry of waiting.values()) {
        entry.allowed = true;
        entry.controller.abort();
      }
  });
  pi.on("tool_call", async (event, ctx) => {
    if (event.toolName !== "bash") return undefined;
    if (!ctx.hasUI) return { block: true, reason: "Nobody can be asked." };
    const approvalId = `stand-in-${++next}`;
    const entry = { controller: new AbortController(), allowed: false };
    waiting.set(approvalId, entry);
    const signal = ctx.signal ? AbortSignal.any([ctx.signal, entry.controller.signal]) : entry.controller.signal;
    let allowed = false;
    try {
      const request = {
        approvalId,
        toolCallId: event.toolCallId,
        tool: event.toolName,
        args: event.input ?? {},
        presentation: { kind: "command", title: "Stand-in asks", code: String(event.input?.command ?? ""), reveal: "command" },
      };
      allowed = await ctx.ui.confirm("openghost:approval", JSON.stringify(request), { signal });
    } finally {
      waiting.delete(approvalId);
    }
    if (signal.aborted) {
      allowed = entry.allowed;
      ctx.ui.setStatus(`openghost:${approvalId}:approval`, JSON.stringify({ decision: allowed ? "allow" : null }));
    }
    return allowed ? undefined : { block: true, reason: "The stand-in was not allowed." };
  });
  pi.registerCommand("og-mode", {
    description: "Says the access mode OpenGhost relayed",
    handler: async (_args, ctx) => ctx.ui.notify(`og-mode got ${mode}`, "warning"),
  });
}
