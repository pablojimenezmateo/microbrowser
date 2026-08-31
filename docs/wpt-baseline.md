# The WPT baseline

**Generated**, by `microbrowser_wpt --summary docs/wpt-baseline.md`. Do not edit it: the
next run overwrites it, and that overwrite is the point -- the diff of this file is
what a session moved. The argument for the instrument is `docs/adr/0040`; the work it
sequences is `docs/wpt-plan.md`.

This file is written from `--summary-state` alone, and that state is committed at
`tests/wpt/summary-state.tsv`. `tools/wpt/baseline.sh` is what fills it -- sharded and
resumable, because a shard's counts are written only when it finishes (plan task B6).
Re-measure one area into this table with:

```
microbrowser_wpt --testharness-only --summary docs/wpt-baseline.md \
    --summary-state tests/wpt/summary-state.tsv <area>/
```

and commit both files: the run replaces that area's row and its causes, and keeps
every other row from the state. **Pass the state file.** Without it the document is
rewritten down to the areas the run measured -- complete-looking and wrong -- which
happened three times before the state was committed, so the writer refuses when the
table it is about to replace has more rows than the run has areas.

WPT revision: `4120ac0deb573634d8b7cd74c38ae9d647eebdb5`

1396185 of 1559777 subtests pass (89.5%) over 23146 tests.

**Do not quote that number.** Subtests are not comparable across areas: `encoding/legacy-mb-japanese` alone is 28% of every subtest here.
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
| `FileAPI` | 11 | 10 | 1 | 0 | 0 | 415 | 191 | 46.0 |
| `FileAPI/BlobURL` | 5 | 0 | 0 | 5 | 0 | 18 | 0 | 0.0 |
| `FileAPI/FileReader` | 2 | 1 | 0 | 1 | 0 | 2 | 1 | 50.0 |
| `FileAPI/blob` | 23 | 23 | 0 | 0 | 0 | 583 | 568 | 97.4 |
| `FileAPI/file` | 21 | 21 | 0 | 0 | 0 | 313 | 249 | 79.6 |
| `FileAPI/filelist-section` | 1 | 1 | 0 | 0 | 0 | 7 | 7 | 100.0 |
| `FileAPI/reading-data-section` | 26 | 26 | 0 | 0 | 0 | 96 | 96 | 100.0 |
| `FileAPI/url` | 15 | 6 | 3 | 6 | 0 | 69 | 26 | 37.7 |
| `IndexedDB` | 265 | 237 | 10 | 18 | 0 | 1872 | 71 | 3.8 |
| `IndexedDB/crashtests` | 1 | 1 | 0 | 0 | 0 | 1 | 1 | 100.0 |
| `console` | 19 | 19 | 0 | 0 | 0 | 107 | 102 | 95.3 |
| `content-security-policy` | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0.0 |
| `content-security-policy/base-uri` | 6 | 4 | 0 | 2 | 0 | 8 | 4 | 50.0 |
| `content-security-policy/blob` | 6 | 6 | 0 | 0 | 0 | 6 | 0 | 0.0 |
| `content-security-policy/child-src` | 9 | 6 | 0 | 3 | 0 | 11 | 5 | 45.5 |
| `content-security-policy/connect-src` | 30 | 30 | 0 | 0 | 0 | 38 | 0 | 0.0 |
| `content-security-policy/default-src` | 4 | 3 | 0 | 1 | 0 | 14 | 7 | 50.0 |
| `content-security-policy/embedded-enforcement` | 23 | 3 | 0 | 20 | 0 | 304 | 13 | 4.3 |
| `content-security-policy/font-src` | 5 | 0 | 0 | 5 | 0 | 5 | 0 | 0.0 |
| `content-security-policy/form-action` | 13 | 10 | 0 | 3 | 0 | 10 | 3 | 30.0 |
| `content-security-policy/frame-ancestors` | 34 | 11 | 0 | 23 | 0 | 36 | 8 | 22.2 |
| `content-security-policy/frame-src` | 12 | 5 | 0 | 7 | 0 | 17 | 4 | 23.5 |
| `content-security-policy/gen` | 260 | 0 | 0 | 260 | 0 | 2480 | 0 | 0.0 |
| `content-security-policy/generic` | 28 | 12 | 0 | 16 | 0 | 99 | 16 | 16.2 |
| `content-security-policy/img-src` | 15 | 4 | 0 | 11 | 0 | 20 | 5 | 25.0 |
| `content-security-policy/inheritance` | 26 | 4 | 3 | 16 | 3 | 58 | 1 | 1.7 |
| `content-security-policy/inside-worker` | 10 | 6 | 0 | 4 | 0 | 10 | 0 | 0.0 |
| `content-security-policy/media-src` | 10 | 0 | 0 | 10 | 0 | 29 | 0 | 0.0 |
| `content-security-policy/meta` | 5 | 4 | 0 | 1 | 0 | 6 | 1 | 16.7 |
| `content-security-policy/navigation` | 6 | 0 | 0 | 6 | 0 | 6 | 0 | 0.0 |
| `content-security-policy/nonce-hiding` | 8 | 3 | 1 | 4 | 0 | 69 | 13 | 18.8 |
| `content-security-policy/object-src` | 13 | 0 | 0 | 13 | 0 | 7 | 0 | 0.0 |
| `content-security-policy/parsing` | 2 | 0 | 0 | 2 | 0 | 19 | 0 | 0.0 |
| `content-security-policy/plugin-types` | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| `content-security-policy/report-hash` | 9 | 0 | 0 | 9 | 0 | 0 | 0 | 0.0 |
| `content-security-policy/reporting` | 31 | 7 | 1 | 22 | 1 | 33 | 4 | 12.1 |
| `content-security-policy/reporting-api` | 11 | 3 | 0 | 8 | 0 | 24 | 4 | 16.7 |
| `content-security-policy/resource-hints` | 9 | 0 | 5 | 4 | 0 | 26 | 0 | 0.0 |
| `content-security-policy/sandbox` | 17 | 13 | 0 | 4 | 0 | 26 | 1 | 3.8 |
| `content-security-policy/script-src` | 90 | 54 | 0 | 36 | 0 | 197 | 57 | 28.9 |
| `content-security-policy/script-src-attr-elem` | 8 | 3 | 0 | 5 | 0 | 10 | 3 | 30.0 |
| `content-security-policy/securitypolicyviolation` | 24 | 2 | 0 | 22 | 0 | 91 | 5 | 5.5 |
| `content-security-policy/style-src` | 42 | 24 | 0 | 18 | 0 | 86 | 47 | 54.7 |
| `content-security-policy/style-src-attr-elem` | 7 | 3 | 0 | 4 | 0 | 11 | 3 | 27.3 |
| `content-security-policy/svg` | 5 | 3 | 0 | 2 | 0 | 5 | 0 | 0.0 |
| `content-security-policy/unsafe-eval` | 15 | 14 | 0 | 1 | 0 | 20 | 1 | 5.0 |
| `content-security-policy/unsafe-hashes` | 24 | 1 | 0 | 23 | 0 | 12 | 1 | 8.3 |
| `content-security-policy/wasm-unsafe-eval` | 10 | 8 | 0 | 2 | 0 | 33 | 0 | 0.0 |
| `content-security-policy/webrtc` | 5 | 5 | 0 | 0 | 0 | 5 | 0 | 0.0 |
| `content-security-policy/worker-src` | 31 | 21 | 0 | 10 | 0 | 34 | 6 | 17.6 |
| `content-security-policy/xslt` | 3 | 2 | 0 | 1 | 0 | 3 | 0 | 0.0 |
| `cookies` | 3 | 3 | 0 | 0 | 0 | 3 | 1 | 33.3 |
| `cookies/attributes` | 9 | 3 | 0 | 6 | 0 | 524 | 0 | 0.0 |
| `cookies/domain` | 5 | 0 | 0 | 5 | 0 | 12 | 1 | 8.3 |
| `cookies/encoding` | 1 | 0 | 0 | 1 | 0 | 6 | 0 | 0.0 |
| `cookies/name` | 2 | 0 | 0 | 2 | 0 | 111 | 0 | 0.0 |
| `cookies/ordering` | 1 | 1 | 0 | 0 | 0 | 1 | 0 | 0.0 |
| `cookies/origin-bound-cookies` | 2 | 2 | 0 | 0 | 0 | 2 | 0 | 0.0 |
| `cookies/partitioned-cookies` | 7 | 2 | 1 | 4 | 0 | 6 | 0 | 0.0 |
| `cookies/path` | 2 | 1 | 0 | 1 | 0 | 17 | 0 | 0.0 |
| `cookies/prefix` | 11 | 5 | 6 | 0 | 0 | 183 | 0 | 0.0 |
| `cookies/samesite` | 22 | 6 | 0 | 16 | 0 | 126 | 0 | 0.0 |
| `cookies/samesite-none-secure` | 1 | 1 | 0 | 0 | 0 | 1 | 0 | 0.0 |
| `cookies/schemeful-same-site` | 4 | 1 | 0 | 3 | 0 | 6 | 0 | 0.0 |
| `cookies/secure` | 6 | 3 | 0 | 3 | 0 | 10 | 2 | 20.0 |
| `cookies/size` | 2 | 0 | 0 | 2 | 0 | 25 | 0 | 0.0 |
| `cookies/third-party-cookies` | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| `cookies/value` | 2 | 0 | 0 | 2 | 0 | 94 | 0 | 0.0 |
| `cors` | 27 | 19 | 1 | 7 | 0 | 228 | 37 | 16.2 |
| `css/CSS2` | 66 | 64 | 0 | 2 | 0 | 2480 | 1095 | 44.2 |
| `css/css-align` | 234 | 232 | 0 | 2 | 0 | 5186 | 960 | 18.5 |
| `css/css-animations` | 124 | 117 | 0 | 7 | 0 | 1403 | 441 | 31.4 |
| `css/css-backgrounds` | 121 | 121 | 0 | 0 | 0 | 6181 | 804 | 13.0 |
| `css/css-box` | 83 | 78 | 0 | 5 | 0 | 1097 | 376 | 34.3 |
| `css/css-cascade` | 96 | 91 | 0 | 5 | 0 | 803 | 295 | 36.7 |
| `css/css-color` | 62 | 62 | 0 | 0 | 0 | 11338 | 4799 | 42.3 |
| `css/css-conditional` | 223 | 39 | 184 | 0 | 0 | 1868 | 1669 | 89.3 |
| `css/css-display` | 33 | 30 | 0 | 3 | 0 | 438 | 100 | 22.8 |
| `css/css-flexbox` | 369 | 363 | 0 | 6 | 0 | 4638 | 756 | 16.3 |
| `css/css-fonts` | 163 | 148 | 1 | 14 | 0 | 8149 | 2827 | 34.7 |
| `css/css-grid` | 652 | 327 | 0 | 325 | 0 | 8268 | 658 | 8.0 |
| `css/css-images` | 41 | 41 | 0 | 0 | 0 | 3587 | 378 | 10.5 |
| `css/css-multicol` | 94 | 91 | 0 | 3 | 0 | 1596 | 61 | 3.8 |
| `css/css-overflow` | 202 | 187 | 1 | 14 | 0 | 1092 | 304 | 27.8 |
| `css/css-position` | 111 | 107 | 1 | 3 | 0 | 1478 | 328 | 22.2 |
| `css/css-shapes` | 144 | 126 | 0 | 18 | 0 | 6149 | 536 | 8.7 |
| `css/css-sizing` | 205 | 185 | 6 | 14 | 0 | 5274 | 798 | 15.1 |
| `css/css-syntax` | 40 | 40 | 0 | 0 | 0 | 429 | 387 | 90.2 |
| `css/css-tables` | 135 | 129 | 1 | 5 | 0 | 854 | 228 | 26.7 |
| `css/css-text` | 368 | 254 | 10 | 104 | 0 | 2927 | 1234 | 42.2 |
| `css/css-text-decor` | 48 | 47 | 0 | 1 | 0 | 1276 | 102 | 8.0 |
| `css/css-transforms` | 107 | 105 | 0 | 2 | 0 | 5508 | 1087 | 19.7 |
| `css/css-transitions` | 120 | 95 | 0 | 25 | 0 | 3305 | 304 | 9.2 |
| `css/css-ui` | 121 | 114 | 1 | 6 | 0 | 1936 | 338 | 17.5 |
| `css/css-values` | 268 | 248 | 0 | 20 | 0 | 10030 | 1362 | 13.6 |
| `css/css-variables` | 59 | 47 | 0 | 12 | 0 | 542 | 213 | 39.3 |
| `css/css-writing-modes` | 82 | 74 | 0 | 8 | 0 | 299 | 62 | 20.7 |
| `css/cssom` | 190 | 170 | 0 | 20 | 0 | 2174 | 1664 | 76.5 |
| `css/cssom-view` | 225 | 208 | 0 | 16 | 1 | 2145 | 938 | 43.7 |
| `css/selectors` | 277 | 253 | 1 | 23 | 0 | 5783 | 4413 | 76.3 |
| `custom-elements` | 44 | 44 | 0 | 0 | 0 | 1052 | 250 | 23.8 |
| `custom-elements/form-associated` | 18 | 14 | 3 | 1 | 0 | 103 | 3 | 2.9 |
| `custom-elements/htmlconstructor` | 2 | 2 | 0 | 0 | 0 | 20 | 2 | 10.0 |
| `custom-elements/parser` | 11 | 9 | 0 | 2 | 0 | 22 | 8 | 36.4 |
| `custom-elements/reactions` | 57 | 57 | 0 | 0 | 0 | 534 | 199 | 37.3 |
| `custom-elements/registries` | 40 | 32 | 3 | 5 | 0 | 2322 | 2033 | 87.6 |
| `custom-elements/state` | 5 | 4 | 1 | 0 | 0 | 28 | 2 | 7.1 |
| `custom-elements/upgrading` | 7 | 6 | 0 | 1 | 0 | 29 | 1 | 3.4 |
| `dom` | 10 | 10 | 0 | 0 | 0 | 2034 | 1012 | 49.8 |
| `dom/abort` | 10 | 8 | 0 | 2 | 0 | 72 | 67 | 93.1 |
| `dom/collections` | 10 | 10 | 0 | 0 | 0 | 53 | 7 | 13.2 |
| `dom/events` | 202 | 180 | 0 | 22 | 0 | 844 | 444 | 52.6 |
| `dom/lists` | 5 | 5 | 0 | 0 | 0 | 189 | 187 | 98.9 |
| `dom/nodes` | 327 | 289 | 0 | 38 | 0 | 15069 | 14052 | 93.3 |
| `dom/observable` | 52 | 52 | 0 | 0 | 0 | 525 | 2 | 0.4 |
| `dom/ranges` | 58 | 56 | 0 | 2 | 0 | 30521 | 29103 | 95.4 |
| `dom/traversal` | 18 | 18 | 0 | 0 | 0 | 1608 | 1584 | 98.5 |
| `domparsing` | 34 | 34 | 0 | 0 | 0 | 534 | 422 | 79.0 |
| `domparsing/tentative` | 26 | 17 | 0 | 9 | 0 | 905 | 28 | 3.1 |
| `domxpath` | 32 | 30 | 0 | 2 | 0 | 126 | 2 | 1.6 |
| `encoding` | 74 | 60 | 0 | 14 | 0 | 50950 | 50803 | 99.7 |
| `encoding/legacy-mb-japanese` | 482 | 472 | 2 | 8 | 0 | 442614 | 439938 | 99.4 |
| `encoding/legacy-mb-korean` | 435 | 435 | 0 | 0 | 0 | 410448 | 410448 | 100.0 |
| `encoding/legacy-mb-schinese` | 6 | 6 | 0 | 0 | 0 | 1017 | 1017 | 100.0 |
| `encoding/legacy-mb-tchinese` | 283 | 283 | 0 | 0 | 0 | 268916 | 267471 | 99.5 |
| `encoding/streams` | 13 | 12 | 0 | 1 | 0 | 111 | 0 | 0.0 |
| `eventsource` | 76 | 40 | 0 | 36 | 0 | 112 | 1 | 0.9 |
| `eventsource/dedicated-worker` | 10 | 9 | 0 | 1 | 0 | 13 | 0 | 0.0 |
| `eventsource/shared-worker` | 7 | 7 | 0 | 0 | 0 | 10 | 0 | 0.0 |
| `fetch/api` | 223 | 207 | 2 | 14 | 0 | 3304 | 1147 | 34.7 |
| `fetch/compression-dictionary` | 31 | 25 | 0 | 6 | 0 | 101 | 2 | 2.0 |
| `fetch/connection-pool` | 1 | 0 | 0 | 1 | 0 | 9 | 0 | 0.0 |
| `fetch/content-encoding` | 13 | 13 | 0 | 0 | 0 | 36 | 4 | 11.1 |
| `fetch/content-length` | 5 | 4 | 0 | 1 | 0 | 41 | 1 | 2.4 |
| `fetch/content-type` | 5 | 3 | 0 | 2 | 0 | 155 | 18 | 11.6 |
| `fetch/corb` | 14 | 8 | 0 | 6 | 0 | 77 | 33 | 42.9 |
| `fetch/cross-origin-resource-policy` | 12 | 9 | 0 | 3 | 0 | 68 | 21 | 30.9 |
| `fetch/data-urls` | 3 | 3 | 0 | 0 | 0 | 161 | 79 | 49.1 |
| `fetch/fetch-later` | 47 | 34 | 0 | 13 | 0 | 154 | 8 | 5.2 |
| `fetch/h1-parsing` | 20 | 19 | 0 | 1 | 0 | 231 | 14 | 6.1 |
| `fetch/http-cache` | 14 | 14 | 0 | 0 | 0 | 142 | 0 | 0.0 |
| `fetch/images` | 1 | 0 | 0 | 1 | 0 | 1 | 0 | 0.0 |
| `fetch/local-network-access` | 16 | 0 | 9 | 7 | 0 | 35 | 0 | 0.0 |
| `fetch/metadata` | 87 | 42 | 6 | 39 | 0 | 1670 | 2 | 0.1 |
| `fetch/nosniff` | 6 | 3 | 0 | 3 | 0 | 65 | 18 | 27.7 |
| `fetch/orb` | 17 | 9 | 0 | 7 | 0 | 169 | 22 | 13.0 |
| `fetch/origin` | 1 | 0 | 0 | 1 | 0 | 43 | 0 | 0.0 |
| `fetch/range` | 11 | 10 | 0 | 1 | 0 | 80 | 4 | 5.0 |
| `fetch/redirect-navigate` | 2 | 0 | 0 | 2 | 0 | 121 | 0 | 0.0 |
| `fetch/redirects` | 2 | 1 | 0 | 1 | 0 | 7 | 1 | 14.3 |
| `fetch/security` | 11 | 7 | 0 | 4 | 0 | 90 | 20 | 22.2 |
| `fetch/stale-while-revalidate` | 6 | 2 | 0 | 4 | 0 | 6 | 0 | 0.0 |
| `hr-time` | 15 | 12 | 0 | 3 | 0 | 58 | 45 | 77.6 |
| `html/anonymous-iframe` | 33 | 15 | 3 | 15 | 0 | 30 | 0 | 0.0 |
| `html/browsers` | 754 | 302 | 24 | 428 | 0 | 2241 | 277 | 12.4 |
| `html/canvas` | 3326 | 3276 | 1 | 49 | 0 | 5678 | 1068 | 18.8 |
| `html/capability-delegation` | 6 | 0 | 0 | 6 | 0 | 16 | 0 | 0.0 |
| `html/cross-origin-embedder-policy` | 88 | 51 | 8 | 29 | 0 | 446 | 42 | 9.4 |
| `html/cross-origin-opener-policy` | 158 | 49 | 36 | 55 | 18 | 485 | 5 | 1.0 |
| `html/document-isolation-policy` | 38 | 34 | 2 | 2 | 0 | 152 | 1 | 0.7 |
| `html/dom` | 277 | 264 | 0 | 13 | 0 | 67642 | 59926 | 88.6 |
| `html/editing` | 223 | 91 | 0 | 132 | 0 | 793 | 426 | 53.7 |
| `html/embedded-content` | 1 | 1 | 0 | 0 | 0 | 2 | 0 | 0.0 |
| `html/infrastructure` | 117 | 74 | 0 | 42 | 1 | 683 | 190 | 27.8 |
| `html/interaction` | 193 | 186 | 2 | 5 | 0 | 740 | 295 | 39.9 |
| `html/links` | 6 | 4 | 0 | 2 | 0 | 6 | 3 | 50.0 |
| `html/meta` | 1 | 0 | 0 | 1 | 0 | 3 | 0 | 0.0 |
| `html/obsolete` | 14 | 14 | 0 | 0 | 0 | 53 | 40 | 75.5 |
| `html/rendering` | 153 | 141 | 2 | 10 | 0 | 9692 | 7264 | 74.9 |
| `html/scripting` | 2 | 2 | 0 | 0 | 0 | 3 | 1 | 33.3 |
| `html/select` | 1 | 1 | 0 | 0 | 0 | 5 | 0 | 0.0 |
| `html/semantics` | 2262 | 1680 | 37 | 545 | 0 | 18440 | 5467 | 29.6 |
| `html/syntax` | 378 | 76 | 0 | 302 | 0 | 3475 | 2865 | 82.4 |
| `html/the-xhtml-syntax` | 13 | 1 | 0 | 12 | 0 | 1 | 0 | 0.0 |
| `html/user-activation` | 21 | 12 | 0 | 9 | 0 | 33 | 10 | 30.3 |
| `html/webappapis` | 345 | 269 | 9 | 66 | 1 | 3328 | 1214 | 36.5 |
| `intersection-observer` | 106 | 100 | 1 | 5 | 0 | 319 | 132 | 41.4 |
| `intersection-observer/v2` | 38 | 23 | 0 | 15 | 0 | 70 | 20 | 28.6 |
| `media-source` | 73 | 50 | 0 | 23 | 0 | 484 | 118 | 24.4 |
| `media-source/dedicated-worker` | 7 | 7 | 0 | 0 | 0 | 62 | 3 | 4.8 |
| `media-source/mse-for-webcodecs` | 3 | 0 | 3 | 0 | 0 | 0 | 0 | 0.0 |
| `mimesniff/media` | 1 | 0 | 0 | 1 | 0 | 42 | 0 | 0.0 |
| `mimesniff/mime-types` | 3 | 3 | 0 | 0 | 0 | 3837 | 2633 | 68.6 |
| `mimesniff/sniffing` | 3 | 1 | 2 | 0 | 0 | 7 | 2 | 28.6 |
| `navigation-timing` | 58 | 29 | 0 | 29 | 0 | 334 | 130 | 38.9 |
| `performance-timeline` | 58 | 39 | 2 | 17 | 0 | 157 | 75 | 47.8 |
| `performance-timeline/not-restored-reasons` | 13 | 1 | 0 | 12 | 0 | 13 | 0 | 0.0 |
| `png` | 3 | 0 | 0 | 3 | 0 | 3 | 0 | 0.0 |
| `referrer-policy/4K` | 108 | 48 | 0 | 60 | 0 | 392 | 0 | 0.0 |
| `referrer-policy/4K+1` | 108 | 48 | 0 | 60 | 0 | 392 | 0 | 0.0 |
| `referrer-policy/4K-1` | 108 | 48 | 0 | 60 | 0 | 392 | 0 | 0.0 |
| `referrer-policy/css-integration` | 24 | 16 | 0 | 8 | 0 | 60 | 0 | 0.0 |
| `referrer-policy/gen` | 1001 | 118 | 0 | 883 | 0 | 6921 | 0 | 0.0 |
| `referrer-policy/generic` | 42 | 18 | 0 | 24 | 0 | 110 | 3 | 2.7 |
| `resize-observer` | 35 | 26 | 0 | 9 | 0 | 96 | 28 | 29.2 |
| `resource-timing` | 103 | 63 | 2 | 38 | 0 | 994 | 31 | 3.1 |
| `resource-timing/initiator-type` | 16 | 12 | 0 | 4 | 0 | 50 | 8 | 16.0 |
| `resource-timing/tentative` | 26 | 21 | 1 | 4 | 0 | 45 | 0 | 0.0 |
| `selection` | 88 | 84 | 0 | 4 | 0 | 33810 | 33616 | 99.4 |
| `selection/anonymous` | 3 | 3 | 0 | 0 | 0 | 13 | 2 | 15.4 |
| `selection/bidi` | 3 | 3 | 0 | 0 | 0 | 92 | 0 | 0.0 |
| `selection/caret` | 3 | 3 | 0 | 0 | 0 | 27 | 9 | 33.3 |
| `selection/contenteditable` | 12 | 12 | 0 | 0 | 0 | 250 | 12 | 4.8 |
| `selection/shadow-dom` | 13 | 13 | 0 | 0 | 0 | 80 | 18 | 22.5 |
| `selection/textcontrols` | 9 | 9 | 0 | 0 | 0 | 78 | 11 | 14.1 |
| `shadow-dom` | 66 | 62 | 0 | 4 | 0 | 763 | 417 | 54.7 |
| `shadow-dom/declarative` | 56 | 47 | 0 | 9 | 0 | 7791 | 7733 | 99.3 |
| `shadow-dom/focus` | 37 | 35 | 0 | 2 | 0 | 89 | 14 | 15.7 |
| `shadow-dom/focus-navigation` | 45 | 45 | 0 | 0 | 0 | 89 | 9 | 10.1 |
| `shadow-dom/leaktests` | 4 | 4 | 0 | 0 | 0 | 16 | 8 | 50.0 |
| `shadow-dom/reference-target` | 15 | 13 | 0 | 2 | 0 | 858 | 696 | 81.1 |
| `shadow-dom/untriaged` | 54 | 53 | 0 | 1 | 0 | 251 | 216 | 86.1 |
| `storage` | 28 | 24 | 0 | 4 | 0 | 103 | 34 | 33.0 |
| `storage/buckets` | 10 | 9 | 0 | 1 | 0 | 100 | 17 | 17.0 |
| `streams` | 3 | 2 | 0 | 1 | 0 | 230 | 10 | 4.3 |
| `streams/piping` | 13 | 13 | 0 | 0 | 0 | 204 | 0 | 0.0 |
| `streams/readable-byte-streams` | 11 | 10 | 0 | 1 | 0 | 233 | 8 | 3.4 |
| `streams/readable-streams` | 22 | 22 | 0 | 0 | 0 | 371 | 36 | 9.7 |
| `streams/transferable` | 12 | 8 | 1 | 3 | 0 | 75 | 0 | 0.0 |
| `streams/transform-streams` | 12 | 12 | 0 | 0 | 0 | 134 | 0 | 0.0 |
| `streams/writable-streams` | 16 | 16 | 0 | 0 | 0 | 196 | 0 | 0.0 |
| `subresource-integrity` | 1 | 0 | 0 | 1 | 0 | 48 | 17 | 35.4 |
| `subresource-integrity/integrity-policy` | 8 | 1 | 0 | 7 | 0 | 72 | 3 | 4.2 |
| `subresource-integrity/signatures` | 15 | 14 | 0 | 1 | 0 | 177 | 60 | 33.9 |
| `subresource-integrity/unencoded-digest` | 9 | 7 | 0 | 2 | 0 | 210 | 7 | 3.3 |
| `svg` | 3 | 3 | 0 | 0 | 0 | 1778 | 93 | 5.2 |
| `svg/animations` | 289 | 260 | 1 | 28 | 0 | 322 | 4 | 1.2 |
| `svg/coordinate-systems` | 8 | 7 | 0 | 1 | 0 | 39 | 0 | 0.0 |
| `svg/embedded` | 2 | 1 | 0 | 1 | 0 | 6 | 0 | 0.0 |
| `svg/extensibility` | 7 | 5 | 0 | 2 | 0 | 5 | 3 | 60.0 |
| `svg/fonts` | 3 | 3 | 0 | 0 | 0 | 3 | 0 | 0.0 |
| `svg/geometry` | 61 | 12 | 0 | 49 | 0 | 283 | 27 | 9.5 |
| `svg/import` | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| `svg/interact` | 33 | 17 | 0 | 16 | 0 | 109 | 46 | 42.2 |
| `svg/linking` | 18 | 5 | 0 | 13 | 0 | 34 | 0 | 0.0 |
| `svg/painting` | 70 | 6 | 0 | 64 | 0 | 283 | 0 | 0.0 |
| `svg/path` | 41 | 29 | 0 | 12 | 0 | 208 | 0 | 0.0 |
| `svg/pservers` | 12 | 3 | 0 | 9 | 0 | 11 | 0 | 0.0 |
| `svg/render` | 1 | 1 | 0 | 0 | 0 | 1 | 1 | 100.0 |
| `svg/scripted` | 9 | 3 | 0 | 6 | 0 | 4 | 1 | 25.0 |
| `svg/shapes` | 7 | 0 | 0 | 7 | 0 | 0 | 0 | 0.0 |
| `svg/struct` | 16 | 5 | 0 | 11 | 0 | 49 | 7 | 14.3 |
| `svg/styling` | 16 | 13 | 0 | 3 | 0 | 50 | 18 | 36.0 |
| `svg/svg-in-svg` | 1 | 1 | 0 | 0 | 0 | 1 | 1 | 100.0 |
| `svg/text` | 32 | 9 | 0 | 23 | 0 | 39 | 0 | 0.0 |
| `svg/types` | 86 | 59 | 4 | 23 | 0 | 571 | 58 | 10.2 |
| `uievents` | 3 | 3 | 0 | 0 | 0 | 167 | 40 | 24.0 |
| `uievents/click` | 7 | 3 | 0 | 4 | 0 | 7 | 3 | 42.9 |
| `uievents/constructors` | 2 | 2 | 0 | 0 | 0 | 20 | 2 | 10.0 |
| `uievents/interface` | 3 | 1 | 0 | 2 | 0 | 8 | 0 | 0.0 |
| `uievents/keyboard` | 5 | 4 | 0 | 1 | 0 | 15 | 3 | 20.0 |
| `uievents/legacy` | 1 | 1 | 0 | 0 | 0 | 4 | 0 | 0.0 |
| `uievents/legacy-domevents-tests` | 4 | 3 | 0 | 1 | 0 | 4 | 3 | 75.0 |
| `uievents/mouse` | 22 | 18 | 0 | 4 | 0 | 138 | 18 | 13.0 |
| `uievents/order-of-events` | 16 | 9 | 0 | 7 | 0 | 12 | 6 | 50.0 |
| `uievents/textInput` | 7 | 5 | 0 | 2 | 0 | 24 | 1 | 4.2 |
| `upgrade-insecure-requests` | 1 | 0 | 1 | 0 | 0 | 8 | 0 | 0.0 |
| `upgrade-insecure-requests/gen` | 196 | 195 | 0 | 1 | 0 | 992 | 0 | 0.0 |
| `url` | 77 | 75 | 0 | 2 | 0 | 15655 | 15652 | 100.0 |
| `user-timing` | 58 | 56 | 0 | 2 | 0 | 726 | 678 | 93.4 |
| `web-animations` | 1 | 1 | 0 | 0 | 0 | 230 | 55 | 23.9 |
| `web-animations/animation-model` | 25 | 25 | 0 | 0 | 0 | 146 | 23 | 15.8 |
| `web-animations/animation-trigger` | 3 | 3 | 0 | 0 | 0 | 3 | 0 | 0.0 |
| `web-animations/interfaces` | 42 | 38 | 0 | 4 | 0 | 829 | 50 | 6.0 |
| `web-animations/responsive` | 40 | 40 | 0 | 0 | 0 | 99 | 4 | 4.0 |
| `web-animations/timing-model` | 28 | 26 | 0 | 2 | 0 | 399 | 6 | 1.5 |
| `webmessaging` | 60 | 33 | 3 | 24 | 0 | 86 | 30 | 34.9 |
| `webmessaging/broadcastchannel` | 14 | 9 | 1 | 4 | 0 | 68 | 17 | 25.0 |
| `webmessaging/message-channels` | 23 | 16 | 0 | 7 | 0 | 39 | 17 | 43.6 |
| `webmessaging/multi-globals` | 4 | 4 | 0 | 0 | 0 | 4 | 2 | 50.0 |
| `webmessaging/with-options` | 9 | 2 | 0 | 7 | 0 | 9 | 0 | 0.0 |
| `webmessaging/with-ports` | 24 | 10 | 0 | 14 | 0 | 39 | 1 | 2.6 |
| `webmessaging/without-ports` | 27 | 13 | 0 | 14 | 0 | 43 | 6 | 14.0 |
| `websockets` | 382 | 96 | 135 | 151 | 0 | 1081 | 174 | 16.1 |
| `websockets/binary` | 12 | 8 | 0 | 4 | 0 | 12 | 0 | 0.0 |
| `websockets/closing-handshake` | 9 | 6 | 0 | 3 | 0 | 9 | 0 | 0.0 |
| `websockets/constructor` | 55 | 44 | 0 | 11 | 0 | 544 | 20 | 3.7 |
| `websockets/cookies` | 21 | 14 | 0 | 7 | 0 | 21 | 0 | 0.0 |
| `websockets/interfaces` | 168 | 142 | 0 | 26 | 0 | 289 | 77 | 26.6 |
| `websockets/keeping-connection-open` | 3 | 2 | 0 | 1 | 0 | 3 | 0 | 0.0 |
| `websockets/multi-globals` | 2 | 2 | 0 | 0 | 0 | 3 | 0 | 0.0 |
| `websockets/opening-handshake` | 14 | 8 | 0 | 6 | 0 | 14 | 2 | 14.3 |
| `websockets/security` | 4 | 3 | 0 | 1 | 0 | 4 | 1 | 25.0 |
| `websockets/stream` | 21 | 21 | 0 | 0 | 0 | 159 | 0 | 0.0 |
| `websockets/unload-a-document` | 11 | 0 | 0 | 11 | 0 | 11 | 0 | 0.0 |
| `webstorage` | 54 | 38 | 1 | 15 | 0 | 1282 | 1166 | 91.0 |
| `workers` | 112 | 81 | 1 | 30 | 0 | 281 | 53 | 18.9 |
| `workers/baseurl` | 7 | 3 | 0 | 4 | 0 | 3 | 0 | 0.0 |
| `workers/constructors` | 35 | 30 | 1 | 4 | 0 | 139 | 84 | 60.4 |
| `workers/examples` | 2 | 2 | 0 | 0 | 0 | 3 | 3 | 100.0 |
| `workers/interfaces` | 66 | 58 | 0 | 8 | 0 | 157 | 59 | 37.6 |
| `workers/modules` | 25 | 14 | 2 | 9 | 0 | 237 | 6 | 2.5 |
| `workers/multi-globals` | 1 | 1 | 0 | 0 | 0 | 1 | 0 | 0.0 |
| `workers/same-site-cookies` | 6 | 6 | 0 | 0 | 0 | 6 | 0 | 0.0 |
| `workers/semantics` | 30 | 21 | 0 | 9 | 0 | 274 | 47 | 17.2 |
| `xhr` | 398 | 362 | 4 | 32 | 0 | 1924 | 696 | 36.2 |
| `xhr/formdata` | 27 | 27 | 0 | 0 | 0 | 119 | 84 | 70.6 |

## Why the harness never reported

Ranked by tests affected. One line here is worth more than a page of the table
above: a test whose harness failed reports *no* subtests, so these are invisible in
the pass rate and are the largest block of unrealised coverage in the suite.

| tests | cause | example |
|--:|---|---|
| 4853 | TIMEOUT:  | `FileAPI/BlobURL/opaque-origin.html` |
| 211 | TIMEOUT: the page never reported | `FileAPI/url/url-in-tags-revoke.window.html` |
| 129 | ERROR: Error in remote: assert_true: Browser does not support WebSocket expected true got false | `websockets/Close-1000-reason.any.worker.html?default` |
| 110 | ERROR: Error: assert_implements: Basic support for size container queries required undefined | `css/css-conditional/container-queries/animation-container-type-dynamic.html` |
| 54 | ERROR: Error: assert_implements: Basic support for scroll-state container queries required undefined | `css/css-conditional/container-queries/scroll-state/at-container-scrollable-parsing.html` |
| 52 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: test is not defined ReferenceError: test is not defined at <a... | `css/css-values/urls/resolve-relative-to-base.sub.html` |
| 37 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: test_valid_value is not defined ReferenceError: test_valid_va... | `svg/geometry/parsing/cx-valid.svg` |
| 36 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: test_computed_value is not defined ReferenceError: test_compu... | `svg/geometry/parsing/cx-computed.svg` |
| 36 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: test_invalid_value is not defined ReferenceError: test_invali... | `svg/geometry/parsing/cx-invalid.svg` |
| 20 | ERROR: Error: assert_implements: Basic support for style container queries required undefined | `css/css-conditional/container-queries/at-container-style-parsing.html` |
| 18 | CRASH: killed by signal Broken pipe | `html/cross-origin-opener-policy/iframe-popup-same-origin-allow-popups-to-same-origin.https.html?1-2` |
| 18 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: async_test is not defined ReferenceError: async_test is not d... | `content-security-policy/base-uri/report-uri-does-not-respect-base-uri.sub.html` |
| 14 | ERROR: TypeError: undefined (addTextTrack) is not a function | `html/semantics/embedded-content/media-elements/interfaces/TextTrack/activeCues.html` |
| 11 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: test_invalid_attribute_value is not defined ReferenceError: t... | `svg/geometry/parsing/cx-attribute-invalid.svg` |
| 11 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: test_valid_attribute_value is not defined ReferenceError: tes... | `svg/geometry/parsing/cx-attribute-valid.svg` |
| 10 | ERROR: Error: assert_implements: undefined | `css/css-text/text-spacing-trim/text-spacing-trim-combinations-001.html?class=htb&test=MO:FH` |
| 8 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: promise_test is not defined ReferenceError: promise_test is n... | `content-security-policy/navigation/to-javascript-url-script-src.html` |
| 8 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: [object Object] (test) is not a function TypeError: [object Object... | `svg/linking/scripted/a.hreflang-getter-01.svg` |
| 7 | ERROR: TypeError: this attribute has no supported tokens | `content-security-policy/resource-hints/prefetch-allowed-by-any-directive.sub.html` |
| 6 | CRASH: killed by signal Segmentation fault | `content-security-policy/inheritance/inheritance-from-initiator.sub.html?scheme=blob` |
| 6 | ERROR: DOMMatrix is not defined | `IndexedDB/structured-clone.any.worker.html?1-20` |
| 6 | ERROR: Error in remote: WebSocket is not defined | `websockets/Close-delayed.any.worker.html?default` |
| 6 | ERROR: Error: assert_true: expected true got undefined | `fetch/local-network-access/iframe.tentative.https.window.html?include=from-local` |
| 6 | ERROR: Test named 'Basic usage' specified N 'cleanup' function, and N failed. | `css/css-sizing/contain-intrinsic-size/auto-006.html` |
| 5 | ERROR: Error in remote: script ran too long | `IndexedDB/blob-composite-blob-reads.any.worker.html` |
| 5 | ERROR: TypeError: undefined (set_rph_registration_mode) is not a function | `html/webappapis/system-state-and-capabilities/the-navigator-object/protocol-handler-fragment-nosw.https.html` |
| 5 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: opener is not defined ReferenceError: opener is not defined a... | `content-security-policy/form-action/form-action-src-allowed-target-blank.sub.html` |
| 4 | ERROR: ReferenceError: caches is not defined | `html/cross-origin-embedder-policy/cache-storage-reporting-dedicated-worker.https.html` |
| 4 | ERROR: Test named 'location.href' specified N 'cleanup' function, and N failed. | `html/browsers/browsing-the-web/navigating-across-documents/initial-empty-document/window-open-204-fragment.html` |
| 4 | ERROR: TypeError: cannot read property 'baseVal' of undefined | `svg/types/scripted/SVGLength-convertToSpecifiedUnits-font-change.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: assert_not_inherited is not defined ReferenceError: assert_no... | `svg/geometry/inheritance.svg` |
| 3 | ERROR: Error: assert_equals: expected "done" but got "Python handlers are not implemented" | `html/cross-origin-embedder-policy/credentialless/video.https.window.html` |
| 3 | ERROR: Error: assert_false: expected false got undefined | `fetch/local-network-access/navigate.tentative.window.html?include=from-local` |
| 3 | ERROR: Error: assert_implements: SourceBuffer prototype hasOwnProperty "appendEncodedChunks", used here to feature detect MSE-for-WebCodecs impleme... | `media-source/mse-for-webcodecs/tentative/mediasource-encrypted-webcodecs-appendencodedchunks-play.https.html` |
| 3 | ERROR: Test named 'No replace before load, triggered by window.open() on a non-_self window' specified N 'cleanup' function, and N failed. | `html/browsers/browsing-the-web/navigating-across-documents/replace-before-load/window-open-popup-during-load.html` |
| 3 | ERROR: undefined (createObjectURL) is not a function | `FileAPI/url/url-with-fetch.any.worker.html` |
| 2 | ERROR: Error in remote: transferring objects is not supported | `webmessaging/Channel_postMessage_clone_port.any.worker.html` |
| 2 | ERROR: Error: '?feature=bidi' is missing when importing testdriver.js but the test is using WebDriver BiDi APIs | `html/semantics/permission-element/geolocation-element/get-current-position-error.html` |
| 2 | ERROR: Error: assert_implements: requestStorageAccess is not supported. undefined | `html/anonymous-iframe/hasStorageAccess.tentative.https.window.html` |
| 2 | ERROR: Error: assert_true: expected true got false | `html/rendering/widgets/baseline-alignment-and-overflow.tentative.html` |
| 2 | ERROR: RangeError: script ran too long | `encoding/legacy-mb-japanese/iso-2022-jp/iso2022jp-encode-form.html?7001-last` |
| 2 | ERROR: ReferenceError: SharedWorker is not defined | `workers/constructors/SharedWorker/setting-port-members.html` |
| 2 | ERROR: ReferenceError: VTTCue is not defined | `html/semantics/embedded-content/media-elements/interfaces/TextTrackCue/onenter.html` |
| 2 | ERROR: Test named '"none" top-level: navigating a frame back from "require-corp" should succeed' specified N 'cleanup' function, and N failed. | `html/cross-origin-embedder-policy/iframe-history-none-require-corp.https.html` |
| 2 | ERROR: Test named 'sec-fetch-site - Not sent to non-trustworthy same-origin destination' specified N 'cleanup' function, and N failed. | `fetch/metadata/generated/element-meta-refresh.optional.sub.html` |
| 2 | ERROR: Test named 'sec-fetch-site - Same origin' specified N 'cleanup' function, and N failed. | `fetch/metadata/generated/element-meta-refresh.https.optional.sub.html` |
| 2 | ERROR: Timeout while running cleanup for test named " Cross-Origin-Opener-Policy: noopener-allow-popups means that the opener has no access to the ... | `html/cross-origin-opener-policy/coop-noopener-allow-popups.https.html` |
| 2 | ERROR: Timeout while running cleanup for test named "Same origin openee redirected to same-origin with same-origin-allow-popups". | `html/cross-origin-opener-policy/reporting/document-reporting/reporting-redirect-with-same-origin-allow-popups.https.html` |
| 2 | ERROR: Timeout while running cleanup for test named "same-origin document opening popup redirect from https://N.N.N.N:N to https://N.N.N.N:N with r... | `html/cross-origin-opener-policy/popup-redirect-cache.https.html?6-7` |
| 2 | ERROR: Timeout while running cleanup for test named "same-origin document opening popup redirect from https://localhost:N to https://N.N.N.N:N with... | `html/cross-origin-opener-policy/popup-redirect-cache.https.html?2-3` |
| 2 | ERROR: TypeError: undefined (parseHTMLUnsafe) is not a function | `html/webappapis/dynamic-markup-insertion/html-unsafe-methods/Document-parseHTMLUnsafe-style-attribute.html` |
| 2 | ERROR: TypeError: undefined (showModal) is not a function | `css/css-position/backdrop-inherit-computed.html` |
| 2 | ERROR: expected ';' (line N) | `xhr/abort-upload-event-abort.any.worker.html` |
| 2 | TIMEOUT: killed after the wall-clock budget | `css/selectors/invalidation/has-complexity.html` |
| 2 | TIMEOUT: the page never reported; first script error: ../support/dedicated-worker-helper.js: ReferenceError: assert_worker_is_loaded is not defined R... | `content-security-policy/worker-src/dedicated-worker-src-child-fallback.sub.html` |
| 2 | TIMEOUT: the page never reported; first script error: ../support/service-worker-helper.js: ReferenceError: assert_service_worker_is_loaded is not def... | `content-security-policy/worker-src/service-worker-src-child-fallback.https.sub.html` |
| 2 | TIMEOUT: the page never reported; first script error: ../support/shared-worker-helper.js: ReferenceError: assert_shared_worker_is_loaded is not defin... | `content-security-policy/worker-src/shared-worker-src-child-fallback.sub.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: assert_inherited is not defined ReferenceError: assert_inheri... | `svg/interact/inheritance.svg` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: test_interpolation is not defined ReferenceError: test_interp... | `svg/path/property/d-interpolation-relative-absolute.svg` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'postMessage' of undefined TypeError: cannot ... | `content-security-policy/form-action/form-action-self-allowed-target-blank.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'setAttribute' of undefined TypeError: cannot... | `content-security-policy/nonce-hiding/script-nonces-hidden-meta.sub.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (close) is not a function TypeError: undefined (close) i... | `html/browsers/windows/noreferrer-null-opener.html` |
| 1 | CRASH: killed by signal Aborted | `html/webappapis/structured-clone/structured-clone.any.html` |
| 1 | ERROR: Error in remote: Illegal constructor: ReadableStream | `xhr/send-data-es-object.any.worker.html` |
| 1 | ERROR: Error: assert_implements: Static import must be supported on module dedicated worker to run this test. undefined | `workers/modules/dedicated-worker-parse-error-failure.html` |
| 1 | ERROR: Error: assert_implements: requestStorageAccessFor is not supported. undefined | `html/anonymous-iframe/requestStorageAccessFor.tentative.https.window.html` |
| 1 | ERROR: N duplicate test name: "Calling function () { [source unavailable] } doesn't affect index iteration" | `IndexedDB/idbcursor-iterating-update.any.html` |
| 1 | ERROR: N duplicate test name: "Nonce on SCRIPT tag don't leak via CSS side-channels." | `content-security-policy/nonce-hiding/nonces-css-selector.html` |
| 1 | ERROR: N duplicate test name: "Should throw TypeError for function "function () { [source unavailable] }"." | `webstorage/missing_arguments.window.html` |
| 1 | ERROR: N duplicate test name: "The source of the request from function () { [source unavailable] } is the index itself" | `IndexedDB/idbindex-request-source.any.html` |
| 1 | ERROR: N duplicate test name: "The source of the request from function () { [source unavailable] } is the object store itself" | `IndexedDB/idbobjectstore-request-source.any.html` |
| 1 | ERROR: N duplicate test name: "Unresolvable percentage heights are resolved as Npx in first pass (scrollable overflow)" | `css/css-tables/height-distribution/percentage-sizing-of-table-cell-children.html` |
| 1 | ERROR: N duplicate test name: "sending ND canvas ImageBitmap to http://N.N.N.N:N" | `webmessaging/postMessage_cross_domain_image_transfer_2d.sub.htm` |
| 1 | ERROR: N duplicate test names: " block-start", " block-end" | `css/css-overflow/scroll-overflow-padding-block-001.html` |
| 1 | ERROR: N duplicate test names: "A with tabindex=N should be focusable.", "A with tabindex=-N should be focusable.", "A with tabindex=invalid should... | `html/interaction/focus/tabindex-focus-flag.html` |
| 1 | ERROR: N duplicate test names: "Computed display of form inside DIV should be 'none'", "Computed display of form inside DIV should be 'none' (!impo... | `html/rendering/non-replaced-elements/tables/form-in-tables.html` |
| 1 | ERROR: NetworkError: importScripts could not load ../beta/redirect.py?location=http://localhost:N/html/semantics/scripting-N/the-script-element/mod... | `html/semantics/scripting-1/the-script-element/module/dynamic-import/alpha/base-url-worker-importScripts.html` |
| 1 | ERROR: Origin is not defined | `html/browsers/origin/api/origin-from.any.worker.html` |
| 1 | ERROR: ReferenceError: FileReaderSync is not defined | `FileAPI/FileReaderSync.worker.html` |
| 1 | ERROR: ReferenceError: PathND is not defined | `html/semantics/embedded-content/the-canvas-element/historical.html` |
| 1 | ERROR: ReferenceError: assertSpeculationRulesIsSupported is not defined | `html/browsers/browsing-the-web/history-traversal/pagereveal/order-in-prerender-activation.https.html` |
| 1 | ERROR: Test named '"Flush autofocus candidates" should be happen before a scroll event and animation frame callbacks' specified N 'cleanup' functio... | `html/interaction/focus/the-autofocus-attribute/update-the-rendering.html` |
| 1 | ERROR: Test named './link-upgrade/basic-link-no-upgrade.sub.html' specified N 'cleanup' function, and N failed. | `upgrade-insecure-requests/link-upgrade.sub.https.html` |
| 1 | ERROR: Test named ':open pseudo-class invalidation with dialog.show() + dialog.open = false' specified N 'cleanup' function, and N failed. | `css/selectors/invalidation/open-pseudo-class-in-has.html` |
| 1 | ERROR: Test named 'Adding definition to scoped registry should not upgrade nodes in closed windows' specified N 'cleanup' function, and N failed. | `custom-elements/registries/scoped-registry-define-upgrade-criteria.html` |
| 1 | ERROR: Test named 'Adoption with global registry' specified N 'cleanup' function, and N failed. | `custom-elements/registries/adoption.window.html` |
| 1 | ERROR: Test named 'Blob charset should override any auto-detected charset.' specified N 'cleanup' function, and N failed. | `FileAPI/url/url-charset.window.html` |
| 1 | ERROR: Test named 'BroadcastChannel messages aren't received from a cross-partition iframe' specified N 'cleanup' functions, and N failed. | `webmessaging/broadcastchannel/cross-partition.https.tentative.html` |
| 1 | ERROR: Test named 'COOP+COEP blob URL popup: window.open()' specified N 'cleanup' function, and N failed. | `html/cross-origin-opener-policy/coep-blob-popup.https.html` |
| 1 | ERROR: Test named 'CSP check precedes X-Frame-Options check' specified N 'cleanup' function, and N failed. | `html/browsers/browsing-the-web/navigating-across-documents/failure-check-sequence.https.html` |
| 1 | ERROR: Test named 'CSP: sandbox allow-popups allow-scripts; CSP sandbox popup navigate to Cross-Origin-Opener-Policy document should work' specifie... | `html/cross-origin-opener-policy/coop-csp-sandbox-navigate.https.html` |
| 1 | ERROR: Test named 'CSP: sandbox allow-popups allow-scripts; CSP sandboxed Cross-Origin-Opener-Policy popup should result in a network error' specif... | `html/cross-origin-opener-policy/coop-csp-sandbox.https.html` |
| 1 | ERROR: Test named 'Cloning with global registry' specified N 'cleanup' function, and N failed. | `custom-elements/registries/Document-importNode-cross-document.window.html` |
| 1 | ERROR: Test named 'Content starting with <!-- without space or > is sniffed as HTML' specified N 'cleanup' function, and N failed. | `mimesniff/sniffing/html-comment.tentative.window.html` |
| 1 | ERROR: Test named 'Cross-Origin-Embedder-Policy is inherited by about:blank popup.' specified N 'cleanup' function, and N failed. | `html/cross-origin-embedder-policy/about-blank-popup.https.html` |
| 1 | ERROR: Test named 'Cross-Origin-Opener-Policy only works over secure contexts' specified N 'cleanup' function, and N failed. | `html/cross-origin-opener-policy/no-https.html` |
| 1 | ERROR: Test named 'Custom element with HTMLSubmitButtonBehavior is implicitly focusable' specified N 'cleanup' function, and N failed. | `custom-elements/form-associated/ElementInternals-behavior-accessibility.tentative.html` |
| 1 | ERROR: Test named 'Custom submit button with form method=dialog closes dialog with behavior value' specified N 'cleanup' function, and N failed. | `custom-elements/form-associated/ElementInternals-submit-behavior-dialog.tentative.html` |
| 1 | ERROR: Test named 'Dialog closedby=any parent, popover child' specified N 'cleanup' function, and N failed. | `html/semantics/interactive-elements/the-dialog-element/dialog-popover-closedby-simple.html` |
| 1 | ERROR: Test named 'HTML is not sniffed for a "feed": atom' specified N 'cleanup' function, and N failed. | `mimesniff/sniffing/html.window.html` |

## Why subtests fail

Ranked by *distinct tests* affected rather than by subtests, because that is the
number a fix unblocks. Digits are collapsed to `N`; quoted values are not, because
`expected "block" but got "inline"` and `expected "Npx" but got "Npx"` are
different bugs and a bucket labelled `assert_equals` is not actionable.

| tests | subtests | message | example |
|--:|--:|---|---|
| 2995 | 5838 | Test timed out | `FileAPI/BlobURL/opaque-origin.html` |
| 2619 | 16824 | NOTRUN (no message) | `FileAPI/BlobURL/opaque-origin.html` |
| 1771 | 2014 | OffscreenCanvas is not defined | `html/canvas/element/drawing-images-to-the-canvas/2d.drawImage.detachedcanvas.html` |
| 528 | 1464 | promise_test: Unhandled rejection with value: object "Error: Python handlers are not implemented" | `content-security-policy/gen/top.http-rp/script-src-self/script-tag.http.html` |
| 347 | 1009 | assert_equals: expected N but got N | `IndexedDB/file_support.sub.html` |
| 287 | 315 | promise_test: Unhandled rejection with value: object "ReferenceError: OffscreenCanvas is not defined" | `html/canvas/element/global-hdr-headroom/clli-mdcv-png.html` |
| 256 | 1071 | assert_true: expected true got false | `FileAPI/Blob-methods-from-detached-frame.html` |
| 234 | 6453 | assert_not_equals: property should be set got disallowed value "" | `css/css-align/gaps/gap-parsing-002.html` |
| 202 | 202 | undefined (pauseAnimations) is not a function | `svg/animations/additive-values-width-animation.html` |
| 193 | 28304 | assert_true: 'from' value should be supported expected true got false | `css/CSS2/floats/float-no-interpolation.html` |
| 114 | 507 | assert_throws_dom: function "function () { [source unavailable] }" did not throw | `IndexedDB/idbobjectstore_createIndex.any.html` |
| 107 | 356 | synchronous XMLHttpRequest is not supported | `websockets/cookies/007.html?default` |
| 106 | 305 | promise_test: Unhandled rejection with value: object "ReferenceError: SharedWorker is not defined" | `content-security-policy/inside-worker/sharedworker-connect-src.sub.html` |
| 103 | 595 | promise_test: Unhandled rejection with value: object "[object Object]" | `cors/script-304.html` |
| 101 | 266 | promise_test: Unhandled rejection with value: object "SyntaxError: invalid JSON: expected a number" | `cookies/secure/set-from-http.https.sub.html` |
| 98 | 823 | assert_equals: expected "rgb(N, N, N)" but got "rgb(N, N, N)" | `content-security-policy/style-src/stylenonce-allowed.sub.html` |
| 93 | 507 | promise_test: Unhandled rejection with value: object "TypeError: Failed to fetch" | `FileAPI/url/url-with-fetch.any.html` |
| 88 | 98 | undefined (write) is not a function | `content-security-policy/script-src/script-src-strict_dynamic_parser_inserted_correct_nonce.html` |
| 76 | 497 | assert_equals: expected "Npx" but got "Npx" | `content-security-policy/style-src/style-src-injected-inline-style-allowed-with-content-hash.html` |
| 75 | 208 | assert_throws_js: function "function () { [source unavailable] }" did not throw | `IndexedDB/idbfactory_open.any.html` |
| 72 | 2772 | assert_true: 'to' value should be supported expected true got false | `css/css-align/animation/column-gap-composition.html` |
| 70 | 109 | SharedWorker is not defined | `content-security-policy/sandbox/shared-worker-sandbox.html` |
| 69 | 560 | assert_unreached: Should have rejected: undefined Reached unreachable code | `fetch/api/basic/header-value-null-byte.any.html` |
| 67 | 85 | promise_test: Unhandled rejection with value: object "ReferenceError: createImageBitmap is not defined" | `html/canvas/element/compositing/2d.composite.canvas.destination-atop.html` |
| 65 | 207 | assert_false: expected false got true | `cors/preflight-failure.htm` |
| 65 | 94 | promise_test: Unhandled rejection with value: object "TypeError: undefined (requestPaint) is not a function" | `html/canvas/element/manual/draw-element-image/backdrop-filter-bounds-expansion.tentative.html` |
| 64 | 92 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'ready' of undefined" | `css/css-align/baseline-of-single-axis-scroll-container.html` |
| 63 | 170 | cannot read property 'length' of undefined | `IndexedDB/keypath-special-identifiers.any.html` |
| 63 | 70 | cannot read property 'postMessage' of null | `FileAPI/url/cross-global-revoke.sub.html` |
| 59 | 365 | assert_equals: expected (object) null but got (undefined) undefined | `css/css-cascade/scope-cssom.html` |
| 57 | 215 | assert_unreached: Reached unreachable code | `content-security-policy/reporting/report-only-in-meta.sub.html` |
| 57 | 62 | cannot set property 'mode' of undefined | `html/infrastructure/urls/resolving-urls/query-encoding/utf-8.html?include=loading` |
| 56 | 290 | undefined (__defineSetter__) is not a function | `IndexedDB/abort-in-initial-upgradeneeded.any.html` |
| 53 | 133 | assert_equals: expected "Npx" but got "" | `css/css-align/gaps/column-gap-animation-001.html` |
| 50 | 568 | Illegal constructor: ReadableStream | `fetch/api/basic/request-upload.any.html` |
| 49 | 335 | promise_test: Unhandled rejection with value: object "TypeError: Illegal constructor: ReadableStream" | `encoding/streams/decode-ignore-bom.any.html` |
| 47 | 101 | assert_true: expected true got undefined | `content-security-policy/script-src/script-src-event-handler-on-inline-script.html` |
| 46 | 1533 | assert_equals: expected "Npx " but got "Npx " | `css/CSS2/linebox/animations/line-height-interpolation.html` |
| 46 | 314 | promise_test: Unhandled rejection with value: object "Error: Network Error" | `referrer-policy/4K/gen/top.http-rp/no-referrer-when-downgrade/xhr.http.html` |
| 46 | 187 | promise_test: Unhandled rejection with value: object "TypeError: undefined (getAnimations) is not a function" | `css/css-animations/CSSAnimation-canceling.tentative.html` |
| 46 | 248 | undefined (getAnimations) is not a function | `css/css-animations/CSSAnimation-animationName.tentative.html` |
| 45 | 243 | assert_equals: Expected success event, but got upgradeneeded event instead expected "success" but got "upgradeneeded" | `IndexedDB/bindings-inject-keys-bypass.any.html` |
| 44 | 143 | cannot read property 'document' of null | `content-security-policy/inheritance/blob-url-inherits-from-initiator.sub.html` |
| 43 | 123 | undefined (open) is not a function | `css/css-transitions/dynamic-root-element.html` |
| 40 | 41 | promise_test: Unhandled rejection with value: object "Error: unsupported action source: wheel" | `css/css-overflow/scroll-markers/scroll-marker-selection-in-padded-scroller.html` |
| 40 | 518 | promise_test: Unhandled rejection with value: object "ReferenceError: *import* is not defined" | `content-security-policy/connect-src/connect-src-json-import-allowed.sub.html` |
| 39 | 312 | assert_true: Browser does not support WebSocket expected true got false | `websockets/Create-asciiSep-protocol-string.any.worker.html?default` |
| 38 | 38 | assert_equals: Red channel of the pixel at (N, N) expected N but got N | `html/canvas/element/drawing-rectangles-to-the-canvas/2d.strokeRect.zero.2.html` |
| 38 | 82 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'currentTime' of undefined" | `css/css-animations/display-contents-animates.html` |
| 37 | 300 | assert_equals: expected (string) "" but got (undefined) undefined | `content-security-policy/nonce-hiding/nonces.html` |
| 37 | 37 | undefined (setCurrentTime) is not a function | `svg/animations/conditional-processing-02.html` |
| 34 | 34 | assert_equals: expected "done" but got "Python handlers are not implemented" | `html/anonymous-iframe/cookie.tentative.https.window.html` |
| 33 | 133 | assert_equals: expected "Popup loaded" but got "Python handlers are not implemented" | `html/cross-origin-opener-policy/coep-with-cross-origin.https.html` |
| 33 | 211 | assert_equals: expected (number) N but got (undefined) undefined | `css/cssom-view/cssom-view-img-attributes-001.html` |
| 32 | 198 | promise_test: Unhandled rejection with value: object "ReferenceError: Observable is not defined" | `dom/observable/tentative/crashtests/observable-gc.any.html` |
| 31 | 32 | EventSource is not defined | `eventsource/event-data.any.worker.html` |
| 31 | 55 | assert_equals: entries.length expected N but got N | `intersection-observer/clip-path.html` |
| 31 | 329 | cannot read property 'baseVal' of undefined | `svg/animations/attribute-value-unaffected-by-animation-002.html` |
| 30 | 104 | assert_array_equals: lengths differ, expected array ["constructed"] length N, got [] length N | `custom-elements/reactions/Node.html` |
| 30 | 37 | assert_equals: expected (undefined) undefined but got (number) N | `dom/collections/HTMLCollection-supported-property-indices.html` |
| 30 | 57 | promise_test: Unhandled rejection with value: object "ReferenceError: service_worker_unregister_and_register is not defined" | `content-security-policy/sandbox/service-worker-sandbox.https.html` |
| 30 | 30 | undefined (toDataURL) is not a function | `html/canvas/element/layers/2d.layer.malformed-operations.html` |
| 28 | 70 | undefined (getElementsByName) is not a function | `dom/nodes/moveBefore/moveBefore-name-map.html` |
| 27 | 32 | promise_test: Unhandled rejection with value: object "TypeError: undefined (showModal) is not a function" | `css/css-animations/dialog-backdrop-animation.html` |
| 24 | 24 | assert_equals: <div class="container"> <div class="item rtl" style="justify-self: center;" data-expected-height="N" data-offset-y="N"></div>... | `css/css-align/abspos/justify-self-vlr-ltr-htb.html` |
| 24 | 24 | assert_equals: <div class="container"> <div class="item rtl" style="justify-self: right;" data-expected-height="N" data-offset-y="N"></div> ... | `css/css-align/abspos/justify-self-vlr-ltr-htb.html` |
| 24 | 24 | assert_equals: <div class="container"> <div class="item" style="justify-self: center;" data-expected-height="N" data-offset-y="N"></div> </d... | `css/css-align/abspos/justify-self-vlr-ltr-htb.html` |
| 24 | 24 | assert_equals: <div class="container"> <div class="item" style="justify-self: right;" data-expected-height="N" data-offset-y="N"></div> </di... | `css/css-align/abspos/justify-self-vlr-ltr-htb.html` |
| 24 | 24 | assert_equals: <div class="container"> <div class="item" style="justify-self: self-end;" data-expected-height="N" data-offset-y="N"></div> <... | `css/css-align/abspos/justify-self-vlr-ltr-htb.html` |
| 24 | 176 | assert_equals: <div data-offset-x="N"><span></span></div> offsetLeft expected N but got N | `css/css-flexbox/alignment/flex-align-baseline-fieldset-002.html` |
| 24 | 45 | undefined (showModal) is not a function | `css/css-position/sticky-dialog.html` |
| 23 | 397 | KeyframeEffect is not defined | `css/css-animations/AnimationEffect-getComputedTiming.tentative.html` |
| 23 | 79 | assert_equals: expected "N" but got "N" | `css/css-flexbox/getcomputedstyle/flexbox_computedstyle_order-inherit.html` |
| 23 | 71 | assert_equals: expected "none" but got "" | `css/css-animations/parsing/animation-computed.html` |
| 23 | 157 | assert_equals: expected true but got false | `css/css-conditional/js/CSS-supports-L3.html` |
| 22 | 110 | Illegal constructor: CustomElementRegistry | `custom-elements/registries/Construct.html` |
| 22 | 266 | Observable is not defined | `dom/observable/tentative/observable-catch.any.html` |
| 22 | 88 | assert_equals: expected "rgb(N, N, N)" but got "rgba(N, N, N, N)" | `css/css-backgrounds/inheritance.sub.html` |
| 22 | 136 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'location' of null" | `fetch/metadata/generated/window-location.sub.html` |
| 21 | 30 | assert_greater_than: expected a number greater than N but got N | `css/css-tables/tentative/table-fixed-minmax.html` |
| 20 | 171 | WritableStream is not defined | `streams/piping/general.any.html` |
| 20 | 20 | assert_equals: <div class="container"> <div class="item rtl" style="justify-self: self-start;" data-expected-height="N" data-offset-y="N"></... | `css/css-align/abspos/justify-self-vlr-ltr-htb.html` |
| 20 | 38 | assert_equals: expected "N" but got "" | `css/css-animations/animation-iteration-count-calc.html` |
| 20 | 35 | assert_equals: expected "Ready" but got "Python handlers are not implemented" | `html/cross-origin-opener-policy/reporting/access-reporting/property-blur.https.html` |
| 20 | 145 | assert_equals: expected false but got true | `css/css-conditional/js/CSS-supports-L4.html` |
| 20 | 20 | promise_test: Unhandled rejection with value: object "Error: Condition did not become true after N frames" | `dom/events/non-cancelable-when-passive/non-passive-touchmove-event-listener-on-body.html` |
| 20 | 136 | promise_test: Unhandled rejection with value: object "ReferenceError: WebSocketStream is not defined" | `websockets/stream/tentative/abort.any.html?wpt_flags=h2` |
| 20 | 75 | promise_test: Unhandled rejection with value: object "ReferenceError: with_iframe is not defined" | `fetch/content-encoding/br/br-navigation.https.window.html` |
| 19 | 73 | XPathResult is not defined | `dom/xpath-result-single-node-value-nullable.html` |
| 19 | 27 | assert_false: expected false got undefined | `css/css-conditional/at-supports-matches.html` |
| 19 | 52 | cannot read property 'N' of undefined | `css/css-animations/keyframes-rule-caching.html` |
| 19 | 542 | promise_test: Unhandled rejection with value: object "ReferenceError: indexedDB is not defined" | `IndexedDB/get-databases.any.worker.html` |
| 19 | 26 | undefined (addEventListener) is not a function | `eventsource/eventsource-eventtarget.any.html` |
| 19 | 44 | undefined (addTextTrack) is not a function | `html/semantics/embedded-content/media-elements/interfaces/HTMLElement/HTMLMediaElement/addTextTrack.html` |
| 18 | 18 | assert_equals: <div class="container"> <div class="item rtl" style="justify-self: end;" data-expected-height="N" data-offset-y="N"></div> </... | `css/css-align/abspos/justify-self-vlr-ltr-htb.html` |
| 18 | 18 | assert_equals: <div class="container"> <div class="item rtl" style="justify-self: start;" data-expected-height="N" data-offset-y="N"></div> ... | `css/css-align/abspos/justify-self-vlr-ltr-htb.html` |
| 18 | 18 | assert_equals: <div class="container"> <div class="item" style="justify-self: end;" data-expected-height="N" data-offset-y="N"></div> </div>... | `css/css-align/abspos/justify-self-vlr-ltr-htb.html` |
| 18 | 18 | assert_equals: <div class="container"> <div class="item" style="justify-self: start;" data-expected-height="N" data-offset-y="N"></div> </di... | `css/css-align/abspos/justify-self-vlr-ltr-htb.html` |
| 18 | 23 | assert_equals: expected "" but got "auto" | `css/css-align/parsing/gap-invalid.html` |
| 18 | 80 | assert_equals: expected "Iframe loaded" but got "Python handlers are not implemented" | `html/cross-origin-opener-policy/iframe-popup-same-origin-allow-popups-to-same-origin-allow-popups.https.html?9-last` |

23365 distinct subtest messages and 188 distinct harness messages behind these numbers.
