// Fails when the committed dist/ is not what the sources build to, so the
// plugin OpenGhost ships is always the reviewed source. Exit 77: skipped (no
// dependencies installed; run `npm ci` first).
import { spawnSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
if (!existsSync(join(root, "node_modules", "esbuild"))) {
  console.log("plugin-permissions: dependencies not installed; skipped.");
  process.exit(77);
}
const scratch = mkdtempSync(join(tmpdir(), "plugin-permissions-bundle-"));
try {
  const built = spawnSync(process.execPath, [join(root, "scripts", "bundle.mjs"), "--out", scratch], {
    stdio: "inherit",
  });
  if (built.status !== 0) process.exit(1);
  let stale = false;
  for (const file of ["plugin-permissions.js", "web-tree-sitter.wasm", "tree-sitter-bash.wasm"]) {
    if (!readFileSync(join(scratch, file)).equals(readFileSync(join(root, "dist", file)))) {
      console.error(`dist/${file} is stale: run \`npm run bundle\` and commit the result.`);
      stale = true;
    }
  }
  process.exit(stale ? 1 : 0);
} finally {
  rmSync(scratch, { recursive: true, force: true });
}
