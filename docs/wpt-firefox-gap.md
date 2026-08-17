# The Firefox gap, one test file at a time

**Generated** by `tools/wpt/firefox-gap.py`. Do not edit by hand.

Firefox version: 156.0a1
Firefox run date: 2026-08-16
Firefox's WPT revision: `7327d61f88e2`
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
| Firefox passes | 19204 | 17961 | 37165 |
| **we pass** | **4293** | **0** | **4293** |
| **Firefox passes, we fail** | **14911** | **17961** | **32872** |
| both fail | 3498 | 3020 | 6518 |
| we pass, Firefox fails (audit) | 367 | 0 | 367 |

**11.6% of what Firefox passes.** Every reftest is counted as a failure, which is what a sample of
them measures: reftests are run by the runner and recorded by
nothing, so an expectation file's silence about one is not a pass.

### Why the testharness gap fails

| cause | files |
|---|--:|
| the harness never reported (TIMEOUT/ERROR/CRASH) | 6153 |
| subtests ran and failed | 8758 |

A blocked file is worth more than its count: none of its subtests are
in any denominator anywhere, so it is invisible in every rate.

## Ranked: test files Firefox passes and we do not

`blocked` is where the harness never reported; `feature` is where it
reported and subtests failed. A refused area is a decision with a name
(`docs/wpt-refusals.tsv`) and its row is marked.

| area | gap | blocked | feature | we pass | firefox passes | in scope | refusal |
|---|--:|--:|--:|--:|--:|--:|---|
| `html/canvas` | 2654 | 976 | 1678 | 334 | 2988 | 3315 |  |
| `html/semantics` | 1517 | 512 | 1005 | 282 | 1799 | 2235 |  |
| `referrer-policy/gen` | 951 | 865 | 86 | 0 | 951 | 1001 |  |
| `html/browsers` | 549 | 298 | 251 | 58 | 607 | 754 | **partial** ADR 0026 §6 -- window.open returns null, opener absent |
| `css/css-grid` | 511 | 415 | 96 | 6 | 517 | 652 |  |
| `websockets` | 364 | 285 | 79 | 6 | 370 | 382 | **partial** infrastructure -- our server does not speak the WebSocket protocol |
| `xhr` | 296 | 29 | 267 | 87 | 383 | 398 |  |
| `css/css-text` | 281 | 102 | 179 | 43 | 324 | 368 |  |
| `html/webappapis` | 278 | 120 | 158 | 32 | 310 | 345 |  |
| `svg/animations` | 267 | 37 | 230 | 3 | 270 | 289 |  |
| `IndexedDB` | 251 | 40 | 211 | 12 | 263 | 265 | **partial** ADR 0021/0038 -- memory tier only, no disk persistence by default |
| `css/css-flexbox` | 248 | 172 | 76 | 95 | 343 | 369 |  |
| `css/css-align` | 195 | 65 | 130 | 4 | 199 | 234 |  |
| `dom/nodes` | 186 | 55 | 131 | 119 | 305 | 327 |  |
| `content-security-policy/gen` | 184 | 184 | 0 | 0 | 184 | 260 |  |
| `html/syntax` | 172 | 124 | 48 | 170 | 342 | 378 |  |
| `css/css-values` | 157 | 19 | 138 | 9 | 166 | 268 |  |
| `fetch/api` | 152 | 13 | 139 | 30 | 182 | 223 |  |
| `css/css-conditional` | 147 | 122 | 25 | 1 | 148 | 223 |  |
| `css/selectors` | 143 | 36 | 107 | 110 | 253 | 277 | **partial** ADR 0012/0016/0033 -- :visited matches nothing (privacy) |
| `css/css-shapes` | 134 | 17 | 117 | 0 | 134 | 144 |  |
| `css/css-fonts` | 123 | 17 | 106 | 15 | 138 | 163 |  |
| `html/dom` | 118 | 15 | 103 | 65 | 183 | 277 |  |
| `css/cssom-view` | 117 | 15 | 102 | 62 | 179 | 225 |  |
| `dom/events` | 110 | 24 | 86 | 65 | 175 | 202 |  |
| `css/cssom` | 109 | 20 | 89 | 63 | 172 | 190 |  |
| `referrer-policy/4K+1` | 108 | 65 | 43 | 0 | 108 | 108 |  |
| `referrer-policy/4K-1` | 108 | 64 | 44 | 0 | 108 | 108 |  |
| `referrer-policy/4K` | 108 | 64 | 44 | 0 | 108 | 108 |  |
| `html/rendering` | 102 | 20 | 82 | 34 | 136 | 153 |  |
| `upgrade-insecure-requests/gen` | 99 | 0 | 99 | 0 | 99 | 196 |  |
| `css/css-backgrounds` | 97 | 1 | 96 | 8 | 105 | 121 |  |
| `css/css-transitions` | 97 | 21 | 76 | 8 | 105 | 120 |  |
| `css/css-animations` | 95 | 11 | 84 | 13 | 108 | 124 |  |
| `css/css-transforms` | 93 | 1 | 92 | 7 | 100 | 107 |  |
| `css/css-overflow` | 88 | 29 | 59 | 11 | 99 | 202 |  |
| `css/css-sizing` | 87 | 27 | 60 | 16 | 103 | 183 |  |
| `css/css-tables` | 83 | 3 | 80 | 29 | 112 | 135 |  |
| `svg/types` | 83 | 26 | 57 | 0 | 83 | 86 |  |
| `intersection-observer` | 77 | 21 | 56 | 13 | 90 | 106 |  |
| `eventsource` | 75 | 37 | 38 | 1 | 76 | 76 |  |
| `html/interaction` | 75 | 8 | 67 | 14 | 89 | 193 |  |
| `resource-timing` | 71 | 36 | 35 | 8 | 79 | 103 |  |
| `workers` | 71 | 28 | 43 | 28 | 99 | 111 | **partial** ADR 0022 §1 -- SharedWorker refused; DedicatedWorker implemented |
| `css/css-position` | 68 | 5 | 63 | 25 | 93 | 111 |  |
| `css/css-ui` | 67 | 4 | 63 | 19 | 86 | 116 |  |
| `fetch/metadata` | 66 | 34 | 32 | 0 | 66 | 87 |  |
| `css/css-cascade` | 65 | 17 | 48 | 18 | 83 | 96 |  |
| `svg/painting` | 65 | 59 | 6 | 0 | 65 | 70 |  |
| `html/cross-origin-embedder-policy` | 61 | 27 | 34 | 0 | 61 | 88 |  |
| `css/css-multicol` | 60 | 8 | 52 | 7 | 67 | 94 |  |
| `css/css-writing-modes` | 60 | 10 | 50 | 19 | 79 | 82 |  |
| `content-security-policy/script-src` | 57 | 37 | 20 | 24 | 81 | 90 |  |
| `html/editing` | 57 | 25 | 32 | 24 | 81 | 217 |  |
| `media-source` | 57 | 19 | 38 | 2 | 59 | 72 |  |
| `svg/geometry` | 54 | 42 | 12 | 0 | 54 | 61 |  |
| `custom-elements/reactions` | 52 | 0 | 52 | 4 | 56 | 57 |  |
| `webmessaging` | 50 | 34 | 16 | 7 | 57 | 60 |  |
| `shadow-dom` | 47 | 6 | 41 | 14 | 61 | 66 |  |
| `html/cross-origin-opener-policy` | 46 | 4 | 42 | 53 | 99 | 157 |  |
| `navigation-timing` | 42 | 28 | 14 | 6 | 48 | 58 |  |
| `html/infrastructure` | 41 | 15 | 26 | 55 | 96 | 117 |  |
| `css/CSS2` | 40 | 7 | 33 | 22 | 62 | 66 |  |
| `css/css-variables` | 40 | 13 | 27 | 9 | 49 | 59 |  |
| `webstorage` | 39 | 25 | 14 | 10 | 49 | 54 |  |
| `css/css-text-decor` | 38 | 0 | 38 | 6 | 44 | 48 |  |
| `custom-elements` | 37 | 7 | 30 | 4 | 41 | 44 |  |
| `web-animations/interfaces` | 37 | 7 | 30 | 0 | 37 | 42 |  |
| `css/css-color` | 33 | 1 | 32 | 15 | 48 | 62 |  |
| `shadow-dom/focus` | 33 | 1 | 32 | 3 | 36 | 37 |  |
| `selection` | 32 | 4 | 28 | 36 | 68 | 88 |  |
| `referrer-policy/generic` | 31 | 17 | 14 | 0 | 31 | 42 |  |
| `workers/interfaces` | 30 | 9 | 21 | 34 | 64 | 66 |  |
| `content-security-policy/frame-ancestors` | 29 | 24 | 5 | 4 | 33 | 34 |  |
| `content-security-policy/connect-src` | 28 | 0 | 28 | 0 | 28 | 30 |  |
| `performance-timeline` | 28 | 18 | 10 | 17 | 45 | 58 |  |
| `shadow-dom/untriaged` | 28 | 1 | 27 | 24 | 52 | 54 |  |
| `svg/path` | 28 | 7 | 21 | 0 | 28 | 41 |  |
| `workers/constructors` | 28 | 5 | 23 | 7 | 35 | 35 |  |
| `content-security-policy/worker-src` | 27 | 10 | 17 | 3 | 30 | 31 |  |
| `css/css-images` | 27 | 2 | 25 | 2 | 29 | 41 |  |
| `svg/text` | 27 | 18 | 9 | 0 | 27 | 32 |  |
| `content-security-policy/style-src` | 26 | 14 | 12 | 12 | 38 | 42 |  |
| `css/css-box` | 26 | 1 | 25 | 10 | 36 | 83 |  |
| `domxpath` | 26 | 3 | 23 | 0 | 26 | 32 |  |
| `web-animations/timing-model` | 26 | 4 | 22 | 0 | 26 | 28 |  |
| `content-security-policy/reporting` | 25 | 19 | 6 | 1 | 26 | 31 |  |
| `cors` | 25 | 8 | 17 | 0 | 25 | 27 |  |
| `referrer-policy/css-integration` | 24 | 8 | 16 | 0 | 24 | 24 |  |
| `workers/semantics` | 24 | 13 | 11 | 4 | 28 | 30 |  |
| `content-security-policy/unsafe-hashes` | 23 | 23 | 0 | 1 | 24 | 24 |  |
| `webmessaging/with-ports` | 23 | 13 | 10 | 1 | 24 | 24 |  |
| `webmessaging/without-ports` | 23 | 13 | 10 | 4 | 27 | 27 |  |
| `fetch/compression-dictionary` | 22 | 3 | 19 | 1 | 23 | 31 |  |
| `resize-observer` | 22 | 10 | 12 | 12 | 34 | 35 |  |
| `web-animations/responsive` | 22 | 0 | 22 | 1 | 23 | 40 |  |
| `css/css-syntax` | 20 | 0 | 20 | 15 | 35 | 40 |  |
| `encoding` | 20 | 13 | 7 | 48 | 68 | 74 |  |
| `shadow-dom/focus-navigation` | 20 | 0 | 20 | 3 | 23 | 45 |  |
| `content-security-policy/securitypolicyviolation` | 19 | 17 | 2 | 0 | 19 | 24 |  |
| `svg/interact` | 19 | 12 | 7 | 6 | 25 | 33 |  |
| `xhr/formdata` | 19 | 2 | 17 | 8 | 27 | 27 |  |
| `storage` | 18 | 1 | 17 | 0 | 18 | 28 |  |
| `web-animations/animation-model` | 18 | 0 | 18 | 2 | 20 | 25 |  |
| `workers/modules` | 18 | 7 | 11 | 0 | 18 | 25 |  |
| `content-security-policy/generic` | 17 | 15 | 2 | 8 | 25 | 28 |  |
| `content-security-policy/inheritance` | 17 | 15 | 2 | 2 | 19 | 26 |  |
| `streams/readable-streams` | 17 | 1 | 16 | 0 | 17 | 22 |  |
| `svg/linking` | 17 | 13 | 4 | 0 | 17 | 18 |  |
| `content-security-policy/sandbox` | 16 | 3 | 13 | 0 | 16 | 17 |  |
| `streams/writable-streams` | 16 | 0 | 16 | 0 | 16 | 16 |  |
| `css/css-display` | 15 | 3 | 12 | 4 | 19 | 33 |  |
| `FileAPI/url` | 14 | 12 | 2 | 0 | 14 | 15 |  |
| `uievents/order-of-events` | 14 | 7 | 7 | 2 | 16 | 16 |  |
| `webmessaging/broadcastchannel` | 14 | 6 | 8 | 0 | 14 | 14 |  |
| `custom-elements/form-associated` | 13 | 1 | 12 | 2 | 15 | 18 |  |
| `domparsing` | 13 | 3 | 10 | 15 | 28 | 34 | **partial** ADR 0012 -- DOMParser absent -- it returns a second Document |
| `html/the-xhtml-syntax` | 13 | 13 | 0 | 0 | 13 | 13 |  |
| `webmessaging/message-channels` | 13 | 2 | 11 | 4 | 17 | 23 |  |
| `content-security-policy/img-src` | 12 | 11 | 1 | 3 | 15 | 15 |  |
| `encoding/streams` | 12 | 1 | 11 | 0 | 12 | 13 |  |
| `html/obsolete` | 12 | 0 | 12 | 2 | 14 | 14 |  |
| `streams/piping` | 12 | 0 | 12 | 0 | 12 | 13 |  |
| `streams/transferable` | 12 | 4 | 8 | 0 | 12 | 12 |  |
| `svg/pservers` | 12 | 9 | 3 | 0 | 12 | 12 |  |
| `uievents/mouse` | 12 | 5 | 7 | 7 | 19 | 22 |  |
| `content-security-policy/form-action` | 11 | 8 | 3 | 1 | 12 | 13 |  |
| `cookies/prefix` | 11 | 6 | 5 | 0 | 11 | 11 |  |
| `fetch/content-encoding` | 11 | 0 | 11 | 2 | 13 | 13 |  |
| `streams/readable-byte-streams` | 11 | 1 | 10 | 0 | 11 | 11 |  |
| `svg/styling` | 11 | 2 | 9 | 0 | 11 | 16 |  |
| `user-timing` | 11 | 9 | 2 | 47 | 58 | 58 |  |
| `content-security-policy/media-src` | 10 | 9 | 1 | 0 | 10 | 10 |  |
| `content-security-policy/unsafe-eval` | 10 | 1 | 9 | 1 | 11 | 15 |  |
| `content-security-policy/wasm-unsafe-eval` | 10 | 2 | 8 | 0 | 10 | 10 |  |
| `dom/collections` | 10 | 0 | 10 | 0 | 10 | 10 |  |
| `dom/ranges` | 10 | 5 | 5 | 27 | 37 | 58 |  |
| `dom/traversal` | 10 | 0 | 10 | 7 | 17 | 18 |  |
| `encoding/legacy-mb-japanese` | 10 | 10 | 0 | 472 | 482 | 482 |  |
| `eventsource/dedicated-worker` | 10 | 1 | 9 | 0 | 10 | 10 |  |
| `html/document-isolation-policy` | 10 | 0 | 10 | 0 | 10 | 38 |  |
| `content-security-policy/frame-src` | 9 | 7 | 2 | 0 | 9 | 12 |  |
| `content-security-policy/object-src` | 9 | 9 | 0 | 0 | 9 | 13 |  |
| `content-security-policy/reporting-api` | 9 | 8 | 1 | 1 | 10 | 11 |  |
| `custom-elements/parser` | 9 | 3 | 6 | 2 | 11 | 11 |  |
| `fetch/range` | 9 | 1 | 8 | 0 | 9 | 11 |  |
| `hr-time` | 9 | 3 | 6 | 4 | 13 | 15 |  |
| `html/user-activation` | 9 | 8 | 1 | 1 | 10 | 21 |  |
| `selection/shadow-dom` | 9 | 0 | 9 | 0 | 9 | 13 |  |
| `shadow-dom/declarative` | 9 | 2 | 7 | 12 | 21 | 56 |  |
| `svg/scripted` | 9 | 6 | 3 | 0 | 9 | 9 |  |
| `webmessaging/with-options` | 9 | 7 | 2 | 0 | 9 | 9 |  |
| `fetch/local-network-access` | 8 | 8 | 0 | 0 | 8 | 16 |  |
| `streams/transform-streams` | 8 | 0 | 8 | 0 | 8 | 12 |  |
| `svg/coordinate-systems` | 8 | 1 | 7 | 0 | 8 | 8 |  |
| `websockets/interfaces` | 8 | 2 | 6 | 160 | 168 | 168 |  |
| `FileAPI/file` | 7 | 0 | 7 | 14 | 21 | 21 |  |
| `content-security-policy/nonce-hiding` | 7 | 5 | 2 | 1 | 8 | 8 |  |
| `content-security-policy/style-src-attr-elem` | 7 | 4 | 3 | 0 | 7 | 7 |  |
| `cookies/partitioned-cookies` | 7 | 6 | 1 | 0 | 7 | 7 |  |
| `eventsource/shared-worker` | 7 | 0 | 7 | 0 | 7 | 7 |  |
| `fetch/cross-origin-resource-policy` | 7 | 2 | 5 | 4 | 11 | 12 |  |
| `fetch/http-cache` | 7 | 0 | 7 | 0 | 7 | 14 |  |
| `svg/struct` | 7 | 2 | 5 | 0 | 7 | 16 |  |
| `uievents/textInput` | 7 | 0 | 7 | 0 | 7 | 7 |  |
| `workers/baseurl` | 7 | 4 | 3 | 0 | 7 | 7 |  |
| `content-security-policy/blob` | 6 | 0 | 6 | 0 | 6 | 6 |  |
| `content-security-policy/navigation` | 6 | 6 | 0 | 0 | 6 | 6 |  |
| `content-security-policy/script-src-attr-elem` | 6 | 5 | 1 | 2 | 8 | 8 |  |
| `cookies/attributes` | 6 | 4 | 2 | 0 | 6 | 9 |  |
| `cookies/samesite` | 6 | 6 | 0 | 0 | 6 | 22 |  |
| `cookies/secure` | 6 | 3 | 3 | 0 | 6 | 6 |  |
| `fetch/nosniff` | 6 | 3 | 3 | 0 | 6 | 6 |  |
| `svg/shapes` | 6 | 6 | 0 | 0 | 6 | 7 |  |
| `content-security-policy/font-src` | 5 | 5 | 0 | 0 | 5 | 5 |  |
| `cookies/domain` | 5 | 5 | 0 | 0 | 5 | 5 |  |
| `custom-elements/state` | 5 | 1 | 4 | 0 | 5 | 5 |  |
| `custom-elements/upgrading` | 5 | 0 | 5 | 0 | 5 | 7 |  |
| `dom/abort` | 5 | 2 | 3 | 5 | 10 | 10 |  |
| `dom` | 5 | 0 | 5 | 4 | 9 | 10 |  |
| `encoding/legacy-mb-tchinese` | 5 | 0 | 5 | 278 | 283 | 283 |  |
| `fetch/corb` | 5 | 3 | 2 | 5 | 10 | 14 |  |
| `html/anonymous-iframe` | 5 | 1 | 4 | 0 | 5 | 33 |  |
| `resource-timing/initiator-type` | 5 | 0 | 5 | 4 | 9 | 16 |  |
| `selection/contenteditable` | 5 | 0 | 5 | 5 | 10 | 12 |  |
| `selection/textcontrols` | 5 | 0 | 5 | 4 | 9 | 9 |  |
| `content-security-policy/default-src` | 4 | 1 | 3 | 0 | 4 | 4 |  |
| `content-security-policy/meta` | 4 | 0 | 4 | 0 | 4 | 5 |  |
| `content-security-policy/svg` | 4 | 1 | 3 | 0 | 4 | 5 |  |
| `custom-elements/registries` | 4 | 0 | 4 | 1 | 5 | 40 |  |
| `fetch/security` | 4 | 1 | 3 | 0 | 4 | 11 |  |
| `fetch/stale-while-revalidate` | 4 | 2 | 2 | 0 | 4 | 6 |  |
| `uievents/click` | 4 | 4 | 0 | 3 | 7 | 7 |  |
| `uievents/keyboard` | 4 | 1 | 3 | 1 | 5 | 5 |  |
| `webmessaging/multi-globals` | 4 | 0 | 4 | 0 | 4 | 4 |  |
| `FileAPI` | 3 | 2 | 1 | 4 | 7 | 11 |  |
| `FileAPI/BlobURL` | 3 | 1 | 2 | 0 | 3 | 5 |  |
| `FileAPI/blob` | 3 | 0 | 3 | 19 | 22 | 23 |  |
| `content-security-policy/base-uri` | 3 | 1 | 2 | 2 | 5 | 6 |  |
| `content-security-policy/child-src` | 3 | 3 | 0 | 2 | 5 | 9 |  |
| `content-security-policy/inside-worker` | 3 | 1 | 2 | 0 | 3 | 10 |  |
| `content-security-policy/resource-hints` | 3 | 3 | 0 | 0 | 3 | 9 |  |
| `content-security-policy/webrtc` | 3 | 0 | 3 | 0 | 3 | 5 |  |
| `content-security-policy/xslt` | 3 | 1 | 2 | 0 | 3 | 3 |  |
| `fetch/content-length` | 3 | 1 | 2 | 0 | 3 | 5 |  |
| `fetch/data-urls` | 3 | 0 | 3 | 0 | 3 | 3 |  |
| `fetch/orb` | 3 | 3 | 0 | 4 | 7 | 17 |  |
| `mimesniff/sniffing` | 3 | 2 | 1 | 0 | 3 | 3 |  |
| `png` | 3 | 3 | 0 | 0 | 3 | 3 |  |
| `selection/bidi` | 3 | 0 | 3 | 0 | 3 | 3 |  |
| `shadow-dom/leaktests` | 3 | 0 | 3 | 0 | 3 | 4 |  |
| `streams` | 3 | 1 | 2 | 0 | 3 | 3 | **partial** ADR 0020 §1 -- new ReadableStream({start}) is an illegal constructor |
| `subresource-integrity/unencoded-digest` | 3 | 1 | 2 | 0 | 3 | 9 |  |
| `svg/extensibility` | 3 | 1 | 2 | 3 | 6 | 7 |  |
| `svg/fonts` | 3 | 0 | 3 | 0 | 3 | 3 |  |
| `uievents/interface` | 3 | 2 | 1 | 0 | 3 | 3 |  |
| `content-security-policy/parsing` | 2 | 2 | 0 | 0 | 2 | 2 |  |
| `content-security-policy/plugin-types` | 2 | 2 | 0 | 0 | 2 | 2 |  |
| `cookies` | 2 | 0 | 2 | 1 | 3 | 3 |  |
| `cookies/path` | 2 | 2 | 0 | 0 | 2 | 2 |  |
| `custom-elements/htmlconstructor` | 2 | 0 | 2 | 0 | 2 | 2 |  |
| `fetch/content-type` | 2 | 1 | 1 | 0 | 2 | 5 |  |
| `fetch/h1-parsing` | 2 | 1 | 1 | 0 | 2 | 20 |  |
| `fetch/redirect-navigate` | 2 | 2 | 0 | 0 | 2 | 2 |  |
| `fetch/redirects` | 2 | 1 | 1 | 0 | 2 | 2 |  |
| `svg/embedded` | 2 | 1 | 1 | 0 | 2 | 2 |  |
| `uievents/constructors` | 2 | 0 | 2 | 0 | 2 | 2 |  |
| `websockets/multi-globals` | 2 | 0 | 2 | 0 | 2 | 2 |  |
| `websockets/opening-handshake` | 2 | 1 | 1 | 12 | 14 | 14 |  |
| `workers/same-site-cookies` | 2 | 0 | 2 | 0 | 2 | 6 |  |
| `FileAPI/FileReader` | 1 | 1 | 0 | 1 | 2 | 2 |  |
| `content-security-policy/embedded-enforcement` | 1 | 1 | 0 | 1 | 2 | 23 |  |
| `cookies/encoding` | 1 | 1 | 0 | 0 | 1 | 1 |  |
| `cookies/ordering` | 1 | 0 | 1 | 0 | 1 | 1 |  |
| `cookies/origin-bound-cookies` | 1 | 0 | 1 | 0 | 1 | 2 |  |
| `cookies/samesite-none-secure` | 1 | 0 | 1 | 0 | 1 | 1 |  |
| `cookies/schemeful-same-site` | 1 | 1 | 0 | 0 | 1 | 4 |  |
| `cookies/size` | 1 | 1 | 0 | 0 | 1 | 2 |  |
| `dom/lists` | 1 | 0 | 1 | 3 | 4 | 5 |  |
| `fetch/images` | 1 | 1 | 0 | 0 | 1 | 1 |  |
| `fetch/origin` | 1 | 1 | 0 | 0 | 1 | 1 |  |
| `html/capability-delegation` | 1 | 1 | 0 | 0 | 1 | 6 |  |
| `html/embedded-content` | 1 | 0 | 1 | 0 | 1 | 1 |  |
| `html/links` | 1 | 0 | 1 | 3 | 4 | 6 |  |
| `html/meta` | 1 | 1 | 0 | 0 | 1 | 1 |  |
| `html/scripting` | 1 | 0 | 1 | 1 | 2 | 2 |  |
| `html/select` | 1 | 0 | 1 | 0 | 1 | 1 |  |
| `intersection-observer/v2` | 1 | 0 | 1 | 1 | 2 | 38 |  |
| `mimesniff/media` | 1 | 1 | 0 | 0 | 1 | 1 |  |
| `selection/anonymous` | 1 | 0 | 1 | 2 | 3 | 3 |  |
| `selection/caret` | 1 | 0 | 1 | 1 | 2 | 3 |  |
| `shadow-dom/reference-target` | 1 | 0 | 1 | 0 | 1 | 15 |  |
| `subresource-integrity/integrity-policy` | 1 | 0 | 1 | 4 | 5 | 8 |  |
| `subresource-integrity` | 1 | 1 | 0 | 0 | 1 | 1 |  |
| `svg` | 1 | 0 | 1 | 1 | 2 | 3 |  |
| `uievents` | 1 | 0 | 1 | 1 | 2 | 3 |  |
| `uievents/legacy-domevents-tests` | 1 | 1 | 0 | 3 | 4 | 4 |  |
| `uievents/legacy` | 1 | 0 | 1 | 0 | 1 | 1 |  |
| `url` | 1 | 1 | 0 | 45 | 46 | 77 |  |
| `websockets/cookies` | 1 | 1 | 0 | 19 | 20 | 21 |  |
| `websockets/unload-a-document` | 1 | 1 | 0 | 6 | 7 | 11 |  |
| `workers/multi-globals` | 1 | 0 | 1 | 0 | 1 | 1 |  |

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
| `svg/render` | 1 | 1 |
| `svg/svg-in-svg` | 1 | 1 |
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
| `html/editing` | 90 |
| `html/semantics` | 72 |
| `url` | 30 |
| `css/css-text` | 20 |
| `html/syntax` | 18 |
| `fetch/h1-parsing` | 17 |
| `css/css-sizing` | 12 |
| `selection` | 12 |
| `html/dom` | 9 |
| `css/css-overflow` | 6 |
| `html/infrastructure` | 5 |
| `css/css-grid` | 4 |
| `css/cssom` | 4 |
| `css/css-display` | 3 |
| `css/css-syntax` | 3 |
| `css/css-ui` | 3 |
| `css/cssom-view` | 3 |
| `html/browsers` | 3 |
| `shadow-dom/declarative` | 3 |
| `websockets/unload-a-document` | 3 |
| `content-security-policy/inheritance` | 2 |
| `css/css-flexbox` | 2 |
| `css/css-images` | 2 |
| `css/css-position` | 2 |
| `css/css-values` | 2 |

