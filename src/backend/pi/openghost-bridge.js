// OpenGhost's bridge into Pi, loaded with `pi --mode rpc -e`. Pi's RPC has no
// provider catalog or sign-in commands, so `/openghost {json}` exposes Pi's own:
// the session's ModelRuntime lists providers and runs Pi's login and logout,
// and Pi's auth storage keeps every credential. Replies are setStatus lines:
//   openghost:<token>         {ok, error?, providers?}  once, when the op ends
//   openghost:<token>:event   a Pi auth event (auth_url, device_code, info, progress)
//   openghost:<token>:prompt  {promptId, type, message, placeholder?, options?}
//   openghost:<token>:withdrawn {promptId}  Pi took a prompt back unanswered
// A prompt is answered by {op: "answer", promptId, value}. Each sign-in is its own
// flow, named by its token: {op: "cancel", provider, flow} cancels that flow only,
// and a new sign-in for the provider ends the one before. {op: "refresh"} rereads
// Pi's models.json and stored credentials (as `providers` also does first), so
// changes made outside OpenGhost (another `pi /login`, an edited models.json) are
// seen, and the session's selected model is retaken from the reread registry.
// {op: "mode", mode, seq} sets Ask / Auto / Full (src/backend/pi/openghost-policy.js);
// an update numbered lower than the last applied is ignored.
// A tool call that needs approval waits on Pi's own extension UI: a confirm whose
// title is "openghost:approval" and whose message is the request as JSON
// {approvalId, toolCallId, tool, args, presentation}; Allow runs it, anything else
// blocks it. A request Pi took back unanswered (Stop, or a mode that no longer
// asks) is said by  openghost:<approvalId>:approval {decision: "allow"|null}.
// {op: "retry"} retries Pi's failed latest reply in place (OpenGhost's Retry).
// {op: "context", instructions, files} sets what every run's system prompt
// carries from Settings → General: the standing instructions and the pinned text
// files ({name, text}, in Pi's own `<file name>` form), each its own named
// section: system prompt, never history, so compaction keeps them. {op: "mark", entry} records an OpenGhost turn's start or
// end in Pi's session (a custom entry: kept in the session file, never context).
import { SettingsManager } from "@earendil-works/pi-coding-agent";
import { describe, needsApproval } from "./openghost-policy.js";

const RETRY = "openghost-retry"; // The custom message that starts a retry run.
const MARK = "openghost-turn"; // OpenGhost's turn records in Pi's session.
const INSTRUCTIONS = "openghost-instructions"; // The system prompt sections.
const FILES = "openghost-files";
let instructions = "";
let files = "";
const escape = (name) => name.replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
const APPROVAL = "openghost:approval"; // The confirm OpenGhost shows as an approval card.
const MODES = new Set(["ask", "auto", "full"]);
let mode = "ask"; // Until OpenGhost says otherwise: never less than asked for.
let modeSeq = 0; // The latest mode update applied.
const approvals = new Map(); // approvalId -> {name, input, cwd, controller, allowed}
let nextApproval = 0;
const logins = new Map(); // provider -> {token, controller}
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
  // Every tool call, the model's or another tool's, passes here before it runs.
  pi.on("tool_call", async (event, ctx) => {
    const name = event.toolName, input = event.input, cwd = ctx.cwd;
    if (!needsApproval(name, input, { mode, cwd })) return undefined;
    if (!ctx.hasUI) return { block: true, reason: "This step needs the user's approval, and nobody can be asked." };
    const approvalId = `og-approval-${++nextApproval}`;
    const controller = new AbortController();
    const entry = { name, input, cwd, controller, allowed: false };
    approvals.set(approvalId, entry);
    const signal = ctx.signal ? AbortSignal.any([ctx.signal, controller.signal]) : controller.signal;
    let allowed = false;
    try {
      const request = { approvalId, toolCallId: event.toolCallId, tool: name, args: input ?? {}, presentation: describe(name, input, cwd) };
      allowed = await ctx.ui.confirm(APPROVAL, JSON.stringify(request), { signal });
    } catch {
      allowed = false; // Blocked: a failure never runs the step.
    } finally {
      approvals.delete(approvalId);
    }
    if (signal.aborted) {
      allowed = entry.allowed; // Taken back: allowed only by a mode that no longer asks.
      ctx.ui.setStatus(`openghost:${approvalId}:approval`, JSON.stringify({ decision: allowed ? "allow" : null }));
    }
    if (allowed) return undefined;
    return { block: true, reason: ctx.signal?.aborted ? "Stopped before this step ran." : "The user did not allow this step." };
  });
  pi.on("before_agent_start", (event) => {
    const sections = event.systemPromptOptions.sections;
    if (instructions) sections[INSTRUCTIONS] = instructions;
    else delete sections[INSTRUCTIONS];
    if (files) sections[FILES] = files;
    else delete sections[FILES];
  });
  // Continuations (including Retry and its tool loop) do not run
  // before_agent_start, and Pi rebuilds their prompt from its base options.
  // Enforce the same named sections at the request boundary as well. This is a
  // system delta, never a synthetic user message; compaction's checkpoint and
  // the rest of Pi's prompt/tool declarations remain intact.
  pi.on("context_with_system", (event) => {
    let current = {};
    for (const message of event.messages) {
      if (message.role !== "system") continue;
      if (message.replace) current = {};
      Object.assign(current, message.sections);
    }
    const sections = { [INSTRUCTIONS]: instructions || null, [FILES]: files || null };
    if (Object.entries(sections).every(([key, value]) => (current[key] || null) === value)) return;
    return { messages: [...event.messages, { role: "system", content: "", sections, timestamp: Date.now() }] };
  });
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
    description: "OpenGhost frontend bridge (providers, login, logout, refresh, access mode, retry, context, turn records)",
    handler: async (args, ctx) => {
      let request;
      try {
        request = JSON.parse(args);
      } catch {
        // Still answered, so nothing waits on it: its token, if one can be read.
        const token = /"token"\s*:\s*"([^"]+)"/.exec(args)?.[1];
        if (token) ctx.ui.setStatus(`openghost:${token}`, JSON.stringify({ ok: false, error: "OpenGhost sent a malformed bridge request." }));
        return;
      }
      const status = (suffix, value) => ctx.ui.setStatus(`openghost:${request.token}${suffix}`, JSON.stringify(value));
      const runtime = ctx.modelRegistry.runtime; // The session's own ModelRuntime.
      try {
        if (request.op === "providers") {
          await runtime.refresh({ allowNetwork: false, signal: AbortSignal.timeout(15_000) });
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
          logins.get(request.provider)?.controller.abort(); // One sign-in per provider.
          const controller = new AbortController();
          const flow = { token: request.token, controller };
          logins.set(request.provider, flow);
          let key = request.op === "setKey" ? request.key : undefined;
          const ask = (prompt) => new Promise((resolve, reject) => {
            const promptId = `p${++nextPrompt}`;
            const abort = () => {
              if (prompts.delete(promptId)) status(":withdrawn", { promptId });
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
            if (logins.get(request.provider) === flow) logins.delete(request.provider);
          }
          status("", { ok: true });
        } else if (request.op === "answer") {
          const prompt = prompts.get(request.promptId);
          prompts.delete(request.promptId);
          if (prompt) prompt.resolve(String(request.value ?? ""));
          status("", { ok: !!prompt, error: prompt ? undefined : "That sign-in step has ended." });
        } else if (request.op === "cancel") {
          const flow = logins.get(request.provider);
          if (flow && (!request.flow || flow.token === request.flow)) flow.controller.abort();
          status("", { ok: true });
        } else if (request.op === "refresh") {
          await runtime.refresh({ allowNetwork: false, signal: AbortSignal.timeout(15_000) });
          // The session keeps the model object it selected before the reread: it
          // takes the refreshed one, so its next request uses the new configuration.
          // Same model, same thinking level; only an idle session is switched.
          const current = ctx.model;
          const fresh = current && runtime.getModel(current.provider, current.id);
          if (fresh && fresh !== current && ctx.isIdle()) {
            const thinking = pi.getThinkingLevel();
            if (await pi.setModel(fresh)) pi.setThinkingLevel(thinking);
          }
          status("", { ok: true });
        } else if (request.op === "mode") {
          if (!MODES.has(request.mode)) throw new Error(`Unknown access mode: ${request.mode}`);
          // Updates are numbered by OpenGhost: one older than the last applied is
          // ignored, however Pi happens to order the two requests.
          const seq = Number.isSafeInteger(request.seq) ? request.seq : undefined;
          if (seq !== undefined && seq < modeSeq) {
            status("", { ok: true, mode, stale: true });
            return;
          }
          if (seq !== undefined) modeSeq = seq;
          mode = request.mode;
          // Waiting approvals the new mode no longer asks for are allowed; the rest wait.
          for (const entry of approvals.values())
            if (!needsApproval(entry.name, entry.input, { mode, cwd: entry.cwd })) {
              entry.allowed = true;
              entry.controller.abort();
            }
          status("", { ok: true, mode });
        } else if (request.op === "retry") {
          // No new or repeated input: Pi continues its own context from where the
          // failed reply left it, so earlier tool effects are not run again.
          if (!ctx.isIdle()) throw new Error("Pi is still busy.");
          const failed = latest(ctx.sessionManager);
          if (failed?.type !== "message" || failed.message.role !== "assistant" || failed.message.stopReason !== "error")
            throw new Error("Pi's latest reply did not fail, so there is nothing to retry.");
          // Validate on Pi's active branch too: an extension can navigate or
          // append context without going through the frontend's turn journal.
          const branch = ctx.sessionManager.getBranch();
          const at = branch.findIndex((entry) => entry.id === failed.id);
          const owner = branch.slice(0, at).findLast((entry) =>
            entry.type === "custom" && entry.customType === MARK && entry.data?.event === "start");
          if (!request.failedTurnId || owner?.data?.turn !== request.failedTurnId)
            throw new Error("Pi's failed reply no longer belongs to the requested turn.");
          if (branch.slice(at + 1).some((entry) =>
            ["compaction", "branch_summary", "context_edit"].includes(entry.type)))
            throw new Error("Pi's context changed after that reply failed; an exact Retry is unavailable.");
          // The context_with_system hook also covers this continuation. The
          // frontend supplied this retry's standing instructions before admission.
          status("", { ok: true });
          pi.sendMessage({ customType: RETRY, content: [], display: false }, { triggerTurn: true });
        } else if (request.op === "context") {
          instructions = String(request.instructions ?? "");
          const pinned = Array.isArray(request.files) ? request.files : [];
          files = pinned.length === 0 ? "" : [
            "Files the user keeps at hand in every chat:",
            ...pinned.map((f) => `<file name="${escape(String(f.name))}">\n${String(f.text ?? "")}\n</file>`),
          ].join("\n\n");
          status("", { ok: true });
        } else if (request.op === "mark") {
          pi.appendEntry(MARK, request.entry);
          status("", { ok: true });
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
