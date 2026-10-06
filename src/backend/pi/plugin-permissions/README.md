# plugin-permissions

OpenGhost's Pi permission plugin: the sole permission policy and enforcement
layer of an OpenGhost chat. It runs inside Pi, beside OpenGhost's bridge, and
decides every tool call: allow, deny, or ask the user on OpenGhost's approval
card. OpenGhost owns the Ask / Auto / Full selection, the card and its
transport; this plugin owns the decision.

See [docs/plugin-permissions.md](../../../../docs/plugin-permissions.md) for the
design and the OpenGhost boundary, [docs/configuration.md](docs/configuration.md)
for the rule files, and [PROVENANCE.md](PROVENANCE.md) for where it comes from
(`@gotgenes/pi-permission-system` 40.0.0, MIT).

## Layout

| Path | What |
| --- | --- |
| `src/` | The TypeScript sources. `src/openghost/` is OpenGhost's integration; the rest is the donor's engine. |
| `test/` | The vitest suite: the donor's retained tests and `test/openghost/`. |
| `dist/` | **Committed build output** that OpenGhost ships: `plugin-permissions.js` (one ES module) and the two WASM grammars. Compiled into OpenGhost as Qt resources (`:/pi/plugin-permissions/`) and passed to `pi -e` for every chat. |
| `schemas/permissions.schema.json` | The config file's JSON Schema, generated from `src/config/config-schema.ts`. |
| `scripts/` | `bundle.mjs`, `check-bundle.mjs`, `generate-schema.mjs`, `test.mjs`. |

## Working on it

Building OpenGhost needs no Node: it compiles the committed `dist/`. Changing
the plugin does:

```sh
cd src/backend/pi/plugin-permissions
npm ci
npm test               # the vitest suite
npm run check          # tsc --noEmit
npm run bundle         # rebuild dist/ — commit the result
npm run gen:schema     # after changing the config schema
```

CTest runs the suite (`native_plugin_permissions_test`) and checks that `dist/`
is what the sources build to (`native_plugin_permissions_bundle_check`); both
are skipped without `npm ci`. `native_pi_real_test` runs the shipped bundle in
real Pi.
