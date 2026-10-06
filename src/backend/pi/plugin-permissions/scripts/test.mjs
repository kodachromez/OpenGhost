// Runs the plugin's vitest suite for CTest. Exit 77: skipped (no dependencies
// installed; run `npm ci` in src/backend/pi/plugin-permissions first).
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const vitest = join(root, "node_modules", "vitest", "vitest.mjs");
if (!existsSync(vitest)) {
  console.log("plugin-permissions: dependencies not installed; skipped.");
  process.exit(77);
}
const run = spawnSync(process.execPath, [vitest, "run"], { cwd: root, stdio: "inherit" });
process.exit(run.status ?? 1);
