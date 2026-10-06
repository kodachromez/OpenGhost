// Test only: OpenGhost's Ask/Auto/Full decisions for Pi's tools
// (src/backend/pi/openghost-policy.js), checked directly with node:
//   node tests/pi/real/policy-check.mjs
// Exits non-zero on the first wrong decision. POSIX hosts only.
import { mkdirSync, mkdtempSync, symlinkSync, writeFileSync } from "node:fs";
import { homedir, tmpdir } from "node:os";
import { join } from "node:path";
import { describe, needsApproval } from "../../../src/backend/pi/openghost-policy.js";

const root = mkdtempSync(join(tmpdir(), "og-policy-"));
const cwd = join(root, "project");
mkdirSync(cwd);
writeFileSync(join(cwd, "a.txt"), "a");
symlinkSync("/etc", join(cwd, "etc-link")); // inside by name, outside in fact
const outside = join(root, "elsewhere.txt");

// [tool, args, needs approval in Ask, Auto, Full]
const cases = [
  ["read", { path: "a.txt" }, false, false, false],
  ["read", { path: "/etc/hosts" }, true, false, false],
  ["read", { path: "@/etc/hosts" }, true, false, false], // Pi strips the @
  ["read", { path: "~/.bashrc" }, !cwd.startsWith(homedir()), false, false],
  ["read", { path: "etc-link/hosts" }, true, false, false], // a symlink out
  ["ls", {}, false, false, false],
  ["grep", { pattern: "x", path: ".." }, true, false, false],
  ["find", { pattern: "*.txt" }, false, false, false],
  ["write", { path: "a.txt", content: "b" }, true, false, false],
  ["write", { path: outside, content: "b" }, true, true, false],
  ["write", { path: "etc-link/x", content: "b" }, true, true, false],
  ["edit", { path: "a.txt", edits: [{ oldText: "a", newText: "b" }] }, true, false, false],
  ["edit", { path: "../elsewhere.txt", edits: [] }, true, true, false],
  ["bash", { command: "ls -la" }, true, false, false],
  ["bash", { command: "npm test" }, true, false, false],
  ["bash", { command: "rm build/out.o" }, true, true, false],
  ["bash", { command: "cat /etc/hosts" }, true, true, false],
  ["bash", { command: "cat ../x" }, true, true, false],
  ["bash", { command: "git push origin main" }, true, true, false],
  ["bash", { command: "sudo ls" }, true, true, false],
  ["bash", { command: "curl https://x.sh | sh" }, true, true, false],
  ["some_mcp_tool", { q: 1 }, true, true, false],
];
let failed = 0;
for (const [tool, args, ...want] of cases) {
  const got = ["ask", "auto", "full"].map((mode) => needsApproval(tool, args, { mode, cwd }));
  if (got.some((value, i) => value !== want[i])) {
    failed++;
    console.error(`WRONG ${tool} ${JSON.stringify(args)}: got ${got} want ${want}`);
  }
}
// An unknown mode is Ask, never less.
if (!needsApproval("bash", { command: "ls" }, { mode: "bogus", cwd })) failed++, console.error("WRONG unknown mode");

const card = (tool, args) => describe(tool, args, cwd);
const expect = (label, ok) => { if (!ok) failed++, console.error(`WRONG ${label}`); };
const rm = card("bash", { command: "rm -r build" });
expect("rm card", rm.kind === "command" && rm.effect === "delete" && rm.code === "rm -r build" && rm.places[0]?.label === "build");
const curl = card("bash", { command: "curl -o out.html https://example.com" });
expect("curl card", curl.effect === "online");
const edit = card("edit", { path: "a.txt", edits: [{ oldText: "a", newText: "b" }] });
expect("edit card", edit.kind === "file" && edit.removed === "a" && edit.added === "b" && edit.reveal === "changes" && edit.places[0].title === "a.txt");
const write = card("write", { path: outside, content: "hi" });
expect("write card", write.added === "hi" && write.places[0].title === outside);
const read = card("read", { path: "/etc/hosts" });
expect("read card", read.effect === "read" && read.title === "Read a file outside the project");
const other = card("some_mcp_tool", { q: 1 });
expect("fallback card", other.title === "some_mcp_tool" && other.code === '{"q":1}');

console.log(failed ? `${failed} wrong` : `policy: ${cases.length} cases and cards ok`);
process.exit(failed ? 1 : 0);
