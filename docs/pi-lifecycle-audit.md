# Pi lifecycle remediation audit

Scope: M02, M03, M13, M14 and M16, reviewing `957e15c` and `e8dc3cc`.
The original **FIXED** labels were too broad: the happy paths worked, but the
admission, Stop, steering and accounting races below were not covered. This
follow-up hardens those paths without changing the UI or attachment support.

## Findings addressed

| Area | Finding | Correction / regression |
| --- | --- | --- |
| M02 | Any successful prompt response, even an unknown/missing disposition, cleared the composer. Settlement without a final assistant response became success. | Validate `started/queued/handled`; malformed admission closes the child and requires reconciliation. Unsupported/missing final outcomes report an error. `unknownDispositionIsNotAcceptance`, `noFinalResponseIsNotSuccess`. |
| M02 / M14 | `GetSession` could report Missing while an unacknowledged prompt was still outstanding. Process loss was labelled a definite rejection. | Outstanding admission returns `acceptance_pending`; transport loss is uncertain and invalidates the cached session lookup. The 60-second admission deadline now closes the child even if no reply ever arrives. `unresolvedAdmissionIsNotMissing`. |
| M03 | Stop before deferred dispatch did not withdraw the start. | Track pending start/retry cancellation before dispatch and during session loading. Separate tests cover unsent withdrawal and sent-but-late acceptance. |
| M03 / M13 | Stop ignored a failed `clear_queue`; an in-flight input handler could enqueue after the clear. A new turn could start between `agent_settled` and the abort reply. | Wait for steering admission, then clear, then abort. Fence starts/configuration until cancellation completes. A failed clear/abort retires the child and returns failure, never successful Stop. Queue-clear failure, delayed-steer and delayed-abort-ack tests. |
| M13 | A steer admitted after natural settlement was labelled notApplied. Empty transformed text could never match delivery. | Defer completion until the in-flight receipt and queue cleanup finish. Preserve queued/unconfirmed rather than false notApplied; represent queued text as optional so empty text is matchable. Refuse additional steering during settlement/cleanup. |
| M02 / M14 | Evicting the 64-entry display journal also lost live deduplication; Retry could mistake eviction for absence. | Retain accepted client identities independently of display events; recover evicted failed-turn journals from Pi before revalidating Retry. `dedupeAndRetrySurviveJournalEviction`. |
| M14 | The bridge checked only whether the latest assistant failed, not whether it belonged to the requested remote turn. Context edits/compaction could make its “exact” continuation false. | Pass `failedTurnId`, check its owning start record on the active branch, and refuse intervening context edits, compaction or branch summaries. The bridge unit check also preserves earlier tool results and strips repeated retry triggers/failed replies. |
| M16 | Local Stop discarded identities of assistant attempts beginning afterward, so their actual usage could be lost. Replayed starts invented elapsed-time origins. | Keep post-Stop message identities without rendering text; charge their actual provider/model usage. Update final timing at backend settlement and leave replay timing unknown. File-backed ledger close/reopen test verifies attribution, cache counts and no extra reasoning charge. |
| Transport | An expired bridge request could still be sent after bridge discovery; complete oversized stdout records bypassed the bound. | Remove expired requests from the unsent queue; check complete as well as incomplete records before parsing. Short-deadline bridge-discovery regression. |

Relevant implementation: `src/backend/pi_backend.{cpp,h}`,
`src/backend/pi_process.cpp`, `src/backend/pi/openghost-bridge.js`, and
`src/frontend/chat_service.cpp`.

## Verification

- Release build with `OPENGHOST_BUILD_SMOKE_TEST=ON`.
- Existing `native_pi_test`, plus `native_pi_audit_test` and its adversarial
  JSONL fixture in `tests/pi-audit/pi` (no model, network, tools or credentials).
- Required contract/browser/disconnected/fake UI smokes, and the full CTest suite.
- `node tests/pi_retry_bridge.mjs` (also runnable with Bun): optional production
  bridge-handler/context-filter unit checks using a mocked extension API.
  Node/Bun is **not** a native build or CTest dependency.
- `git diff --check`.

The final commit is also built/tested in an isolated staged-tree export. Attachment/UI
work was committed separately during this audit; it is not part of this audit's diff.
This is scripted protocol and frontend
qualification, **not** a new real-Pi/faux-provider or live-provider qualification.
The 60-second production admission/abort deadlines and the 64 MiB stdout limit
were inspected, not exhaustively exercised with wall-clock/large-record tests.

## Remaining limits — do not read these fixes as full backend qualification

- Steering identity is still text/FIFO-based. An extension inserting identical
  text can be confused with frontend steering. Cleared/unobserved inputs remain
  **unconfirmed**, not definitively notApplied. Unsupported attachment steering
  is refused rather than silently dropping its payload.
- A rejected start on an existing chat still fails closed under the frontend's
  reconciliation rules; it cannot blindly resend the rejected input. Extension
  command/dialog behavior remains a separate integration gap (M15).
- Exact Retry is a guarded continuation, not regeneration and not a rollback.
  Earlier tool results remain in context; a subsequent model decision can still
  repeat an effect. Changed branch/context is refused, not guessed.
- Usage covers final assistant-message increments. Nested-tool, compaction,
  branch-summary and cache-warming usage are not included. Recovery replay does
  not recharge; it also does not backfill usage never received/committed before
  a crash. The ledger remains debounced and save failures have no dedicated UI:
  it is a persisted local counter, **not a crash-atomic billing ledger**.
- Pi's final turn markers and the local display/usage stores are not one atomic
  transaction. No restart supervisor, arbitrary extension compatibility,
  cross-platform or real-provider reliability claim is made by these tests.
