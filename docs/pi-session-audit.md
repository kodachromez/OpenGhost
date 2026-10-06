# Pi sessions, recovery, deletion, models and instructions audit

Scope: M04–M08 in `e8dc3cc`, with the attachment/context changes in `71caa0e`
and lifecycle hardening in `4618623`. The original FIXED labels covered useful
happy paths, not all of the guarantees described. No reference/UI redesign,
provider authentication or permission-policy work is included here.

## Findings and corrections

| Item | Result |
| --- | --- |
| **M04 — separate histories and switching** | Per-chat children/session files and owner-routed events hold up in the focused tests. Existing tests cover returning to a previous chat and two simultaneous replies with Stop reaching only its owner. Added coverage verifies a mini session has separate history and is deleted with its parent. Mini UI remains unexposed. |
| **M05 — honest recovery** | **Reproduced:** a persisted assistant `stop`/`length` without a turn-end record was recovered as successful, even though boundary continuations, queued work or overflow recovery could still be pending. Successful recovery now requires the matching end marker; an otherwise unfinished run reports `interrupted`, retaining its recorded text without resending. Recovery also stops collecting messages at that end marker, rather than attributing later extension output to the old turn. Recorded errors/aborts remain non-successful outcomes. |
| **M06 — dispose before deletion** | **Reproduced:** idle reaping detached a closing child from its chat before it exited. Delete then removed the file immediately; the child's shutdown hook could recreate it after deletion had succeeded. Track the retiring child until exit, join repeated close requests, and refuse reopening while closing/deleting. Added tests cover this race, actual mini-file removal, and a folder's partial deletion failure preserving the failed/unattempted chats. |
| **M07 — canonical model/default** | **Reproduced:** a successful `set_model` reply without a model invented confirmation from the requested IDs. Reject incomplete canonical replies. Start/Retry acknowledgements now carry Pi's actual model to the frontend and local chat index too; previously only ConfigureSession adopted it. Failure to read the control child's default no longer silently falls back to catalog order. Tests cover alias canonicalization, malformed replies, persisted first-start selection, defaults, refusals and per-chat isolation. |
| **M08 — instructions through continuation/compaction** | **Reproduced with real Pi 1.0.4, offline:** Retry bypasses `before_agent_start`; Pi's next-turn refresh rebuilds base prompt options, dropping OpenGhost's sections. Keep the existing named sections on ordinary prompts and enforce them through `context_with_system` on every model request, including Retry and its tool loop. Instructions/pinned files are system deltas, not synthetic user messages. Edits and clearing apply on the next start/Retry; steering retains the active run's context. |

### Regressions

`tests/pi.cpp` / `tests/pi/pi` add:

- `recoveryRequiresSettlement`
- `recoveredTurnExcludesMessagesAfterItsEnd`
- `deletionWaitsForAnAlreadyRetiringChild`
- `folderDeletionFailureKeepsTheRemainder`
- `miniHistoryIsSeparateAndDeletedWithItsParent`
- `failedDefaultReadIsNotAnArbitrarySelection`
- `modelRepliesMustBeCanonical`

The unsettled-recovery, retiring-child deletion and malformed-model tests were
run before the corrections and all failed. The scripted Pi is only a protocol
fixture: it does not execute the production bridge or prove real durability.

`tests/pi_context_runtime.mjs` is a separate, optional real-Pi SDK qualification.
It loads the production bridge hooks, uses a temporary profile, disables resource
discovery/tools, and supplies Pi's faux provider (no live calls or credentials).
It asserts the actual model-facing system prompt after ordinary prompts, real
compaction, Retry/tool continuation, session disposal/reopen, edits and clearing.
The Retry assertion failed with the original bridge and passes with the fix.

```sh
PI_SDK_ROOT=/path/to/@earendil-works/pi-coding-agent \
  node tests/pi_context_runtime.mjs
node tests/pi_retry_bridge.mjs
```

Node/Pi remain optional test runtimes, not native build dependencies. This SDK
check exercises Pi's context implementation, not the C++ subprocess transport or
a real provider.

## Verification

- Fresh isolated Release build with `OPENGHOST_BUILD_SMOKE_TEST=ON` and the
  default browser enabled.
- All **12 CTest checks** passed, including `native_pi_test`,
  `native_pi_audit_test`, `native_contract_test`, `native_browser_test`,
  `native_ui_smoke` and `native_fake_ui_smoke`.
- Both optional bridge checks above passed against installed Pi 1.0.4.
- `git diff --check` passed.

The isolated tree is based on `d5a59c8` plus only this audit's changes. Unrelated
frontend-plugin/attachment/UI work changed concurrently in the main worktree;
its initial smoke failures are not included in, or claimed fixed by, this audit.

## Remaining limits

- Pi persists named prompt sections in **system messages** and compaction
  checkpoints. The older claim that instructions never occur in Pi session
  messages/files was inaccurate: they are excluded from **user conversation
  messages**, not from persisted system context. Treat Pi sessions as sensitive.
- Exact Retry preserves the failed conversational context and earlier tool
  results, but deliberately applies the standing context supplied for this
  retry. It is not rollback and cannot guarantee no future repeated effects.
- Backend files, frontend index/display checkpoints and the usage ledger are
  not one crash-atomic transaction. A lost end-marker write now yields a
  conservative interruption instead of guessed success. Local deletion/save
  failures described in `repository-readiness.md` are still not uniformly
  surfaced; replay does not backfill usage lost before local recording.
- A chat containing only a handled extension command may have no persisted Pi
  conversation. Missing backend history remains display-only, never resent.
- Single private profile/writer is assumed. No interprocess exclusion, hostile
  filesystem/symlink protection, arbitrary extension navigation/session-switch
  compatibility, monotonic cross-restart sequence guarantee under clock changes,
  restart supervisor, live-provider or cross-platform qualification is claimed.
- Removing a session does not undo tools' prior external effects. Thinking
  levels, permission enforcement and auth lifetimes remain separate audit items.
