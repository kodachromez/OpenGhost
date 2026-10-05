# Approval/backend presentation limits: upstream comparison

## Baseline and scope

Compared `frontend-only-rust-backend` at
`1cd53153d6c64aefbe33204a16252881e5ecbbcb` against freshly fetched
`upstream/main` (`ANDRETRIPOL/OpenGhost`) at
`42f0dc879f3cc443552d6b27a9b7998d3cc1deee`. `origin/main` names the same
upstream revision. No changes to main, ABP DTOs, backend data, permission policy,
transport limits, or the ordinary upstream Markdown/approval/error renderer.

The audit follows branch-added/changed approval and backend presentation into
chat, provider settings, model groups and usage. It is not a general renderer
resource-hardening exercise.

## Classification

| Path / presentation | Stock upstream evidence | Classification / disposition |
| --- | --- | --- |
| `approval-card.js`: `headline`, command/quote/place text, `lines` | Same renderer: one span per title word, full command/quote strings, three displayed places, fourteen displayed diff lines but whole-string splitting. `agent-tools.js::describe` supplies unrestricted descriptions, command/content/path/query strings, and a compact JSON fallback. | **Inherited. Unchanged.** No new character cap that could hide an executable command suffix. |
| `approval-card.js::ApprovalCard.present`: fallback JSON | Stock `agent-tools.js::describe` uses `JSON.stringify(args)`. The branch introduced `JSON.stringify(args, null, 2)` for every tool lacking presentation. Deeply nested small JSON can expand quadratically in indentation alone. | **Branch regression. Fixed:** restore compact JSON, bounding indentation amplification without dropping any argument. |
| `chat.js::fail`, `backend-client.js::explain`, `settings.js::setStatus` / account status | Stock chat renders `error.message` directly; stock settings render provider/API/login errors, account email and plan without character caps. Provider adapters also expose remote model names/custom effort levels; these can already drive large picker DOMs. | **Inherited. Unchanged:** no blanket truncation of errors/status/model names, model counts, or assistant text. |
| `chat.js::applyEvent` tool/reasoning/status payloads | This branch renders only a fixed working indicator for ordinary tools. `tool.progress` and `reasoning.delta` have no view; completion only updates bookkeeping. | **No new string-rendering sink.** Tool names/results/progress do not become UI text/cards. |
| `settings.js`: provider/auth copy, provider sections; `model-stage.js`: provider groups | Stock provider names/groups, auth label/hint/action/placeholder/link destinations and provider section count are fixed application metadata (four provider identities, three settings sections). The branch makes these backend-controlled. | **Branch regression. Fixed:** bounded display copy and provider paging, retaining the complete normalized catalog and exact IDs. Provider ranking/grouping is indexed rather than scanning all providers/models for each group. |
| `settings-usage.js`: provider sections, chart parts/legend/tooltips | Stock iterates four fixed provider IDs. The branch iterates an arbitrary backend/ledger provider set in every section, chart column and tooltip. | **Branch regression. Fixed:** paged sections and bounded chart projections with explicit Other totals. |
| `settings-usage.js`: individual account-limit/model/balance rows and strings | Stock already maps unbounded remote model-limit/balance arrays, plan/credit/model text and local per-model usage rows. | **Inherited. Unchanged.** Provider fan-out is bounded, not the pre-existing contents of each provider's section. |

The classification is about the actual input/render paths, not merely the source
of a string changing from a provider adapter to ABP. Moving an existing unbounded
string sink across that boundary does not by itself justify changing stock UI.

## Narrow presentation bounds

- Approval fallback uses compact JSON, as upstream does. No preview truncation,
  argument mutation, policy change, or changed Allow/Deny response.
- Backend-only provider names/groups and auth labels/actions/placeholders: at
  most **256 UTF-16 code units**, with an ellipsis on shortened text. Clipping
  avoids leaving a dangling high surrogate. Hints: **2,048** code units.
- Backend auth links over **2,048** code units are omitted with a visible notice,
  never turned into a different clickable truncated URL. Full metadata remains
  in `Settings.providers`; IDs, key values, status, catalog/model capabilities
  and auth request parameters are not clipped.
- Provider settings and usage show **16 provider sections per page**. Next and
  Previous replace the page (not append more DOM), retain keyboard focus and
  permit access to the final provider. Off-page auth results/key edits remain
  associated with their original IDs; opening settings for a specific error
  selects its provider's page. A shrinking catalog clamps the page.
- Usage charts, legends and tooltips show at most **16 named providers plus an
  explicit Other providers group**. The named cohort stays consistent across
  legends, individual days, period bars and tooltips. Every token/share still
  contributes to all totals and bars. The full ledger is unchanged; all provider
  detail pages remain reachable. Account-limit polling follows the visible page; retained responses
  are not discarded by paging.
- `Settings.collect`, provider label lookup, and `ModelStage.build` avoid the
  newly possible provider-count multiplier in catalog scans. All models remain
  selectable; the upstream model-count/name rendering behavior is unchanged.

These are not a global renderer memory/CPU guarantee. The original data remains
available in memory; inherited unbounded content and transport parsing are not
silently relabelled as fixed.

## Validation

Focused Node command (46 passing tests):

```sh
node --test test/untrusted-text.test.js test/provider-auth.test.js \
  test/settings-presentation.test.js test/usage-limits.test.js \
  test/model-capabilities.test.js test/usage.test.js
```

New regressions cover compact nested approval JSON with a final command suffix,
large backend auth metadata, non-truncated identities/URLs, provider paging and
pending auth/key edits, catalog shrinkage, bounded usage DOM/legend/tooltips,
visible-page polling and exact unchanged accounting.

Focused real-Electron DOM check:

```sh
OPENGHOST_E2E_OZONE=x11 node test/e2e/presentation-limits.cjs
```

Passed with an isolated profile, hidden window, sandbox enabled, no backend or
credentials, and HTTP(S) blocked. Covers real pager clicks/focus, long metadata,
model-group scan count, unchanged model selection IDs, usage shares and approval
Allow wiring. Default headless Ozone crashed with `SIGSEGV` before test execution
on this host; the existing X11 display was used instead. No sandbox flags were
weakened. The full application/backend/browser smoke suite was not run.

`git diff --check` also passed.
