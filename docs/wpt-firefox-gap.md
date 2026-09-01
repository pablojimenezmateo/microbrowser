# The Firefox gap, one test file at a time

**Generated** by `tools/wpt/firefox-gap.py`. Do not edit by hand.

Firefox version: 157.0a1
Firefox run date: 2026-09-01
Firefox's WPT revision: `10d18c6d8c6d`
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
| **we pass** | **5308** | **7608** | **12916** |
| **Firefox passes, we fail** | **13924** | **10310** | **24234** |
| both fail | 3435 | 2252 | 5687 |
| we pass, Firefox fails (audit) | 339 | 751 | 1090 |

**34.8% of what Firefox passes.** Both halves of the suite are in that number as of
task F9 (2026-08-17). Before it, every reftest counted as a failure --
which was the honest reading while nothing had ever recorded one, because
a format that writes down only failures cannot tell a pass from a test
nobody ran.

### Why the testharness gap fails

| cause | files |
|---|--:|
| the harness never reported (TIMEOUT/ERROR/CRASH) | 4060 |
| subtests ran and failed | 9864 |

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
| `css/CSS2` | 3090 | 0 | 25 | 3065 | 3057 | 6147 | 6279 |  |
| `html/canvas` | 2288 | 24 | 2202 | 62 | 869 | 3157 | 3831 |  |
| `html/semantics` | 1642 | 513 | 1007 | 122 | 429 | 2071 | 2635 |  |
| `css/css-grid` | 1104 | 33 | 455 | 616 | 253 | 1357 | 2253 |  |
| `css/css-writing-modes` | 1061 | 8 | 51 | 1002 | 130 | 1191 | 1222 |  |
| `referrer-policy/gen` | 951 | 865 | 86 | 0 | 0 | 951 | 1001 |  |
| `css/css-text` | 844 | 0 | 79 | 765 | 697 | 1541 | 1915 |  |
| `css/css-flexbox` | 805 | 3 | 212 | 590 | 471 | 1276 | 1369 |  |
| `css/css-transforms` | 562 | 2 | 77 | 483 | 317 | 879 | 896 |  |
| `html/browsers` | 551 | 298 | 251 | 2 | 60 | 611 | 757 | **partial** ADR 0026 §6 -- window.open returns null, opener absent |
| `css/css-backgrounds` | 516 | 0 | 78 | 438 | 250 | 766 | 848 |  |
| `css/css-sizing` | 435 | 7 | 72 | 356 | 147 | 582 | 745 |  |
| `websockets` | 364 | 285 | 79 | 0 | 6 | 370 | 382 | **partial** infrastructure -- our server does not speak the WebSocket protocol |
| `css/css-multicol` | 328 | 0 | 44 | 284 | 123 | 451 | 561 |  |
| `css/css-shapes` | 316 | 17 | 92 | 207 | 30 | 346 | 375 |  |
| `css/css-fonts` | 310 | 8 | 103 | 199 | 155 | 465 | 536 |  |
| `css/css-images` | 307 | 0 | 20 | 287 | 122 | 429 | 501 |  |
| `xhr` | 296 | 29 | 267 | 0 | 87 | 383 | 398 |  |
| `svg/animations` | 283 | 27 | 240 | 16 | 3 | 286 | 305 |  |
| `html/webappapis` | 279 | 121 | 158 | 0 | 32 | 311 | 343 |  |
| `css/css-color` | 253 | 0 | 30 | 223 | 92 | 345 | 369 |  |
| `IndexedDB` | 252 | 41 | 211 | 0 | 12 | 264 | 265 | **partial** ADR 0021/0038 -- memory tier only, no disk persistence by default |
| `css/css-values` | 241 | 14 | 127 | 100 | 74 | 315 | 479 |  |
| `css/css-align` | 214 | 0 | 182 | 32 | 35 | 249 | 296 |  |
| `css/selectors` | 212 | 22 | 117 | 73 | 262 | 474 | 503 | **partial** ADR 0012/0016/0033 -- :visited matches nothing (privacy) |
| `html/rendering` | 211 | 20 | 82 | 109 | 213 | 424 | 465 |  |
| `css/css-conditional` | 210 | 119 | 28 | 63 | 97 | 307 | 395 |  |
| `css/css-position` | 207 | 2 | 60 | 145 | 83 | 290 | 362 |  |
| `html/dom` | 200 | 15 | 100 | 85 | 103 | 303 | 364 |  |
| `content-security-policy/gen` | 184 | 184 | 0 | 0 | 0 | 184 | 260 |  |
| `css/css-text-decor` | 183 | 0 | 30 | 153 | 129 | 312 | 355 |  |
| `html/syntax` | 181 | 124 | 48 | 9 | 183 | 364 | 400 |  |
| `css/css-ui` | 176 | 1 | 55 | 120 | 823 | 999 | 1069 |  |
| `css/css-tables` | 171 | 0 | 79 | 92 | 85 | 256 | 300 |  |
| `css/css-overflow` | 166 | 6 | 63 | 97 | 143 | 309 | 720 |  |
| `dom/nodes` | 155 | 24 | 129 | 2 | 153 | 308 | 330 |  |
| `fetch/api` | 152 | 13 | 139 | 0 | 30 | 182 | 223 |  |
| `dom/events` | 137 | 18 | 119 | 0 | 39 | 176 | 202 |  |
| `css/css-variables` | 134 | 12 | 26 | 96 | 93 | 227 | 241 |  |
| `css/cssom-view` | 119 | 11 | 103 | 5 | 71 | 190 | 236 |  |
| `css/cssom` | 112 | 19 | 89 | 4 | 69 | 181 | 202 |  |
| `css/css-animations` | 108 | 7 | 79 | 22 | 28 | 136 | 155 |  |
| `referrer-policy/4K+1` | 108 | 65 | 43 | 0 | 0 | 108 | 108 |  |
| `referrer-policy/4K-1` | 108 | 64 | 44 | 0 | 0 | 108 | 108 |  |
| `referrer-policy/4K` | 108 | 64 | 44 | 0 | 0 | 108 | 108 |  |
| `upgrade-insecure-requests/gen` | 99 | 0 | 99 | 0 | 0 | 99 | 196 |  |
| `css/css-cascade` | 98 | 5 | 56 | 37 | 32 | 130 | 146 |  |
| `css/css-transitions` | 94 | 18 | 73 | 3 | 17 | 111 | 126 |  |
| `svg/types` | 83 | 11 | 72 | 0 | 0 | 83 | 86 |  |
| `html/interaction` | 76 | 8 | 68 | 0 | 14 | 90 | 194 |  |
| `eventsource` | 75 | 37 | 38 | 0 | 1 | 76 | 76 |  |
| `css/css-display` | 73 | 1 | 8 | 64 | 79 | 152 | 246 |  |
| `workers` | 71 | 28 | 43 | 0 | 28 | 99 | 111 | **partial** ADR 0022 §1 -- SharedWorker refused; DedicatedWorker implemented |
| `resource-timing` | 70 | 33 | 37 | 0 | 8 | 78 | 102 |  |
| `svg/painting` | 68 | 0 | 39 | 29 | 60 | 128 | 156 |  |
| `html/editing` | 67 | 25 | 32 | 10 | 36 | 103 | 241 |  |
| `svg/geometry` | 67 | 1 | 46 | 20 | 12 | 79 | 86 |  |
| `fetch/metadata` | 66 | 32 | 34 | 0 | 0 | 66 | 87 |  |
| `intersection-observer` | 66 | 5 | 60 | 1 | 25 | 91 | 107 |  |
| `html/cross-origin-embedder-policy` | 61 | 27 | 34 | 0 | 0 | 61 | 88 |  |
| `content-security-policy/script-src` | 57 | 37 | 20 | 0 | 24 | 81 | 90 |  |
| `media-source` | 57 | 19 | 38 | 0 | 2 | 59 | 72 |  |
| `shadow-dom` | 53 | 4 | 42 | 7 | 25 | 78 | 83 |  |
| `custom-elements/reactions` | 49 | 0 | 49 | 0 | 7 | 56 | 57 |  |
| `html/cross-origin-opener-policy` | 46 | 4 | 42 | 0 | 53 | 99 | 157 |  |
| `webmessaging` | 44 | 25 | 19 | 0 | 13 | 57 | 60 |  |
| `html/infrastructure` | 41 | 15 | 26 | 0 | 55 | 96 | 118 |  |
| `navigation-timing` | 39 | 22 | 17 | 0 | 9 | 48 | 58 |  |
| `webstorage` | 39 | 25 | 14 | 0 | 10 | 49 | 54 |  |
| `custom-elements` | 37 | 0 | 36 | 1 | 5 | 42 | 45 |  |
| `custom-elements/registries` | 37 | 8 | 29 | 0 | 1 | 38 | 40 |  |
| `selection` | 37 | 4 | 33 | 0 | 33 | 70 | 90 |  |
| `svg/linking` | 36 | 3 | 13 | 20 | 22 | 58 | 63 |  |
| `web-animations/interfaces` | 36 | 4 | 32 | 0 | 0 | 36 | 42 |  |
| `shadow-dom/focus` | 34 | 2 | 30 | 2 | 5 | 39 | 40 |  |
| `web-animations/timing-model` | 32 | 2 | 24 | 6 | 0 | 32 | 34 |  |
| `referrer-policy/generic` | 31 | 17 | 14 | 0 | 0 | 31 | 42 |  |
| `svg/path` | 30 | 0 | 26 | 4 | 15 | 45 | 65 |  |
| `svg/text` | 30 | 0 | 20 | 10 | 42 | 72 | 97 |  |
| `workers/interfaces` | 30 | 9 | 21 | 0 | 34 | 64 | 65 |  |
| `content-security-policy/frame-ancestors` | 29 | 24 | 5 | 0 | 5 | 34 | 34 |  |
| `css/css-box` | 29 | 0 | 28 | 1 | 8 | 37 | 146 |  |
| `content-security-policy/connect-src` | 28 | 0 | 28 | 0 | 0 | 28 | 30 |  |
| `workers/constructors` | 28 | 5 | 23 | 0 | 7 | 35 | 35 |  |
| `png/apng` | 28 | 0 | 0 | 28 | 1 | 29 | 29 |  |
| `content-security-policy/worker-src` | 27 | 10 | 17 | 0 | 3 | 30 | 31 |  |
| `content-security-policy/style-src` | 26 | 14 | 12 | 0 | 12 | 38 | 42 |  |
| `domxpath` | 26 | 2 | 24 | 0 | 0 | 26 | 32 |  |
| `web-animations/responsive` | 26 | 0 | 22 | 4 | 3 | 29 | 46 |  |
| `content-security-policy/reporting` | 25 | 19 | 6 | 0 | 1 | 26 | 31 |  |
| `cors` | 25 | 8 | 17 | 0 | 0 | 25 | 27 |  |
| `svg/struct` | 25 | 1 | 5 | 19 | 25 | 50 | 67 |  |
| `svg/styling` | 25 | 1 | 8 | 16 | 36 | 61 | 82 |  |
| `referrer-policy/css-integration` | 24 | 8 | 16 | 0 | 0 | 24 | 24 |  |
| `resize-observer` | 24 | 8 | 14 | 2 | 13 | 37 | 38 |  |
| `workers/semantics` | 24 | 13 | 11 | 0 | 4 | 28 | 30 |  |
| `content-security-policy/unsafe-hashes` | 23 | 23 | 0 | 0 | 1 | 24 | 24 |  |
| `shadow-dom/untriaged` | 23 | 1 | 18 | 4 | 34 | 57 | 59 |  |
| `svg/coordinate-systems` | 23 | 1 | 7 | 15 | 3 | 26 | 26 |  |
| `svg/shapes` | 23 | 0 | 6 | 17 | 12 | 35 | 36 |  |
| `web-animations/animation-model` | 23 | 0 | 18 | 5 | 3 | 26 | 31 |  |
| `webmessaging/with-ports` | 23 | 14 | 9 | 0 | 1 | 24 | 24 |  |
| `webmessaging/without-ports` | 23 | 14 | 9 | 0 | 4 | 27 | 27 |  |
| `fetch/compression-dictionary` | 22 | 3 | 19 | 0 | 1 | 23 | 31 |  |
| `performance-timeline` | 22 | 10 | 12 | 0 | 23 | 45 | 58 |  |
| `css/css-syntax` | 20 | 0 | 20 | 0 | 16 | 36 | 41 |  |
| `encoding` | 20 | 13 | 7 | 0 | 52 | 72 | 78 |  |
| `content-security-policy/securitypolicyviolation` | 19 | 17 | 2 | 0 | 0 | 19 | 24 |  |
| `shadow-dom/focus-navigation` | 19 | 0 | 19 | 0 | 4 | 23 | 45 |  |
| `xhr/formdata` | 19 | 2 | 17 | 0 | 8 | 27 | 27 |  |
| `content-security-policy/inheritance` | 18 | 16 | 2 | 0 | 2 | 20 | 26 |  |
| `storage` | 18 | 1 | 17 | 0 | 0 | 18 | 28 |  |
| `uievents/mouse` | 18 | 4 | 14 | 0 | 1 | 19 | 22 |  |
| `workers/modules` | 18 | 7 | 11 | 0 | 0 | 18 | 25 |  |
| `content-security-policy/generic` | 17 | 15 | 2 | 0 | 9 | 26 | 29 |  |
| `streams/readable-streams` | 17 | 0 | 17 | 0 | 0 | 17 | 22 |  |
| `svg/interact` | 17 | 4 | 12 | 1 | 9 | 26 | 34 |  |
| `content-security-policy/sandbox` | 16 | 3 | 13 | 0 | 0 | 16 | 17 |  |
| `streams/writable-streams` | 16 | 0 | 16 | 0 | 0 | 16 | 16 |  |
| `svg/embedded` | 16 | 1 | 1 | 14 | 8 | 24 | 26 |  |
| `FileAPI/url` | 15 | 8 | 6 | 1 | 0 | 15 | 16 |  |
| `custom-elements/form-associated` | 15 | 1 | 12 | 2 | 2 | 17 | 20 |  |
| `svg/extensibility` | 14 | 0 | 2 | 12 | 9 | 23 | 25 |  |
| `html/the-xhtml-syntax` | 13 | 13 | 0 | 0 | 2 | 15 | 15 |  |
| `webmessaging/broadcastchannel` | 13 | 5 | 8 | 0 | 1 | 14 | 14 |  |
| `content-security-policy/img-src` | 12 | 11 | 1 | 0 | 3 | 15 | 15 |  |
| `encoding/streams` | 12 | 1 | 11 | 0 | 0 | 12 | 13 |  |
| `html/obsolete` | 12 | 0 | 12 | 0 | 4 | 16 | 16 |  |
| `streams/piping` | 12 | 0 | 12 | 0 | 0 | 12 | 13 |  |
| `streams/transferable` | 12 | 4 | 8 | 0 | 0 | 12 | 12 |  |
| `content-security-policy/form-action` | 11 | 8 | 3 | 0 | 1 | 12 | 13 |  |
| `cookies/prefix` | 11 | 6 | 5 | 0 | 0 | 11 | 11 |  |
| `fetch/content-encoding` | 11 | 0 | 11 | 0 | 2 | 13 | 13 |  |
| `streams/readable-byte-streams` | 11 | 1 | 10 | 0 | 0 | 11 | 11 |  |
| `svg/pservers` | 11 | 1 | 8 | 2 | 25 | 36 | 37 |  |
| `svg/layout` | 11 | 0 | 0 | 11 | 1 | 12 | 12 |  |
| `content-security-policy/media-src` | 10 | 9 | 1 | 0 | 0 | 10 | 10 |  |
| `content-security-policy/unsafe-eval` | 10 | 1 | 9 | 0 | 1 | 11 | 15 |  |
| `content-security-policy/wasm-unsafe-eval` | 10 | 2 | 8 | 0 | 0 | 10 | 10 |  |
| `dom/collections` | 10 | 0 | 10 | 0 | 0 | 10 | 10 |  |
| `dom/ranges` | 10 | 2 | 8 | 0 | 27 | 37 | 58 |  |
| `dom/traversal` | 10 | 0 | 10 | 0 | 7 | 17 | 18 |  |
| `domparsing` | 10 | 0 | 10 | 0 | 18 | 28 | 34 | **partial** ADR 0012 -- DOMParser absent -- it returns a second Document |
| `encoding/legacy-mb-japanese` | 10 | 10 | 0 | 0 | 472 | 482 | 482 |  |
| `eventsource/dedicated-worker` | 10 | 1 | 9 | 0 | 0 | 10 | 10 |  |
| `html/document-isolation-policy` | 10 | 0 | 10 | 0 | 0 | 10 | 38 |  |
| `uievents/order-of-events` | 10 | 7 | 3 | 0 | 6 | 16 | 16 |  |
| `content-security-policy/frame-src` | 9 | 7 | 2 | 0 | 0 | 9 | 12 |  |
| `content-security-policy/object-src` | 9 | 9 | 0 | 0 | 0 | 9 | 13 |  |
| `content-security-policy/reporting-api` | 9 | 8 | 1 | 0 | 1 | 10 | 11 |  |
| `custom-elements/parser` | 9 | 2 | 7 | 0 | 2 | 11 | 11 |  |
| `fetch/range` | 9 | 1 | 8 | 0 | 0 | 9 | 11 |  |
| `hr-time` | 9 | 2 | 7 | 0 | 5 | 14 | 15 |  |
| `html/user-activation` | 9 | 8 | 1 | 0 | 1 | 10 | 21 |  |
| `selection/contenteditable` | 9 | 0 | 9 | 0 | 1 | 10 | 12 |  |
| `selection/shadow-dom` | 9 | 0 | 9 | 0 | 8 | 17 | 22 |  |
| `shadow-dom/declarative` | 9 | 2 | 7 | 0 | 12 | 21 | 56 |  |
| `webmessaging/message-channels` | 9 | 3 | 6 | 0 | 8 | 17 | 23 |  |
| `webmessaging/with-options` | 9 | 7 | 2 | 0 | 0 | 9 | 9 |  |
| `fetch/corb` | 8 | 3 | 2 | 3 | 10 | 18 | 22 |  |
| `fetch/http-cache` | 8 | 0 | 7 | 1 | 0 | 8 | 15 |  |
| `fetch/local-network-access` | 8 | 8 | 0 | 0 | 0 | 8 | 16 |  |
| `streams/transform-streams` | 8 | 0 | 8 | 0 | 0 | 8 | 12 |  |
| `svg/render` | 8 | 0 | 0 | 8 | 2 | 10 | 12 |  |
| `svg/scripted` | 8 | 0 | 8 | 0 | 2 | 10 | 10 |  |
| `user-timing` | 8 | 2 | 6 | 0 | 50 | 58 | 58 |  |
| `websockets/interfaces` | 8 | 2 | 6 | 0 | 160 | 168 | 168 |  |
| `FileAPI/file` | 7 | 0 | 7 | 0 | 14 | 21 | 21 |  |
| `content-security-policy/nonce-hiding` | 7 | 5 | 2 | 0 | 1 | 8 | 8 |  |
| `content-security-policy/style-src-attr-elem` | 7 | 4 | 3 | 0 | 0 | 7 | 7 |  |
| `cookies/partitioned-cookies` | 7 | 6 | 1 | 0 | 0 | 7 | 7 |  |
| `eventsource/shared-worker` | 7 | 0 | 7 | 0 | 0 | 7 | 7 |  |
| `fetch/cross-origin-resource-policy` | 7 | 2 | 5 | 0 | 4 | 11 | 12 |  |
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
| `uievents/textInput` | 6 | 2 | 4 | 0 | 1 | 7 | 7 |  |
| `content-security-policy/font-src` | 5 | 5 | 0 | 0 | 0 | 5 | 5 |  |
| `cookies/domain` | 5 | 5 | 0 | 0 | 0 | 5 | 5 |  |
| `custom-elements/state` | 5 | 1 | 4 | 0 | 0 | 5 | 5 |  |
| `encoding/legacy-mb-tchinese` | 5 | 0 | 5 | 0 | 278 | 283 | 283 |  |
| `html/anonymous-iframe` | 5 | 1 | 4 | 0 | 0 | 5 | 33 |  |
| `resource-timing/initiator-type` | 5 | 0 | 5 | 0 | 4 | 9 | 16 |  |
| `selection/textcontrols` | 5 | 0 | 5 | 0 | 4 | 9 | 9 |  |
| `content-security-policy/default-src` | 4 | 1 | 3 | 0 | 0 | 4 | 4 |  |
| `content-security-policy/meta` | 4 | 0 | 4 | 0 | 0 | 4 | 5 |  |
| `content-security-policy/svg` | 4 | 1 | 3 | 0 | 1 | 5 | 6 |  |
| `dom/abort` | 4 | 2 | 2 | 0 | 6 | 10 | 10 |  |
| `fetch/security` | 4 | 1 | 3 | 0 | 0 | 4 | 11 |  |
| `fetch/stale-while-revalidate` | 4 | 2 | 2 | 0 | 0 | 4 | 6 |  |
| `png` | 4 | 3 | 0 | 1 | 0 | 4 | 4 |  |
| `uievents/click` | 4 | 4 | 0 | 0 | 3 | 7 | 7 |  |
| `uievents/keyboard` | 4 | 1 | 3 | 0 | 1 | 5 | 5 |  |
| `FileAPI/BlobURL` | 3 | 3 | 0 | 0 | 0 | 3 | 5 |  |
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
| `url` | 3 | 1 | 2 | 0 | 43 | 46 | 77 |  |
| `FileAPI` | 2 | 1 | 1 | 0 | 5 | 7 | 11 |  |
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
| `webmessaging/multi-globals` | 2 | 0 | 2 | 0 | 2 | 4 | 4 |  |
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
| `mimesniff/media` | 1 | 1 | 0 | 0 | 0 | 1 | 1 |  |
| `selection/anonymous` | 1 | 0 | 1 | 0 | 2 | 3 | 3 |  |
| `shadow-dom/reference-target` | 1 | 0 | 1 | 0 | 0 | 1 | 15 |  |
| `subresource-integrity/integrity-policy` | 1 | 0 | 1 | 0 | 4 | 5 | 8 |  |
| `subresource-integrity` | 1 | 1 | 0 | 0 | 0 | 1 | 1 |  |
| `svg` | 1 | 0 | 1 | 0 | 1 | 2 | 3 |  |
| `uievents` | 1 | 0 | 1 | 0 | 1 | 2 | 3 |  |
| `uievents/legacy-domevents-tests` | 1 | 1 | 0 | 0 | 3 | 4 | 4 |  |
| `uievents/legacy` | 1 | 0 | 1 | 0 | 0 | 1 | 1 |  |
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
| `intersection-observer/v2` | 2 | 2 |
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
| `css/css-grid` | 355 |
| `html/editing` | 90 |
| `css/css-text` | 86 |
| `html/semantics` | 84 |
| `html/canvas` | 53 |
| `css/CSS2` | 45 |
| `css/css-box` | 32 |
| `url` | 30 |
| `css/css-text-decor` | 22 |
| `css/css-overflow` | 18 |
| `html/syntax` | 18 |
| `fetch/h1-parsing` | 17 |
| `css/css-fonts` | 16 |
| `svg/text` | 16 |
| `selection` | 12 |
| `css/css-ui` | 12 |
| `html/rendering` | 11 |
| `svg/painting` | 11 |
| `svg/styling` | 11 |
| `css/css-images` | 10 |
| `css/css-values` | 9 |
| `html/dom` | 9 |
| `css/css-multicol` | 8 |
| `css/css-flexbox` | 7 |
| `css/css-transforms` | 7 |

