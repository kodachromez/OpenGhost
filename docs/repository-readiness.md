# Standalone repository readiness

Original audit: the native frontend on `cpp-native-extraction`, following the local
state implementation in `185ed65`; documentation recorded in `4542756`. The focused
usage-ledger follow-up below fixes one finding without adding backend integration
or UI features. The frozen `reference/openghost/` and all licensing/provenance
records are preserved.

## Before moving or sharing the repository

1. **Resolve the intended use under the retained terms.** [LICENSE](../LICENSE)
   excludes the OpenGhost name, artwork, animations and visual design from the
   MIT code grant. This modified port retains them. Public distribution cannot
   be justified by the MIT portion alone; obtain the required permission or
   separately address the excluded materials. Private hosting does not grant
   additional rights or waive commercial-use restrictions. Do not delete the
   historical notices to make the project look like a new unencumbered work.
2. **Transfer a repository, not a worktree pointer.** The audited checkout is a
   Git worktree (`.git` is a pointer file), with remotes for the earlier frontend,
   snapshot and upstream repositories. Clone/export the intended branch into an
   independent repository rather than copying that pointer. Review destination
   visibility and remotes before any push; do not reuse the old `origin` blindly.
   No new remote or push is performed by this pass.
3. **Keep a deliberate history/content policy.** Retain `NOTICE.md`, `LICENSE`,
   `licenses/`, `docs/native-import.json` and the unchanged reference with the
   source. Decide whether to preserve history or import a source snapshot; if
   preserving history, audit that history separately. This source/documentation
   review is **not a credential scan of every Git object** or a legal clearance.
   Do not import build trees, user profiles, caches, credentials or screenshots
   containing private conversations. Existing `build*` directories are ignored.
4. **Carry the limits into the new README/issues.** No connected backend, no
   production password encryption, unexposed mini/lock/folder UI, two GCC warnings
   and unqualified Windows/macOS are intentional current status, not completed
   features. The Rust harness, Pi and other backends are future adapter targets,
   not dependencies or working integrations.

## Resolved: usage-ledger automatic save

The audit reproduced skipped automatic saves in
[`UsageStore`](../src/frontend/usage.cpp): `flush()` used `isActive()` as its
pending-write flag, but Qt stops a single-shot timer **before** emitting timeout.
The timeout and later destructor therefore returned without writing.

The fix tracks dirty state independently and clears it only on successful
publication. A failed write returns false from `flush()` and emits `saveFailed`
(the same signal pattern as preferences). In-memory counts remain pending for an
explicit flush, another usage-triggered save or destructor flush. No automatic
retry loop or new error UI is added. Load errors still refuse recording; v2 JSON
and v1 read/upgrade behavior are unchanged. A crash or persistent write failure
can still lose unsaved counts.

`usageLedgerTimerPersists` exercises two real timer-driven publications through a
temporary `FileKeyStore`, without explicit flush/destruction, and failed against
the old implementation. `usageLedgerSaveFailure` injects failed timeout writes,
checks the failure signal/false flush result and unchanged stored data, and tests
explicit, next-record and destructor recovery without losing counts. The existing
round-trip test also checks v1-to-v2 publication and unknown-version refusal.

## Findings needing follow-up

The remaining findings below are not addressed by the usage-ledger fix.

### Local save and deletion failures are not uniformly reported

Required pre-dispatch checkpoints correctly refuse starts/retries/steering on
failure. That guarantee must not be generalized to every local write:

- `ChatService::save` ignores ordinary cache-save return values.
- `Library::create/update` and some metadata operations can change memory before
  persistence succeeds; not every failed update rolls back the in-memory view.
- After remote deletion acknowledgements, `ChatService::remove` ignores the
  library removal result; `Library::remove` ignores cache-removal failures.
  Mini Clear and folder cleanup have similar best-effort local paths.

Qualify disk-full/write/remove failure handling and communicate partial outcomes
before relying on the local library as durable user storage. Backend success
followed by failed local cleanup is not “nothing happened.” Per-file `QSaveFile`
publication does not make the index and multiple cache files transactional.

### Privacy, locking and crypto are unfinished

The shipped app has **no `ChatSealer`** and stores ordinary chat display data in
plaintext. The HMAC/XOR sealer exists only inside contract tests. A production
sealer needs vetted crypto, reference-format tests and a failure/crash review;
`Library::protect/unprotect` also ignore mini-cache `reseal` failures today.
Locking metadata before sealing is ordered recovery behavior, not atomic
confidentiality or encryption of the backend journal. Home space/folder names
remain visible even under the reference's design.

Library/preferences files have no explicit owner-only permissions/ACL policy or
interprocess writer lock. Appearance separately requests restrictive permissions;
it is not evidence that every store does. The path validation is not a sandbox
against symlink races or other host processes. Use a private, single-instance
profile; qualify these boundaries before sensitive-data use.

### Toolchain, platform and packaging qualification

- Resolve or separately qualify the two [GCC metatype warnings](cpp-port.md#known-gcc-metatype-warnings)
  before promising a warning-free/`-Werror` Release build.
- Windows/macOS need actual configure/build/test/runtime runs, not just CMake
  branches. Validate paths/case/long names, file replacement, ACLs, system
  accessibility, native dialogs, graphics, DPI/fonts, IME and keyboard behavior.
- No Qt deployment bundle, signing/notarization pipeline or CI matrix is supplied.
  The [fresh visual audit](visual-parity.md) now supplies a desktop-safe comparison
  harness, but does **not** establish pixel parity (0/156 exact, six manual cases
  excluded). CMake install copies app/assets/notices, not every Qt runtime/plugin
  needed on a clean machine.
- Future backend qualification must test lost acknowledgement/reconnect,
  incarnation/revision boundaries, exact failed-turn Retry, mini-session semantics,
  approval/host lifetimes and truthful usage/errors with that backend. The fake
  alone proves none of its durability or remote-effect guarantees.

## Checks for the documentation audit (`4542756`)

On Linux with CMake 4.4.4, Qt 6.11.2 and GCC 16.2.1:

- Release configure with `OPENGHOST_BUILD_SMOKE_TEST=ON` and a clean-first build
  passed. Exactly the two known GCC metatype warnings remained, unsuppressed.
- `native_contract_test` passed: 28 QtTest passes (26 slots plus init/cleanup).
- `native_ui_smoke` and `native_fake_ui_smoke` passed offscreen/software via
  CTest; all three focused tests passed, with no QML load/binding warnings.
- Both disconnected and fake UI smokes also passed on Wayland/OpenGL, using an
  NVIDIA RTX 4080. The known uninstalled-app portal registration warning appeared;
  no QML load/binding warnings or smoke failures appeared.
- Scoped Markdown links/anchors/fences, `git diff --check` and native-name searches
  passed. Historical names remain only in provenance/notices outside the frozen
  reference. No change to `reference/openghost/` or retained licenses/import
  records is included.

That audit's temporary usage probe established the timer failure; the subsequent
fix adds the committed regression coverage described above. No real backend/provider,
Rust, RPC, FFI, Windows/macOS, full pixel-parity, deployment/signing or whole-history
secret audit was run.

## Checks for the usage-ledger fix

On the same Linux/Qt/GCC toolchain: the focused usage run passed (7 QtTest passes,
including init/cleanup and the three failure-recovery rows); the full native
contract suite passed (32 passes). A clean Release configure/build passed with
only the two existing GCC metatype warnings. Both existing UI smokes passed via
CTest offscreen/software with no QML load/binding warnings. Visual parity work
was not started in that usage-ledger pass; no backend integration, UI feature or
reference change was made.

## Fresh visual audit

See [visual-parity.md](visual-parity.md) and its committed per-fixture CSV. The
new headless/offscreen suite inventories 162 cases, captures 156 twice per renderer
and explicitly excludes six manual/visible cases. No pair is pixel-exact; four
headless effort captures additionally require GPU qualification. Safe rendering
fixes have focused smoke assertions. Release build, three CTest checks and four
harness tests passed; strict parity correctly exits 2. No desktop window or
backend integration was used for this audit.
