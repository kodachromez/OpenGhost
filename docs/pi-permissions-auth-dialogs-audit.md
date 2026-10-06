# Review of the M01 / M12 / M15 fixes

> **Resolved (2026-10-06):** each finding below (F1–F5) was reproduced on
> `snapshot/cpp-native-extraction` (with candidate `d7f1677` carried onto
> `07e9819`), fixed at its root and covered by regression tests. **M01 and M12 now
> PASS, and M15 still PASSES.** See [Resolution](#resolution). The original review
> follows unchanged.

## Verdict and scope

**Do not close M01 or M12 yet.** The ordinary approval and cancel/restart paths
work, but the five findings below remain. **M15 passes its narrowly stated
scope:** unsupported blocking dialogs are explicitly cancelled, and warnings and
errors are surfaced. This is not certification of arbitrary extension behavior.

Audited commit: `d7f1677696dea0270fa184b2b6a88a6c6353daaa`
(`og-m01-m12-m15`), parent `4618623`. Line numbers below refer to that commit.
The review used an isolated worktree, not the concurrent uncommitted changes on
`cpp-native-extraction`. This follow-up changes documentation only; it does not
apply production fixes, merge the candidate, or change the frozen reference.

Environment: Linux, Qt 6.11.2, GCC 16.2.1, installed Pi 1.0.4. Runtime probes used
temporary project/agent directories, `PI_OFFLINE=1`, and the candidate's faux
provider. No live provider, real credential, external model request, or visible
window was used. Authentication race probes used scripted backends, not a real
OAuth service.

| Item | Review result |
| --- | --- |
| M01 — Ask/Auto/Full and approvals | **REOPEN:** two reproducible path-policy bypasses. Ordinary Allow/Deny, Stop, Full and mode-change tests pass. |
| M12 — race-safe auth and fresh status | **REOPEN:** a progress event destroys a pending prompt, unchanged provider flags fail to invalidate chat runtimes, and stale auth completions mutate newer error state. |
| M15 — blocking extension dialogs | **PASS within scope:** real Pi + the native adapter cancels select/confirm/input/editor. No generic dialog UI is claimed. |

## Findings

### F1 — P1: POSIX backslashes can make an outside path appear inside the project

**Location:** `src/backend/pi/openghost-policy.js:80,116–120,534–540`.

The POSIX `PATHS.norm` replaces `\\` with `/`. On Linux, a backslash is a literal
filename character, not a separator. With cwd `/tmp/example/project`, a sibling
named `/tmp/example/project\\elsewhere` therefore compares as a descendant even
though it is outside the project. Following symlinks before this comparison does
not repair the subsequent lossy normalization.

Reproduced through **real Pi's built-in tools and the candidate bridge**, with
every approval request automatically denied:

1. Create `project/` and its sibling `project\\elsewhere/` in a temporary directory.
2. Put a harmless marker in the sibling's `secret.txt`.
3. In Ask, call `read` with that absolute filename.
4. In Auto, call `write` with that sibling's `written.txt`.

Observed:

```text
ask read: approvals=0; outside marker returned
 auto write: approvals=0; write succeeded
outside file exists: True
```

**Required correction:** keep POSIX path components lossless; compare canonical
paths using the host's separator semantics. Add negative tests for literal
backslashes in both roots and targets, in addition to ordinary prefix siblings
and symlink escapes. This is an authorization comparison bug, not a demand for
an OS sandbox or stricter shell heuristics.

### F2 — P1: Ask authorizes a different path from the one Pi's read tool opens

**Location:** `src/backend/pi/openghost-policy.js:90–120,534–537`.

The policy implements the ordinary path expansion, but Pi's `read` also uses
`resolveReadPath` / `resolveReadPathAsync` in
`Pi/dist/core/tools/path-utils.js`. When the exact path is absent, those functions
try screenshot spacing, NFD normalization, curly-quote and combined variants.
The policy checks the nonexistent original path, declares it inside, and never
resolves the fallback that Pi actually reads.

Reproduced on Linux, without a hostile extension or a filesystem race:

1. Inside the temporary project, create `fallback’name.txt` (U+2019) as a symlink
   to a harmless marker outside the project.
2. Do **not** create `fallback'name.txt` (ASCII apostrophe).
3. In Ask, call `read` with `{"path":"fallback'name.txt"}`.

Real Pi returned the outside marker with **zero approval requests**, even though
all requested approvals would have been denied. The shipped tests check only
an exact symlink path, so they miss this fallback.

**Required correction:** authorize the exact path the tool will use, including
its fallback resolution, before the effect. Cover fallback symlinks (curly quote,
NFD and spacing) as well as exact paths. Merely fixing F1 does not fix F2.

### F3 — P1: an auth progress/info notification removes an unanswered prompt

**Location:** `src/backend/pi_backend.cpp:1668–1703` (`PiBackend::step`).

Every non-withdrawal event constructs a new `LoginStep`, initially `waiting`,
carrying only the previous URL and device code. A `progress` or `info` event
therefore discards the active `promptId`, input type, secret flag and options.
The bridge still owns the unresolved prompt promise. The user can no longer
answer it, and login has no overall completion deadline.

A scripted Pi trace sent these consecutive records for one current flow:

```text
:prompt {promptId: "p-og-login-1", type: "text", message: "Code?"}
:event  {type: "progress", message: "Still waiting for your code"}
```

The native adapter emitted:

```text
login step: prompt  prompt=p-og-login-1  message=Code?
login step: waiting prompt=             message=Still waiting for your code
```

`WindowController` replaces the form with each step, so the input disappears
without an answer or `:withdrawn` event. The newly added explicit withdrawal
handling is useful, but does not make notifications implicit withdrawals.

**Required correction:** retain the active prompt through informational events;
only a matching withdrawal, replacement prompt, answer, cancellation or terminal
flow result should end its lifetime. Test concurrent notification/prompt ordering
and select/secret prompts as well as ordinary text. The original cancel/restart
test does not exercise this ordering.

### F4 — P2: external config changes remain stale in already-open chats

**Location:** `src/backend/pi_backend.cpp:484–495,1195–1209`.

The control child's `providers` query increments `m_auth` only when the serialized
provider list changes. That list contains names, supported methods and
configured/stored booleans, not the configuration or credential revision.
Changing a model override, endpoint or configured key can leave it identical.
Each chat then skips `refresh` because `chat.auth == m_auth`.

Reproduced with the **real native PiBackend and real Pi**:

1. Start a chat on the test-only `og-faux/faux` provider/model.
2. Write this into the isolated agent directory's `models.json`:

   ```json
   {"providers":{"og-faux":{"modelOverrides":{"faux":{"name":"Changed outside OpenGhost"}}}}}
   ```

3. Call `ChatService::refresh()` (the Settings refresh path), and wait until the
   model catalog shows `Changed outside OpenGhost`.
4. In the same existing chat, run a test extension command that reports
   `ctx.modelRegistry.runtime.getModel("og-faux", "faux").name`.

Observed:

```text
control catalog: Changed outside OpenGhost
chat Pi: Faux
```

A separate scripted command trace confirmed two control-child refreshes and
**zero** chat-child refreshes across initialization, Settings refresh and the
next turn. The tested auth-mutation path does increment the generation; it is
not evidence for external changes with unchanged status flags.

**Required correction:** invalidate chat runtimes on a real refresh epoch or a
reliable source revision, not the UI provider projection. Verify the next model
request uses the refreshed configuration too. Do not hash or expose raw secrets
in frontend state. The reproduced effect is stale model configuration; it is
not a claim that every Pi credential lookup caches old `auth.json` contents.

### F5 — P2: flow IDs protect the form, but not auth error/status mutation

**Location:** `src/frontend/chat_service.cpp:1823–1845`; related
`src/backend/pi_backend.cpp:1611–1612`.

`authFinished(flow)` now prevents an old completion from closing a new form.
However, `ChatService::authenticate` still unconditionally inserts/removes the
provider error and clears global status before emitting that signal.
`answerLogin` also captures only the provider, so an old answer's late error can
be attached to a replacement flow.

Reproduced through the typed service seam:

1. Begin login A; cancel A; begin login B for the same provider.
2. Complete B with `auth_failed: "B's valid error"`.
3. Complete A with `Reply{Null{}}`, exactly what PiBackend emits for a failed
   cancelled/superseded flow.

Observed:

```text
new flow error before old completion: B's valid error
new flow error after old completion:
```

The old completion clears `m_authErrors[provider]` as well as the status. The
window's new flow check runs too late to protect either. This defect existed in
the service path before the candidate, but directly contradicts M12's claimed
race-safe completion; the new test checks form identity, not error ownership.

**Required correction:** generation-check all auth callbacks before mutating
provider/global state, including answer/cancel/logout callbacks. Keep request
settlement distinct from permission to change the current flow's presentation.
Test both stale success clearing a new failure and stale failure poisoning a new
success, including delayed answer replies.

## M15: what was positively checked

In addition to the candidate's select/error tests, a temporary extension in the
isolated agent directory awaited, in order, `ctx.ui.select`, `confirm`, `input`
and `editor`, without timeouts. It was invoked through **PiBackend + ChatService**.

- The native adapter sent a matching `extension_ui_response` with
  `cancelled:true` for all four requests.
- Each request's title appeared in a declined warning.
- The extension received `undefined`, `false`, `undefined`, `undefined`
  (`[null,false,null,null]` when JSON-serialized as an array).
- Its command completed; the native turn settled instead of hanging.
- The shipped extension-error/warning tests also passed.

No new defect was found in that cancellation path. Unsupported informational
widgets/title/editor-prefill behavior remains outside this narrow pass. An
extension that deliberately ignores cancellation, or executes arbitrary code,
is not made safe or bounded by this protocol handling.

## Build and regression evidence

Commands, from the isolated candidate worktree:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build build-release --parallel 4
ctest --test-dir build-release \
  -R '^(native_contract_test|native_browser_test|native_ui_smoke|native_fake_ui_smoke|native_pi_test|native_pi_audit_test|native_pi_real_test|native_pi_policy_check|native_sign_in_test)$' \
  --output-on-failure
git diff --check
```

Release build (`OPENGHOST_BROWSER=ON`): **passed**. All **9/9** selected checks
passed; the real-Pi test ran, not skipped. These green tests coexist with the
reproduced failures above because they do not cover those cases. Extra probes
were scratch diagnostics, not new committed regression tests or production
changes. `git diff --check` passed.

Not qualified: live provider persistence/success/errors, OAuth/device/manual-code
services, Windows/PowerShell, macOS, browser-host tool approvals, or OS isolation.
The candidate already acknowledges that a later extension can mutate tool
arguments after the bridge's approval; this review does not remove that limit.
Auto's inherited shell-risk heuristic and its permitted outside reads are not
being redefined by this audit.

## Resolution

Branch: `snapshot/cpp-native-extraction`. The audited candidate `d7f1677` was not
yet on that branch, so it was carried onto `07e9819` first. There were two
conflicts. One was 07e9819's default-model check, which was kept. The other was
the retiring-child bookkeeping, where both sides were kept. Only this report came
from the audit branch. Every finding was reproduced on that base before any
production code changed:

| Finding | Reproduced before the fix (same environment) |
| --- | --- |
| F1 | `native_pi_policy_check` made 9 wrong decisions for `project\elsewhere`, `odd\dir` and `..\/`. In `native_pi_real_test::backslashNamesAreNotInsideTheFolder`, Ask read the sibling with **no approval** |
| F2 | The policy check made 4 wrong decisions (curly quote, NFD, NFD+curly, screenshot spacing). Real Pi ran `read "fallback'name.txt"` with **no approval** |
| F3 | In `native_pi_test::notificationsKeepTheOpenPrompt`, the step after `progress` had `promptId` = nullopt. In `native_sign_in_test::notificationsKeepTheFormsInput`, the form lost its input |
| F4 | Real Pi reported `og-model registry=Faux session=Faux` after the control catalog showed `Changed outside OpenGhost` |
| F5 | In `native_contract_test::staleAuthResultsNeverTouchNewerState`, B's error became `""` after A's late `Reply{Null{}}` |

### Root causes and fixes

**F1 — POSIX backslash (M01).** `PATHS.norm` rewrote `\` to `/` before a text
prefix comparison, so a literal backslash in a name acted as a separator.
*Fix* (`src/backend/pi/openghost-policy.js`): the lossy normalization is gone.
`inside()` resolves the root and the target, following symlinks. It then compares
them by path components with node's host `path.relative` (`within()`): the target
is inside if the relative path is `''`, or is not `..`, does not start with `../`
and is not absolute. Windows keeps its own case-insensitive, two-separator rules
through the same call. Command analysis also judges the shell-unescaped spelling
of a POSIX command (the shell reads `..\/x` as `../x`), so neither spelling can
hide an outside path.

**F2 — read fallbacks (M01).** The policy authorized the path as given. Pi's
`read` opens the first existing path among the name and its screenshot-spacing,
NFD, curly-quote and NFD+curly variants (`resolveReadPath`). *Fix*: the policy
takes Pi's own resolved path (`expand()`, mirroring Pi's `resolveToCwd`). For
`read`, it decides on that path **and every variant Pi's existence test would
find**, and every candidate must be inside. Variants that do not exist are never
opened, so a folder with an apostrophe or accent in its name does not ask on
every read. Writes and edits never fall back and are unchanged.

**M01 race and identity boundaries.** No bypass was reproduced at these points
while fixing F1/F2. Each is now closed by code rather than assumed safe:
- An idle chat's Pi now takes a mode change at once (`PiBackend::configure`).
  Before this, a child left in Full kept Full until the next OpenGhost run, so
  an extension-started turn in that child could still run under it.
- Mode updates are numbered (`m_modeSeq`; bridge `{op:"mode", seq}`). The bridge
  ignores an update older than the last one it applied and replies `stale:true`.
  `setMode()` records a held mode only from the latest update's confirmed reply
  on the same child. Until that reply arrives the mode is unknown, so the next run
  sets it again.
- An approval records the child that asked for it. `answer()` replies only to
  that child, and only while it is still the chat's child. An Allow counts only
  while the card's turn is still running. Retiring a child declines and withdraws
  its cards, and a withdrawal never answers another child's dialog.

**F3 — prompt lost on a notification (M12).** `PiBackend::step` built every
non-withdrawal event as a fresh `waiting` step. This dropped the open prompt's ID,
type, secrecy, placeholder and options. *Fix*: the open prompt is now tracked per
provider (`m_prompts`). A progress, info or auth-URL notification keeps the prompt
answerable and shows the notification beneath the question, with the URL, code
and links updated. The prompt closes only on one of these:
- an answer, closed at once when `AnswerLogin` is requested, so a later
  notification about that answer offers nothing;
- a matching withdrawal;
- a newer prompt;
- Cancel;
- a new sign-in;
- the end of the flow.

**F4 — stale chat configuration (M12).** Open chats were invalidated only when
the control child's provider projection (names and flags) changed. When a chat did
reread, Pi's session kept its old selected model object. *Fix*: every successful
control-child reread bumps the configuration generation `m_auth`. That covers
Settings' model query with its `refresh` and the provider query. Each chat's Pi
rereads before its next run. The bridge's `refresh` then takes the session's
selected model again from the reread registry with `pi.setModel`, keeping the
same model and thinking level, and only when idle. The next model request
therefore uses the new configuration. The epoch is a plain counter: no
configuration or secret content is hashed or exposed.

**F5 — stale auth results change state (M12).** `ChatService::authenticate` and
`answerLogin` changed the provider error and the global status on every
completion. *Fix*: each sign-in, key, cancel or logout records a per-provider
attempt number (`m_authAttempt`). An answer belongs to the attempt that was
current when it was sent. Only the provider's latest attempt sets or clears its
error and the status line. A stale completion still settles through
`authFinished(flow)`, which the window already matches to its form, and still
rereads the catalog. Logout, re-login, cancellation and overlapping attempts
therefore settle deterministically: the most recently started attempt owns the
presentation.

### Files changed

- Production: `src/backend/pi/openghost-policy.js`,
  `src/backend/pi/openghost-bridge.js`, `src/backend/pi_backend.{h,cpp}`,
  `src/frontend/chat_service.{h,cpp}`.
- Tests: `tests/pi/real/policy-check.mjs`, `tests/pi_real.cpp`,
  `tests/pi/real/{faux,helper}.ts`, `tests/pi.cpp`, `tests/pi/pi`,
  `tests/sign_in.cpp`, `tests/contract.cpp`, `tests/pi_audit.cpp`.

### Regression tests

| Test | Covers |
| --- | --- |
| `native_pi_policy_check` (+14 cases, three specially named roots) | F1: sibling, prefix, backslash-root and unescaped shell paths. F2: all four fallback kinds, fallbacks that stay inside, writes that never fall back, folders named with an accent or apostrophe |
| `native_pi_real_test::backslashNamesAreNotInsideTheFolder` | F1 through real Pi: Ask read, Auto write and Auto `touch` of the sibling all ask. When denied, nothing happens |
| `native_pi_real_test::readFallbacksAreDecidedOnTheFileRead` | F2 through real Pi: curly-quote, NFD and screenshot-spacing symlinks leading outside ask. A fallback that stays inside does not |
| `native_pi_real_test::anOlderModeUpdateNeverWins` | Numbered mode updates in the real bridge |
| `native_pi_test::fullNeverAsksAndModeChangesReachTheRunningPi` (extended) | An idle child takes Ask at once |
| `native_pi_test::notificationsKeepTheOpenPrompt` | F3: text, select and secret prompts survive progress and info. After an answer, a later notification asks nothing |
| `native_sign_in_test::notificationsKeepTheFormsInput` | F3 in the Settings form: input, placeholder and answer survive |
| `native_pi_real_test::outsideConfigChangesReachOpenChats` | F4 through real Pi: the registry, the session's model **and the next model request** use the edited `models.json` |
| `native_contract_test::staleAuthResultsNeverTouchNewerState` | F5: stale success vs a newer failure, stale failure vs a newer success, and a delayed answer failure across logout and re-login |
| `native_pi_real_test::everyBlockingDialogIsCancelled` | M15: select, confirm, input and editor are each cancelled (`[null,false,null,null]`) and named |

`native_pi_audit_test::unresolvedAdmissionIsNotMissing` (from 07e9819) treated
any two `prompt` lines as "mark, then the real prompt". The candidate's
access-mode request is also a bridge prompt, so on the combined branch the wait
ended one step early. It now waits for exactly one non-bridge prompt. Its
assertion is unchanged.

### Evidence

The environment was the same as above: Linux, Qt 6.11.2, GCC 16.2.1, Pi 1.0.4,
`PI_OFFLINE=1`, isolated agent directories and the faux provider. No live
provider or credential was used.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DOPENGHOST_BUILD_SMOKE_TEST=ON
cmake --build build-release --parallel 12
ctest --test-dir build-release --output-on-failure   # all 15
ctest --test-dir build-release --repeat until-fail:5 -j3 \
  -R '^(native_contract_test|native_pi_test|native_pi_audit_test|native_pi_real_test|native_pi_policy_check|native_sign_in_test)$'
git diff --check
```

- **Build:** the Release build (`OPENGHOST_BROWSER=ON`) passed with no warnings.
- **Tests:** 15/15 passed, including the nine focused tests:
  `native_contract_test`, `native_browser_test`, `native_ui_smoke`,
  `native_fake_ui_smoke`, `native_pi_test`, `native_pi_audit_test`,
  `native_pi_real_test` (13/13, not skipped), `native_pi_policy_check` (36 cases)
  and `native_sign_in_test`.
- **Repeats:** the six Pi and auth tests passed five consecutive times under
  parallel load.
- **Whitespace:** `git diff --check` passed.
- **M04–M08:** session separation, recovery, deletion races, canonical models,
  instructions and Retry are still covered by `native_pi_test` and
  `native_pi_audit_test`, and pass unchanged.

| Item | Result |
| --- | --- |
| M01 | **PASS**: F1/F2 fixed and tested through real Pi; race and identity boundaries closed |
| M12 | **PASS**: F3/F4/F5 fixed; F4 verified through real Pi. Real-provider OAuth, device-code and manual-code flows remain unqualified, as before |
| M15 | **PASS**: the path is unchanged; the four-dialog check is now a committed regression test |

These limits are unchanged:
- The policy decides before a call; it is not an OS sandbox. It does not stop a
  concurrent process from creating or swapping a file or symlink between the
  decision and the effect.
- A later-loaded extension can still rewrite arguments after approval.
- Command risk is still 1.3's text heuristic.
