# The WPT baseline

**Generated**, by `microbrowser_wpt --summary docs/wpt-baseline.md`. Do not edit it: the
next run overwrites it, and that overwrite is the point -- the diff of this file is
what a session moved. The argument for the instrument is `docs/adr/0040`; the work it
sequences is `docs/wpt-plan.md`.

WPT revision: `4120ac0deb573634d8b7cd74c38ae9d647eebdb5`

21345 of 373227 subtests pass (5.7%) over 10329 tests.

**Do not quote that number.** Subtests are not comparable across areas: `encoding/legacy-mb-japanese` alone is 31% of every subtest here.
A suite that tests one index table entry per code point counts differently from
one that tests an algorithm. The per-area column is the measurement; the aggregate
is an artefact of how the suite is written.

A test with no subtests at all -- a reftest, or a testharness page whose harness
died before it ran anything -- contributes nothing to that percentage, so the
harness columns below are the ones to read first. A `TIMEOUT` is not a slow test;
it is a page that never reported, which almost always means something threw before
`done()`.

## Per area

| area | tests | ok | error | timeout | crash | subtests | passed | % |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| `FileAPI` | 11 | 6 | 0 | 5 | 0 | 26 | 10 | 38.5 |
| `FileAPI/BlobURL` | 5 | 4 | 0 | 1 | 0 | 16 | 0 | 0.0 |
| `FileAPI/FileReader` | 2 | 1 | 0 | 1 | 0 | 2 | 0 | 0.0 |
| `FileAPI/blob` | 23 | 13 | 0 | 10 | 0 | 259 | 1 | 0.4 |
| `FileAPI/file` | 21 | 14 | 0 | 7 | 0 | 163 | 0 | 0.0 |
| `FileAPI/filelist-section` | 1 | 1 | 0 | 0 | 0 | 7 | 0 | 0.0 |
| `FileAPI/reading-data-section` | 26 | 13 | 0 | 13 | 0 | 48 | 0 | 0.0 |
| `FileAPI/url` | 15 | 3 | 0 | 12 | 0 | 40 | 12 | 30.0 |
| `IndexedDB` | 265 | 198 | 3 | 64 | 0 | 1040 | 43 | 4.1 |
| `IndexedDB/crashtests` | 1 | 1 | 0 | 0 | 0 | 1 | 1 | 100.0 |
| `console` | 19 | 12 | 0 | 7 | 0 | 29 | 6 | 20.7 |
| `cookies` | 3 | 3 | 0 | 0 | 0 | 3 | 1 | 33.3 |
| `cookies/attributes` | 9 | 3 | 0 | 6 | 0 | 524 | 0 | 0.0 |
| `cookies/domain` | 5 | 0 | 0 | 5 | 0 | 6 | 0 | 0.0 |
| `cookies/encoding` | 1 | 0 | 0 | 1 | 0 | 6 | 0 | 0.0 |
| `cookies/name` | 2 | 0 | 0 | 2 | 0 | 111 | 0 | 0.0 |
| `cookies/ordering` | 1 | 1 | 0 | 0 | 0 | 1 | 0 | 0.0 |
| `cookies/origin-bound-cookies` | 2 | 1 | 0 | 1 | 0 | 2 | 0 | 0.0 |
| `cookies/partitioned-cookies` | 7 | 1 | 1 | 5 | 0 | 7 | 0 | 0.0 |
| `cookies/path` | 2 | 0 | 0 | 2 | 0 | 16 | 0 | 0.0 |
| `cookies/prefix` | 11 | 5 | 6 | 0 | 0 | 183 | 0 | 0.0 |
| `cookies/samesite` | 22 | 15 | 0 | 7 | 0 | 119 | 0 | 0.0 |
| `cookies/samesite-none-secure` | 1 | 1 | 0 | 0 | 0 | 1 | 0 | 0.0 |
| `cookies/schemeful-same-site` | 4 | 2 | 0 | 2 | 0 | 5 | 0 | 0.0 |
| `cookies/secure` | 6 | 3 | 0 | 3 | 0 | 5 | 1 | 20.0 |
| `cookies/size` | 2 | 0 | 0 | 2 | 0 | 25 | 0 | 0.0 |
| `cookies/third-party-cookies` | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| `cookies/value` | 2 | 0 | 0 | 2 | 0 | 94 | 0 | 0.0 |
| `cors` | 27 | 20 | 0 | 7 | 0 | 227 | 36 | 15.9 |
| `css/CSS2` | 66 | 58 | 0 | 8 | 0 | 2464 | 601 | 24.4 |
| `css/css-align` | 234 | 141 | 0 | 93 | 0 | 3689 | 807 | 21.9 |
| `css/css-animations` | 123 | 108 | 5 | 10 | 0 | 1201 | 357 | 29.7 |
| `css/css-backgrounds` | 121 | 120 | 0 | 1 | 0 | 6117 | 493 | 8.1 |
| `css/css-box` | 83 | 42 | 0 | 41 | 0 | 957 | 303 | 31.7 |
| `css/css-cascade` | 82 | 64 | 9 | 9 | 0 | 716 | 11 | 1.5 |
| `css/css-color` | 62 | 61 | 0 | 1 | 0 | 11336 | 1272 | 11.2 |
| `css/css-conditional` | 223 | 35 | 184 | 4 | 0 | 1753 | 1547 | 88.2 |
| `css/css-display` | 33 | 29 | 0 | 4 | 0 | 428 | 72 | 16.8 |
| `css/css-flexbox` | 369 | 186 | 0 | 183 | 0 | 1391 | 210 | 15.1 |
| `css/css-fonts` | 163 | 138 | 1 | 24 | 0 | 7530 | 1195 | 15.9 |
| `css/css-grid` | 652 | 184 | 0 | 468 | 0 | 6460 | 402 | 6.2 |
| `css/css-images` | 41 | 39 | 0 | 2 | 0 | 3580 | 679 | 19.0 |
| `css/css-multicol` | 94 | 82 | 0 | 12 | 0 | 1521 | 41 | 2.7 |
| `css/css-overflow` | 202 | 156 | 2 | 44 | 0 | 957 | 183 | 19.1 |
| `css/css-position` | 111 | 104 | 1 | 6 | 0 | 1375 | 284 | 20.7 |
| `css/css-shapes` | 144 | 126 | 0 | 18 | 0 | 4855 | 741 | 15.3 |
| `css/css-sizing` | 176 | 108 | 1 | 67 | 0 | 4194 | 574 | 13.7 |
| `css/css-syntax` | 40 | 40 | 0 | 0 | 0 | 429 | 59 | 13.8 |
| `css/css-tables` | 135 | 126 | 1 | 8 | 0 | 839 | 208 | 24.8 |
| `css/css-text` | 359 | 250 | 1 | 108 | 0 | 2908 | 345 | 11.9 |
| `css/css-text-decor` | 48 | 47 | 0 | 1 | 0 | 1276 | 99 | 7.8 |
| `css/css-transforms` | 107 | 106 | 0 | 1 | 0 | 5498 | 1317 | 24.0 |
| `css/css-transitions` | 120 | 92 | 3 | 25 | 0 | 3101 | 90 | 2.9 |
| `css/css-ui` | 120 | 109 | 2 | 9 | 0 | 1900 | 174 | 9.2 |
| `css/css-values` | 268 | 231 | 0 | 37 | 0 | 9671 | 802 | 8.3 |
| `css/css-variables` | 59 | 46 | 0 | 13 | 0 | 502 | 194 | 38.6 |
| `css/css-writing-modes` | 82 | 72 | 2 | 8 | 0 | 298 | 63 | 21.1 |
| `css/cssom` | 190 | 149 | 5 | 36 | 0 | 1435 | 708 | 49.3 |
| `css/cssom-view` | 225 | 187 | 0 | 38 | 0 | 1349 | 365 | 27.1 |
| `css/selectors` | 277 | 210 | 10 | 57 | 0 | 1367 | 334 | 24.4 |
| `custom-elements` | 44 | 25 | 0 | 19 | 0 | 598 | 104 | 17.4 |
| `custom-elements/form-associated` | 18 | 14 | 3 | 1 | 0 | 103 | 1 | 1.0 |
| `custom-elements/htmlconstructor` | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| `custom-elements/parser` | 11 | 8 | 0 | 3 | 0 | 20 | 2 | 10.0 |
| `custom-elements/reactions` | 57 | 26 | 0 | 31 | 0 | 372 | 57 | 15.3 |
| `custom-elements/registries` | 40 | 28 | 2 | 10 | 0 | 2207 | 225 | 10.2 |
| `custom-elements/state` | 5 | 4 | 1 | 0 | 0 | 28 | 1 | 3.6 |
| `custom-elements/upgrading` | 7 | 2 | 0 | 5 | 0 | 16 | 1 | 6.2 |
| `dom` | 10 | 10 | 0 | 0 | 0 | 125 | 66 | 52.8 |
| `dom/abort` | 10 | 6 | 0 | 4 | 0 | 37 | 6 | 16.2 |
| `dom/collections` | 10 | 10 | 0 | 0 | 0 | 53 | 7 | 13.2 |
| `dom/events` | 178 | 85 | 1 | 92 | 0 | 552 | 156 | 28.3 |
| `dom/lists` | 5 | 4 | 1 | 0 | 0 | 49 | 30 | 61.2 |
| `dom/nodes` | 327 | 232 | 7 | 87 | 1 | 5158 | 1153 | 22.4 |
| `dom/observable` | 52 | 25 | 0 | 27 | 0 | 244 | 0 | 0.0 |
| `dom/ranges` | 57 | 32 | 24 | 1 | 0 | 242 | 18 | 7.4 |
| `dom/traversal` | 18 | 14 | 3 | 1 | 0 | 55 | 29 | 52.7 |
| `domparsing` | 34 | 21 | 3 | 10 | 0 | 290 | 72 | 24.8 |
| `domparsing/tentative` | 26 | 17 | 0 | 9 | 0 | 905 | 6 | 0.7 |
| `domxpath` | 32 | 24 | 0 | 8 | 0 | 87 | 1 | 1.1 |
| `encoding` | 74 | 40 | 0 | 34 | 0 | 11801 | 68 | 0.6 |
| `encoding/legacy-mb-japanese` | 51 | 5 | 9 | 37 | 0 | 113882 | 1 | 0.0 |
| `encoding/legacy-mb-korean` | 26 | 1 | 0 | 25 | 0 | 66015 | 0 | 0.0 |
| `encoding/legacy-mb-schinese` | 6 | 4 | 0 | 2 | 0 | 660 | 2 | 0.3 |
| `encoding/legacy-mb-tchinese` | 23 | 2 | 0 | 21 | 0 | 40030 | 123 | 0.3 |
| `encoding/streams` | 13 | 12 | 0 | 1 | 0 | 111 | 0 | 0.0 |
| `eventsource` | 76 | 10 | 0 | 66 | 0 | 21 | 1 | 4.8 |
| `eventsource/dedicated-worker` | 10 | 1 | 0 | 9 | 0 | 1 | 0 | 0.0 |
| `eventsource/shared-worker` | 7 | 7 | 0 | 0 | 0 | 10 | 0 | 0.0 |
| `fetch/api` | 218 | 152 | 0 | 66 | 0 | 2269 | 273 | 12.0 |
| `fetch/compression-dictionary` | 31 | 23 | 0 | 8 | 0 | 99 | 0 | 0.0 |
| `fetch/connection-pool` | 1 | 0 | 0 | 1 | 0 | 9 | 0 | 0.0 |
| `fetch/content-encoding` | 13 | 13 | 0 | 0 | 0 | 36 | 4 | 11.1 |
| `fetch/content-length` | 5 | 3 | 0 | 2 | 0 | 39 | 1 | 2.6 |
| `fetch/content-type` | 5 | 2 | 0 | 3 | 0 | 32 | 7 | 21.9 |
| `fetch/corb` | 14 | 8 | 0 | 6 | 0 | 38 | 33 | 86.8 |
| `fetch/cross-origin-resource-policy` | 12 | 5 | 0 | 7 | 0 | 37 | 9 | 24.3 |
| `fetch/data-urls` | 3 | 3 | 0 | 0 | 0 | 161 | 3 | 1.9 |
| `fetch/fetch-later` | 47 | 31 | 0 | 16 | 0 | 118 | 3 | 2.5 |
| `fetch/h1-parsing` | 4 | 2 | 0 | 2 | 0 | 227 | 12 | 5.3 |
| `fetch/http-cache` | 14 | 14 | 0 | 0 | 0 | 142 | 0 | 0.0 |
| `fetch/images` | 1 | 0 | 0 | 1 | 0 | 1 | 0 | 0.0 |
| `fetch/local-network-access` | 16 | 7 | 9 | 0 | 0 | 35 | 0 | 0.0 |
| `fetch/metadata` | 87 | 41 | 2 | 44 | 0 | 1318 | 2 | 0.2 |
| `fetch/nosniff` | 6 | 1 | 0 | 5 | 0 | 64 | 6 | 9.4 |
| `fetch/orb` | 17 | 4 | 0 | 12 | 0 | 5 | 4 | 80.0 |
| `fetch/origin` | 1 | 0 | 0 | 1 | 0 | 43 | 0 | 0.0 |
| `fetch/range` | 11 | 8 | 0 | 3 | 0 | 51 | 2 | 3.9 |
| `fetch/redirect-navigate` | 2 | 1 | 0 | 1 | 0 | 121 | 0 | 0.0 |
| `fetch/redirects` | 2 | 1 | 0 | 1 | 0 | 7 | 0 | 0.0 |
| `fetch/security` | 11 | 2 | 0 | 9 | 0 | 15 | 1 | 6.7 |
| `fetch/stale-while-revalidate` | 6 | 2 | 0 | 4 | 0 | 3 | 0 | 0.0 |
| `hr-time` | 15 | 8 | 0 | 7 | 0 | 13 | 4 | 30.8 |
| `intersection-observer` | 106 | 87 | 0 | 19 | 0 | 180 | 56 | 31.1 |
| `intersection-observer/v2` | 38 | 23 | 0 | 15 | 0 | 54 | 18 | 33.3 |
| `media-source` | 73 | 50 | 0 | 23 | 0 | 268 | 59 | 22.0 |
| `media-source/dedicated-worker` | 7 | 4 | 0 | 3 | 0 | 50 | 1 | 2.0 |
| `media-source/mse-for-webcodecs` | 3 | 0 | 3 | 0 | 0 | 0 | 0 | 0.0 |
| `mimesniff/media` | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0.0 |
| `mimesniff/mime-types` | 3 | 1 | 0 | 2 | 0 | 1939 | 2 | 0.1 |
| `mimesniff/sniffing` | 3 | 3 | 0 | 0 | 0 | 7 | 3 | 42.9 |
| `navigation-timing` | 58 | 25 | 0 | 33 | 0 | 97 | 63 | 64.9 |
| `performance-timeline` | 58 | 27 | 0 | 31 | 0 | 46 | 24 | 52.2 |
| `performance-timeline/not-restored-reasons` | 13 | 13 | 0 | 0 | 0 | 13 | 0 | 0.0 |
| `png` | 3 | 0 | 0 | 3 | 0 | 1 | 0 | 0.0 |
| `resize-observer` | 35 | 25 | 0 | 10 | 0 | 29 | 13 | 44.8 |
| `resource-timing` | 103 | 51 | 1 | 50 | 1 | 538 | 7 | 1.3 |
| `resource-timing/initiator-type` | 16 | 11 | 0 | 5 | 0 | 29 | 7 | 24.1 |
| `resource-timing/tentative` | 26 | 16 | 0 | 10 | 0 | 39 | 0 | 0.0 |
| `selection` | 83 | 34 | 34 | 15 | 0 | 143 | 0 | 0.0 |
| `selection/anonymous` | 3 | 2 | 0 | 1 | 0 | 12 | 0 | 0.0 |
| `selection/bidi` | 3 | 0 | 0 | 3 | 0 | 0 | 0 | 0.0 |
| `selection/caret` | 3 | 3 | 0 | 0 | 0 | 11 | 2 | 18.2 |
| `selection/contenteditable` | 10 | 6 | 0 | 4 | 0 | 61 | 0 | 0.0 |
| `selection/shadow-dom` | 12 | 8 | 0 | 4 | 0 | 51 | 1 | 2.0 |
| `selection/textcontrols` | 7 | 3 | 0 | 4 | 0 | 3 | 0 | 0.0 |
| `shadow-dom` | 66 | 58 | 0 | 8 | 0 | 728 | 71 | 9.8 |
| `shadow-dom/declarative` | 56 | 40 | 4 | 12 | 0 | 7648 | 5 | 0.1 |
| `shadow-dom/focus` | 37 | 31 | 1 | 5 | 0 | 78 | 5 | 6.4 |
| `shadow-dom/focus-navigation` | 45 | 44 | 0 | 1 | 0 | 89 | 2 | 2.2 |
| `shadow-dom/leaktests` | 4 | 3 | 0 | 1 | 0 | 16 | 6 | 37.5 |
| `shadow-dom/reference-target` | 15 | 14 | 0 | 1 | 0 | 768 | 0 | 0.0 |
| `shadow-dom/untriaged` | 54 | 52 | 0 | 2 | 0 | 248 | 157 | 63.3 |
| `storage` | 28 | 11 | 0 | 17 | 0 | 23 | 0 | 0.0 |
| `storage/buckets` | 10 | 8 | 0 | 2 | 0 | 48 | 0 | 0.0 |
| `streams` | 3 | 2 | 0 | 1 | 0 | 3 | 0 | 0.0 |
| `streams/piping` | 13 | 13 | 0 | 0 | 0 | 204 | 0 | 0.0 |
| `streams/readable-byte-streams` | 11 | 10 | 0 | 1 | 0 | 233 | 5 | 2.1 |
| `streams/readable-streams` | 22 | 21 | 0 | 1 | 0 | 362 | 17 | 4.7 |
| `streams/transferable` | 12 | 7 | 0 | 5 | 0 | 71 | 0 | 0.0 |
| `streams/transform-streams` | 12 | 12 | 0 | 0 | 0 | 134 | 0 | 0.0 |
| `streams/writable-streams` | 16 | 16 | 0 | 0 | 0 | 196 | 0 | 0.0 |
| `subresource-integrity` | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0.0 |
| `subresource-integrity/integrity-policy` | 5 | 0 | 0 | 5 | 0 | 37 | 0 | 0.0 |
| `subresource-integrity/signatures` | 15 | 14 | 0 | 1 | 0 | 177 | 43 | 24.3 |
| `subresource-integrity/unencoded-digest` | 9 | 6 | 0 | 3 | 0 | 84 | 28 | 33.3 |
| `svg` | 3 | 3 | 0 | 0 | 0 | 70 | 45 | 64.3 |
| `svg/animations` | 289 | 251 | 1 | 37 | 0 | 282 | 4 | 1.4 |
| `svg/coordinate-systems` | 8 | 7 | 0 | 1 | 0 | 39 | 0 | 0.0 |
| `svg/embedded` | 2 | 1 | 0 | 1 | 0 | 2 | 0 | 0.0 |
| `svg/extensibility` | 7 | 5 | 0 | 2 | 0 | 5 | 3 | 60.0 |
| `svg/fonts` | 3 | 3 | 0 | 0 | 0 | 3 | 0 | 0.0 |
| `svg/geometry` | 61 | 12 | 0 | 49 | 0 | 283 | 27 | 9.5 |
| `svg/import` | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| `svg/interact` | 33 | 14 | 0 | 19 | 0 | 97 | 46 | 47.4 |
| `svg/linking` | 18 | 5 | 0 | 13 | 0 | 34 | 0 | 0.0 |
| `svg/painting` | 70 | 6 | 0 | 64 | 0 | 283 | 0 | 0.0 |
| `svg/path` | 41 | 29 | 0 | 12 | 0 | 208 | 0 | 0.0 |
| `svg/pservers` | 12 | 3 | 0 | 9 | 0 | 11 | 0 | 0.0 |
| `svg/render` | 1 | 1 | 0 | 0 | 0 | 1 | 0 | 0.0 |
| `svg/scripted` | 9 | 3 | 0 | 6 | 0 | 4 | 0 | 0.0 |
| `svg/shapes` | 7 | 0 | 0 | 7 | 0 | 0 | 0 | 0.0 |
| `svg/struct` | 16 | 6 | 0 | 10 | 0 | 40 | 5 | 12.5 |
| `svg/styling` | 16 | 13 | 0 | 3 | 0 | 50 | 4 | 8.0 |
| `svg/svg-in-svg` | 1 | 1 | 0 | 0 | 0 | 1 | 1 | 100.0 |
| `svg/text` | 32 | 9 | 0 | 23 | 0 | 39 | 0 | 0.0 |
| `svg/types` | 86 | 58 | 4 | 24 | 0 | 569 | 58 | 10.2 |
| `uievents` | 3 | 3 | 0 | 0 | 0 | 5 | 2 | 40.0 |
| `uievents/click` | 7 | 0 | 0 | 7 | 0 | 3 | 0 | 0.0 |
| `uievents/constructors` | 2 | 2 | 0 | 0 | 0 | 20 | 0 | 0.0 |
| `uievents/interface` | 3 | 1 | 0 | 2 | 0 | 5 | 0 | 0.0 |
| `uievents/keyboard` | 5 | 1 | 0 | 4 | 0 | 1 | 1 | 100.0 |
| `uievents/legacy` | 1 | 1 | 0 | 0 | 0 | 4 | 0 | 0.0 |
| `uievents/legacy-domevents-tests` | 4 | 3 | 0 | 1 | 0 | 3 | 3 | 100.0 |
| `uievents/mouse` | 17 | 11 | 0 | 6 | 0 | 71 | 1 | 1.4 |
| `uievents/order-of-events` | 16 | 5 | 0 | 11 | 0 | 6 | 0 | 0.0 |
| `uievents/textInput` | 7 | 7 | 0 | 0 | 0 | 24 | 0 | 0.0 |
| `url` | 71 | 39 | 0 | 32 | 0 | 9395 | 2042 | 21.7 |
| `user-timing` | 58 | 29 | 0 | 29 | 0 | 180 | 64 | 35.6 |
| `web-animations` | 1 | 1 | 0 | 0 | 0 | 1 | 0 | 0.0 |
| `web-animations/animation-model` | 25 | 25 | 0 | 0 | 0 | 146 | 20 | 13.7 |
| `web-animations/animation-trigger` | 3 | 3 | 0 | 0 | 0 | 3 | 0 | 0.0 |
| `web-animations/interfaces` | 42 | 34 | 0 | 8 | 0 | 663 | 5 | 0.8 |
| `web-animations/responsive` | 40 | 39 | 0 | 1 | 0 | 99 | 4 | 4.0 |
| `web-animations/timing-model` | 28 | 24 | 0 | 4 | 0 | 394 | 6 | 1.5 |
| `webmessaging` | 60 | 15 | 1 | 44 | 0 | 34 | 12 | 35.3 |
| `webmessaging/broadcastchannel` | 14 | 6 | 0 | 8 | 0 | 34 | 13 | 38.2 |
| `webmessaging/message-channels` | 23 | 9 | 0 | 14 | 0 | 16 | 8 | 50.0 |
| `webmessaging/multi-globals` | 4 | 4 | 0 | 0 | 0 | 4 | 0 | 0.0 |
| `webmessaging/with-options` | 9 | 2 | 0 | 7 | 0 | 2 | 0 | 0.0 |
| `webmessaging/with-ports` | 24 | 11 | 0 | 13 | 0 | 17 | 1 | 5.9 |
| `webmessaging/without-ports` | 27 | 13 | 0 | 14 | 0 | 15 | 2 | 13.3 |
| `webstorage` | 54 | 34 | 1 | 19 | 0 | 1259 | 1164 | 92.5 |
| `workers` | 112 | 29 | 0 | 83 | 0 | 124 | 4 | 3.2 |
| `workers/baseurl` | 7 | 0 | 0 | 7 | 0 | 0 | 0 | 0.0 |
| `workers/constructors` | 35 | 20 | 1 | 14 | 0 | 41 | 0 | 0.0 |
| `workers/examples` | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| `workers/interfaces` | 66 | 5 | 0 | 61 | 0 | 11 | 0 | 0.0 |
| `workers/modules` | 25 | 12 | 1 | 12 | 0 | 139 | 1 | 0.7 |
| `workers/multi-globals` | 1 | 1 | 0 | 0 | 0 | 1 | 0 | 0.0 |
| `workers/same-site-cookies` | 6 | 6 | 0 | 0 | 0 | 6 | 0 | 0.0 |
| `workers/semantics` | 30 | 9 | 0 | 21 | 0 | 17 | 1 | 5.9 |
| `xhr` | 374 | 247 | 0 | 127 | 0 | 1056 | 68 | 6.4 |
| `xhr/formdata` | 27 | 16 | 0 | 11 | 0 | 71 | 0 | 0.0 |

## Why the harness never reported

Ranked by tests affected. One line here is worth more than a page of the table
above: a test whose harness failed reports *no* subtests, so these are invisible in
the pass rate and are the largest block of unrealised coverage in the suite.

| tests | cause | example |
|--:|---|---|
| 1869 | TIMEOUT: the page never reported | `FileAPI/idlharness.worker.html` |
| 703 | TIMEOUT:  | `FileAPI/FileReaderSync.worker.html` |
| 215 | TIMEOUT: the page never reported; first script error: inline script #N: SyntaxError: unexpected token '<' (line N) SyntaxError: unexpected token '<' ... | `custom-elements/Document-createElement-svg.svg` |
| 179 | ERROR: [object Object] | `css/css-conditional/container-queries/animation-container-size.html` |
| 168 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'ready' of undefined TypeError: cannot read p... | `css/CSS2/linebox/vertical-align-top-bottom-001.html` |
| 30 | ERROR: ReferenceError: getSelection is not defined | `selection/addRange-08.html` |
| 30 | ERROR: TypeError: Illegal constructor: Document | `dom/nodes/Node-compareDocumentPosition.html` |
| 27 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot resolve module './support/positioned-grid-descendants.js' T... | `css/css-grid/abspos/orthogonal-positioned-grid-descendants-001.html` |
| 21 | ERROR: TypeError: cannot read property 'N' of undefined | `css/css-conditional/container-queries/at-container-style-parsing.html` |
| 17 | TIMEOUT: killed after the wall-clock budget | `dom/nodes/Node-insertBefore.html` |
| 13 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: getSelection is not defined ReferenceError: getSelection is n... | `selection/getRangeAt.html` |
| 13 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: test is not defined ReferenceError: test is not defined at <a... | `css/css-values/urls/resolve-relative-to-base.sub.html` |
| 9 | ERROR: RangeError: script ran too long | `encoding/legacy-mb-japanese/iso-2022-jp/iso2022jp-encode-form-csiso2022jp.html` |
| 9 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: async_test is not defined ReferenceError: async_test is not d... | `svg/animations/scripted/end-element-on-inactive-element.svg` |
| 9 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: host is not defined ReferenceError: host is not defined at <a... | `css/cssom/selectorText-modification-restyle-002.html` |
| 9 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: target is not defined ReferenceError: target is not defined a... | `css/css-cascade/layer-vs-inline-style.html` |
| 8 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: SharedWorker is not defined ReferenceError: SharedWorker is n... | `fetch/metadata/sharedworker.https.sub.html` |
| 8 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'cssRules' of undefined TypeError: cannot rea... | `css/css-cascade/all-prop-revert-layer.html` |
| 7 | TIMEOUT: the page never reported; first script error: ./support/helpers.js: SyntaxError: expected ')' to close a dynamic import (line N) SyntaxError:... | `shadow-dom/declarative/tentative/shadowrootadoptedstylesheets/shadowrootadoptedstylesheets-async-fetch-disconnect-iframe.html` |
| 7 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: container is not defined ReferenceError: container is not def... | `css/CSS2/positioning/relpos-percentage-left-in-scrollable.html` |
| 7 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: initial is not defined ReferenceError: initial is not defined... | `css/selectors/focus-visible-script-focus-006.tentative.html` |
| 7 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'load' of undefined TypeError: cannot read pr... | `css/css-shapes/shape-outside/values/shape-outside-ellipse-005.html` |
| 7 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot resolve module './support/getComputedStyle-insets.js' TypeE... | `css/cssom/getComputedStyle-insets-relative.html` |
| 7 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (createDocument) is not a function TypeError: undefined ... | `dom/nodes/append-on-Document.html` |
| 6 | TIMEOUT: the page never reported; first script error: /service-workers/service-worker/resources/test-helpers.sub.js: SyntaxError: expected ';' (line ... | `fetch/api/policies/referrer-no-referrer-service-worker.https.html` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: iframe is not defined ReferenceError: iframe is not defined a... | `css/css-values/viewport-units-compute.html` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: measure is not defined ReferenceError: measure is not defined... | `css/css-values/viewport-units-gutter-001.html` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'N' of undefined TypeError: cannot read prope... | `css/css-fonts/test_font_feature_values_parsing.html` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: [object Object] Error: undefined at get_stack (@N) at AssertionError (@N) at ... | `css/css-values/if-invalidation.html` |
| 5 | TIMEOUT: the page never reported; first script error: /trusted-types/support/helper.sub.js: SyntaxError: expected ';' (line N) SyntaxError: expected ... | `domparsing/tentative/stream-html-with-trusted-types-error-in-policy.html` |
| 5 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: testStyle is not defined ReferenceError: testStyle is not def... | `css/css-fonts/parsing/font-face-metric-overrides.html` |
| 5 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: Illegal constructor: CustomElementRegistry TypeError: Illegal cons... | `custom-elements/registries/Document-createElement.html` |
| 5 | TIMEOUT: the page never reported; first script error: support.js?pipe=sub: SyntaxError: expected a property name (line N) SyntaxError: expected a pro... | `cors/credentials-flag.htm` |
| 4 | ERROR: TypeError: cannot read property 'baseVal' of undefined | `svg/types/scripted/SVGLength-convertToSpecifiedUnits-font-change.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: elm is not defined ReferenceError: elm is not defined at <ano... | `css/css-multicol/getclientrects-003.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: scroller is not defined ReferenceError: scroller is not defin... | `css/css-overflow/scroll-markers/scroll-button-disposed-event-target.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: subjectN is not defined ReferenceError: subjectN is not defin... | `css/selectors/invalidation/has-sibling-insertion-removal.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot set property 'onerror' of undefined TypeError: cannot set p... | `custom-elements/cross-realm-callback-report-exception.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (open) is not a function TypeError: undefined (open) is ... | `cookies/domain/domain-attribute-idn-host.sub.https.html` |
| 4 | TIMEOUT: the page never reported; first script error: resources/webperftestharness.js: ReferenceError: ﻿ is not defined ReferenceError: ﻿ is not ... | `resource-timing/resource_connection_reuse_mixed_content.html` |
| 3 | ERROR: TypeError: cannot read property 'cssRules' of undefined | `css/cssom/CSSStyleRule.html` |
| 3 | ERROR: TypeError: undefined (createDocument) is not a function | `dom/nodes/Document-createAttribute.html` |
| 3 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: FontFace is not defined ReferenceError: FontFace is not defin... | `css/css-fonts/mvs-shaping.html` |
| 3 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: trigger is not defined ReferenceError: trigger is not defined... | `css/selectors/focus-visible-script-focus-016.tentative.html` |
| 3 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'baseVal' of undefined TypeError: cannot read... | `svg/types/scripted/SVGLength-ch.html` |
| 3 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'insertRule' of undefined TypeError: cannot r... | `dom/events/webkit-animation-end-event.html` |
| 3 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (getSelection) is not a function TypeError: undefined (g... | `css/css-overflow/scroll-markers/scroll-buttons-selection.html` |
| 3 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (write) is not a function TypeError: undefined (write) i... | `custom-elements/parser/parser-constructs-custom-element-in-document-write.html` |
| 2 | CRASH: killed by signal Segmentation fault | `dom/nodes/moveBefore/relevant-mutations.html` |
| 2 | ERROR: ReferenceError: DOMParser is not defined | `domparsing/DOMParser-parseFromString-html.html` |
| 2 | ERROR: ReferenceError: SharedWorker is not defined | `workers/constructors/SharedWorker/setting-port-members.html` |
| 2 | ERROR: Test named 'Repeated declarative shadow roots keep only the first' specified N 'cleanup' function, and N failed. | `shadow-dom/declarative/declarative-shadow-dom-repeats.html` |
| 2 | ERROR: Test named 'writing-mode: horizontal-tb' specified N 'cleanup' function, and N failed. | `css/css-writing-modes/forms/input-range-block-size.html` |
| 2 | ERROR: TypeError: cannot read property 'length' of undefined | `css/css-transitions/starting-style-cascade.html` |
| 2 | ERROR: TypeError: cannot read property 'supports' of undefined | `fetch/metadata/generated/element-link-prefetch.https.optional.sub.html` |
| 2 | TIMEOUT: the page never reported; first script error: ../editing/include/editor-test-utils.js: SyntaxError: expected ';' (line N) SyntaxError: expect... | `selection/move-by-word-korean.html` |
| 2 | TIMEOUT: the page never reported; first script error: /css/css-scroll-snap/support/common.js: SyntaxError: expected ';' (line N) SyntaxError: expecte... | `css/cssom-view/scrollIntoView-multiple-nested.html` |
| 2 | TIMEOUT: the page never reported; first script error: /css/mediaqueries/resources/matchmedia-utils.js: SyntaxError: expected ';' (line N) SyntaxError... | `css/css-values/random-in-media.tentative.html` |
| 2 | TIMEOUT: the page never reported; first script error: /wai-aria/scripts/aria-utils.js: SyntaxError: expected ';' (line N) SyntaxError: expected ';' (... | `css/css-display/accessibility/display-contents-role-and-label.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: RangeError: invalid code point RangeError: invalid code point at <anonymous> ... | `css/css-text/text-transform/math/text-transform-math-auto-003.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: DOMParser is not defined ReferenceError: DOMParser is not def... | `domxpath/fn-lang.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: b is not defined ReferenceError: b is not defined at <anonymo... | `css/selectors/not-complex.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: button is not defined ReferenceError: button is not defined a... | `css/selectors/focus-visible-005.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: divNonConstructed is not defined ReferenceError: divNonConstr... | `css/cssom/CSSStyleSheet-constructable.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: feConvlveMatrix is not defined ReferenceError: feConvlveMatri... | `svg/animations/svginteger-animation-1.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: has_scope is not defined ReferenceError: has_scope is not def... | `css/selectors/invalidation/is-pseudo-containing-complex-in-has.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: hostN is not defined ReferenceError: hostN is not defined at ... | `css/selectors/is-where-shadow.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: inert is not defined ReferenceError: inert is not defined at ... | `css/css-ui/interactivity-inert-click.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: input is not defined ReferenceError: input is not defined at ... | `selection/textcontrols/selectionchange-bubble.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: outside is not defined ReferenceError: outside is not defined... | `shadow-dom/focus/click-focus-delegatesFocus-click.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: reference is not defined ReferenceError: reference is not def... | `css/css-multicol/inheritance.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: subject is not defined ReferenceError: subject is not defined... | `css/selectors/invalidation/has-invalidation-for-wiping-an-element.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: targetN is not defined ReferenceError: targetN is not defined... | `css/css-animations/animation-base-response-001.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: testInput is not defined ReferenceError: testInput is not def... | `css/css-writing-modes/forms/text-input-baseline.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: testN is not defined ReferenceError: testN is not defined at ... | `shadow-dom/event-post-dispatch.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: trustedTypes is not defined ReferenceError: trustedTypes is n... | `domparsing/tentative/positional-methods-with-parser-options.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'append' of undefined TypeError: cannot read ... | `resource-timing/TAO-match.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'customElements' of undefined TypeError: cann... | `dom/nodes/create-element-realm-after-adoption.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'getComputedStyle' of undefined TypeError: ca... | `css/css-transitions/transition-background-position-with-edge-offset.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (bound) is not a function TypeError: undefined (bound) i... | `IndexedDB/idbindex-cross-realm-methods.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (createProcessingInstruction) is not a function TypeErro... | `dom/nodes/Node-isEqualNode-xhtml.xhtml` |
| 1 | ERROR: N duplicate test name: "Calling function () { [source unavailable] } doesn't affect index iteration" | `IndexedDB/idbcursor-iterating-update.any.html` |
| 1 | ERROR: N duplicate test name: "Should throw TypeError for function "function () { [source unavailable] }"." | `webstorage/missing_arguments.window.html` |
| 1 | ERROR: N duplicate test name: "The source of the request from function () { [source unavailable] } is the index itself" | `IndexedDB/idbindex-request-source.any.html` |
| 1 | ERROR: N duplicate test name: "The source of the request from function () { [source unavailable] } is the object store itself" | `IndexedDB/idbobjectstore-request-source.any.html` |
| 1 | ERROR: N duplicate test name: "Unresolvable percentage heights are resolved as Npx in first pass (scrollable overflow)" | `css/css-tables/height-distribution/percentage-sizing-of-table-cell-children.html` |
| 1 | ERROR: N duplicate test name: "sending ND canvas ImageBitmap to http://N.N.N.N:N" | `webmessaging/postMessage_cross_domain_image_transfer_2d.sub.htm` |
| 1 | ERROR: N duplicate test names: " block-start", " block-end" | `css/css-overflow/scroll-overflow-padding-block-001.html` |
| 1 | ERROR: N duplicate test names: "a.classList in undefined namespace should be DOMTokenList.", "area.classList in undefined namespace should be DOMTo... | `dom/lists/DOMTokenList-coverage-for-attributes.html` |
| 1 | ERROR: N duplicate test names: "touchstart listener is passive by default for HTMLElement", "touchstart listener is passive with {passive:undefined... | `dom/events/passive-by-default.html` |
| 1 | ERROR: ReferenceError: container is not defined | `css/selectors/nth-of-type-namespace.html` |
| 1 | ERROR: Test named '::part():dir() invalidation' specified N 'cleanup' function, and N failed. | `css/selectors/invalidation/part-dir.html` |
| 1 | ERROR: Test named '::part():lang() invalidation' specified N 'cleanup' function, and N failed. | `css/selectors/invalidation/part-lang.html` |
| 1 | ERROR: Test named ':checked & :indeterminate invalidation on <input>' specified N 'cleanup' function, and N failed. | `css/selectors/invalidation/input-pseudo-classes-in-has.html` |
| 1 | ERROR: Test named ':focus via :scope in subject' specified N 'cleanup' function, and N failed. | `css/css-cascade/scope-focus.html` |
| 1 | ERROR: Test named ':focus-within should be adjusted on ancestors when popover enters/exits top layer.' specified N 'cleanup' function, and N failed... | `css/selectors/toplayer-transition-001.html` |
| 1 | ERROR: Test named ':hover via :scope in subject' specified N 'cleanup' function, and N failed. | `css/css-cascade/scope-hover.html` |
| 1 | ERROR: Test named ':in-range in :has() invalidation when setting readonly' specified N 'cleanup' function, and N failed. | `css/selectors/invalidation/input-in-range-in-has-with-readonly.html` |
| 1 | ERROR: Test named ':open pseudo-class invalidation with dialog.show() + dialog.open = false' specified N 'cleanup' function, and N failed. | `css/selectors/invalidation/open-pseudo-class-in-has.html` |
| 1 | ERROR: Test named '> .foo in @scope,.nest created by string valid' specified N 'cleanup' function, and N failed. | `css/css-cascade/at-scope-relative-syntax.html` |

## Why subtests fail

Ranked by *distinct tests* affected rather than by subtests, because that is the
number a fix unblocks. Digits are collapsed to `N`; quoted values are not, because
`expected "block" but got "inline"` and `expected "Npx" but got "Npx"` are
different bugs and a bucket labelled `assert_equals` is not actionable.

| tests | subtests | message | example |
|--:|--:|---|---|
| 257 | 1055 | Test timed out | `FileAPI/FileReader/workers.html` |
| 218 | 823 | assert_true: expected true got false | `console/console-is-a-namespace.any.html` |
| 202 | 202 | undefined (pauseAnimations) is not a function | `svg/animations/additive-type-by-animation.html` |
| 199 | 29244 | assert_true: 'from' value should be supported expected true got false | `css/CSS2/floats-clear/clear-no-interpolation.html` |
| 195 | 192127 | NOTRUN (no message) | `IndexedDB/database-names-by-origin.html` |
| 151 | 446 | assert_equals: expected N but got N | `css/CSS2/abspos/abspos-in-block-in-inline-in-relpos-inline.html` |
| 113 | 115 | assert_equals: expected "" but got "auto" | `css/css-align/parsing/align-content-invalid.html` |
| 105 | 170 | target is not defined | `css/CSS2/normal-flow/block-in-inline-hittest-float-001.html` |
| 90 | 303 | synchronous XMLHttpRequest is not supported | `xhr/XMLHttpRequest-withCredentials.any.html` |
| 84 | 201 | promise_test: Unhandled rejection with value: object "Error: action_sequence() is not implemented by testdriver-vendor.js" | `css/css-overflow/resizer-no-size-change.tentative.html` |
| 80 | 3006 | assert_true: 'to' value should be supported expected true got false | `css/CSS2/linebox/animations/line-height-interpolation.html` |
| 72 | 172 | promise_test: Unhandled rejection with value: object "Error: document.elementsFromPoint unsupported" | `IndexedDB/file_support.sub.html` |
| 69 | 754 | cannot read property 'cssRules' of undefined | `css/css-animations/parsing/keyframes-name-invalid.html` |
| 68 | 1945 | assert_throws_js: function "function TypeError() { [native code] }" is not an Error subtype | `IndexedDB/idbfactory_cmp.any.html` |
| 55 | 70 | assert_equals: expected "" but got "none" | `css/css-animations/parsing/animation-range-end-invalid.html` |
| 55 | 276 | undefined (__defineSetter__) is not a function | `IndexedDB/abort-in-initial-upgradeneeded.any.html` |
| 51 | 57 | assert_equals: expected "" but got "N" | `css/css-align/parsing/column-gap-invalid.html` |
| 48 | 230 | promise_test: Unhandled rejection with value: object "TypeError: Failed to fetch" | `FileAPI/url/url-with-fetch.any.html` |
| 47 | 138 | assert_equals: expected "Npx" but got "" | `css/css-align/gaps/column-gap-animation-001.html` |
| 46 | 562 | Illegal constructor: ReadableStream | `fetch/api/basic/request-upload.any.html` |
| 44 | 70 | SharedWorker is not defined | `eventsource/shared-worker/eventsource-close.htm` |
| 44 | 553 | assert_throws_js: function "function () { [source unavailable] }" did not throw | `FileAPI/blob/Blob-constructor.any.html` |
| 44 | 306 | promise_test: Unhandled rejection with value: object "TypeError: Illegal constructor: ReadableStream" | `encoding/streams/decode-ignore-bom.any.html` |
| 43 | 239 | assert_equals: Expected success event, but got upgradeneeded event instead expected "success" but got "upgradeneeded" | `IndexedDB/bindings-inject-keys-bypass.any.html` |
| 42 | 1124 | assert_throws_dom: function "function () { [source unavailable] }" did not throw | `css/cssom/CSSStyleSheet-constructable-baseURL.html` |
| 42 | 1436 | cannot read property 'N' of undefined | `css/css-animations/CSSAnimation-effect.tentative.html` |
| 41 | 1356 | assert_equals: expected "Npx " but got "Npx " | `css/CSS2/linebox/animations/line-height-interpolation.html` |
| 40 | 167 | promise_test: Unhandled rejection with value: object "TypeError: undefined (getAnimations) is not a function" | `css/css-animations/AnimationEffect-getComputedTiming.tentative.html` |
| 40 | 206 | undefined (getAnimations) is not a function | `css/css-animations/AnimationEffect-getComputedTiming.tentative.html` |
| 38 | 414 | undefined (open) is not a function | `FileAPI/url/url-charset.window.html` |
| 36 | 211 | promise_test: Unhandled rejection with value: object "TypeError: undefined (open) is not a function" | `cookies/samesite/fetch.https.html` |
| 33 | 111 | assert_equals: expected "Npx" but got "Npx" | `css/css-animations/CSSAnimation-compositeOrder.tentative.html` |
| 33 | 295 | assert_unreached: Should have rejected: undefined Reached unreachable code | `fetch/api/basic/header-value-null-byte.any.html` |
| 32 | 48 | assert_true: Failed to create new rendered document expected true got false | `shadow-dom/untriaged/elements-and-dom-objects/extensions-to-element-interface/methods/test-002.html` |
| 31 | 127 | container is not defined | `css/CSS2/positioning/detach-abspos-before-layout.html` |
| 31 | 31 | promise_test: Unhandled rejection with value: object "TypeError: cannot set property 'name' of undefined" | `FileAPI/idlharness.any.html` |
| 30 | 34 | assert_equals: expected "" but got "-Npx" | `css/css-align/parsing/column-gap-invalid.html` |
| 30 | 156 | assert_equals: expected (object) null but got (undefined) undefined | `css/cssom-view/offsetParent-body-and-html.html` |
| 30 | 320 | cannot read property 'baseVal' of undefined | `svg/animations/attribute-value-unaffected-by-animation-002.html` |
| 29 | 58 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'currentTime' of undefined" | `css/css-animations/empty-pseudo-class-with-animation.html` |
| 29 | 29 | undefined (setCurrentTime) is not a function | `svg/animations/conditional-processing-02.html` |
| 27 | 29 | assert_equals: expected "" but got "Npx" | `css/css-backgrounds/parsing/box-shadow-invalid.html` |
| 27 | 58 | cannot read property 'length' of undefined | `FileAPI/filelist-section/filelist.html` |
| 27 | 297 | promise_rejects_js: function "function TypeError() { [native code] }" is not an Error subtype | `FileAPI/url/url-with-fetch.any.html` |
| 25 | 31 | assert_equals: serialization should be canonical expected "Npx" but got "N" | `css/css-align/parsing/column-gap-valid.html` |
| 25 | 112 | main is not defined | `css/css-cascade/at-scope-parsing.html` |
| 23 | 24 | assert_equals: expected "" but got "-N%" | `css/css-align/parsing/column-gap-invalid.html` |
| 22 | 395 | KeyframeEffect is not defined | `css/css-animations/AnimationEffect-getComputedTiming.tentative.html` |
| 21 | 93 | Illegal constructor: CustomElementRegistry | `custom-elements/registries/Construct.html` |
| 21 | 25 | assert_equals: expected "" but got "N%" | `css/css-backgrounds/parsing/border-image-outset-invalid.html` |
| 21 | 54 | assert_equals: expected (number) N but got (undefined) undefined | `FileAPI/blob/Blob-constructor.any.html` |
| 20 | 75 | FormData is not defined | `custom-elements/form-associated/form-disabled-callback.html` |
| 20 | 171 | WritableStream is not defined | `streams/piping/general.any.html` |
| 20 | 28 | assert_equals: expected "rgb(N, N, N)" but got "rgb(N, N, N)" | `css/css-cascade/layer-replaceSync-clears-stale.html` |
| 19 | 7233 | assert_true: color doesn't seem to be supported in the computed style expected true got false | `css/css-color/color-mix-missing-components.html` |
| 19 | 24 | subject is not defined | `css/selectors/invalidation/defined-in-has.html` |
| 18 | 18 | assert_equals: expected "" but got "normal" | `css/css-fonts/parsing/font-optical-sizing-invalid.html` |
| 18 | 58 | undefined (createDocument) is not a function | `css/cssom/historical.html` |
| 17 | 58 | XPathResult is not defined | `dom/xpath-result-single-node-value-nullable.html` |
| 17 | 54 | assert_equals: expected "none" but got "" | `css/css-animations/parsing/animation-computed.html` |
| 17 | 85 | getSelection is not defined | `selection/modify-extend-word-trailing-inline-block.tentative.html` |
| 17 | 116 | promise_test: Unhandled rejection with value: object "ReferenceError: SharedWorker is not defined" | `fetch/api/cors/data-url-shared-worker.html` |
| 17 | 67 | undefined (add) is not a function | `IndexedDB/error-attributes.any.html` |
| 16 | 145 | DOMParser is not defined | `css/selectors/quirks-mode-import.html` |
| 16 | 19 | SVGAnimatedEnumeration is not defined | `svg/types/scripted/SVGAnimatedEnumeration-SVGClipPathElement.html` |
| 16 | 27 | assert_equals: entries.length expected N but got N | `intersection-observer/bounding-box.html` |
| 16 | 18 | assert_equals: expected "" but got "N N" | `css/css-animations/parsing/animation-invalid.html` |
| 16 | 16 | assert_equals: expected "Npx Npx Npx Npx Npx Npx Npx Npx Npx Npx" but got "" | `css/css-grid/grid-lanes/track-sizing/auto-repeat/column-auto-repeat-019.html` |
| 15 | 126 | CROSSDOMAIN is not defined | `cors/client-hint-request-headers-2.tentative.htm` |
| 15 | 15 | assert_equals: IntersectionObserverEntryCount expected N but got N | `intersection-observer/scroll-and-root-margin.html` |
| 15 | 15 | assert_equals: expected "" but got "Npx N%" | `css/css-align/parsing/grid-row-gap-invalid.html` |
| 15 | 86 | assert_equals: expected "normal" but got "" | `css/css-align/gaps/column-gap-parsing-001.html` |
| 15 | 107 | assert_false: expected false got true | `cors/preflight-failure.htm` |
| 15 | 19 | assert_true: expected true got undefined | `css/cssom/HTMLLinkElement-disabled-001.html` |
| 15 | 15 | promise_test: Unhandled rejection with value: object "ReferenceError: iframe is not defined" | `css/css-sizing/contain-intrinsic-size/forget-on-disconnect-in-iframe.html` |
| 15 | 60 | tN is not defined | `css/CSS2/normal-flow/block-in-inline-client-rects-001.html` |
| 15 | 29 | undefined (getElementById) is not a function | `css/css-transforms/transform-origin-in-shadow.html` |
| 14 | 42 | FileReader is not defined | `FileAPI/fileReader.any.html` |
| 14 | 68 | Illegal constructor: HTMLElement | `custom-elements/HTMLElement-attachInternals.html` |
| 14 | 20 | assert_equals: expected "Npx Npx Npx Npx" but got "" | `css/css-grid/grid-lanes/subgrid/grid-subgridded-to-grid-lanes/track-sizing/column-subgrid-auto-fit-003.html` |
| 14 | 14 | assert_equals: expected "hidden" but got "visible" | `css/css-overflow/logical-overflow-001.html` |
| 14 | 36 | assert_equals: expected "rgb(N, N, N)" but got "" | `css/css-animations/responsive/column-rule-color-001.html` |
| 14 | 241 | assert_true: grid-template-columns doesn't seem to be supported in the computed style expected true got false | `css/css-grid/grid-lanes/grid-lanes-grid-template-columns-computed-withcontent.html` |
| 14 | 77 | cannot read property 'insertRule' of undefined | `css/css-animations/KeyframeEffect-target.tentative.html` |
| 14 | 34 | fetchLater is not defined | `fetch/fetch-later/basic.https.window.html` |
| 14 | 95 | promise_test: Unhandled rejection with value: object "ReferenceError: Observable is not defined" | `dom/observable/tentative/crashtests/observable-gc.any.html` |
| 14 | 14 | promise_test: Unhandled rejection with value: object "ReferenceError: createStylesheetHost is not defined" | `shadow-dom/declarative/tentative/shadowrootadoptedstylesheets/shadowrootadoptedstylesheets-async-fetch-disconnect.html` |
| 14 | 17 | promise_test: Unhandled rejection with value: object "ReferenceError: scroller is not defined" | `css/css-overflow/overflow-auto-scrolling-with-margin-and-transform.html` |
| 14 | 14 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'append' of undefined" | `IndexedDB/back-forward-cache-open-transaction.window.html` |
| 14 | 457 | promise_test: Unhandled rejection with value: object "TypeError: undefined is not a function" | `css/cssom-view/element-scroll-arguments.html` |
| 14 | 183 | undefined (getBBox) is not a function | `svg/coordinate-systems/svgtransformlist-replaceitem.html` |
| 13 | 29 | Illegal constructor: Document | `custom-elements/Document-createElementNS.html` |
| 13 | 39 | Illegal constructor: EventTarget | `dom/events/AddEventListenerOptions-once.any.html` |
| 13 | 139 | assert_equals: Response N status is N, not N expected N but got N | `fetch/http-cache/304-update.any.html` |
| 13 | 13 | assert_equals: expected "" but got "-N" | `css/css-animations/parsing/animation-iteration-count-invalid.html` |
| 13 | 17 | assert_equals: expected "N%" but got "" | `css/css-align/gaps/column-gap-parsing-001.html` |
| 13 | 24 | promise_test: Unhandled rejection with value: object "Error: observe_entry: timeout" | `resource-timing/cross-origin-iframe.html` |
| 13 | 31 | targetN is not defined | `css/css-animations/animation-base-response-002.html` |
| 13 | 177 | undefined (createProcessingInstruction) is not a function | `dom/events/EventTarget-this-of-listener.html` |
| 12 | 114 | TransformStream is not defined | `streams/transferable/transform-stream.html` |

23147 distinct subtest messages and 245 distinct harness messages behind these numbers.
