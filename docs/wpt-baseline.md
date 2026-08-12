# The WPT baseline

**Generated**, by `microbrowser_wpt --summary docs/wpt-baseline.md`. Do not edit it: the
next run overwrites it, and that overwrite is the point -- the diff of this file is
what a session moved. The argument for the instrument is `docs/adr/0040`; the work it
sequences is `docs/wpt-plan.md`.

WPT revision: `4120ac0deb573634d8b7cd74c38ae9d647eebdb5`

**Partly re-measured, 2026-08-12 (C10, reflected IDL attributes).** The one row for `html/dom`
is from that run and was merged in **by hand**, for the reason three paragraphs below: 35.8% ->
**95.5%** (21,450 of 59,930 -> 57,411 of 60,139 subtests), 0 unexpected results, 35,847 expectation
lines deleted. `html/dom/reflection-*.html` alone went from 35,560 recorded failures to none --
56,660 of 56,660. **Every other `html/` row is still the M-B baseline and is now wrong in the
optimistic direction**: reflected attributes are read everywhere, so `html/semantics` in particular
should be re-measured before anyone plans against its number. `html/browsers/` was deliberately not
run -- 751 tests of navigation this browser cannot do, almost all of them 20-second timeouts.

**Partly re-measured again, 2026-08-11 (C4, both halves).** The 26 rows for `dom/`,
`custom-elements/`, `shadow-dom/` and `domparsing/` are from that run, merged in by hand for
the reason the next paragraph gives. **Read the counts before the percentages.** The runner's
per-test timeout used to be the same number as testharness.js's own, so a page that timed out was
killed at the instant it began reporting and its subtests were never counted at all; with five
seconds of grace they are. Every denominator in those rows grew -- `dom/nodes` 5177 -> 5451,
`shadow-dom/declarative` 7647 -> 7785, `domparsing` 245 -> 426 -- so a *rate* can fall while
nothing regressed. `domparsing` reads 31.4% -> 18.1% with the same 77 subtests passing.

**Partly re-measured, 2026-08-10 (C1 + C2, then C3).** The per-area rows for `dom/`, `FileAPI/`,
`xhr/`, `domparsing/`, `custom-elements/`, `shadow-dom/` and `IndexedDB/` are from the C1+C2 run;
every `dom/` row was re-measured again by the C3 run (argument conversion, `DOMTokenList`,
`CharacterData`, name validation) -- `dom/nodes` 23.4% -> **49.4%**, `dom/lists` 61.2% -> 67.3%,
and the whole area 23.5% -> **44.2%**. Subtest *counts* moved too (`dom/nodes` 5177 -> 5221,
`dom/events` 540 -> 549): a test whose harness used to die partway now runs to the end, so the
denominator grows with the numerator. Every other row, and the whole ranked-cause section below,
is still the M-B baseline.

Both merges were done **by hand**, and that is a defect in the tooling rather than a choice:
`--summary` rewrites this document from `--summary-state` alone, so a run whose state file does
not already describe every area silently produces a document about only the areas it ran. The
state file at `/tmp/microbrowser-wpt-state.tsv` is a *temporary* file holding this document's
memory, which is the wrong place for it -- it does not survive a reboot, and the failure mode
when it does not is a generated document that looks complete and is not. **The state file
belongs in the repository beside this one.** Until it is there: keep it, or re-measure
everything, and check the row count before committing a regenerated summary.

55202 of 479620 subtests pass (11.5%) over 21265 tests.

**Do not quote that number.** Subtests are not comparable across areas: `encoding/legacy-mb-japanese` alone is 24% of every subtest here.
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
| `FileAPI/FileReader` | 2 | 1 | 0 | 1 | 0 | 1 | 0 | 0.0 |
| `FileAPI/blob` | 23 | 13 | 0 | 10 | 0 | 259 | 1 | 0.4 |
| `FileAPI/file` | 21 | 14 | 0 | 7 | 0 | 163 | 0 | 0.0 |
| `FileAPI/filelist-section` | 1 | 1 | 0 | 0 | 0 | 7 | 0 | 0.0 |
| `FileAPI/reading-data-section` | 26 | 13 | 0 | 13 | 0 | 48 | 0 | 0.0 |
| `FileAPI/url` | 15 | 3 | 0 | 12 | 0 | 39 | 21 | 53.8 |
| `IndexedDB` | 265 | 208 | 3 | 53 | 1 | 1032 | 49 | 4.7 |
| `IndexedDB/crashtests` | 1 | 1 | 0 | 0 | 0 | 1 | 1 | 100.0 |
| `console` | 19 | 12 | 0 | 7 | 0 | 29 | 6 | 20.7 |
| `content-security-policy` | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0.0 |
| `content-security-policy/base-uri` | 6 | 2 | 0 | 4 | 0 | 2 | 0 | 0.0 |
| `content-security-policy/blob` | 6 | 6 | 0 | 0 | 0 | 6 | 0 | 0.0 |
| `content-security-policy/child-src` | 9 | 5 | 0 | 4 | 0 | 5 | 2 | 40.0 |
| `content-security-policy/connect-src` | 30 | 24 | 0 | 6 | 0 | 32 | 0 | 0.0 |
| `content-security-policy/default-src` | 4 | 3 | 0 | 1 | 0 | 14 | 6 | 42.9 |
| `content-security-policy/embedded-enforcement` | 22 | 18 | 0 | 4 | 0 | 281 | 0 | 0.0 |
| `content-security-policy/font-src` | 5 | 0 | 0 | 5 | 0 | 0 | 0 | 0.0 |
| `content-security-policy/form-action` | 13 | 7 | 0 | 6 | 0 | 7 | 0 | 0.0 |
| `content-security-policy/frame-ancestors` | 34 | 10 | 0 | 24 | 0 | 20 | 6 | 30.0 |
| `content-security-policy/frame-src` | 12 | 5 | 0 | 7 | 0 | 8 | 0 | 0.0 |
| `content-security-policy/gen` | 260 | 0 | 0 | 260 | 0 | 2480 | 0 | 0.0 |
| `content-security-policy/generic` | 28 | 12 | 0 | 16 | 0 | 39 | 11 | 28.2 |
| `content-security-policy/img-src` | 15 | 2 | 0 | 13 | 0 | 7 | 1 | 14.3 |
| `content-security-policy/inheritance` | 24 | 10 | 0 | 14 | 0 | 41 | 1 | 2.4 |
| `content-security-policy/inside-worker` | 10 | 5 | 0 | 5 | 0 | 9 | 0 | 0.0 |
| `content-security-policy/media-src` | 10 | 1 | 0 | 9 | 0 | 2 | 0 | 0.0 |
| `content-security-policy/meta` | 5 | 4 | 0 | 1 | 0 | 5 | 1 | 20.0 |
| `content-security-policy/navigation` | 6 | 0 | 0 | 6 | 0 | 0 | 0 | 0.0 |
| `content-security-policy/nonce-hiding` | 8 | 3 | 1 | 4 | 0 | 55 | 8 | 14.5 |
| `content-security-policy/object-src` | 13 | 0 | 0 | 13 | 0 | 0 | 0 | 0.0 |
| `content-security-policy/parsing` | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| `content-security-policy/plugin-types` | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| `content-security-policy/report-hash` | 9 | 0 | 0 | 9 | 0 | 0 | 0 | 0.0 |
| `content-security-policy/reporting` | 31 | 8 | 0 | 23 | 0 | 17 | 3 | 17.6 |
| `content-security-policy/reporting-api` | 11 | 1 | 0 | 10 | 0 | 5 | 0 | 0.0 |
| `content-security-policy/resource-hints` | 9 | 1 | 5 | 3 | 0 | 26 | 0 | 0.0 |
| `content-security-policy/sandbox` | 17 | 14 | 0 | 3 | 0 | 21 | 0 | 0.0 |
| `content-security-policy/script-src` | 90 | 47 | 0 | 43 | 0 | 105 | 45 | 42.9 |
| `content-security-policy/script-src-attr-elem` | 8 | 3 | 0 | 5 | 0 | 3 | 2 | 66.7 |
| `content-security-policy/securitypolicyviolation` | 24 | 2 | 0 | 22 | 0 | 28 | 2 | 7.1 |
| `content-security-policy/style-src` | 42 | 22 | 0 | 20 | 0 | 30 | 14 | 46.7 |
| `content-security-policy/style-src-attr-elem` | 7 | 3 | 0 | 4 | 0 | 3 | 0 | 0.0 |
| `content-security-policy/svg` | 5 | 3 | 0 | 2 | 0 | 3 | 0 | 0.0 |
| `content-security-policy/unsafe-eval` | 15 | 14 | 0 | 1 | 0 | 14 | 1 | 7.1 |
| `content-security-policy/unsafe-hashes` | 24 | 0 | 0 | 24 | 0 | 5 | 0 | 0.0 |
| `content-security-policy/wasm-unsafe-eval` | 10 | 8 | 0 | 2 | 0 | 31 | 0 | 0.0 |
| `content-security-policy/webrtc` | 5 | 5 | 0 | 0 | 0 | 5 | 0 | 0.0 |
| `content-security-policy/worker-src` | 31 | 17 | 0 | 14 | 0 | 25 | 0 | 0.0 |
| `content-security-policy/xslt` | 3 | 3 | 0 | 0 | 0 | 3 | 0 | 0.0 |
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
| `css/selectors` | 277 | 210 | 10 | 57 | 0 | 1353 | 334 | 24.7 |
| `custom-elements` | 44 | 25 | 0 | 19 | 0 | 983 | 196 | 19.9 |
| `custom-elements/form-associated` | 18 | 14 | 3 | 1 | 0 | 103 | 1 | 1.0 |
| `custom-elements/htmlconstructor` | 2 | 0 | 0 | 2 | 0 | 20 | 0 | 0.0 |
| `custom-elements/parser` | 11 | 8 | 0 | 3 | 0 | 20 | 7 | 35.0 |
| `custom-elements/reactions` | 57 | 45 | 0 | 12 | 0 | 514 | 80 | 15.6 |
| `custom-elements/registries` | 40 | 28 | 2 | 10 | 0 | 2233 | 225 | 10.1 |
| `custom-elements/state` | 5 | 4 | 1 | 0 | 0 | 28 | 1 | 3.6 |
| `custom-elements/upgrading` | 7 | 2 | 0 | 5 | 0 | 29 | 1 | 3.4 |
| `dom` | 10 | 10 | 0 | 0 | 0 | 125 | 66 | 52.8 |
| `dom/abort` | 10 | 6 | 0 | 4 | 0 | 37 | 10 | 27.0 |
| `dom/collections` | 10 | 10 | 0 | 0 | 0 | 53 | 7 | 13.2 |
| `dom/events` | 178 | 86 | 0 | 92 | 0 | 677 | 183 | 27.0 |
| `dom/lists` | 5 | 5 | 0 | 0 | 0 | 189 | 144 | 76.2 |
| `dom/nodes` | 327 | 235 | 6 | 85 | 1 | 5451 | 3389 | 62.2 |
| `dom/observable` | 52 | 25 | 0 | 27 | 0 | 251 | 0 | 0.0 |
| `dom/ranges` | 57 | 33 | 24 | 0 | 0 | 259 | 21 | 8.1 |
| `dom/traversal` | 18 | 14 | 3 | 1 | 0 | 56 | 30 | 53.6 |
| `domparsing` | 34 | 21 | 3 | 10 | 0 | 426 | 77 | 18.1 |
| `domparsing/tentative` | 26 | 17 | 0 | 9 | 0 | 905 | 28 | 3.1 |
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
| `html/anonymous-iframe` | 33 | 25 | 3 | 5 | 0 | 39 | 0 | 0.0 |
| `html/browsers` | 747 | 364 | 15 | 368 | 0 | 1469 | 142 | 9.7 |
| `html/canvas` | 3326 | 2245 | 4 | 1077 | 0 | 3015 | 369 | 12.2 |
| `html/capability-delegation` | 6 | 0 | 2 | 4 | 0 | 6 | 0 | 0.0 |
| `html/cross-origin-embedder-policy` | 88 | 45 | 5 | 38 | 0 | 345 | 15 | 4.3 |
| `html/cross-origin-opener-policy` | 114 | 90 | 2 | 22 | 0 | 583 | 2 | 0.3 |
| `html/document-isolation-policy` | 38 | 35 | 1 | 2 | 0 | 150 | 0 | 0.0 |
| `html/dom` | 264 | 240 | 0 | 24 | 0 | 60139 | 57411 | 95.5 |
| `html/editing` | 115 | 58 | 0 | 57 | 0 | 614 | 39 | 6.4 |
| `html/embedded-content` | 1 | 1 | 0 | 0 | 0 | 2 | 0 | 0.0 |
| `html/infrastructure` | 65 | 35 | 0 | 29 | 1 | 463 | 45 | 9.7 |
| `html/interaction` | 192 | 182 | 2 | 8 | 0 | 712 | 48 | 6.7 |
| `html/links` | 6 | 4 | 0 | 2 | 0 | 4 | 3 | 75.0 |
| `html/meta` | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0.0 |
| `html/obsolete` | 14 | 14 | 0 | 0 | 0 | 53 | 7 | 13.2 |
| `html/rendering` | 153 | 131 | 2 | 20 | 0 | 7169 | 6153 | 85.8 |
| `html/scripting` | 2 | 2 | 0 | 0 | 0 | 3 | 1 | 33.3 |
| `html/select` | 1 | 1 | 0 | 0 | 0 | 5 | 0 | 0.0 |
| `html/semantics` | 2196 | 1493 | 63 | 636 | 4 | 14678 | 2262 | 15.4 |
| `html/syntax` | 211 | 70 | 0 | 141 | 0 | 3045 | 497 | 16.3 |
| `html/the-xhtml-syntax` | 13 | 0 | 0 | 13 | 0 | 0 | 0 | 0.0 |
| `html/user-activation` | 19 | 3 | 0 | 16 | 0 | 6 | 0 | 0.0 |
| `html/webappapis` | 345 | 204 | 10 | 131 | 0 | 1206 | 441 | 36.6 |
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
| `referrer-policy/4K` | 108 | 44 | 0 | 64 | 0 | 386 | 0 | 0.0 |
| `referrer-policy/4K+1` | 108 | 43 | 0 | 65 | 0 | 376 | 0 | 0.0 |
| `referrer-policy/4K-1` | 108 | 43 | 0 | 65 | 0 | 382 | 0 | 0.0 |
| `referrer-policy/css-integration` | 24 | 15 | 0 | 9 | 0 | 56 | 0 | 0.0 |
| `referrer-policy/gen` | 1001 | 86 | 0 | 915 | 0 | 6544 | 0 | 0.0 |
| `referrer-policy/generic` | 42 | 17 | 0 | 25 | 0 | 78 | 3 | 3.8 |
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
| `shadow-dom` | 66 | 58 | 0 | 8 | 0 | 739 | 84 | 11.4 |
| `shadow-dom/declarative` | 56 | 40 | 4 | 12 | 0 | 7785 | 112 | 1.4 |
| `shadow-dom/focus` | 37 | 31 | 1 | 5 | 0 | 78 | 5 | 6.4 |
| `shadow-dom/focus-navigation` | 45 | 45 | 0 | 0 | 0 | 89 | 2 | 2.2 |
| `shadow-dom/leaktests` | 4 | 3 | 0 | 1 | 0 | 16 | 7 | 43.8 |
| `shadow-dom/reference-target` | 15 | 14 | 0 | 1 | 0 | 768 | 0 | 0.0 |
| `shadow-dom/untriaged` | 54 | 52 | 0 | 2 | 0 | 251 | 180 | 71.7 |
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
| `svg/animations` | 289 | 251 | 1 | 37 | 0 | 280 | 4 | 1.4 |
| `svg/coordinate-systems` | 8 | 7 | 0 | 1 | 0 | 39 | 0 | 0.0 |
| `svg/embedded` | 2 | 1 | 0 | 1 | 0 | 2 | 0 | 0.0 |
| `svg/extensibility` | 7 | 5 | 0 | 2 | 0 | 5 | 3 | 60.0 |
| `svg/fonts` | 3 | 3 | 0 | 0 | 0 | 3 | 0 | 0.0 |
| `svg/geometry` | 61 | 12 | 0 | 49 | 0 | 283 | 27 | 9.5 |
| `svg/import` | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| `svg/interact` | 33 | 14 | 0 | 19 | 0 | 96 | 46 | 47.9 |
| `svg/linking` | 18 | 5 | 0 | 13 | 0 | 31 | 0 | 0.0 |
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
| `upgrade-insecure-requests` | 1 | 1 | 0 | 0 | 0 | 8 | 0 | 0.0 |
| `upgrade-insecure-requests/gen` | 196 | 196 | 0 | 0 | 0 | 992 | 0 | 0.0 |
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
| `websockets` | 380 | 50 | 0 | 330 | 0 | 263 | 167 | 63.5 |
| `websockets/binary` | 4 | 0 | 0 | 4 | 0 | 3 | 0 | 0.0 |
| `websockets/closing-handshake` | 3 | 0 | 0 | 3 | 0 | 0 | 0 | 0.0 |
| `websockets/constructor` | 19 | 8 | 0 | 11 | 0 | 175 | 6 | 3.4 |
| `websockets/cookies` | 9 | 2 | 0 | 7 | 0 | 3 | 0 | 0.0 |
| `websockets/interfaces` | 79 | 54 | 0 | 25 | 0 | 118 | 22 | 18.6 |
| `websockets/keeping-connection-open` | 1 | 0 | 0 | 1 | 0 | 1 | 0 | 0.0 |
| `websockets/multi-globals` | 2 | 2 | 0 | 0 | 0 | 3 | 0 | 0.0 |
| `websockets/opening-handshake` | 7 | 0 | 0 | 7 | 0 | 4 | 0 | 0.0 |
| `websockets/security` | 2 | 1 | 0 | 1 | 0 | 1 | 0 | 0.0 |
| `websockets/stream` | 21 | 21 | 0 | 0 | 0 | 159 | 0 | 0.0 |
| `websockets/unload-a-document` | 5 | 3 | 0 | 2 | 0 | 4 | 0 | 0.0 |
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
| `xhr` | 374 | 250 | 0 | 124 | 0 | 1049 | 69 | 6.6 |
| `xhr/formdata` | 27 | 16 | 0 | 11 | 0 | 71 | 0 | 0.0 |

## Why the harness never reported

Ranked by tests affected. One line here is worth more than a page of the table
above: a test whose harness failed reports *no* subtests, so these are invisible in
the pass rate and are the largest block of unrealised coverage in the suite.

| tests | cause | example |
|--:|---|---|
| 4253 | TIMEOUT: the page never reported | `FileAPI/idlharness.worker.html` |
| 2473 | TIMEOUT:  | `FileAPI/FileReaderSync.worker.html` |
| 222 | TIMEOUT: the page never reported; first script error: inline script #N: SyntaxError: unexpected token '<' (line N) SyntaxError: unexpected token '<' ... | `custom-elements/Document-createElement-svg.svg` |
| 184 | ERROR: [object Object] | `css/css-conditional/container-queries/animation-container-size.html` |
| 168 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'ready' of undefined TypeError: cannot read p... | `css/CSS2/linebox/vertical-align-top-bottom-001.html` |
| 63 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (write) is not a function TypeError: undefined (write) i... | `custom-elements/parser/parser-constructs-custom-element-in-document-write.html` |
| 45 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot resolve module '../resources/helpers.mjs' TypeError: cannot... | `html/browsers/origin/origin-keyed-agent-clusters/1-iframe/parent-yes-child-no-subdomain.sub.https.html` |
| 32 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (open) is not a function TypeError: undefined (open) is ... | `content-security-policy/sandbox/window-reuse-unsandboxed.html` |
| 30 | ERROR: ReferenceError: getSelection is not defined | `selection/addRange-08.html` |
| 30 | ERROR: TypeError: Illegal constructor: Document | `dom/nodes/Node-compareDocumentPosition.html` |
| 27 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot resolve module './support/positioned-grid-descendants.js' T... | `css/css-grid/abspos/orthogonal-positioned-grid-descendants-001.html` |
| 24 | TIMEOUT: the page never reported; first script error: inline script #N: SyntaxError: expected ';' SyntaxError: expected ';' | `html/semantics/document-metadata/the-style-element/tentative/style-element-csp-script-src-allowed.html` |
| 22 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot resolve module './resources/helpers.mjs' TypeError: cannot ... | `html/browsers/browsing-the-web/overlapping-navigations-and-traversals/cross-document-nav-stop.html` |
| 21 | ERROR: TypeError: cannot read property 'N' of undefined | `css/css-conditional/container-queries/at-container-style-parsing.html` |
| 19 | TIMEOUT: killed after the wall-clock budget | `dom/nodes/Node-insertBefore.html` |
| 16 | ERROR: TypeError: undefined (open) is not a function | `html/browsers/the-window-object/open-close/open-features-negative-innerwidth-innerheight.html` |
| 15 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: async_test is not defined ReferenceError: async_test is not d... | `content-security-policy/base-uri/report-uri-does-not-respect-base-uri.sub.html` |
| 15 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: test is not defined ReferenceError: test is not defined at <a... | `css/css-values/urls/resolve-relative-to-base.sub.html` |
| 14 | ERROR: TypeError: undefined (addTextTrack) is not a function | `html/semantics/embedded-content/media-elements/interfaces/TextTrack/activeCues.html` |
| 13 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: getSelection is not defined ReferenceError: getSelection is n... | `selection/getRangeAt.html` |
| 12 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: SharedWorker is not defined ReferenceError: SharedWorker is n... | `content-security-policy/inside-worker/sharedworker-report-only.sub.html` |
| 11 | TIMEOUT: the page never reported; first script error: /websockets/Close-N-reason.any.js: TypeError: undefined (addEventListener) is not a function Ty... | `websockets/Close-1000-reason.any.html?wss` |
| 10 | TIMEOUT: the page never reported; first script error: ../support/checkReport.sub.js?reportExists=false: TypeError: cannot read property 'trim' of und... | `content-security-policy/object-src/object-src-no-url-allowed.html` |
| 9 | ERROR: RangeError: script ran too long | `encoding/legacy-mb-japanese/iso-2022-jp/iso2022jp-encode-form-csiso2022jp.html` |
| 9 | TIMEOUT: the page never reported; first script error: /websockets/Close-N-verify-code.any.js: TypeError: undefined (addEventListener) is not a functi... | `websockets/Close-1000-verify-code.any.html?wss` |
| 9 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: host is not defined ReferenceError: host is not defined at <a... | `css/cssom/selectorText-modification-restyle-002.html` |
| 9 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: target is not defined ReferenceError: target is not defined a... | `css/css-cascade/layer-vs-inline-style.html` |
| 9 | TIMEOUT: the page never reported; first script error: inline script #N: SyntaxError: expected ')' to close a dynamic import (line N) SyntaxError: exp... | `content-security-policy/connect-src/connect-src-json-import-allowed.sub.html` |
| 9 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot set property 'onerror' of undefined TypeError: cannot set p... | `custom-elements/cross-realm-callback-report-exception.html` |
| 8 | TIMEOUT: the page never reported; first script error: /service-workers/service-worker/resources/test-helpers.sub.js: SyntaxError: expected ';' (line ... | `fetch/api/policies/referrer-no-referrer-service-worker.https.html` |
| 8 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: container is not defined ReferenceError: container is not def... | `css/CSS2/positioning/relpos-percentage-left-in-scrollable.html` |
| 8 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'cssRules' of undefined TypeError: cannot rea... | `css/css-cascade/all-prop-revert-layer.html` |
| 7 | ERROR: TypeError: cannot read property 'supports' of undefined | `content-security-policy/resource-hints/prefetch-allowed-by-any-directive.sub.html` |
| 7 | TIMEOUT: the page never reported; first script error: ./support/helpers.js: SyntaxError: expected ')' to close a dynamic import (line N) SyntaxError:... | `shadow-dom/declarative/tentative/shadowrootadoptedstylesheets/shadowrootadoptedstylesheets-async-fetch-disconnect-iframe.html` |
| 7 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: initial is not defined ReferenceError: initial is not defined... | `css/selectors/focus-visible-script-focus-006.tentative.html` |
| 7 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: promise_test is not defined ReferenceError: promise_test is n... | `content-security-policy/navigation/to-javascript-url-script-src.html` |
| 7 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'load' of undefined TypeError: cannot read pr... | `css/css-shapes/shape-outside/values/shape-outside-ellipse-005.html` |
| 7 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot resolve module './support/getComputedStyle-insets.js' TypeE... | `css/cssom/getComputedStyle-insets-relative.html` |
| 7 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (createDocument) is not a function TypeError: undefined ... | `dom/nodes/append-on-Document.html` |
| 6 | CRASH: killed by signal Segmentation fault | `dom/nodes/moveBefore/relevant-mutations.html` |
| 6 | TIMEOUT: the page never reported; first script error: /reporting/resources/report-helper.js: SyntaxError: expected ';' (line N) SyntaxError: expected... | `content-security-policy/report-hash/script-src.https.window.html` |
| 6 | TIMEOUT: the page never reported; first script error: /websockets/Close-N.any.js: TypeError: undefined (addEventListener) is not a function TypeError... | `websockets/Close-1000.any.html?default` |
| 6 | TIMEOUT: the page never reported; first script error: /websockets/Send-binary-arraybufferview-floatN.any.js: TypeError: undefined (addEventListener) ... | `websockets/Send-binary-arraybufferview-float64.any.html?default` |
| 6 | TIMEOUT: the page never reported; first script error: /websockets/Send-binary-arraybufferview-uintN-offset-length.any.js: TypeError: undefined (addEv... | `websockets/Send-binary-arraybufferview-uint16-offset-length.any.html?default` |
| 6 | TIMEOUT: the page never reported; first script error: /websockets/Send-binary-arraybufferview-uintN-offset.any.js: TypeError: undefined (addEventList... | `websockets/Send-binary-arraybufferview-uint32-offset.any.html?default` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: iframe is not defined ReferenceError: iframe is not defined a... | `css/css-values/viewport-units-compute.html` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: measure is not defined ReferenceError: measure is not defined... | `css/css-values/viewport-units-gutter-001.html` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: a timer callback must be a function TypeError: a timer callback mu... | `content-security-policy/reporting/report-multiple-violations-02.html` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'N' of undefined TypeError: cannot read prope... | `css/css-fonts/test_font_feature_values_parsing.html` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'location' of undefined TypeError: cannot rea... | `html/semantics/embedded-content/the-iframe-element/iframe-loading-lazy-history-pushState.html` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot resolve module './resources/common.js' TypeError: cannot re... | `html/semantics/embedded-content/bfcache/embedded-img.html` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: [object Object] Error: undefined at get_stack (@N) at AssertionError (@N) at ... | `css/css-values/if-invalidation.html` |
| 5 | ERROR: Error: unimplemented | `html/webappapis/system-state-and-capabilities/the-navigator-object/protocol-handler-fragment-nosw.https.html` |
| 5 | TIMEOUT: the page never reported; first script error: ../support/checkReport.sub.js?reportField=violated-directive&reportValue=img-src%N%Nnone%N: Typ... | `content-security-policy/reporting/report-blocked-data-uri.html` |
| 5 | TIMEOUT: the page never reported; first script error: /trusted-types/support/helper.sub.js: SyntaxError: expected ';' (line N) SyntaxError: expected ... | `domparsing/tentative/stream-html-with-trusted-types-error-in-policy.html` |
| 5 | TIMEOUT: the page never reported; first script error: /websockets/Send-binary-arraybufferview-intN.any.js: TypeError: undefined (addEventListener) is... | `websockets/Send-binary-arraybufferview-int32.any.html?default` |
| 5 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: aN is not defined ReferenceError: aN is not defined at test_s... | `html/editing/the-hidden-attribute/beforematch-attribute-removal-001.html` |
| 5 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: testStyle is not defined ReferenceError: testStyle is not def... | `css/css-fonts/parsing/font-face-metric-overrides.html` |
| 5 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: trustedTypes is not defined ReferenceError: trustedTypes is n... | `content-security-policy/script-src/script-src-trusted_types_eval_with_report_only_require_trusted_types_eval.html` |
| 5 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: Illegal constructor: CustomElementRegistry TypeError: Illegal cons... | `custom-elements/registries/Document-createElement.html` |
| 5 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot resolve module './resources/referrer-checker.py?name=same' ... | `html/semantics/scripting-1/the-script-element/module/referrer-same-origin.sub.html` |
| 5 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (showModal) is not a function TypeError: undefined (show... | `css/css-position/backdrop-tree-abiding-slotted.html` |
| 5 | TIMEOUT: the page never reported; first script error: support.js?pipe=sub: SyntaxError: expected a property name (line N) SyntaxError: expected a pro... | `cors/credentials-flag.htm` |
| 4 | ERROR: ReferenceError: caches is not defined | `html/cross-origin-embedder-policy/cache-storage-reporting-dedicated-worker.https.html` |
| 4 | ERROR: TypeError: cannot read property 'baseVal' of undefined | `svg/types/scripted/SVGLength-convertToSpecifiedUnits-font-change.html` |
| 4 | TIMEOUT: the page never reported; first script error: ../support/checkReport.sub.js?reportField=violated-directive&reportValue=script-src%N%Nnonce-ab... | `content-security-policy/inheritance/sandboxed-blob-scheme.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: elm is not defined ReferenceError: elm is not defined at <ano... | `css/css-multicol/getclientrects-003.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: scroller is not defined ReferenceError: scroller is not defin... | `css/css-overflow/scroll-markers/scroll-button-disposed-event-target.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: subjectN is not defined ReferenceError: subjectN is not defin... | `css/selectors/invalidation/has-sibling-insertion-removal.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'setAttribute' of undefined TypeError: cannot... | `content-security-policy/nonce-hiding/svgscript-nonces-hidden.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot resolve module './input-types.js' TypeError: cannot resolve... | `html/semantics/forms/the-input-element/cloning-steps.html` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot resolve module './resources/data-url-test.mjs' TypeError: c... | `html/browsers/origin/origin-keyed-agent-clusters/getter-special-cases/data-url-no.https.html` |
| 4 | TIMEOUT: the page never reported; first script error: resources/webperftestharness.js: ReferenceError: ﻿ is not defined ReferenceError: ﻿ is not ... | `resource-timing/resource_connection_reuse_mixed_content.html` |
| 3 | ERROR: TypeError: cannot read property 'cssRules' of undefined | `css/cssom/CSSStyleRule.html` |
| 3 | ERROR: TypeError: undefined (createDocument) is not a function | `dom/nodes/Document-createAttribute.html` |
| 3 | TIMEOUT: the page never reported; first script error: ../support/checkReport.sub.js?reportField=violated-directive&reportValue=img-src%Nhttp%NA%NF%NF... | `content-security-policy/reporting/multiple-report-policies.html` |
| 3 | TIMEOUT: the page never reported; first script error: /html/cross-origin-embedder-policy/credentialless/iframe-coep-credentialless.https.window.js: T... | `html/cross-origin-embedder-policy/credentialless/iframe-coep-credentialless.https.window.html?1-4` |
| 3 | TIMEOUT: the page never reported; first script error: /html/cross-origin-embedder-policy/credentialless/iframe-coep-require-corp.https.window.js: Typ... | `html/cross-origin-embedder-policy/credentialless/iframe-coep-require-corp.https.window.html?1-4` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Close-Reason-NBytes.any.js: TypeError: undefined (addEventListener) is not a functi... | `websockets/Close-Reason-124Bytes.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Close-delayed.any.js: TypeError: undefined (addEventListener) is not a function Typ... | `websockets/Close-delayed.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Close-onlyReason.any.js: TypeError: undefined (addEventListener) is not a function ... | `websockets/Close-onlyReason.any.html?wss` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Close-readyState-Closing.any.js: TypeError: undefined (addEventListener) is not a f... | `websockets/Close-readyState-Closing.any.html?wss` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Close-reason-unpaired-surrogates.any.js: TypeError: undefined (addEventListener) is... | `websockets/Close-reason-unpaired-surrogates.any.html?wpt_flags=h2` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Close-server-initiated-close.any.js: TypeError: undefined (addEventListener) is not... | `websockets/Close-server-initiated-close.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Close-undefined.any.js: TypeError: undefined (addEventListener) is not a function T... | `websockets/Close-undefined.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Create-valid-url-array-protocols.any.js: TypeError: undefined (addEventListener) is... | `websockets/Create-valid-url-array-protocols.any.html?wpt_flags=h2` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Create-valid-url-binaryType-blob.any.js: TypeError: undefined (addEventListener) is... | `websockets/Create-valid-url-binaryType-blob.any.html?wpt_flags=h2` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Create-valid-url-protocol-setCorrectly.any.js: TypeError: undefined (addEventListen... | `websockets/Create-valid-url-protocol-setCorrectly.any.html?wss` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Create-valid-url-protocol-string.any.js: TypeError: undefined (addEventListener) is... | `websockets/Create-valid-url-protocol-string.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Create-valid-url-protocol.any.js: TypeError: undefined (addEventListener) is not a ... | `websockets/Create-valid-url-protocol.any.html?wpt_flags=h2` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Create-valid-url.any.js: TypeError: undefined (addEventListener) is not a function ... | `websockets/Create-valid-url.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Send-Nbyte-data.any.js: TypeError: undefined (addEventListener) is not a function T... | `websockets/Send-0byte-data.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Send-binary-NK-arraybuffer.any.js: TypeError: undefined (addEventListener) is not a... | `websockets/Send-binary-65K-arraybuffer.any.html?wss` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Send-binary-arraybuffer.any.js: TypeError: undefined (addEventListener) is not a fu... | `websockets/Send-binary-arraybuffer.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Send-binary-arraybufferview-intN-offset.any.js: TypeError: undefined (addEventListe... | `websockets/Send-binary-arraybufferview-int16-offset.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Send-data.any.js: TypeError: undefined (addEventListener) is not a function TypeErr... | `websockets/Send-data.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Send-null.any.js: TypeError: undefined (addEventListener) is not a function TypeErr... | `websockets/Send-null.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Send-paired-surrogates.any.js: TypeError: undefined (addEventListener) is not a fun... | `websockets/Send-paired-surrogates.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Send-unicode-data.any.js: TypeError: undefined (addEventListener) is not a function... | `websockets/Send-unicode-data.any.html?default` |
| 3 | TIMEOUT: the page never reported; first script error: /websockets/Send-unpaired-surrogates.any.js: TypeError: undefined (addEventListener) is not a f... | `websockets/Send-unpaired-surrogates.any.html?default` |

## Why subtests fail

Ranked by *distinct tests* affected rather than by subtests, because that is the
number a fix unblocks. Digits are collapsed to `N`; quoted values are not, because
`expected "block" but got "inline"` and `expected "Npx" but got "Npx"` are
different bugs and a bucket labelled `assert_equals` is not actionable.

| tests | subtests | message | example |
|--:|--:|---|---|
| 1662 | 3054 | Test timed out | `FileAPI/FileReader/workers.html` |
| 1603 | 200592 | NOTRUN (no message) | `IndexedDB/database-names-by-origin.html` |
| 889 | 1048 | OffscreenCanvas is not defined | `html/canvas/element/drawing-images-to-the-canvas/2d.drawImage.detachedcanvas.html` |
| 512 | 1444 | promise_test: Unhandled rejection with value: object "Error: Python handlers are not implemented" | `content-security-policy/gen/top.http-rp/script-src-self/sharedworker-module.http.html` |
| 300 | 1060 | assert_true: expected true got false | `console/console-is-a-namespace.any.html` |
| 218 | 587 | assert_equals: expected N but got N | `css/CSS2/abspos/abspos-in-block-in-inline-in-relpos-inline.html` |
| 202 | 202 | undefined (pauseAnimations) is not a function | `svg/animations/additive-type-by-animation.html` |
| 201 | 757 | promise_test: Unhandled rejection with value: object "TypeError: undefined (open) is not a function" | `content-security-policy/inheritance/auxiliary-blank-document.html` |
| 200 | 29292 | assert_true: 'from' value should be supported expected true got false | `css/CSS2/floats-clear/clear-no-interpolation.html` |
| 160 | 768 | undefined (open) is not a function | `FileAPI/url/url-charset.window.html` |
| 148 | 166 | promise_test: Unhandled rejection with value: object "ReferenceError: OffscreenCanvas is not defined" | `html/canvas/element/global-hdr-headroom/clli-mdcv-png.html` |
| 147 | 391 | promise_test: Unhandled rejection with value: object "Error: action_sequence() is not implemented by testdriver-vendor.js" | `css/css-overflow/resizer-no-size-change.tentative.html` |
| 131 | 388 | promise_test: Unhandled rejection with value: object "Error: document.elementsFromPoint unsupported" | `IndexedDB/file_support.sub.html` |
| 117 | 2014 | assert_throws_js: function "function TypeError() { [native code] }" is not an Error subtype | `IndexedDB/idbfactory_cmp.any.html` |
| 114 | 117 | assert_equals: expected "" but got "auto" | `css/css-align/parsing/align-content-invalid.html` |
| 110 | 110 | assert_equals: Red channel of the pixel at (N, N) expected N but got N | `html/canvas/element/canvas-host/2d.canvas.host.initial.reset.same.html` |
| 107 | 172 | target is not defined | `css/CSS2/normal-flow/block-in-inline-hittest-float-001.html` |
| 105 | 304 | promise_test: Unhandled rejection with value: object "ReferenceError: SharedWorker is not defined" | `content-security-policy/inside-worker/sharedworker-connect-src.sub.html` |
| 103 | 218 | promise_test: Unhandled rejection with value: object "[object Object]" | `fetch/cross-origin-resource-policy/iframe-loads.html` |
| 91 | 304 | synchronous XMLHttpRequest is not supported | `websockets/cookies/007.html` |
| 88 | 1280 | assert_throws_dom: function "function () { [source unavailable] }" did not throw | `css/cssom/CSSStyleSheet-constructable-baseURL.html` |
| 88 | 537 | promise_test: Unhandled rejection with value: object "TypeError: Failed to fetch" | `FileAPI/url/url-with-fetch.any.html` |
| 82 | 240 | promise_test: Unhandled rejection with value: object "SyntaxError: invalid JSON: expected a number" | `fetch/api/abort/general.any.html` |
| 80 | 3006 | assert_true: 'to' value should be supported expected true got false | `css/CSS2/linebox/animations/line-height-interpolation.html` |
| 75 | 612 | assert_throws_js: function "function () { [source unavailable] }" did not throw | `FileAPI/blob/Blob-constructor.any.html` |
| 75 | 177 | cannot read property 'length' of undefined | `FileAPI/filelist-section/filelist.html` |
| 72 | 78 | undefined (write) is not a function | `content-security-policy/script-src/script-src-strict_dynamic_parser_inserted_correct_nonce.html` |
| 71 | 774 | cannot read property 'cssRules' of undefined | `css/css-animations/parsing/keyframes-name-invalid.html` |
| 70 | 73 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'append' of undefined" | `IndexedDB/back-forward-cache-open-transaction.window.html` |
| 65 | 81 | promise_test: Unhandled rejection with value: object "TypeError: undefined (blob) is not a function" | `content-security-policy/media-src/media-src-blocked-blob-url.html` |
| 62 | 365 | assert_equals: expected (number) N but got (undefined) undefined | `FileAPI/blob/Blob-constructor.any.html` |
| 62 | 64 | undefined (createPattern) is not a function | `html/canvas/element/canvas-host/2d.canvas.host.initial.reset.pattern.html` |
| 61 | 78 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'ready' of undefined" | `css/css-align/baseline-of-single-axis-scroll-container.html` |
| 60 | 96 | assert_true: Failed to create new rendered document expected true got false | `html/semantics/scripting-1/the-template-element/additions-to-parsing-xhtml-documents/node-document.html` |
| 60 | 67 | promise_test: Unhandled rejection with value: object "ReferenceError: canvas is not defined" | `html/canvas/element/manual/draw-element-image/backdrop-filter-bounds-expansion.tentative.html` |
| 57 | 89 | SharedWorker is not defined | `content-security-policy/sandbox/shared-worker-sandbox.html` |
| 56 | 63 | assert_equals: expected "" but got "N" | `css/css-align/parsing/column-gap-invalid.html` |
| 56 | 72 | assert_equals: expected "" but got "none" | `css/css-animations/parsing/animation-range-end-invalid.html` |
| 56 | 224 | assert_equals: expected (object) null but got (undefined) undefined | `css/cssom-view/offsetParent-body-and-html.html` |
| 56 | 278 | undefined (__defineSetter__) is not a function | `IndexedDB/abort-in-initial-upgradeneeded.any.html` |
| 51 | 1461 | cannot read property 'N' of undefined | `css/css-animations/CSSAnimation-effect.tentative.html` |
| 51 | 88 | promise_test: Unhandled rejection with value: object "ReferenceError: itemN is not defined" | `css/css-overflow/scroll-markers/scroll-marker-selection-picks-closest.html` |
| 50 | 150 | assert_equals: expected "Npx" but got "" | `css/css-align/gaps/column-gap-animation-001.html` |
| 48 | 79 | assert_true: expected true got undefined | `content-security-policy/script-src/script-src-event-handler-on-inline-script.html` |
| 48 | 54 | undefined (drawImage) is not a function | `fetch/redirects/subresource-fragments.html` |
| 46 | 562 | Illegal constructor: ReadableStream | `fetch/api/basic/request-upload.any.html` |
| 46 | 662 | assert_equals: expected (string) "" but got (undefined) undefined | `FileAPI/blob/Blob-constructor.any.html` |
| 46 | 308 | promise_test: Unhandled rejection with value: object "TypeError: Illegal constructor: ReadableStream" | `encoding/streams/decode-ignore-bom.any.html` |
| 44 | 44 | assert_true: track not supported expected true got false | `html/semantics/embedded-content/media-elements/track/track-element/cors/003.html` |
| 43 | 239 | assert_equals: Expected success event, but got upgradeneeded event instead expected "success" but got "upgradeneeded" | `IndexedDB/bindings-inject-keys-bypass.any.html` |
| 42 | 146 | assert_false: expected false got true | `cors/preflight-failure.htm` |
| 42 | 42 | undefined (roundRect) is not a function | `html/canvas/element/path-objects/2d.path.roundrect.1.radius.dompointinit.html` |
| 41 | 1356 | assert_equals: expected "Npx " but got "Npx " | `css/CSS2/linebox/animations/line-height-interpolation.html` |
| 41 | 301 | promise_test: Unhandled rejection with value: object "Error: Network Error" | `referrer-policy/4K/gen/top.http-rp/no-referrer-when-downgrade/xhr.http.html` |
| 40 | 136 | assert_equals: expected "Npx" but got "Npx" | `content-security-policy/style-src/style-src-injected-inline-style-allowed-with-content-hash.html` |
| 40 | 167 | promise_test: Unhandled rejection with value: object "TypeError: undefined (getAnimations) is not a function" | `css/css-animations/AnimationEffect-getComputedTiming.tentative.html` |
| 40 | 206 | undefined (getAnimations) is not a function | `css/css-animations/AnimationEffect-getComputedTiming.tentative.html` |
| 39 | 39 | promise_test: Unhandled rejection with value: object "TypeError: cannot set property 'name' of undefined" | `FileAPI/idlharness.any.html` |
| 37 | 309 | assert_unreached: Should have rejected: undefined Reached unreachable code | `fetch/api/basic/header-value-null-byte.any.html` |
| 36 | 323 | promise_rejects_js: function "function TypeError() { [native code] }" is not an Error subtype | `FileAPI/url/url-with-fetch.any.html` |
| 34 | 34 | undefined (createLinearGradient) is not a function | `html/canvas/element/canvas-host/2d.canvas.host.initial.reset.gradient.html` |
| 34 | 35 | undefined (reset) is not a function | `custom-elements/form-associated/form-reset-callback.html` |
| 32 | 50 | assert_false: expected false got undefined | `dom/events/EventTarget-dispatchEvent-returnvalue.html` |
| 31 | 343 | DOMParser is not defined | `content-security-policy/xslt/xsltprocessor-include-blocked.html` |
| 31 | 127 | container is not defined | `css/CSS2/positioning/detach-abspos-before-layout.html` |
| 30 | 34 | assert_equals: expected "" but got "-Npx" | `css/css-align/parsing/column-gap-invalid.html` |
| 30 | 320 | cannot read property 'baseVal' of undefined | `svg/animations/attribute-value-unaffected-by-animation-002.html` |
| 29 | 74 | cannot read property 'document' of undefined | `css/cssom/getComputedStyle-dynamic-subdoc.html` |
| 29 | 58 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'currentTime' of undefined" | `css/css-animations/empty-pseudo-class-with-animation.html` |
| 29 | 82 | undefined (createDocument) is not a function | `content-security-policy/nonce-hiding/nonce-hiding-move-document.html` |
| 29 | 29 | undefined (setCurrentTime) is not a function | `svg/animations/conditional-processing-02.html` |
| 28 | 55 | promise_test: Unhandled rejection with value: object "ReferenceError: service_worker_unregister_and_register is not defined" | `content-security-policy/sandbox/service-worker-sandbox.https.html` |
| 28 | 324 | promise_test: Unhandled rejection with value: object "TypeError: undefined (showPopover) is not a function" | `css/selectors/active-toplayer-001.html` |
| 27 | 29 | assert_equals: expected "" but got "Npx" | `css/css-backgrounds/parsing/box-shadow-invalid.html` |
| 27 | 36 | assert_equals: expected "rgb(N, N, N)" but got "rgb(N, N, N)" | `css/css-cascade/layer-replaceSync-clears-stale.html` |
| 26 | 26 | assert_equals: canvas.width === N (got N[number], expected N[number]) expected N but got N | `html/canvas/element/canvas-host/2d.canvas.host.size.attributes.parse.em.html` |
| 26 | 57 | undefined (getElementsByName) is not a function | `dom/nodes/moveBefore/moveBefore-name-map.html` |
| 26 | 26 | undefined (toDataURL) is not a function | `html/canvas/element/layers/2d.layer.malformed-operations.html` |
| 25 | 31 | assert_equals: serialization should be canonical expected "Npx" but got "N" | `css/css-align/parsing/column-gap-valid.html` |
| 25 | 112 | main is not defined | `css/css-cascade/at-scope-parsing.html` |
| 24 | 97 | FormData is not defined | `custom-elements/form-associated/form-disabled-callback.html` |
| 23 | 24 | assert_equals: expected "" but got "-N%" | `css/css-align/parsing/column-gap-invalid.html` |
| 23 | 212 | cannot read property 'append' of undefined | `content-security-policy/embedded-enforcement/allow_csp_from-header.html` |
| 22 | 395 | KeyframeEffect is not defined | `css/css-animations/AnimationEffect-getComputedTiming.tentative.html` |
| 22 | 67 | assert_equals: expected "N" but got "N" | `css/css-flexbox/getcomputedstyle/flexbox_computedstyle_order-inherit.html` |
| 21 | 93 | Illegal constructor: CustomElementRegistry | `custom-elements/registries/Construct.html` |
| 21 | 25 | assert_equals: expected "" but got "N%" | `css/css-backgrounds/parsing/border-image-outset-invalid.html` |
| 21 | 50 | document.elementsFromPoint unsupported | `css/css-position/sticky/sticky-after-input.html` |
| 21 | 25 | promise_test: Unhandled rejection with value: object "TypeError: undefined (showModal) is not a function" | `css/css-animations/dialog-backdrop-animation.html` |
| 21 | 21 | undefined (createRadialGradient) is not a function | `html/canvas/element/fill-and-stroke-styles/2d.gradient.radial.cone.bottom.html` |
| 20 | 171 | WritableStream is not defined | `streams/piping/general.any.html` |
| 20 | 61 | assert_equals: expected "none" but got "" | `css/css-animations/parsing/animation-computed.html` |
| 20 | 136 | promise_test: Unhandled rejection with value: object "ReferenceError: WebSocketStream is not defined" | `websockets/stream/tentative/abort.any.html?wpt_flags=h2` |
| 20 | 93 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'set' of undefined" | `content-security-policy/frame-src/frame-src-cross-origin-same-document-navigation.window.html` |
| 19 | 594 | assert_equals: expected true but got false | `css/css-conditional/js/CSS-supports-L3.html` |
| 19 | 7233 | assert_true: color doesn't seem to be supported in the computed style expected true got false | `css/css-color/color-mix-missing-components.html` |
| 19 | 23 | promise_test: Unhandled rejection with value: object "ReferenceError: target is not defined" | `css/css-animations/display-none-to-display-block-dont-cancel.tentative.html` |
| 19 | 73 | promise_test: Unhandled rejection with value: object "ReferenceError: with_iframe is not defined" | `fetch/content-encoding/br/br-navigation.https.window.html` |
| 19 | 24 | subject is not defined | `css/selectors/invalidation/defined-in-has.html` |
| 19 | 44 | undefined (addTextTrack) is not a function | `html/semantics/embedded-content/media-elements/interfaces/HTMLElement/HTMLMediaElement/addTextTrack.html` |

29886 distinct subtest messages and 463 distinct harness messages behind these numbers.
