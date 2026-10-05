# Standalone repository readiness

Audit scope: the native frontend on `cpp-native-extraction`, following the local
state implementation in `185ed65`. This pass changes documentation and stale
comments only. It adds no backend adapter, transport, RPC, FFI or frontend
feature. The frozen `reference/openghost/` and all licensing/provenance records
are preserved.

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

## Findings needing follow-up

These are not fixed in this documentation-only pass.

### Usage ledger automatic save is skipped

[`UsageStore`](../src/frontend/usage.cpp) starts a single-shot 800 ms timer in
`save()`, connects timeout to `flush()`, then has `flush()` return immediately
when `!m_timer.isActive()`. Qt stops a single-shot timer before emitting timeout.
Consequently the normal timeout saves nothing, and a destructor/explicit flush
after timeout also returns success without writing. This is worse than merely
losing the last 800 ms on a crash.

A temporary, uncommitted QtCore probe linked against `native_contract` reproduced
this on Qt 6.11.2:

1. Create `UsageStore` over `MemoryKeyStore` and record 10 input tokens.
2. Run the event loop for `SaveDelay + 300 ms` without another record.
3. Check the key, explicitly flush, then destroy the store.

Observed: `usage` absent after timeout; `flush()` returned true; `usage` still
absent after destruction. The existing `usageLedgerPersists` test flushes by
destruction **before** timeout, so it does not cover this failure. Follow-up
should track dirty state independently of timer activity, test actual elapsed
timeout and failed writes, and surface failures. No new production/test behavior
was committed just to make this documentation pass green.

### Local save and deletion failures are not uniformly reported

Required pre-dispatch checkpoints correctly refuse starts/retries/steering on
failure. That guarantee must not be generalized to every local write:

- `ChatService::save` ignores ordinary cache-save return values.
- `Library::create/update` and some metadata operations can change memory before
  persistence succeeds; not every failed update rolls back the in-memory view.
- After remote deletion acknowledgements, `ChatService::remove` ignores the
  library removal result; `Library::remove` ignores cache-removal failures.
  Mini Clear and folder cleanup have similar best-effort local paths.
- `UsageStore` stops its timer before a write and does not retain a separate
  failed-write dirty state; its error reporting covers load, not all save errors.

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
- No Qt deployment bundle, signing/notarization pipeline, CI matrix or full
  visual-parity suite is supplied. CMake install copies app/assets/notices, not
  every Qt runtime/plugin needed on a clean machine.
- Future backend qualification must test lost acknowledgement/reconnect,
  incarnation/revision boundaries, exact failed-turn Retry, mini-session semantics,
  approval/host lifetimes and truthful usage/errors with that backend. The fake
  alone proves none of its durability or remote-effect guarantees.

## Checks for this pass

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

The temporary usage probe above is an additional diagnostic, not a new committed
regression test or a passing durability qualification. No real backend/provider,
Rust, RPC, FFI, Windows/macOS, full pixel-parity, deployment/signing or whole-history
secret audit was run.
