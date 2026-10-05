# Reply pictures and YouTube cards

Scope: `reply-media-presentation`, completing the slice after `dc0a9e04`.
Authority: frozen `reference/openghost/{markdown,media-embed,media-slider,link-chip}.js`,
`styles.css` and `desktop/{main,preload}.js`. No changes to that reference,
agent contracts, Rust, RPC, FFI, browser host or browser tools.

## Presentation

`markdown::mediaItems` recognizes picture-only/link-picture/YouTube paragraphs
and non-task lists. Prose, mixed/task lists and quoted pictures stay ordinary
Markdown. An incomplete live picture list holds a quiet plate and requests no
pictures until sealed. Video cards can appear and obtain metadata while that
plate is live. Equal video URL/link-word keys are deduplicated and reconciled
without destroying existing cards.

`MediaBlock.qml` supplies captions and active source-page links, held-image
consent buttons, failed-image links and responsive YouTube card grids.
`MediaStack.qml` uses the reference frame/ratio clamps, fanned transforms,
blurred letterbox backdrops, spring/hover spread, arrows, dots (counter above
ten), Home/End/arrow keys, drag/fling and horizontal trackpad gesture rules.
Vertical wheel input remains transcript scrolling. Active-slide accessibility
and button actions are native; a picture click opens its source page, not a
video player. Reduced motion settles the stack without its spring.

`MediaGlass.qml` recreates CSS `backdrop-filter` using `ShaderEffectSource` and
`MultiEffect`: play button, duration pill and dots use 10 px blur/saturate(1.4);
arrows use 12 px/saturate(1.5). Sampling has a three-sigma margin, then a rounded
mask and the reference tint/border. The source plane excludes controls to avoid
recursive sampling. A non-layer parent avoids Qt reusing a thumbnail layer's
texture with the sampling rectangle. Half-pixel borders disable pixel rounding.
Mask thresholds retain rounded antialiased edges. No WebEngine or custom browser
surface is involved. Software scenegraph retains tints; RHI/OpenGL is needed for
these Qt effects. Qt and CSS blur kernels/rasterization are not pixel-identical.

## Exact title lookup audit and host seam

The reference does **not** derive a title from a thumbnail or invent a generic
video name:

1. `media-embed.js` captures `window.openghost?.videoInfo`. Every valid video card
   calls it, including cards whose link already supplies a title/channel/time.
2. `desktop/preload.js` forwards only the ID through `media:video-info`;
   `desktop/main.js` checks `fromApp(event)` and the eleven-character
   `[A-Za-z0-9_-]` ID. It requests
   `https://www.youtube.com/oembed?url=<encoded HTTPS watch URL>&format=json`
   using `net.fetch`, with `AbortSignal.timeout(10000)`.
3. Non-OK HTTP, malformed JSON, exceptions or timeout produce null. Valid JSON
   maps `title` and `author_name` to `{title, by}` (JavaScript string coercion).
4. The renderer accepts only a nonempty title. It fills an empty link title and,
   independently, an absent explicit channel. Supplied link words and duration
   always win. Failure leaves those words intact, or no title and “YouTube”.
5. `asked` shares both in-flight and completed promises, including failures, for
   the process. `openghost.media.info` stores successful results, evicting oldest
   insertion keys above 300. Storage failures do not hide successful metadata.

Native equivalents in `src/videoinfo.*`:

- `VideoInfoService::lookup(id, Done)` is the real injectable frontend host
  interface. It has no backend DTO, generic fetch method, RPC or FFI dependency.
  Completion is once on the caller/UI thread, or cancelled on service destruction.
- `NetworkVideoInfo` is the production Qt Network implementation of that oEmbed
  operation. `src/main.cpp` selects it explicitly; an unconfigured singleton has
  no hidden network fallback. Tests install an offline service before rendering
  and do not read/write the production metadata cache.
- `VideoTitles` shares requests, guards late completions after host replacement
  or destruction, and retains up to 300 successes in insertion order. The local
  `AppDataLocation/media-info.json` is an ordered JSON array, atomically replaced
  with `QSaveFile`; it is not a migration of browser localStorage. Failures are
  not persisted. Unreadable/corrupt cache data is ignored. Save failure is advisory.
- Metadata is rendered as **plain text**. A service failure, missing service,
  title-less answer, missing author or missing thumbnail never fabricates a title.

Intentional native safety bounds beyond the reference: 64 KiB JSON; exact string
fields rather than coercing objects/numbers into labels; three redirects at most,
remaining on HTTPS `www.youtube.com/oembed` for the same video/query; no userinfo,
custom port, cookies, cache, authentication reuse or ambient proxy. The reference
uses the Electron network redirect defaults, has no explicit response-byte cap,
and checks its IPC sender. Native has no IPC entry point to authenticate. These
refusals retain the same visible fallback. No external backend is needed later
for this capability; a different embedding host can replace the service.

## Image resource policy and bounds

Global QML networking remains denied by `denyNetwork()`. Decoded images enter
only through `image://openghost-media/`. `MediaLoader` and the metadata service
use separate frontend-owned Qt network managers; neither grants QML general
network access nor exposes agent credentials.

Automatic image loads require the exact reference **HTTPS** preview patterns:
Bing thumbnails, YouTube thumbnails, Wikimedia uploads and encrypted Google
thumbnails. Other HTTP(S) images require a gallery click. Non-HTTP(S), URL
userinfo and invalid addresses refuse. Automatic redirects must remain trusted;
consented redirects may remain HTTP(S). Every hop is rechecked, at most three.

`media::get` owns the entire-exchange deadline (9 seconds for pictures, 10 for
metadata), not merely an inactivity timeout, and incrementally bounds bytes even
without Content-Length. Image capture is capped at **16 MiB**; no cookies,
authentication reuse or network cache. TLS errors are not ignored. A decoder
accepts PNG/JPEG/WebP/GIF (first frame), at most 8192 px per edge, scales display
pixels to 1600 px, and preserves natural dimensions for layout. Process image
results are bounded to 256 entries/192 MiB, unlike the reference gallery's own
size map/browser image cache. There is no automatic mutation/backend retry.

YouTube tries `hq720.jpg`, then `mqdefault.jpg`, on either a load error or a
natural width of at most 120 px (YouTube's absent-thumbnail stand-in). If neither
exists, the plate/play/duration disappear; title/author/link remain. Image and
metadata failure are independent.

## Qualification and remaining boundaries

See [visual-parity.md](visual-parity.md#replymedia-completion-after-dc0a9e04) for
all media fixtures, separate component/full-window measurements and startup-crash
evidence. `tests/media.cpp` covers parsing, policy, metadata/cache lifecycle,
loopback redirects/deadlines/byte limits and QML denial. `tests/smoke.cpp` exercises
real QML consent, leafing, source changes, thumbnail fallback, late titles,
explicit-title precedence, deduplication/card identity and rendered surfaces.
`tests/parity/run.py --media` runs only the relevant media/reference inventory.

Known remaining differences are explicit, not hidden passes:

- Qt/CSS blur kernels, image sampling, mask edges and glyph metrics differ;
  long-title wrapping/elision can differ. No exact-pixel or all-GPU claim.
- Source chips/YouTube metadata retain the native local globe. The cross-cutting
  `link-chip.js` DuckDuckGo favicon lookup (including its host/base-host fallback)
  is still unported, not silently enabled through the image trust list. Parity
  fixtures block favicons on both sides and therefore exercise the real globe
  fallback. Audit row L03 remains unchanged.
- Sent image/video attachments, file preparation, browser tools and browser
  downloads are separate, unimplemented paths. The sent-image parity fixture
  deliberately shows that boundary; it is not counted as a completed reply card.
- Production HTTPS is implemented, but tests are offline/local: they do not
  establish YouTube availability from every network or validate real billing/
  backend behavior. The pre-existing Qt Shapes startup crash is not repaired here.
