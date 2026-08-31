# The Firefox gap, one test file at a time

**Generated** by `tools/wpt/firefox-gap.py`. Do not edit by hand.

Firefox version: 157.0a1
Firefox run date: 2026-08-31
Firefox's WPT revision: `7e755825e0b5`
Our pinned WPT revision: `4120ac0deb57`

The unit is a **test file**, and a file counts as passed only when every
subtest in it passed. That is the one comparison that is the same
measurement on both sides: a subtest *rate* cannot be compared across
engines, because a test that dies before `done()` contributes zero
subtests to its own denominator and its full count to Firefox's. Read
`docs/wpt-firefox-ceiling.md` with that in mind -- and read the
`blocked` column here first, because a blocked file is plumbing rather
than a specification gap.

## Where this browser is

| | testharness files | reftest files | total |
|---|--:|--:|--:|
| in our scope | 23146 | 20998 | 44144 |
| Firefox passes | 19232 | 17918 | 37150 |
| **we pass** | **5111** | **7609** | **12720** |
| **Firefox passes, we fail** | **14121** | **10309** | **24430** |
| both fail | 3442 | 2207 | 5649 |
| we pass, Firefox fails (audit) | 332 | 796 | 1128 |

**34.2% of what Firefox passes.** Both halves of the suite are in that number as of
task F9 (2026-08-17). Before it, every reftest counted as a failure --
which was the honest reading while nothing had ever recorded one, because
a format that writes down only failures cannot tell a pass from a test
nobody ran.

### Why the testharness gap fails

| cause | files |
|---|--:|
| the harness never reported (TIMEOUT/ERROR/CRASH) | 5701 |
| subtests ran and failed | 8420 |

A blocked file is worth more than its count: none of its subtests are
in any denominator anywhere, so it is invisible in every rate.

### How to read the reftest column

A reftest renders two pages and compares the pixels, so it has no
subtests, no harness status and no `blocked`/`feature` distinction: it
agrees with its reference or it does not. Two things about the number:

- **Two blank pages agree exactly.** A reference that failed to load
  therefore passes against any test at all, and the run counts those
  separately -- `microbrowser_wpt --reftests-only` closes with
  `N reftests: M passed, K of those with both pages blank`. K was 757 of
  7,394 when this was first measured. They are not deducted, because
  wptrunner compares screenshots without asking what is on them and a
  rule of our own would make the two sides incomparable; some of them
  are real, since a reftest whose point is that nothing is visible
  passes blank in every engine.
- **About eight of 20,998 are intermittent**, measured over two full
  runs of the same binary. Seven of the eight are `@font-face` tests
  whose font this browser never loads and one is an animation; the
  runner's `--retries` smooths a disagreement but not a recording run.

## Ranked: test files Firefox passes and we do not

`gap` is the whole of it. `blocked` is where the harness never reported
and `feature` is where it reported and subtests failed -- both of those
are testharness columns. `reftest` is the pixel half, which has neither
distinction. A refused area is a decision with a name
(`docs/wpt-refusals.tsv`) and its row is marked.

| area | gap | blocked | feature | reftest | we pass | firefox passes | in scope | refusal |
|---|--:|--:|--:|--:|--:|--:|--:|---|
| `css/CSS2` | 3094 | 2 | 23 | 3069 | 3053 | 6147 | 6279 |  |
| `html/canvas` | 2278 | 974 | 1255 | 49 | 879 | 3157 | 3831 |  |
| `html/semantics` | 1637 | 513 | 1007 | 117 | 434 | 2071 | 2635 |  |
| `css/css-grid` | 1102 | 305 | 187 | 610 | 255 | 1357 | 2253 |  |
| `css/css-writing-modes` | 1063 | 8 | 51 | 1004 | 128 | 1191 | 1222 |  |
| `referrer-policy/gen` | 951 | 865 | 86 | 0 | 0 | 951 | 1001 |  |
| `css/css-text` | 909 | 98 | 46 | 765 | 632 | 1541 | 1915 |  |
| `css/css-flexbox` | 843 | 4 | 221 | 618 | 433 | 1276 | 1369 |  |
| `css/css-transforms` | 575 | 2 | 78 | 495 | 304 | 879 | 896 |  |
| `html/browsers` | 551 | 298 | 251 | 2 | 60 | 611 | 757 | **partial** ADR 0026 §6 -- window.open returns null, opener absent |
| `css/css-backgrounds` | 529 | 0 | 79 | 450 | 237 | 766 | 848 |  |
| `css/css-sizing` | 433 | 8 | 71 | 354 | 149 | 582 | 745 |  |
| `websockets` | 364 | 285 | 79 | 0 | 6 | 370 | 382 | **partial** infrastructure -- our server does not speak the WebSocket protocol |
| `css/css-multicol` | 326 | 2 | 42 | 282 | 125 | 451 | 561 |  |
| `css/css-shapes` | 316 | 17 | 92 | 207 | 30 | 346 | 375 |  |
| `css/css-images` | 308 | 0 | 20 | 288 | 121 | 429 | 501 |  |
| `css/css-fonts` | 305 | 10 | 101 | 194 | 160 | 465 | 536 |  |
| `xhr` | 296 | 29 | 267 | 0 | 87 | 383 | 398 |  |
| `svg/animations` | 280 | 37 | 230 | 13 | 6 | 286 | 305 |  |
| `html/webappapis` | 279 | 121 | 158 | 0 | 32 | 311 | 343 |  |
| `css/css-color` | 255 | 0 | 30 | 225 | 90 | 345 | 369 |  |
| `IndexedDB` | 252 | 41 | 211 | 0 | 12 | 264 | 265 | **partial** ADR 0021/0038 -- memory tier only, no disk persistence by default |
| `css/css-values` | 243 | 14 | 127 | 102 | 72 | 315 | 479 |  |
| `css/css-align` | 214 | 2 | 180 | 32 | 35 | 249 | 296 |  |
| `css/selectors` | 214 | 22 | 119 | 73 | 260 | 474 | 503 | **partial** ADR 0012/0016/0033 -- :visited matches nothing (privacy) |
| `html/rendering` | 212 | 20 | 82 | 110 | 212 | 424 | 465 |  |
| `css/css-conditional` | 208 | 119 | 28 | 61 | 99 | 307 | 395 |  |
| `css/css-position` | 208 | 2 | 60 | 146 | 82 | 290 | 362 |  |
| `html/dom` | 198 | 15 | 100 | 83 | 105 | 303 | 364 |  |
| `dom/nodes` | 188 | 55 | 131 | 2 | 120 | 308 | 330 |  |
| `content-security-policy/gen` | 184 | 184 | 0 | 0 | 0 | 184 | 260 |  |
| `css/css-text-decor` | 183 | 0 | 30 | 153 | 129 | 312 | 355 |  |
| `html/syntax` | 181 | 124 | 48 | 9 | 183 | 364 | 400 |  |
| `css/css-overflow` | 176 | 6 | 63 | 107 | 133 | 309 | 720 |  |
| `css/css-ui` | 175 | 1 | 55 | 119 | 824 | 999 | 1069 |  |
| `css/css-tables` | 173 | 2 | 77 | 94 | 83 | 256 | 300 |  |
| `fetch/api` | 152 | 13 | 139 | 0 | 30 | 182 | 223 |  |
| `css/css-variables` | 132 | 12 | 26 | 94 | 95 | 227 | 241 |  |
| `css/cssom-view` | 119 | 13 | 101 | 5 | 71 | 190 | 236 |  |
| `css/cssom` | 112 | 19 | 89 | 4 | 69 | 181 | 202 |  |
| `dom/events` | 111 | 24 | 87 | 0 | 65 | 176 | 202 |  |
| `referrer-policy/4K+1` | 108 | 65 | 43 | 0 | 0 | 108 | 108 |  |
| `referrer-policy/4K-1` | 108 | 64 | 44 | 0 | 0 | 108 | 108 |  |
| `referrer-policy/4K` | 108 | 64 | 44 | 0 | 0 | 108 | 108 |  |
| `css/css-animations` | 105 | 7 | 79 | 19 | 31 | 136 | 155 |  |
| `upgrade-insecure-requests/gen` | 99 | 0 | 99 | 0 | 0 | 99 | 196 |  |
| `css/css-cascade` | 98 | 5 | 56 | 37 | 32 | 130 | 146 |  |
| `css/css-transitions` | 93 | 18 | 73 | 2 | 18 | 111 | 126 |  |
| `svg/painting` | 93 | 59 | 6 | 28 | 35 | 128 | 156 |  |
| `svg/types` | 83 | 26 | 57 | 0 | 0 | 83 | 86 |  |
| `intersection-observer` | 78 | 21 | 56 | 1 | 13 | 91 | 107 |  |
| `css/css-display` | 76 | 3 | 8 | 65 | 76 | 152 | 246 |  |
| `html/interaction` | 76 | 8 | 68 | 0 | 14 | 90 | 194 |  |
| `eventsource` | 75 | 37 | 38 | 0 | 1 | 76 | 76 |  |
| `svg/geometry` | 75 | 42 | 12 | 21 | 4 | 79 | 86 |  |
| `workers` | 71 | 28 | 43 | 0 | 28 | 99 | 111 | **partial** ADR 0022 §1 -- SharedWorker refused; DedicatedWorker implemented |
| `resource-timing` | 70 | 35 | 35 | 0 | 8 | 78 | 102 |  |
| `fetch/metadata` | 66 | 34 | 32 | 0 | 0 | 66 | 87 |  |
| `html/editing` | 65 | 25 | 32 | 8 | 38 | 103 | 241 |  |
| `html/cross-origin-embedder-policy` | 61 | 27 | 34 | 0 | 0 | 61 | 88 |  |
| `content-security-policy/script-src` | 57 | 37 | 20 | 0 | 24 | 81 | 90 |  |
| `media-source` | 57 | 19 | 38 | 0 | 2 | 59 | 72 |  |
| `shadow-dom` | 54 | 6 | 41 | 7 | 24 | 78 | 83 |  |
| `custom-elements/reactions` | 52 | 0 | 52 | 0 | 4 | 56 | 57 |  |
| `webmessaging` | 50 | 34 | 16 | 0 | 7 | 57 | 60 |  |
| `html/cross-origin-opener-policy` | 46 | 4 | 42 | 0 | 53 | 99 | 157 |  |
| `navigation-timing` | 42 | 28 | 14 | 0 | 6 | 48 | 58 |  |
| `html/infrastructure` | 41 | 15 | 26 | 0 | 55 | 96 | 118 |  |
| `webstorage` | 39 | 25 | 14 | 0 | 10 | 49 | 54 |  |
| `custom-elements` | 38 | 7 | 30 | 1 | 4 | 42 | 45 |  |
| `custom-elements/registries` | 37 | 9 | 28 | 0 | 1 | 38 | 40 |  |
| `web-animations/interfaces` | 36 | 7 | 29 | 0 | 0 | 36 | 42 |  |
| `shadow-dom/focus` | 35 | 1 | 32 | 2 | 4 | 39 | 40 |  |
| `svg/linking` | 35 | 13 | 4 | 18 | 23 | 58 | 63 |  |
| `svg/text` | 35 | 18 | 9 | 8 | 37 | 72 | 97 |  |
| `selection` | 32 | 4 | 28 | 0 | 38 | 70 | 90 |  |
| `shadow-dom/untriaged` | 32 | 1 | 27 | 4 | 25 | 57 | 59 |  |
| `svg/path` | 32 | 7 | 21 | 4 | 13 | 45 | 65 |  |
| `referrer-policy/generic` | 31 | 17 | 14 | 0 | 0 | 31 | 42 |  |
| `workers/interfaces` | 30 | 9 | 21 | 0 | 34 | 64 | 65 |  |
| `content-security-policy/frame-ancestors` | 29 | 24 | 5 | 0 | 5 | 34 | 34 |  |
| `css/css-box` | 29 | 0 | 28 | 1 | 8 | 37 | 146 |  |
| `content-security-policy/connect-src` | 28 | 0 | 28 | 0 | 0 | 28 | 30 |  |
| `performance-timeline` | 28 | 18 | 10 | 0 | 17 | 45 | 58 |  |
| `web-animations/timing-model` | 28 | 4 | 22 | 2 | 4 | 32 | 34 |  |
| `workers/constructors` | 28 | 5 | 23 | 0 | 7 | 35 | 35 |  |
| `png/apng` | 28 | 0 | 0 | 28 | 1 | 29 | 29 |  |
| `content-security-policy/worker-src` | 27 | 10 | 17 | 0 | 3 | 30 | 31 |  |
| `content-security-policy/style-src` | 26 | 14 | 12 | 0 | 12 | 38 | 42 |  |
| `domxpath` | 26 | 3 | 23 | 0 | 0 | 26 | 32 |  |
| `svg/struct` | 26 | 2 | 5 | 19 | 24 | 50 | 67 |  |
| `content-security-policy/reporting` | 25 | 19 | 6 | 0 | 1 | 26 | 31 |  |
| `cors` | 25 | 8 | 17 | 0 | 0 | 25 | 27 |  |
| `referrer-policy/css-integration` | 24 | 8 | 16 | 0 | 0 | 24 | 24 |  |
| `resize-observer` | 24 | 10 | 12 | 2 | 13 | 37 | 38 |  |
| `workers/semantics` | 24 | 13 | 11 | 0 | 4 | 28 | 30 |  |
| `content-security-policy/unsafe-hashes` | 23 | 23 | 0 | 0 | 1 | 24 | 24 |  |
| `web-animations/animation-model` | 23 | 0 | 18 | 5 | 3 | 26 | 31 |  |
| `web-animations/responsive` | 23 | 0 | 22 | 1 | 6 | 29 | 46 |  |
| `webmessaging/with-ports` | 23 | 13 | 10 | 0 | 1 | 24 | 24 |  |
| `webmessaging/without-ports` | 23 | 13 | 10 | 0 | 4 | 27 | 27 |  |
| `fetch/compression-dictionary` | 22 | 3 | 19 | 0 | 1 | 23 | 31 |  |
| `svg/coordinate-systems` | 22 | 1 | 7 | 14 | 4 | 26 | 26 |  |
| `svg/shapes` | 22 | 6 | 0 | 16 | 13 | 35 | 36 |  |
| `svg/styling` | 22 | 2 | 9 | 11 | 39 | 61 | 82 |  |
| `css/css-syntax` | 20 | 0 | 20 | 0 | 16 | 36 | 41 |  |
| `encoding` | 20 | 13 | 7 | 0 | 52 | 72 | 78 |  |
| `shadow-dom/focus-navigation` | 20 | 0 | 20 | 0 | 3 | 23 | 45 |  |
| `content-security-policy/securitypolicyviolation` | 19 | 17 | 2 | 0 | 0 | 19 | 24 |  |
| `svg/interact` | 19 | 12 | 7 | 0 | 7 | 26 | 34 |  |
| `xhr/formdata` | 19 | 2 | 17 | 0 | 8 | 27 | 27 |  |
| `content-security-policy/inheritance` | 18 | 16 | 2 | 0 | 2 | 20 | 26 |  |
| `storage` | 18 | 1 | 17 | 0 | 0 | 18 | 28 |  |
| `workers/modules` | 18 | 7 | 11 | 0 | 0 | 18 | 25 |  |
| `content-security-policy/generic` | 17 | 15 | 2 | 0 | 9 | 26 | 29 |  |
| `streams/readable-streams` | 17 | 1 | 16 | 0 | 0 | 17 | 22 |  |
| `content-security-policy/sandbox` | 16 | 3 | 13 | 0 | 0 | 16 | 17 |  |
| `streams/writable-streams` | 16 | 0 | 16 | 0 | 0 | 16 | 16 |  |
| `FileAPI/url` | 15 | 12 | 2 | 1 | 0 | 15 | 16 |  |
| `custom-elements/form-associated` | 15 | 1 | 12 | 2 | 2 | 17 | 20 |  |
| `svg/extensibility` | 14 | 1 | 2 | 11 | 9 | 23 | 25 |  |
| `svg/pservers` | 14 | 9 | 3 | 2 | 22 | 36 | 37 |  |
| `uievents/order-of-events` | 14 | 7 | 7 | 0 | 2 | 16 | 16 |  |
| `webmessaging/broadcastchannel` | 14 | 6 | 8 | 0 | 0 | 14 | 14 |  |
| `domparsing` | 13 | 3 | 10 | 0 | 15 | 28 | 34 | **partial** ADR 0012 -- DOMParser absent -- it returns a second Document |
| `html/the-xhtml-syntax` | 13 | 13 | 0 | 0 | 2 | 15 | 15 |  |
| `webmessaging/message-channels` | 13 | 2 | 11 | 0 | 4 | 17 | 23 |  |
| `content-security-policy/img-src` | 12 | 11 | 1 | 0 | 3 | 15 | 15 |  |
| `encoding/streams` | 12 | 1 | 11 | 0 | 0 | 12 | 13 |  |
| `html/obsolete` | 12 | 0 | 12 | 0 | 4 | 16 | 16 |  |
| `streams/piping` | 12 | 0 | 12 | 0 | 0 | 12 | 13 |  |
| `streams/transferable` | 12 | 4 | 8 | 0 | 0 | 12 | 12 |  |
| `uievents/mouse` | 12 | 5 | 7 | 0 | 7 | 19 | 22 |  |
| `content-security-policy/form-action` | 11 | 8 | 3 | 0 | 1 | 12 | 13 |  |
| `cookies/prefix` | 11 | 6 | 5 | 0 | 0 | 11 | 11 |  |
| `fetch/content-encoding` | 11 | 0 | 11 | 0 | 2 | 13 | 13 |  |
| `streams/readable-byte-streams` | 11 | 1 | 10 | 0 | 0 | 11 | 11 |  |
| `user-timing` | 11 | 9 | 2 | 0 | 47 | 58 | 58 |  |
| `content-security-policy/media-src` | 10 | 9 | 1 | 0 | 0 | 10 | 10 |  |
| `content-security-policy/unsafe-eval` | 10 | 1 | 9 | 0 | 1 | 11 | 15 |  |
| `content-security-policy/wasm-unsafe-eval` | 10 | 2 | 8 | 0 | 0 | 10 | 10 |  |
| `dom/collections` | 10 | 0 | 10 | 0 | 0 | 10 | 10 |  |
| `dom/ranges` | 10 | 5 | 5 | 0 | 27 | 37 | 58 |  |
| `dom/traversal` | 10 | 0 | 10 | 0 | 7 | 17 | 18 |  |
| `encoding/legacy-mb-japanese` | 10 | 10 | 0 | 0 | 472 | 482 | 482 |  |
| `eventsource/dedicated-worker` | 10 | 1 | 9 | 0 | 0 | 10 | 10 |  |
| `hr-time` | 10 | 4 | 6 | 0 | 4 | 14 | 15 |  |
| `html/document-isolation-policy` | 10 | 0 | 10 | 0 | 0 | 10 | 38 |  |
| `svg/layout` | 10 | 0 | 0 | 10 | 2 | 12 | 12 |  |
| `content-security-policy/frame-src` | 9 | 7 | 2 | 0 | 0 | 9 | 12 |  |
| `content-security-policy/object-src` | 9 | 9 | 0 | 0 | 0 | 9 | 13 |  |
| `content-security-policy/reporting-api` | 9 | 8 | 1 | 0 | 1 | 10 | 11 |  |
| `custom-elements/parser` | 9 | 3 | 6 | 0 | 2 | 11 | 11 |  |
| `fetch/range` | 9 | 1 | 8 | 0 | 0 | 9 | 11 |  |
| `html/user-activation` | 9 | 8 | 1 | 0 | 1 | 10 | 21 |  |
| `selection/shadow-dom` | 9 | 0 | 9 | 0 | 8 | 17 | 22 |  |
| `shadow-dom/declarative` | 9 | 2 | 7 | 0 | 12 | 21 | 56 |  |
| `svg/scripted` | 9 | 6 | 3 | 0 | 1 | 10 | 10 |  |
| `webmessaging/with-options` | 9 | 7 | 2 | 0 | 0 | 9 | 9 |  |
| `fetch/corb` | 8 | 3 | 2 | 3 | 10 | 18 | 22 |  |
| `fetch/http-cache` | 8 | 0 | 7 | 1 | 0 | 8 | 15 |  |
| `fetch/local-network-access` | 8 | 8 | 0 | 0 | 0 | 8 | 16 |  |
| `streams/transform-streams` | 8 | 0 | 8 | 0 | 0 | 8 | 12 |  |
| `svg/embedded` | 8 | 1 | 1 | 6 | 16 | 24 | 26 |  |
| `websockets/interfaces` | 8 | 2 | 6 | 0 | 160 | 168 | 168 |  |
| `FileAPI/file` | 7 | 0 | 7 | 0 | 14 | 21 | 21 |  |
| `content-security-policy/nonce-hiding` | 7 | 5 | 2 | 0 | 1 | 8 | 8 |  |
| `content-security-policy/style-src-attr-elem` | 7 | 4 | 3 | 0 | 0 | 7 | 7 |  |
| `cookies/partitioned-cookies` | 7 | 6 | 1 | 0 | 0 | 7 | 7 |  |
| `eventsource/shared-worker` | 7 | 0 | 7 | 0 | 0 | 7 | 7 |  |
| `fetch/cross-origin-resource-policy` | 7 | 2 | 5 | 0 | 4 | 11 | 12 |  |
| `svg/render` | 7 | 0 | 0 | 7 | 3 | 10 | 12 |  |
| `uievents/textInput` | 7 | 0 | 7 | 0 | 0 | 7 | 7 |  |
| `workers/baseurl` | 7 | 4 | 3 | 0 | 0 | 7 | 7 |  |
| `content-security-policy/blob` | 6 | 0 | 6 | 0 | 0 | 6 | 6 |  |
| `content-security-policy/navigation` | 6 | 6 | 0 | 0 | 0 | 6 | 6 |  |
| `content-security-policy/script-src-attr-elem` | 6 | 5 | 1 | 0 | 2 | 8 | 8 |  |
| `cookies/attributes` | 6 | 4 | 2 | 0 | 0 | 6 | 9 |  |
| `cookies/samesite` | 6 | 6 | 0 | 0 | 0 | 6 | 22 |  |
| `cookies/secure` | 6 | 3 | 3 | 0 | 0 | 6 | 6 |  |
| `custom-elements/upgrading` | 6 | 0 | 6 | 0 | 0 | 6 | 7 |  |
| `dom` | 6 | 0 | 5 | 1 | 4 | 10 | 11 |  |
| `fetch/nosniff` | 6 | 3 | 3 | 0 | 0 | 6 | 6 |  |
| `content-security-policy/font-src` | 5 | 5 | 0 | 0 | 0 | 5 | 5 |  |
| `cookies/domain` | 5 | 5 | 0 | 0 | 0 | 5 | 5 |  |
| `custom-elements/state` | 5 | 1 | 4 | 0 | 0 | 5 | 5 |  |
| `dom/abort` | 5 | 2 | 3 | 0 | 5 | 10 | 10 |  |
| `encoding/legacy-mb-tchinese` | 5 | 0 | 5 | 0 | 278 | 283 | 283 |  |
| `html/anonymous-iframe` | 5 | 1 | 4 | 0 | 0 | 5 | 33 |  |
| `resource-timing/initiator-type` | 5 | 0 | 5 | 0 | 4 | 9 | 16 |  |
| `selection/contenteditable` | 5 | 0 | 5 | 0 | 5 | 10 | 12 |  |
| `selection/textcontrols` | 5 | 0 | 5 | 0 | 4 | 9 | 9 |  |
| `content-security-policy/default-src` | 4 | 1 | 3 | 0 | 0 | 4 | 4 |  |
| `content-security-policy/meta` | 4 | 0 | 4 | 0 | 0 | 4 | 5 |  |
| `content-security-policy/svg` | 4 | 1 | 3 | 0 | 1 | 5 | 6 |  |
| `fetch/security` | 4 | 1 | 3 | 0 | 0 | 4 | 11 |  |
| `fetch/stale-while-revalidate` | 4 | 2 | 2 | 0 | 0 | 4 | 6 |  |
| `png` | 4 | 3 | 0 | 1 | 0 | 4 | 4 |  |
| `uievents/click` | 4 | 4 | 0 | 0 | 3 | 7 | 7 |  |
| `uievents/keyboard` | 4 | 1 | 3 | 0 | 1 | 5 | 5 |  |
| `webmessaging/multi-globals` | 4 | 0 | 4 | 0 | 0 | 4 | 4 |  |
| `FileAPI` | 3 | 2 | 1 | 0 | 4 | 7 | 11 |  |
| `FileAPI/BlobURL` | 3 | 1 | 2 | 0 | 0 | 3 | 5 |  |
| `FileAPI/blob` | 3 | 0 | 3 | 0 | 19 | 22 | 23 |  |
| `content-security-policy/base-uri` | 3 | 1 | 2 | 0 | 2 | 5 | 6 |  |
| `content-security-policy/child-src` | 3 | 3 | 0 | 0 | 2 | 5 | 9 |  |
| `content-security-policy/inside-worker` | 3 | 1 | 2 | 0 | 0 | 3 | 10 |  |
| `content-security-policy/resource-hints` | 3 | 3 | 0 | 0 | 0 | 3 | 9 |  |
| `content-security-policy/webrtc` | 3 | 0 | 3 | 0 | 0 | 3 | 5 |  |
| `content-security-policy/xslt` | 3 | 1 | 2 | 0 | 0 | 3 | 3 |  |
| `fetch/content-length` | 3 | 1 | 2 | 0 | 0 | 3 | 5 |  |
| `fetch/data-urls` | 3 | 0 | 3 | 0 | 0 | 3 | 3 |  |
| `fetch/orb` | 3 | 3 | 0 | 0 | 6 | 9 | 19 |  |
| `mimesniff/sniffing` | 3 | 2 | 1 | 0 | 0 | 3 | 3 |  |
| `selection/bidi` | 3 | 0 | 3 | 0 | 0 | 3 | 3 |  |
| `shadow-dom/leaktests` | 3 | 0 | 3 | 0 | 0 | 3 | 4 |  |
| `streams` | 3 | 1 | 2 | 0 | 0 | 3 | 3 | **partial** ADR 0020 §1 -- new ReadableStream({start}) is an illegal constructor |
| `subresource-integrity/unencoded-digest` | 3 | 1 | 2 | 0 | 0 | 3 | 9 |  |
| `svg/fonts` | 3 | 0 | 3 | 0 | 1 | 4 | 4 |  |
| `uievents/interface` | 3 | 2 | 1 | 0 | 0 | 3 | 3 |  |
| `content-security-policy/parsing` | 2 | 2 | 0 | 0 | 0 | 2 | 2 |  |
| `content-security-policy/plugin-types` | 2 | 2 | 0 | 0 | 0 | 2 | 2 |  |
| `cookies` | 2 | 0 | 2 | 0 | 1 | 3 | 3 |  |
| `cookies/path` | 2 | 2 | 0 | 0 | 0 | 2 | 2 |  |
| `custom-elements/htmlconstructor` | 2 | 0 | 2 | 0 | 0 | 2 | 2 |  |
| `fetch/content-type` | 2 | 1 | 1 | 0 | 0 | 2 | 5 |  |
| `fetch/h1-parsing` | 2 | 1 | 1 | 0 | 0 | 2 | 20 |  |
| `fetch/redirect-navigate` | 2 | 2 | 0 | 0 | 0 | 2 | 2 |  |
| `fetch/redirects` | 2 | 1 | 1 | 0 | 0 | 2 | 2 |  |
| `html/select` | 2 | 0 | 1 | 1 | 0 | 2 | 2 |  |
| `selection/caret` | 2 | 0 | 1 | 1 | 4 | 6 | 7 |  |
| `uievents/constructors` | 2 | 0 | 2 | 0 | 0 | 2 | 2 |  |
| `websockets/multi-globals` | 2 | 0 | 2 | 0 | 0 | 2 | 2 |  |
| `websockets/opening-handshake` | 2 | 1 | 1 | 0 | 12 | 14 | 14 |  |
| `workers/same-site-cookies` | 2 | 0 | 2 | 0 | 0 | 2 | 6 |  |
| `FileAPI/FileReader` | 1 | 1 | 0 | 0 | 1 | 2 | 2 |  |
| `content-security-policy/embedded-enforcement` | 1 | 1 | 0 | 0 | 1 | 2 | 23 |  |
| `cookies/encoding` | 1 | 1 | 0 | 0 | 0 | 1 | 1 |  |
| `cookies/ordering` | 1 | 0 | 1 | 0 | 0 | 1 | 1 |  |
| `cookies/origin-bound-cookies` | 1 | 0 | 1 | 0 | 0 | 1 | 2 |  |
| `cookies/samesite-none-secure` | 1 | 0 | 1 | 0 | 0 | 1 | 1 |  |
| `cookies/schemeful-same-site` | 1 | 1 | 0 | 0 | 0 | 1 | 4 |  |
| `cookies/size` | 1 | 1 | 0 | 0 | 0 | 1 | 2 |  |
| `dom/lists` | 1 | 0 | 1 | 0 | 3 | 4 | 5 |  |
| `fetch/images` | 1 | 1 | 0 | 0 | 0 | 1 | 1 |  |
| `fetch/origin` | 1 | 1 | 0 | 0 | 0 | 1 | 1 |  |
| `html/capability-delegation` | 1 | 1 | 0 | 0 | 0 | 1 | 6 |  |
| `html/embedded-content` | 1 | 0 | 1 | 0 | 0 | 1 | 1 |  |
| `html/links` | 1 | 0 | 1 | 0 | 3 | 4 | 6 |  |
| `html/meta` | 1 | 1 | 0 | 0 | 0 | 1 | 1 |  |
| `html/scripting` | 1 | 0 | 1 | 0 | 1 | 2 | 2 |  |
| `intersection-observer/v2` | 1 | 0 | 1 | 0 | 1 | 2 | 38 |  |
| `mimesniff/media` | 1 | 1 | 0 | 0 | 0 | 1 | 1 |  |
| `selection/anonymous` | 1 | 0 | 1 | 0 | 2 | 3 | 3 |  |
| `shadow-dom/reference-target` | 1 | 0 | 1 | 0 | 0 | 1 | 15 |  |
| `subresource-integrity/integrity-policy` | 1 | 0 | 1 | 0 | 4 | 5 | 8 |  |
| `subresource-integrity` | 1 | 1 | 0 | 0 | 0 | 1 | 1 |  |
| `svg` | 1 | 0 | 1 | 0 | 1 | 2 | 3 |  |
| `uievents` | 1 | 0 | 1 | 0 | 1 | 2 | 3 |  |
| `uievents/legacy-domevents-tests` | 1 | 1 | 0 | 0 | 3 | 4 | 4 |  |
| `uievents/legacy` | 1 | 0 | 1 | 0 | 0 | 1 | 1 |  |
| `url` | 1 | 1 | 0 | 0 | 45 | 46 | 77 |  |
| `websockets/cookies` | 1 | 1 | 0 | 0 | 19 | 20 | 21 |  |
| `websockets/unload-a-document` | 1 | 1 | 0 | 0 | 6 | 7 | 11 |  |
| `workers/multi-globals` | 1 | 0 | 1 | 0 | 0 | 1 | 1 |  |
| `svg/as-image` | 1 | 0 | 0 | 1 | 0 | 1 | 1 |  |
| `svg/print` | 1 | 0 | 0 | 1 | 0 | 1 | 1 |  |

### Refusals that apply across these rows

From `docs/wpt-refusals.tsv`. Every one is *partial*: it names failures
inside an area whose other tests are ordinary bugs, so none of these is
a reason to leave an area alone.

| area | kind | what is refused |
|---|---|---|
| `IndexedDB` | **partial** | ADR 0021/0038 -- memory tier only, no disk persistence by default |
| `content-security-policy` | **partial** | ADR 0020 §3 -- report-uri/report-to/Report-Only not implemented |
| `css/selectors` | **partial** | ADR 0012/0016/0033 -- :visited matches nothing (privacy) |
| `domparsing` | **partial** | ADR 0012 -- DOMParser absent -- it returns a second Document |
| `html` | **partial** | ADR 0011/0012/0026 -- document.write deliberately unimplemented |
| `html/browsers` | **partial** | ADR 0026 §6 -- window.open returns null, opener absent |
| `streams` | **partial** | ADR 0020 §1 -- new ReadableStream({start}) is an illegal constructor |
| `websockets` | **partial** | infrastructure -- our server does not speak the WebSocket protocol |
| `workers` | **partial** | ADR 0022 §1 -- SharedWorker refused; DedicatedWorker implemented |

## Areas with no gap left

| area | we pass | firefox passes |
|---|--:|--:|
| `FileAPI/filelist-section` | 1 | 1 |
| `FileAPI/reading-data-section` | 26 | 26 |
| `IndexedDB/crashtests` | 1 | 1 |
| `console` | 14 | 14 |
| `domparsing/tentative` | 1 | 1 |
| `encoding/legacy-mb-korean` | 435 | 435 |
| `encoding/legacy-mb-schinese` | 6 | 6 |
| `fetch/fetch-later` | 2 | 2 |
| `mimesniff/mime-types` | 1 | 1 |
| `png/errors` | 1 | 1 |
| `svg/svg-in-svg` | 1 | 1 |
| `svg/top-level-document` | 1 | 1 |
| `websockets/binary` | 12 | 12 |
| `websockets/closing-handshake` | 9 | 9 |
| `websockets/constructor` | 55 | 55 |
| `websockets/keeping-connection-open` | 3 | 3 |
| `websockets/security` | 4 | 4 |
| `workers/examples` | 2 | 2 |

## Audit: files we record as passing that Firefox fails

An expectation file records only failures, so absence means PASS -- and
a test that was never run is absent too. Every file here is either a
real divergence worth a comment or a test nobody has run; there is no
third possibility, and telling them apart needs one run.

| area | files |
|---|--:|
| `css/css-grid` | 371 |
| `html/editing` | 90 |
| `css/css-text` | 86 |
| `html/semantics` | 84 |
| `html/canvas` | 61 |
| `css/CSS2` | 45 |
| `url` | 30 |
| `css/css-box` | 29 |
| `css/css-text-decor` | 22 |
| `css/css-ui` | 22 |
| `css/css-overflow` | 19 |
| `html/syntax` | 18 |
| `fetch/h1-parsing` | 17 |
| `svg/text` | 17 |
| `css/css-fonts` | 16 |
| `selection` | 12 |
| `html/rendering` | 11 |
| `css/css-images` | 11 |
| `svg/painting` | 11 |
| `css/css-values` | 10 |
| `html/dom` | 9 |
| `css/css-multicol` | 8 |
| `css/css-transforms` | 8 |
| `svg/styling` | 8 |
| `css/css-flexbox` | 7 |

