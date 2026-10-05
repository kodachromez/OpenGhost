# Browser host-tool lifecycle (F16)

The existing OpenGhost browser UI, guest isolation, login partition and Take Control/hand-back behavior are unchanged. This supplements the ABP host-tool contract; it supersedes the pre-fix browser behavior recorded in audit §4.

## Availability and targets

- `initialize.host.tools` includes the 11 browser schemas only when the desktop browser bridge exists.
- `host.browser` is an explicit object even when closed/empty: `available`, `status` (`unavailable`, `empty`, `lazy`, `loading`, `ready`, `failed`, `gone`), `control` (`agent`/`user`), `open`, `tabs`, `signedIn`, and `signedInVerified:false`. Submission observations are **not** verified login/provider auth state.
- `host.browser.changed {browser}` notifications report lifecycle/control/context changes. Tab entries include stable `tabId`, display position `n`, `state`, `loading`, renderer `revision`, title, URL and active flag. These are hints; tool dispatch always rechecks the live target.
- All host-tool calls, including tab mutations, are serialized across chats. The target is captured at receipt, not after queuing. Optional `tabId` explicitly selects a tab; otherwise the active target is pinned. A changed/closed target fails instead of silently retargeting.
- `browser_tabs switch/close` **require `tabId`**, not the positional `tab`. `new` fails with `tab_limit` at 12 tabs rather than evicting one. User tab controls retain their existing behavior.
- Page results carry `data.tabId` and opaque `data.pageId`. `click`, `type`, `select`, `press` and `scroll` **require `pageId`** from the latest observation; other page tools may also supply it as a precondition. Navigation, another input operation, or partial/cancelled input invalidates earlier page identities. Re-snapshot after stale-target errors. Refs are usable only with their accompanying page identity, not durable element handles. Covered/moved ref targets fail before input.

## Readiness, cancellation and errors

First `dom-ready` has a 15-second deadline and rejects on close, failed initial load, crash or cancellation. A subsequent readiness attempt can recreate a failed/crashed guest. The renderer call budget is 90 seconds including its queue; the main operation budget is 75 seconds including its queue. CDP/isolated-world calls are bounded by 12 seconds, navigation by 30 seconds and loading settle by 15 seconds. User hand-back waiting remains intentionally unbounded.

Cancellation/timeouts release waiters and prevent subsequent dispatches from late continuations. Navigation loads are stopped on failure/cancellation. Already-issued browser input/JS cannot be rolled back; cancellation or a timeout is not proof that no partial side effect occurred. Re-observe before retrying. Page reads/snapshots are observations, not DOM transactions.

Failures use `isError:true`, `status:'error'` (or the existing cancellation/hand-back status), text explaining the failure and `data.code`. Codes include `unavailable`, `timeout`, `wait_timeout`, `navigation_failed`, `tab_gone`, `guest_crashed`, `stale_tab`, `stale_page`, `stale_ref`, `stale_target`, `element_covered`, `stale_read`, `tab_limit`, `invalid_request` and fallback `browser_error`. Wait-for-text expiry and navigation failures no longer look successful.

## Result metadata

- Snapshots: `refs`, `coverage`, `truncated`, `scroll`, and operation-scoped `downloads`. DOM/visibility heuristics and timed quiet are unchanged; cross-origin iframes remain unreadable and snapshot/read shadow-DOM coverage differs.
- Screenshots: image plus `width`, `height`, `scale`, `pageWidth`, `pageHeight`, `truncated`. Full-page capture is still capped at four viewport heights; divide image coordinates by `scale` for page pixels.
- Reads: frozen HTML-derived text, not recomputed live HTML for each slice. First call omits `readId`; subsequent calls supply the returned `tabId`, `readId` and `start=data.end`. Metadata includes `start`, `end`, `total`, `hasMore`, `offsetUnit:'utf16'`, `sourceTruncated`, `truncated` and `coverage`. A refresh, navigation/input, or lost tab invalidates continuation. Source HTML remains capped at 4 Mi UTF-16 code units and each text slice at 40,000. Hidden text may be included; form controls, iframe content and shadow roots are omitted. These limitations are explicit, not a new extraction engine.
- Completed downloads are attributed to the guest and operation active when the download began. Later/other calls do not consume a global one-shot download list. Late completion still produces the existing UI download toast, not a claim that a different operation downloaded it.

Focused checks: `node --test test/browser-lifecycle.test.js test/cancellation.test.js test/untrusted-text.test.js` and `node test/e2e/browser-lifecycle.mjs` (headless Electron, local pages only).
