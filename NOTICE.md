# Attribution and scope

The native presentation is adapted from Ghosty's `native-ghosty/`, which ports
OpenGhost by Andrew (Copyright © 2026 Andrew) to Qt Quick. The authoritative
visual/behavior reference for this fork is the dissected OpenGhost **1.3.0**
frontend in `reference/openghost/`.

The copied material includes OpenGhost's procedural ghost, original 1.3 splash,
UI motion, layout, icons and native Markdown/highlighting/TeX/diagram rendering
rules. `resources/openghost.png` is the unchanged upstream icon (also present
in `reference/openghost/desktop/icon.png`). Source revisions are recorded in
`docs/native-import.json`. The Tool Calls frontend plugin (`plugins/tool_calls/`)
adapts Ghosty's native tool card (`ToolCard.qml`, `toolcard.*`) and its tool
entrance motion; Ghosty's subagent activity panel is not included.

**Ghosty's custom mascot splash, its assets/font, alternate/ripple animations,
and custom appearance choices are not included.** This fork is not official
OpenGhost. It is a local native extraction, not a redistribution permission.

Retained terms and historical notices:

- [Current OpenGhost terms](LICENSE), also preserved with the reference.
- [Terms accompanying Ghosty's OpenGhost source](licenses/ghosty-openghost-LICENSE).
- [Ghosty's original native attribution](licenses/ghosty-native-NOTICE.md).
- [Ghosty's original OpenGhost attribution](licenses/ghosty-openghost-NOTICE.md).

`src/backend/pi/plugin-permissions/` (plugin-permissions, OpenGhost's Pi
permission plugin) is derived from
[`@gotgenes/pi-permission-system`](https://github.com/gotgenes/pi-packages)
40.0.0 (commit `191011f3585f96796b44a29dd20c70bfac49febe`), itself a fork of
MasuRii/pi-permission-system, under the MIT License, Copyright (c) 2026 MasuRii
and Christopher D. Lasher. Its bundle also contains zod, web-tree-sitter and
tree-sitter-bash (MIT). See [PROVENANCE.md](src/backend/pi/plugin-permissions/PROVENANCE.md),
[the donor's license](licenses/plugin-permissions-LICENSE) and
[the bundled packages' licenses](licenses/plugin-permissions-third-party.md).

Historical notices describe their original directory layout and may mention
components not imported here; they are preserved for provenance. The source-code
MIT grant does **not** cover the excluded name, artwork, animations or visual
design. Do not publish or distribute this modified UI on the assumption that
those materials are MIT-licensed; the upstream terms require separate permission.
