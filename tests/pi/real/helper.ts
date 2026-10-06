// Test only: another extension's interactions, as a loaded third-party extension
// would make them. `/og-ask` waits on a select dialog, then says what it got;
// `/og-fail` fails. `/og-dialogs` waits on each of select, confirm, input and
// editor in turn and says what it got; `/og-model` says the og-faux model's name
// in Pi's registry and in this session. `/og-mode` says the access mode
// plugin-permissions itself enforces (its own handle), or "none" without it.
// A reply calling a tool with `og-exit` in its input makes this Pi exit soon after,
// as a crashed Pi would, while the call still waits on its approval.
export default function (pi) {
  pi.on("message_end", (event) => {
    if (event.message?.role === "assistant" && JSON.stringify(event.message.content ?? "").includes("og-exit"))
      setTimeout(() => process.exit(1), 1500);
  });
  pi.registerCommand("og-ask", {
    description: "Asks a question in a dialog",
    handler: async (_args, ctx) => {
      const value = await ctx.ui.select("Pick one", ["a", "b"]);
      ctx.ui.notify(`og-ask got ${value ?? "nothing"}`, "warning");
    },
  });
  pi.registerCommand("og-dialogs", {
    description: "Asks every kind of blocking dialog",
    handler: async (_args, ctx) => {
      const got = [
        await ctx.ui.select("Pick a letter", ["a", "b"]),
        await ctx.ui.confirm("Really?", "Sure?"),
        await ctx.ui.input("Your name", "name"),
        await ctx.ui.editor("Edit this", "text"),
      ];
      ctx.ui.notify(`og-dialogs got ${JSON.stringify(got)}`, "warning");
    },
  });
  pi.registerCommand("og-model", {
    description: "Says the og-faux model's name",
    handler: async (_args, ctx) => {
      const registry = ctx.modelRegistry.runtime.getModel("og-faux", "faux")?.name;
      ctx.ui.notify(`og-model registry=${registry} session=${ctx.model?.name}`, "warning");
    },
  });
  pi.registerCommand("og-mode", {
    description: "Says the access mode plugin-permissions enforces",
    handler: async (_args, ctx) => {
      const plugin = globalThis[Symbol.for("openghost:plugin-permissions")];
      const held = plugin?.mode?.();
      ctx.ui.notify(`og-mode got ${held ? `${held.mode} ${held.seq}` : "none"}`, "warning");
    },
  });
  pi.registerCommand("og-fail", {
    description: "Fails",
    handler: async () => {
      throw new Error("helper broke");
    },
  });
}
