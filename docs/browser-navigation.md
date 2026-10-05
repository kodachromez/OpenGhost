# Browser navigation outcomes

Addresses finding 8 of issue #21. This patch depends on the browser lifecycle
PR (findings 2–4): it reuses operation cancellation, deadlines, guarded dispatch
and coded generic-tool errors instead of duplicating that machinery.

URL navigation waits for its own `loadURL` promise, avoiding misattribution of
an abort from a previous superseded load. Back/forward/reload require actual
load completion or main-frame same-document navigation. Stopped loading or
readable partial DOM alone is not success. Main-frame load/provisional failures
remain failures through later commits, settling and snapshot evaluation.

`ERR_ABORTED` is `navigation_aborted`; other load errors are `navigation_failed`.
Cancellation remains `cancelled` and deadlines remain `timeout`. Failed loads
are stopped; cleanup errors do not mask the original result. Operation listeners
and timers are removed. A successful subsequent navigation is not poisoned by
an earlier failure. Errors stay errors through upstream AgentTools formatting.

Run `npm test`, `node test/e2e/browser-navigation.mjs` and
`node test/e2e/browser-cancellation.mjs`. Offline tests include redirects,
history/reload, same-document navigation, failed/aborted loads, listener cleanup,
and a readable local partial page reaching the real 30-second load deadline.
Already-issued Chromium effects are not rolled back.
