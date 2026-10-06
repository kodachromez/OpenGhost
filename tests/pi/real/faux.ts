// Test only: a scripted model inside real Pi (Pi AI's own faux provider), so the
// OpenGhost bridge runs in real Pi with no network or credentials. The prompt
// picks the reply: `call <tool> <json args>` asks for that tool call, then says
// "done" once the tool's result (or refusal) is back; anything else is echoed.
import { createFauxCore, fauxAssistantMessage, fauxText, fauxToolCall } from "@earendil-works/pi-ai";

export default function (pi) {
  const core = createFauxCore({ api: "og-faux", provider: "og-faux", models: [{ id: "faux", name: "Faux" }], tokensPerSecond: 100000 });
  const reply = (context) => {
    const messages = context.messages;
    const last = messages.at(-1);
    if (last?.role === "toolResult") {
      const text = (last.content ?? []).map((c) => c.text ?? "").join("");
      return fauxAssistantMessage(`done: ${last.isError ? "error" : "ok"}: ${text.slice(0, 200)}`);
    }
    const user = [...messages].reverse().find((m) => m.role === "user");
    const text = typeof user?.content === "string" ? user.content : (user?.content ?? []).map((c) => c.text ?? "").join("");
    const call = /^call (\w+) (.*)$/s.exec(text.trim());
    if (call) return fauxAssistantMessage([fauxText("calling"), fauxToolCall(call[1], JSON.parse(call[2]))], { stopReason: "toolUse" });
    return fauxAssistantMessage(`echo: ${text}`);
  };
  core.setResponses(Array.from({ length: 1000 }, () => reply));
  pi.registerProvider("og-faux", {
    api: "og-faux",
    apiKey: "faux",
    baseUrl: "http://127.0.0.1:9",
    streamSimple: core.streamSimple,
    models: [{ id: "faux", name: "Faux", reasoning: false, input: ["text"], cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 }, contextWindow: 100000, maxTokens: 1000 }],
  });
}
