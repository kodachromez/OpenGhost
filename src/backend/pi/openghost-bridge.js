// OpenGhost's bridge into Pi, loaded with `pi --mode rpc -e`. Pi's RPC has no
// provider catalog or sign-in commands, so `/openghost {json}` exposes Pi's own:
// the session's ModelRuntime lists providers and runs Pi's login and logout,
// and Pi's auth storage keeps every credential. Replies are setStatus lines:
//   openghost:<token>         {ok, error?, providers?}  once, when the op ends
//   openghost:<token>:event   a Pi auth event (auth_url, device_code, info, progress)
//   openghost:<token>:prompt  {promptId, type, message, placeholder?, options?}
// A prompt is answered by {op: "answer", promptId, value} (or cancelled).
// {op: "retry"} retries Pi's failed latest reply in place (OpenGhost's Retry).
import { SettingsManager } from "@earendil-works/pi-coding-agent";

const RETRY = "openghost-retry"; // The custom message that starts a retry run.
const logins = new Map(); // provider -> AbortController
const prompts = new Map(); // promptId -> {resolve, reject}
let nextPrompt = 0;

// The latest context message on the active branch, skipping retry triggers.
function latest(sessionManager) {
  const branch = sessionManager.getBranch();
  for (let i = branch.length - 1; i >= 0; i--) {
    const entry = branch[i];
    if (entry.type === "custom_message" && entry.customType === RETRY) continue;
    if (entry.type === "message" || entry.type === "custom_message") return entry;
  }
  return undefined;
}

export default function (pi) {
  // A retry trigger never reaches the model, nor do the failed replies it retried:
  // the retry sees exactly the context the failed reply saw.
  pi.on("context", (event) => {
    const messages = [];
    for (const message of event.messages) {
      if (message.role !== "custom" || message.customType !== RETRY) messages.push(message);
      else while (messages.at(-1)?.role === "assistant" && messages.at(-1).stopReason === "error") messages.pop();
    }
    return { messages };
  });
  pi.registerCommand("openghost", {
    description: "OpenGhost frontend bridge (providers, login, logout, retry)",
    handler: async (args, ctx) => {
      let request;
      try {
        request = JSON.parse(args);
      } catch {
        return;
      }
      const status = (suffix, value) => ctx.ui.setStatus(`openghost:${request.token}${suffix}`, JSON.stringify(value));
      const runtime = ctx.modelRegistry.runtime; // The session's own ModelRuntime.
      try {
        if (request.op === "providers") {
          const stored = new Set(
            (await runtime.listCredentials({ signal: AbortSignal.timeout(15_000) })).map((c) => c.providerId));
          const providers = runtime.getProviders().map((provider) => ({
            id: provider.id,
            name: provider.name || provider.id,
            oauth: provider.auth.oauth ? provider.auth.oauth.name ?? "" : null,
            apiKey: provider.auth.apiKey?.login ? provider.auth.apiKey.name ?? "" : null,
            connected: runtime.getProviderAuthStatus(provider.id).configured === true,
            stored: stored.has(provider.id),
          }));
          status("", { ok: true, providers });
        } else if (request.op === "login" || request.op === "setKey") {
          logins.get(request.provider)?.abort();
          const controller = new AbortController();
          logins.set(request.provider, controller);
          let key = request.op === "setKey" ? request.key : undefined;
          const ask = (prompt) => new Promise((resolve, reject) => {
            const promptId = `p${++nextPrompt}`;
            const abort = () => {
              prompts.delete(promptId);
              reject(new Error("Login cancelled"));
            };
            for (const signal of [controller.signal, prompt.signal]) {
              if (signal?.aborted) return abort();
              signal?.addEventListener("abort", abort, { once: true });
            }
            prompts.set(promptId, { resolve, reject: abort });
            status(":prompt", {
              promptId,
              type: prompt.type,
              message: prompt.message,
              placeholder: prompt.placeholder,
              options: prompt.options?.map(({ id, label }) => ({ id, label })),
            });
          });
          try {
            await runtime.login(request.provider, request.op === "setKey" ? "api_key" : "oauth", {
              signal: controller.signal,
              prompt: async (prompt) => {
                if (key !== undefined && prompt.type === "secret") { // The key typed in Settings.
                  const value = key;
                  key = undefined;
                  return value;
                }
                return ask(prompt);
              },
              notify: (event) => status(":event", event),
            }, { getDeviceId: () => SettingsManager.create(ctx.cwd).getOrCreateDeviceId() }); // As /login.
          } finally {
            if (logins.get(request.provider) === controller) logins.delete(request.provider);
          }
          status("", { ok: true });
        } else if (request.op === "answer") {
          const prompt = prompts.get(request.promptId);
          prompts.delete(request.promptId);
          if (prompt) prompt.resolve(String(request.value ?? ""));
          status("", { ok: !!prompt, error: prompt ? undefined : "That sign-in step has ended." });
        } else if (request.op === "cancel") {
          logins.get(request.provider)?.abort();
          status("", { ok: true });
        } else if (request.op === "retry") {
          // No new or repeated input: Pi continues its own context from where the
          // failed reply left it, so earlier tool effects are not run again.
          if (!ctx.isIdle()) throw new Error("Pi is still busy.");
          const failed = latest(ctx.sessionManager);
          if (failed?.type !== "message" || failed.message.role !== "assistant" || failed.message.stopReason !== "error")
            throw new Error("Pi's latest reply did not fail, so there is nothing to retry.");
          status("", { ok: true });
          pi.sendMessage({ customType: RETRY, content: [], display: false }, { triggerTurn: true });
        } else if (request.op === "logout") {
          await runtime.logout(request.provider, { signal: AbortSignal.timeout(15_000) });
          status("", { ok: true });
        } else {
          status("", { ok: false, error: `Unknown OpenGhost bridge op: ${request.op}` });
        }
      } catch (error) {
        status("", { ok: false, error: error instanceof Error ? error.message : String(error) });
      }
    },
  });
}
