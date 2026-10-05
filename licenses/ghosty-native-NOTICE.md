# Attribution and scope

Native Ghosty ports OpenGhost's presentation (by Andrew, Copyright (c) 2026
Andrew; OpenGhost 1.1.0, tag `v1.1.0`, commit
`18a5a4a4a655bce2268455f9af90afe67406b507`) to Qt Quick: the ghost artwork
and its motion, the splash, the palette and layout, the line icons, and the
Markdown, highlighting, TeX and diagram rendering rules.
`resources/ghosty.png` is the upstream icon, unchanged. This is an
independently adapted Ghosty client, **not official OpenGhost**.

The Ghosty startup splash (`qml/GhostySplash.qml`, `src/mascot.cpp`,
`shaders/splash_mist.frag`, `shaders/splash_aura.frag`) ports the owner's
OpenGhost `no-backend` splash (commit `2a108d5`):
`resources/splash/ghosty-down.png` is its `assets/splash-poses/down.png`,
unchanged. `resources/splash/Nunito-Bold.ttf` is its Nunito 700 latin
subset converted from WOFF2 to TrueType, under the SIL Open Font License in
`resources/splash/Nunito-OFL.txt`.

The upstream terms in [`../openghost/LICENSE`](../openghost/LICENSE) apply:
the name, ghost artwork (icon, glyph and splash ghost), animations and visual
design are excluded from the upstream MIT grant and have its non-commercial
restrictions; keep this notice and do not present the client as official
OpenGhost. Provenance of the Electron port is in
[`../openghost/NOTICE.md`](../openghost/NOTICE.md). If `openghost/` is
retired, its `LICENSE` moves here first (see
[docs/native-ghosty.md §9](../docs/native-ghosty.md#9-electron-retirement-plan)).
