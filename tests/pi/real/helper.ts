// Test only: another extension's interactions, as a loaded third-party extension
// would make them. `/og-ask` waits on a select dialog, then says what it got;
// `/og-fail` fails.
export default function (pi) {
  pi.registerCommand("og-ask", {
    description: "Asks a question in a dialog",
    handler: async (_args, ctx) => {
      const value = await ctx.ui.select("Pick one", ["a", "b"]);
      ctx.ui.notify(`og-ask got ${value ?? "nothing"}`, "warning");
    },
  });
  pi.registerCommand("og-fail", {
    description: "Fails",
    handler: async () => {
      throw new Error("helper broke");
    },
  });
}
