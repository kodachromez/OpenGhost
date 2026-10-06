# Review of the M01 / M12 / M15 fixes

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
