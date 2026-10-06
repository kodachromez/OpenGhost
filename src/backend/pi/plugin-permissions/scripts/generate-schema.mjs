// Regenerates schemas/permissions.schema.json from the zod source of truth
// (src/config/config-schema.ts), as the donor's gen:schema did. Never edit the
// JSON by hand: test/config/config-schema.test.ts fails if it drifts.
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { build } from "esbuild";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const scratch = mkdtempSync(join(tmpdir(), "plugin-permissions-schema-"));
try {
  const module = join(scratch, "config-schema.mjs");
  await build({
    entryPoints: [join(root, "src", "config", "config-schema.ts")],
    outfile: module,
    bundle: true,
    platform: "node",
    format: "esm",
    logLevel: "warning",
  });
  const { buildPermissionsJsonSchema } = await import(pathToFileURL(module).href);
  const output = join(root, "schemas", "permissions.schema.json");
  writeFileSync(output, `${JSON.stringify(buildPermissionsJsonSchema(), null, 2)}\n`);
  console.log(`Wrote ${output}`);
} finally {
  rmSync(scratch, { recursive: true, force: true });
}
