# M09–M11 attachment and pinned-file audit

Audited `71caa0e` as retained in `d5a59c8`, then verified against `07e9819`.
The frozen `reference/openghost/` was read, not changed. No dependencies, new
controls, extraction services or backend features were added by this audit.

## Findings and corrections

| Item | Finding | Correction |
| --- | --- | --- |
| M09 / M11 | `QStringDecoder` was used as a streaming decoder for a complete file. An unfinished UTF-8 sequence at EOF silently disappeared, so an attachment or pinned copy could contain less than the selected file. | Stateless decoding refuses incomplete sequences. Regression cases cover unfinished two-, three- and four-byte sequences, including General's add path. |
| M10 / M11 | UTF-8-valid, NUL-free binary data consisting of control bytes was accepted as text. | Apply the reference reader's 8,192-byte / 1% control-byte sniff in addition to strict UTF-8 and NUL rejection; normal whitespace and ESC retain the reference treatment. |
| M09 / M10 | GIF and BMP were prepared and sent, but `ChatEntry.qml` recognized only PNG/JPEG/WebP for its Preview control. | Include all five retained picture formats. Both production UI smokes assert five picture controls and no fabricated preview error on a text card. |
| M09 / M10 | Original JPEG bytes kept EXIF orientation, but preview decoded without applying it. Portrait photos could display sideways. | Preview uses `QImageReader` with auto-transform, as preparation does. An EXIF-tagged JPEG is selected, sent through the window and scripted Pi, and checked for correctly rotated preview dimensions. |

All four findings were reproduced against the old production code with the new
regressions before applying the corrections. The very-thin-image boundary
(3,000 × 1 and 1 × 3,000 resized to a minimum one-pixel edge) already worked and
is now covered; no resizing change was needed.

## General Files (M11)

The local path is functional, not an always-failing placeholder:

- Both UI smokes deliver external URL drag-enter/drop events to the production
  General Files target, verifying that the composer refusal does not intercept
  them. They also invoke the same `addFiles` handler used by the chooser.
- Re-adding replaces the copy; a failed PDF selection reports its reason and
  leaves the old copy intact. Remove is exercised.
- File-store tests cover persistence, the 20-file and 200,000-character bounds,
  replacing a file at the count limit, normalized equivalent paths, all-or-nothing
  selection, and failed save/removal without publishing changed preferences.
- The scripted-Pi tests check names and complete text in the system context on
  subsequent runs and new chats, removal, and exclusion from conversation history.

There was also a **real Retry retention gap** in the old bridge: a continuation
could lose the pinned/instruction sections because it bypasses
`before_agent_start`. This was corrected separately in **`07e9819`**, using
`context_with_system`; it is not part of this audit's code patch. Running
`tests/pi_context_runtime.mjs` against the old bridge reproduced the failure.
Against `07e9819` it passes ordinary prompt, compaction, Retry/tool-loop, restart
and clearing checks using the installed Pi SDK with a faux provider and temporary
storage. See [the session audit](pi-session-audit.md).

## Verification

A separate checkout of `07e9819` plus only this audit's patch was used to exclude
concurrent, unrelated plugin edits in the shared working tree.

- Release configure/build, `OPENGHOST_BUILD_SMOKE_TEST=ON`, `OPENGHOST_BROWSER=ON`.
- **All 12 CTest tests pass**, including `native_attachments_test`, both Pi suites,
  `native_contract_test`, `native_browser_test`, browser operations/automation,
  `native_ui_smoke` and `native_fake_ui_smoke`.
- A subsequent fake UI smoke run intermittently segfaulted in Qt Quick Shapes'
  software renderer (`QQuickWindow::rendererInterface` on `QSGSoftwareRend`).
  Three immediate repeat runs passed. This rendering instability is not fixed
  by the attachment patch; the passing suite is not a crash-free guarantee.
- `git diff --check` passes.
- The real-Pi offline context test above passes. No live provider request,
  credentials, visible desktop interaction or native OS file-dialog automation.

## Limits remain explicit

Text and decodable pictures are supported; PDF/office/audio/video extraction is
not. Those selections are refused rather than attached as empty metadata. Pinned
files are text only. Clipboard/composer file drops remain unsupported. Sent photo
previews are memory-only and disappear on eviction/restart. A GIF preview shows
its decoded first frame, not an animated player. These checks do not establish
full reference visual parity or live-model acceptance.
