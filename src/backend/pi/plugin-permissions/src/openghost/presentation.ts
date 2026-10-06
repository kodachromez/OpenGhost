/**
 * What OpenGhost's approval card shows for an ask (OpenGhost fork).
 *
 * The card's `presentation` field: a headline in plain words, what the step
 * does and where, and the command or the changes behind a reveal. The wording
 * is OpenGhost 1.3's (`agent-tools.js` describe, i18n.js). This is a render
 * over an ask the gates already raised; it decides nothing, and a field it
 * cannot fill is left out (the card falls back to the tool and arguments).
 */
import { basename, isAbsolute, relative, resolve } from "node:path";
import type { PromptPayload } from "#src/presentation/prompt-payload";

export interface ApprovalPlace {
  kind: "folder" | "file" | "site";
  label: string;
  title: string;
}

/** The card's presentation (OpenGhost's `ApprovalPresentation`). */
export interface ApprovalPresentation {
  kind: "command" | "file" | "web";
  title: string;
  effect?: "read" | "change" | "run";
  badge?: boolean;
  places?: ApprovalPlace[];
  code?: string;
  removed?: string;
  added?: string;
  quote?: string;
  reveal?: "command" | "changes" | "content";
}

/** OpenGhost 1.3's English wording. */
const TEXT = {
  command: "Run a command",
  commandOutside: "Run a command that reaches outside the project",
  write: "Write a file",
  writeOutside: "Write a file outside the project",
  edit: "Edit a file",
  editOutside: "Edit a file outside the project",
  read: "Read a file",
  readOutside: "Read a file outside the project",
  list: "Look into a folder",
  listOutside: "Look into a folder outside the project",
};

const LOOKING = new Set(["ls", "grep", "find"]);

export function presentAsk(
  payload: PromptPayload,
  title: string,
  toolName: string,
  args: Record<string, unknown>,
  cwd: string | undefined,
): ApprovalPresentation {
  const request = payload.request;
  const outside =
    payload.kind === "external_directory" ||
    payload.kind === "bash_external_directory";
  const subagent = request.requester.forwarded
    ? ` (${request.requester.agentName ?? "subagent"})`
    : "";
  const path = typeof args.path === "string" ? args.path : request.value;
  const place = (): ApprovalPlace => ({
    kind: LOOKING.has(toolName) ? "folder" : "file",
    label: basename(path) || path,
    title: shown(path, cwd),
  });

  if (
    payload.kind === "bash" ||
    payload.kind === "bash_external_directory" ||
    toolName === "bash"
  ) {
    const command =
      typeof args.command === "string" ? args.command : request.value;
    return {
      kind: "command",
      title: (outside ? TEXT.commandOutside : TEXT.command) + subagent,
      effect: "run",
      code: command,
      reveal: "command",
    };
  }
  switch (toolName) {
    case "write":
      return {
        kind: "file",
        title: (outside ? TEXT.writeOutside : TEXT.write) + subagent,
        effect: "change",
        places: [place()],
        ...(typeof args.content === "string" ? { added: args.content } : {}),
        reveal: "content",
      };
    case "edit": {
      const edits = Array.isArray(args.edits) ? args.edits : [];
      const side = (key: "oldText" | "newText") =>
        edits
          .map((edit: unknown) =>
            edit && typeof edit === "object"
              ? String((edit as Record<string, unknown>)[key] ?? "")
              : "",
          )
          .join("\n");
      return {
        kind: "file",
        title: (outside ? TEXT.editOutside : TEXT.edit) + subagent,
        effect: "change",
        places: [place()],
        removed: side("oldText"),
        added: side("newText"),
        reveal: "changes",
      };
    }
    case "read":
      return {
        kind: "file",
        title: (outside ? TEXT.readOutside : TEXT.read) + subagent,
        effect: "read",
        places: [place()],
      };
    case "ls":
    case "grep":
    case "find":
      return {
        kind: "file",
        title: (outside ? TEXT.listOutside : TEXT.list) + subagent,
        effect: "read",
        places: [place()],
      };
  }
  let code = request.value;
  try {
    if (Object.keys(args).length > 0) code = JSON.stringify(args, null, 2);
  } catch {
    // Unserializable input: the request's own value stands in.
  }
  return {
    kind: "command",
    title: (toolName || title) + subagent,
    effect: "run",
    code,
    reveal: "command",
  };
}

/** A path as the card names it: relative to the folder when inside it. */
function shown(path: string, cwd: string | undefined): string {
  if (!cwd) return path;
  const full = isAbsolute(path) ? path : resolve(cwd, path);
  const rel = relative(cwd, full);
  return rel && !rel.startsWith("..") && !isAbsolute(rel) ? rel : full;
}
