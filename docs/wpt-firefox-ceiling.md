# WPT Firefox Ceiling

**Generated** by `tools/wpt/firefox-ref.py`. Do not edit by hand.

Firefox version: 156.0a1
Firefox run date: 2026-08-15
WPT revision: `40f78009c815`

The ceiling for each area is Firefox's pass rate on the same tests,
or `refused` when an ADR says this browser will not implement the area.
A gap between microbrowser and the ceiling is a bug; a test Firefox
also fails is not our problem (yet); a test we refuse is a decision
with a name.

| area | us % | us pass/total | firefox % | firefox pass/total | ceiling | gap | note |
|---|--:|---|--:|---|---|---|---|
| `FileAPI` | 38.5 | 10/26 | 98.1 | 419/427 | 98.1 | 59.7 |  |
| `FileAPI/BlobURL` | 0.0 | 0/16 | 66.7 | 12/18 | 66.7 | 66.7 |  |
| `FileAPI/FileReader` | 0.0 | 0/1 | 100.0 | 2/2 | 100.0 | 100.0 |  |
| `FileAPI/blob` | 0.4 | 1/259 | 94.8 | 587/619 | 94.8 | 94.4 |  |
| `FileAPI/file` | 0.0 | 0/163 | 100.0 | 313/313 | 100.0 | 100.0 |  |
| `FileAPI/filelist-section` | 0.0 | 0/7 | 100.0 | 7/7 | 100.0 | 100.0 |  |
| `FileAPI/reading-data-section` | 0.0 | 0/48 | 100.0 | 96/96 | 100.0 | 100.0 |  |
| `FileAPI/url` | 53.8 | 21/39 | 97.8 | 131/134 | 97.8 | 43.9 |  |
| `IndexedDB` | 4.7 | 49/1032 | 99.9 | 5829/5837 | 99.9 | 95.1 |  |
| `IndexedDB/crashtests` | 100.0 | 1/1 | 100.0 | 4/4 | 100.0 | done |  |
| `console` | 20.7 | 6/29 | 95.3 | 102/107 | 95.3 | 74.6 |  |
| `content-security-policy` |  |  |  |  | no firefox data | - |  |
| `content-security-policy/base-uri` | 0.0 | 0/2 | 90.9 | 10/11 | 90.9 | 90.9 |  |
| `content-security-policy/blob` | 0.0 | 0/6 | 100.0 | 6/6 | 100.0 | 100.0 |  |
| `content-security-policy/child-src` | 40.0 | 2/5 | 71.4 | 10/14 | 71.4 | 31.4 |  |
| `content-security-policy/connect-src` | 0.0 | 0/32 | 94.7 | 36/38 | 94.7 | 94.7 |  |
| `content-security-policy/default-src` | 42.9 | 6/14 | 100.0 | 15/15 | 100.0 | 57.1 |  |
| `content-security-policy/embedded-enforcement` | 0.0 | 0/281 | 52.6 | 160/304 | 52.6 | 52.6 |  |
| `content-security-policy/font-src` |  |  | 100.0 | 5/5 | 100.0 |  |  |
| `content-security-policy/form-action` | 0.0 | 0/7 | 85.7 | 12/14 | 85.7 | 85.7 |  |
| `content-security-policy/frame-ancestors` | 30.0 | 6/20 | 97.2 | 35/36 | 97.2 | 67.2 |  |
| `content-security-policy/frame-src` | 0.0 | 0/8 | 82.4 | 14/17 | 82.4 | 82.4 |  |
| `content-security-policy/gen` | 0.0 | 0/2480 | 80.3 | 1992/2480 | 80.3 | 80.3 |  |
| `content-security-policy/generic` | 28.2 | 11/39 | 93.3 | 97/104 | 93.3 | 65.1 |  |
| `content-security-policy/img-src` | 14.3 | 1/7 | 100.0 | 20/20 | 100.0 | 85.7 |  |
| `content-security-policy/inheritance` | 2.4 | 1/41 | 82.9 | 68/82 | 82.9 | 80.5 |  |
| `content-security-policy/inside-worker` | 0.0 | 0/9 | 83.9 | 99/118 | 83.9 | 83.9 |  |
| `content-security-policy/media-src` | 0.0 | 0/2 | 100.0 | 29/29 | 100.0 | 100.0 |  |
| `content-security-policy/meta` | 20.0 | 1/5 | 83.3 | 5/6 | 83.3 | 63.3 |  |
| `content-security-policy/navigation` |  |  | 100.0 | 29/29 | 100.0 |  |  |
| `content-security-policy/nonce-hiding` | 14.5 | 8/55 | 100.0 | 97/97 | 100.0 | 85.5 |  |
| `content-security-policy/object-src` |  |  | 71.4 | 10/14 | 71.4 |  |  |
| `content-security-policy/parsing` |  |  | 100.0 | 19/19 | 100.0 |  |  |
| `content-security-policy/plugin-types` |  |  | 100.0 | 2/2 | 100.0 |  |  |
| `content-security-policy/report-hash` |  |  | 37.5 | 27/72 | 37.5 |  |  |
| `content-security-policy/reporting` | 17.6 | 3/17 | 90.2 | 55/61 | 90.2 | 72.5 |  |
| `content-security-policy/reporting-api` | 0.0 | 0/5 | 97.1 | 33/34 | 97.1 | 97.1 |  |
| `content-security-policy/resource-hints` | 0.0 | 0/26 | 13.9 | 5/36 | 13.9 | 13.9 |  |
| `content-security-policy/sandbox` | 0.0 | 0/21 | 96.3 | 26/27 | 96.3 | 96.3 |  |
| `content-security-policy/script-src` | 42.9 | 45/105 | 83.6 | 179/214 | 83.6 | 40.8 |  |
| `content-security-policy/script-src-attr-elem` | 66.7 | 2/3 | 100.0 | 10/10 | 100.0 | 33.3 |  |
| `content-security-policy/securitypolicyviolation` | 7.1 | 2/28 | 78.1 | 89/114 | 78.1 | 70.9 |  |
| `content-security-policy/style-src` | 46.7 | 14/30 | 94.3 | 82/87 | 94.3 | 47.6 |  |
| `content-security-policy/style-src-attr-elem` | 0.0 | 0/3 | 100.0 | 11/11 | 100.0 | 100.0 |  |
| `content-security-policy/svg` | 0.0 | 0/3 | 80.0 | 4/5 | 80.0 | 80.0 |  |
| `content-security-policy/unsafe-eval` | 7.1 | 1/14 | 73.9 | 17/23 | 73.9 | 66.8 |  |
| `content-security-policy/unsafe-hashes` | 0.0 | 0/5 | 100.0 | 24/24 | 100.0 | 100.0 |  |
| `content-security-policy/wasm-unsafe-eval` | 0.0 | 0/31 | 100.0 | 129/129 | 100.0 | 100.0 |  |
| `content-security-policy/webrtc` | 0.0 | 0/5 | 60.0 | 3/5 | 60.0 | 60.0 |  |
| `content-security-policy/worker-src` | 0.0 | 0/25 | 95.0 | 38/40 | 95.0 | 95.0 |  |
| `content-security-policy/xslt` | 0.0 | 0/3 | 100.0 | 3/3 | 100.0 | 100.0 |  |
| `cookies` | 33.3 | 1/3 | 98.3 | 619/630 | 98.3 | 64.9 |  |
| `cookies/attributes` | 0.0 | 0/524 | 99.0 | 583/589 | 99.0 | 99.0 |  |
| `cookies/domain` | 0.0 | 0/6 | 100.0 | 18/18 | 100.0 | 100.0 |  |
| `cookies/encoding` | 0.0 | 0/6 | 100.0 | 6/6 | 100.0 | 100.0 |  |
| `cookies/name` | 0.0 | 0/111 | 87.4 | 97/111 | 87.4 | 87.4 |  |
| `cookies/ordering` | 0.0 | 0/1 | 100.0 | 5/5 | 100.0 | 100.0 |  |
| `cookies/origin-bound-cookies` | 0.0 | 0/2 | 50.0 | 1/2 | 50.0 | 50.0 |  |
| `cookies/partitioned-cookies` | 0.0 | 0/7 | 92.9 | 13/14 | 92.9 | 92.9 |  |
| `cookies/path` | 0.0 | 0/16 | 100.0 | 17/17 | 100.0 | 100.0 |  |
| `cookies/prefix` | 0.0 | 0/183 | 100.0 | 183/183 | 100.0 | 100.0 |  |
| `cookies/samesite` | 0.0 | 0/119 | 60.6 | 77/127 | 60.6 | 60.6 |  |
| `cookies/samesite-none-secure` | 0.0 | 0/1 | 100.0 | 1/1 | 100.0 | 100.0 |  |
| `cookies/schemeful-same-site` | 0.0 | 0/5 | 50.0 | 3/6 | 50.0 | 50.0 |  |
| `cookies/secure` | 20.0 | 1/5 | 100.0 | 10/10 | 100.0 | 80.0 |  |
| `cookies/size` | 0.0 | 0/25 | 96.0 | 24/25 | 96.0 | 96.0 |  |
| `cookies/third-party-cookies` |  |  | 100.0 | 1/1 | 100.0 |  |  |
| `cookies/value` | 0.0 | 0/94 | 93.6 | 88/94 | 93.6 | 93.6 |  |
| `cors` | 15.9 | 36/227 | 99.1 | 449/453 | 99.1 | 83.3 |  |
| `css/CSS2` | 24.4 | 601/2464 | 98.9 | 2476/2503 | 98.9 | 74.5 |  |
| `css/css-align` | 21.9 | 807/3689 | 94.0 | 4909/5220 | 94.0 | 72.2 |  |
| `css/css-animations` | 29.7 | 357/1201 | 94.6 | 1328/1404 | 94.6 | 64.9 |  |
| `css/css-backgrounds` | 8.1 | 493/6117 | 95.6 | 5909/6181 | 95.6 | 87.5 |  |
| `css/css-box` | 31.7 | 303/957 | 84.2 | 939/1115 | 84.2 | 52.6 |  |
| `css/css-cascade` | 1.5 | 11/716 | 98.9 | 1553/1570 | 98.9 | 97.4 |  |
| `css/css-color` | 11.2 | 1272/11336 | 95.4 | 11094/11631 | 95.4 | 84.2 |  |
| `css/css-conditional` | 88.2 | 1547/1753 | 97.3 | 2859/2939 | 97.3 | 9.0 |  |
| `css/css-display` | 16.8 | 72/428 | 74.2 | 412/555 | 74.2 | 57.4 |  |
| `css/css-flexbox` | 15.1 | 210/1391 | 97.2 | 4582/4713 | 97.2 | 82.1 |  |
| `css/css-fonts` | 15.9 | 1195/7530 | 96.8 | 8186/8453 | 96.8 | 81.0 |  |
| `css/css-grid` | 6.2 | 402/6460 | 89.4 | 13169/14728 | 89.4 | 83.2 |  |
| `css/css-images` | 19.0 | 679/3580 | 97.5 | 3498/3587 | 97.5 | 78.6 |  |
| `css/css-multicol` | 2.7 | 41/1521 | 78.1 | 1260/1613 | 78.1 | 75.4 |  |
| `css/css-overflow` | 19.1 | 183/957 | 68.9 | 754/1094 | 68.9 | 49.8 |  |
| `css/css-position` | 20.7 | 284/1375 | 91.4 | 1354/1482 | 91.4 | 70.7 |  |
| `css/css-shapes` | 15.3 | 741/4855 | 84.3 | 6337/7514 | 84.3 | 69.1 |  |
| `css/css-sizing` | 13.7 | 574/4194 | 80.9 | 4797/5931 | 80.9 | 67.2 |  |
| `css/css-syntax` | 13.8 | 59/429 | 76.7 | 332/433 | 76.7 | 62.9 |  |
| `css/css-tables` | 24.8 | 208/839 | 89.9 | 813/904 | 89.9 | 65.1 |  |
| `css/css-text` | 11.9 | 345/2908 | 90.3 | 4081/4518 | 90.3 | 78.5 |  |
| `css/css-text-decor` | 7.8 | 99/1276 | 98.2 | 1269/1292 | 98.2 | 90.5 |  |
| `css/css-transforms` | 24.0 | 1317/5498 | 99.2 | 5472/5514 | 99.2 | 75.3 |  |
| `css/css-transitions` | 2.9 | 90/3101 | 88.2 | 2915/3305 | 88.2 | 85.3 |  |
| `css/css-ui` | 9.2 | 174/1900 | 91.6 | 1773/1936 | 91.6 | 82.4 |  |
| `css/css-values` | 8.3 | 802/9671 | 56.2 | 5766/10259 | 56.2 | 47.9 |  |
| `css/css-variables` | 38.6 | 194/502 | 86.3 | 506/586 | 86.3 | 47.7 |  |
| `css/css-writing-modes` | 21.1 | 63/298 | 93.7 | 373/398 | 93.7 | 72.6 |  |
| `css/cssom` | 49.3 | 708/1435 | 97.4 | 3914/4020 | 97.4 | 48.0 |  |
| `css/cssom-view` | 27.1 | 365/1349 | 86.3 | 2239/2595 | 86.3 | 59.2 |  |
| `css/selectors` | 24.7 | 334/1353 | 94.4 | 5698/6034 | 94.4 | 69.7 |  |
| `custom-elements` | 19.9 | 196/983 | 98.4 | 1036/1053 | 98.4 | 78.4 |  |
| `custom-elements/form-associated` | 1.0 | 1/103 | 100.0 | 104/104 | 100.0 | 99.0 |  |
| `custom-elements/htmlconstructor` | 0.0 | 0/20 | 100.0 | 20/20 | 100.0 | 100.0 |  |
| `custom-elements/parser` | 35.0 | 7/20 | 100.0 | 34/34 | 100.0 | 65.0 |  |
| `custom-elements/reactions` | 15.6 | 80/514 | 97.4 | 520/534 | 97.4 | 81.8 |  |
| `custom-elements/registries` | 10.1 | 225/2233 | 89.2 | 2072/2322 | 89.2 | 79.2 |  |
| `custom-elements/state` | 3.6 | 1/28 | 100.0 | 29/29 | 100.0 | 96.4 |  |
| `custom-elements/upgrading` | 3.4 | 1/29 | 79.3 | 23/29 | 79.3 | 75.9 |  |
| `dom` | 52.8 | 66/125 | 99.8 | 2733/2739 | 99.8 | 47.0 |  |
| `dom/abort` | 27.0 | 10/37 | 100.0 | 72/72 | 100.0 | 73.0 |  |
| `dom/collections` | 13.2 | 7/53 | 100.0 | 53/53 | 100.0 | 86.8 |  |
| `dom/events` | 27.0 | 183/677 | 93.4 | 819/877 | 93.4 | 66.4 |  |
| `dom/lists` | 76.2 | 144/189 | 100.0 | 189/189 | 100.0 | 23.8 |  |
| `dom/nodes` | 62.2 | 3389/5451 | 98.7 | 14910/15106 | 98.7 | 36.5 |  |
| `dom/observable` | 0.0 | 0/251 | 0.4 | 2/525 | 0.4 | done |  |
| `dom/ranges` | 8.1 | 21/259 | 99.6 | 44392/44583 | 99.6 | 91.5 |  |
| `dom/traversal` | 53.6 | 30/56 | 99.7 | 1603/1608 | 99.7 | 46.1 |  |
| `domparsing` | 18.1 | 77/426 | 94.3 | 509/540 | 94.3 | 76.2 |  |
| `domparsing/tentative` | 3.1 | 28/905 | 2.7 | 33/1202 | 2.7 | done |  |
| `domxpath` | 1.1 | 1/87 | 98.2 | 1133/1154 | 98.2 | 97.0 |  |
| `encoding` | 0.6 | 68/11801 | 98.9 | 23583/23838 | 98.9 | 98.4 |  |
| `encoding/legacy-mb-japanese` | 0.0 | 1/113882 | 100.0 | 447723/447723 | 100.0 | 100.0 |  |
| `encoding/legacy-mb-korean` | 0.0 | 0/66015 | 100.0 | 410448/410448 | 100.0 | 100.0 |  |
| `encoding/legacy-mb-schinese` | 0.3 | 2/660 | 100.0 | 1017/1017 | 100.0 | 99.7 |  |
| `encoding/legacy-mb-tchinese` | 0.3 | 123/40030 | 100.0 | 268916/268916 | 100.0 | 99.7 |  |
| `encoding/streams` | 0.0 | 0/111 | 95.8 | 452/472 | 95.8 | 95.8 |  |
| `eventsource` | 4.8 | 1/21 | 100.0 | 118/118 | 100.0 | 95.2 |  |
| `eventsource/dedicated-worker` | 0.0 | 0/1 | 100.0 | 13/13 | 100.0 | 100.0 |  |
| `eventsource/shared-worker` | 0.0 | 0/10 | 100.0 | 10/10 | 100.0 | 100.0 |  |
| `fetch/api` | 12.0 | 273/2269 | 93.2 | 7527/8072 | 93.2 | 81.2 |  |
| `fetch/compression-dictionary` | 0.0 | 0/99 | 89.3 | 92/103 | 89.3 | 89.3 |  |
| `fetch/connection-pool` | 0.0 | 0/9 | 88.9 | 8/9 | 88.9 | 88.9 |  |
| `fetch/content-encoding` | 11.1 | 4/36 | 100.0 | 117/117 | 100.0 | 88.9 |  |
| `fetch/content-length` | 2.6 | 1/39 | 90.5 | 38/42 | 90.5 | 87.9 |  |
| `fetch/content-type` | 21.9 | 7/32 | 94.8 | 147/155 | 94.8 | 73.0 |  |
| `fetch/corb` | 86.8 | 33/38 | 92.2 | 71/77 | 92.2 | 5.4 |  |
| `fetch/cross-origin-resource-policy` | 24.3 | 9/37 | 99.1 | 106/107 | 99.1 | 74.7 |  |
| `fetch/data-urls` | 1.9 | 3/161 | 100.0 | 623/623 | 100.0 | 98.1 |  |
| `fetch/fetch-later` | 2.5 | 3/118 | 5.8 | 9/154 | 5.8 | 3.3 |  |
| `fetch/h1-parsing` | 5.3 | 12/227 | 36.3 | 85/234 | 36.3 | 31.0 |  |
| `fetch/http-cache` | 0.0 | 0/142 | 78.2 | 430/550 | 78.2 | 78.2 |  |
| `fetch/images` | 0.0 | 0/1 | 100.0 | 1/1 | 100.0 | 100.0 |  |
| `fetch/local-network-access` | 0.0 | 0/35 | 71.2 | 47/66 | 71.2 | 71.2 |  |
| `fetch/metadata` | 0.2 | 2/1318 | 95.0 | 2203/2320 | 95.0 | 94.8 |  |
| `fetch/nosniff` | 9.4 | 6/64 | 100.0 | 65/65 | 100.0 | 90.6 |  |
| `fetch/orb` | 80.0 | 4/5 | 84.6 | 143/169 | 84.6 | 4.6 |  |
| `fetch/origin` | 0.0 | 0/43 | 100.0 | 43/43 | 100.0 | 100.0 |  |
| `fetch/range` | 3.9 | 2/51 | 92.1 | 93/101 | 92.1 | 88.2 |  |
| `fetch/redirect-navigate` | 0.0 | 0/121 | 100.0 | 121/121 | 100.0 | 100.0 |  |
| `fetch/redirects` | 0.0 | 0/7 | 100.0 | 7/7 | 100.0 | 100.0 |  |
| `fetch/security` | 6.7 | 1/15 | 65.6 | 59/90 | 65.6 | 58.9 |  |
| `fetch/stale-while-revalidate` | 0.0 | 0/3 | 77.8 | 7/9 | 77.8 | 77.8 |  |
| `hr-time` | 30.8 | 4/13 | 98.6 | 144/146 | 98.6 | 67.9 |  |
| `html` |  |  | 81.0 | 294/363 | 81.0 |  |  |
| `html/anonymous-iframe` | 0.0 | 0/39 | 26.3 | 15/57 | 26.3 | 26.3 |  |
| `html/browsers` | 9.7 | 142/1469 | 80.1 | 2447/3055 | 80.1 | 70.4 |  |
| `html/canvas` | 12.2 | 369/3015 | 78.5 | 4517/5755 | 78.5 | 66.2 |  |
| `html/capability-delegation` | 0.0 | 0/6 | 56.2 | 9/16 | 56.2 | 56.2 |  |
| `html/cross-origin-embedder-policy` | 4.3 | 15/345 | 76.9 | 443/576 | 76.9 | 72.6 |  |
| `html/cross-origin-opener-policy` | 0.3 | 2/583 | 79.1 | 510/645 | 79.1 | 78.7 |  |
| `html/document-isolation-policy` | 0.0 | 0/150 | 64.7 | 123/190 | 64.7 | 64.7 |  |
| `html/dom` | 96.0 | 57759/60138 | 97.1 | 65690/67654 | 97.1 | 1.1 |  |
| `html/editing` | 6.4 | 39/614 | 71.9 | 577/802 | 71.9 | 65.6 |  |
| `html/embedded-content` | 0.0 | 0/2 | 100.0 | 2/2 | 100.0 | 100.0 |  |
| `html/infrastructure` | 9.7 | 45/463 | 97.9 | 1614/1649 | 97.9 | 88.2 |  |
| `html/interaction` | 6.7 | 48/712 | 58.3 | 434/744 | 58.3 | 51.6 |  |
| `html/links` | 75.0 | 3/4 | 66.7 | 4/6 | 66.7 | done |  |
| `html/meta` |  |  | 100.0 | 3/3 | 100.0 |  |  |
| `html/obsolete` | 13.2 | 7/53 | 100.0 | 53/53 | 100.0 | 86.8 |  |
| `html/rendering` | 85.8 | 6153/7169 | 98.3 | 11429/11632 | 98.3 | 12.4 |  |
| `html/scripting` | 33.3 | 1/3 | 100.0 | 3/3 | 100.0 | 66.7 |  |
| `html/select` | 0.0 | 0/5 | 100.0 | 5/5 | 100.0 | 100.0 |  |
| `html/semantics` | 15.4 | 2262/14678 | 92.4 | 19099/20667 | 92.4 | 77.0 |  |
| `html/syntax` | 16.3 | 497/3045 | 96.3 | 8507/8830 | 96.3 | 80.0 |  |
| `html/the-xhtml-syntax` |  |  | 100.0 | 25607/25607 | 100.0 |  |  |
| `html/user-activation` | 0.0 | 0/6 | 74.4 | 32/43 | 74.4 | 74.4 |  |
| `html/webappapis` | 36.6 | 441/1206 | 91.8 | 3328/3624 | 91.8 | 55.3 |  |
| `intersection-observer` | 31.1 | 56/180 | 86.9 | 298/343 | 86.9 | 55.8 |  |
| `intersection-observer/v2` | 33.3 | 18/54 | 21.4 | 15/70 | 21.4 | done |  |
| `media-source` | 22.0 | 59/268 | 75.1 | 471/627 | 75.1 | 53.1 |  |
| `media-source/dedicated-worker` | 2.0 | 1/50 | 4.8 | 3/62 | 4.8 | 2.8 |  |
| `media-source/mse-for-webcodecs` |  |  |  |  | no firefox data | - |  |
| `mimesniff/media` |  |  | 100.0 | 42/42 | 100.0 |  |  |
| `mimesniff/mime-types` | 0.1 | 2/1939 | 54.8 | 2103/3837 | 54.8 | 54.7 |  |
| `mimesniff/sniffing` | 42.9 | 3/7 | 100.0 | 7/7 | 100.0 | 57.1 |  |
| `navigation-timing` | 64.9 | 63/97 | 93.9 | 431/459 | 93.9 | 29.0 |  |
| `performance-timeline` | 52.2 | 24/46 | 91.6 | 305/333 | 91.6 | 39.4 |  |
| `performance-timeline/not-restored-reasons` | 0.0 | 0/13 | 0.0 | 0/12 | 0.0 | done |  |
| `png` | 0.0 | 0/1 | 100.0 | 3/3 | 100.0 | 100.0 |  |
| `referrer-policy/4K` | 0.0 | 0/386 | 100.0 | 416/416 | 100.0 | 100.0 |  |
| `referrer-policy/4K+1` | 0.0 | 0/376 | 100.0 | 416/416 | 100.0 | 100.0 |  |
| `referrer-policy/4K-1` | 0.0 | 0/382 | 100.0 | 416/416 | 100.0 | 100.0 |  |
| `referrer-policy/css-integration` | 0.0 | 0/56 | 100.0 | 71/71 | 100.0 | 100.0 |  |
| `referrer-policy/gen` | 0.0 | 0/6544 | 94.1 | 6630/7042 | 94.1 | 94.1 |  |
| `referrer-policy/generic` | 3.8 | 3/78 | 79.3 | 88/111 | 79.3 | 75.4 |  |
| `resize-observer` | 44.8 | 13/29 | 96.9 | 127/131 | 96.9 | 52.1 |  |
| `resource-timing` | 1.3 | 7/538 | 87.1 | 924/1061 | 87.1 | 85.8 |  |
| `resource-timing/initiator-type` | 24.1 | 7/29 | 74.0 | 37/50 | 74.0 | 49.9 |  |
| `resource-timing/tentative` | 0.0 | 0/39 | 0.0 | 0/73 | 0.0 | done |  |
| `selection` | 0.0 | 0/143 | 97.6 | 33019/33840 | 97.6 | 97.6 |  |
| `selection/anonymous` | 0.0 | 0/12 | 100.0 | 13/13 | 100.0 | 100.0 |  |
| `selection/bidi` |  |  | 100.0 | 92/92 | 100.0 |  |  |
| `selection/caret` | 18.2 | 2/11 | 85.2 | 23/27 | 85.2 | 67.0 |  |
| `selection/contenteditable` | 0.0 | 0/61 | 95.2 | 240/252 | 95.2 | 95.2 |  |
| `selection/shadow-dom` | 2.0 | 1/51 | 85.0 | 68/80 | 85.0 | 83.0 |  |
| `selection/textcontrols` | 0.0 | 0/3 | 100.0 | 78/78 | 100.0 | 100.0 |  |
| `shadow-dom` | 11.4 | 84/739 | 94.8 | 723/763 | 94.8 | 83.4 |  |
| `shadow-dom/declarative` | 99.2 | 7731/7791 | 99.4 | 7744/7793 | 99.4 | done |  |
| `shadow-dom/focus` | 6.4 | 5/78 | 99.1 | 108/109 | 99.1 | 92.7 |  |
| `shadow-dom/focus-navigation` | 2.2 | 2/89 | 47.2 | 42/89 | 47.2 | 44.9 |  |
| `shadow-dom/leaktests` | 43.8 | 7/16 | 93.8 | 15/16 | 93.8 | 50.0 |  |
| `shadow-dom/reference-target` | 0.0 | 0/768 | 98.0 | 6138/6263 | 98.0 | 98.0 |  |
| `shadow-dom/untriaged` | 71.7 | 180/251 | 98.8 | 248/251 | 98.8 | 27.1 |  |
| `storage` | 0.0 | 0/23 | 88.6 | 210/237 | 88.6 | 88.6 |  |
| `storage/buckets` | 0.0 | 0/48 | 17.9 | 68/379 | 17.9 | 17.9 |  |
| `streams` | 0.0 | 0/3 | 100.0 | 994/994 | 100.0 | 100.0 |  |
| `streams/piping` | 0.0 | 0/204 | 99.6 | 912/916 | 99.6 | 99.6 |  |
| `streams/readable-byte-streams` | 2.1 | 5/233 | 100.0 | 996/996 | 100.0 | 97.9 |  |
| `streams/readable-streams` | 4.7 | 17/362 | 95.3 | 1383/1451 | 95.3 | 90.6 |  |
| `streams/transferable` | 0.0 | 0/71 | 100.0 | 87/87 | 100.0 | 100.0 |  |
| `streams/transform-streams` | 0.0 | 0/134 | 90.2 | 481/533 | 90.2 | 90.2 |  |
| `streams/writable-streams` | 0.0 | 0/196 | 100.0 | 784/784 | 100.0 | 100.0 |  |
| `subresource-integrity` |  |  | 100.0 | 48/48 | 100.0 |  |  |
| `subresource-integrity/integrity-policy` | 0.0 | 0/37 | 76.4 | 55/72 | 76.4 | 76.4 |  |
| `subresource-integrity/signatures` | 24.3 | 43/177 | 51.3 | 97/189 | 51.3 | 27.0 |  |
| `subresource-integrity/unencoded-digest` | 33.3 | 28/84 | 66.7 | 112/168 | 66.7 | 33.3 |  |
| `svg` | 64.3 | 45/70 | 97.5 | 1786/1832 | 97.5 | 33.2 |  |
| `svg/animations` | 1.4 | 4/280 | 94.2 | 307/326 | 94.2 | 92.7 |  |
| `svg/coordinate-systems` | 0.0 | 0/39 | 100.0 | 54/54 | 100.0 | 100.0 |  |
| `svg/embedded` | 0.0 | 0/2 | 100.0 | 6/6 | 100.0 | 100.0 |  |
| `svg/extensibility` | 60.0 | 3/5 | 85.7 | 6/7 | 85.7 | 25.7 |  |
| `svg/fonts` | 0.0 | 0/3 | 100.0 | 3/3 | 100.0 | 100.0 |  |
| `svg/geometry` | 9.5 | 27/283 | 93.8 | 571/609 | 93.8 | 84.2 |  |
| `svg/import` |  |  |  |  | no firefox data | - |  |
| `svg/interact` | 47.9 | 46/96 | 87.8 | 231/263 | 87.8 | 39.9 |  |
| `svg/linking` | 0.0 | 0/31 | 96.2 | 50/52 | 96.2 | 96.2 |  |
| `svg/painting` | 0.0 | 0/283 | 97.6 | 648/664 | 97.6 | 97.6 |  |
| `svg/path` | 0.0 | 0/208 | 86.1 | 409/475 | 86.1 | 86.1 |  |
| `svg/pservers` | 0.0 | 0/11 | 100.0 | 61/61 | 100.0 | 100.0 |  |
| `svg/render` | 0.0 | 0/1 | 100.0 | 1/1 | 100.0 | 100.0 |  |
| `svg/scripted` | 0.0 | 0/4 | 100.0 | 10/10 | 100.0 | 100.0 |  |
| `svg/shapes` |  |  | 94.4 | 17/18 | 94.4 |  |  |
| `svg/struct` | 12.5 | 5/40 | 74.6 | 47/63 | 74.6 | 62.1 |  |
| `svg/styling` | 8.0 | 4/50 | 96.1 | 391/407 | 96.1 | 88.1 |  |
| `svg/svg-in-svg` | 100.0 | 1/1 |  |  | no firefox data | - |  |
| `svg/text` | 0.0 | 0/39 | 87.4 | 125/143 | 87.4 | 87.4 |  |
| `svg/types` | 10.2 | 58/569 | 98.8 | 854/864 | 98.8 | 88.6 |  |
| `uievents` | 40.0 | 2/5 | 98.8 | 165/167 | 98.8 | 58.8 |  |
| `uievents/click` | 0.0 | 0/3 | 100.0 | 9/9 | 100.0 | 100.0 |  |
| `uievents/constructors` | 0.0 | 0/20 | 100.0 | 20/20 | 100.0 | 100.0 |  |
| `uievents/interface` | 0.0 | 0/5 | 100.0 | 8/8 | 100.0 | 100.0 |  |
| `uievents/keyboard` | 100.0 | 1/1 | 100.0 | 17/17 | 100.0 | done |  |
| `uievents/legacy` | 0.0 | 0/4 | 100.0 | 4/4 | 100.0 | 100.0 |  |
| `uievents/legacy-domevents-tests` | 100.0 | 3/3 | 100.0 | 5/5 | 100.0 | done |  |
| `uievents/mouse` | 1.4 | 1/71 | 95.4 | 145/152 | 95.4 | 94.0 |  |
| `uievents/order-of-events` | 0.0 | 0/6 | 100.0 | 17/17 | 100.0 | 100.0 |  |
| `uievents/textInput` | 0.0 | 0/24 | 100.0 | 24/24 | 100.0 | 100.0 |  |
| `upgrade-insecure-requests` | 0.0 | 0/8 | 62.5 | 5/8 | 62.5 | 62.5 |  |
| `upgrade-insecure-requests/gen` | 0.0 | 0/992 | 52.0 | 516/992 | 52.0 | 52.0 |  |
| `url` | 97.9 | 9696/9909 | 89.9 | 13867/15420 | 89.9 | done |  |
| `user-timing` | 35.6 | 64/180 | 100.0 | 862/862 | 100.0 | 64.4 |  |
| `web-animations` | 0.0 | 0/1 | 73.0 | 168/230 | 73.0 | 73.0 |  |
| `web-animations/animation-model` | 13.7 | 20/146 | 99.4 | 2617/2632 | 99.4 | 85.7 |  |
| `web-animations/animation-trigger` | 0.0 | 0/3 | 0.0 | 0/3 | 0.0 | done |  |
| `web-animations/interfaces` | 0.8 | 5/663 | 97.5 | 834/855 | 97.5 | 96.8 |  |
| `web-animations/responsive` | 4.0 | 4/99 | 75.8 | 75/99 | 75.8 | 71.7 |  |
| `web-animations/timing-model` | 1.5 | 6/394 | 99.5 | 397/399 | 99.5 | 98.0 |  |
| `webmessaging` | 35.3 | 12/34 | 96.6 | 86/89 | 96.6 | 61.3 |  |
| `webmessaging/broadcastchannel` | 38.2 | 13/34 | 100.0 | 72/72 | 100.0 | 61.8 |  |
| `webmessaging/message-channels` | 50.0 | 8/16 | 74.4 | 29/39 | 74.4 | 24.4 |  |
| `webmessaging/multi-globals` | 0.0 | 0/4 | 100.0 | 4/4 | 100.0 | 100.0 |  |
| `webmessaging/with-options` | 0.0 | 0/2 | 100.0 | 9/9 | 100.0 | 100.0 |  |
| `webmessaging/with-ports` | 5.9 | 1/17 | 100.0 | 39/39 | 100.0 | 94.1 |  |
| `webmessaging/without-ports` | 13.3 | 2/15 | 100.0 | 43/43 | 100.0 | 86.7 |  |
| `websockets` | 63.5 | 167/263 | 98.7 | 1087/1101 | 98.7 | 35.2 |  |
| `websockets/binary` | 0.0 | 0/3 | 100.0 | 12/12 | 100.0 | 100.0 |  |
| `websockets/closing-handshake` |  |  | 100.0 | 9/9 | 100.0 |  |  |
| `websockets/constructor` | 3.4 | 6/175 | 98.6 | 544/552 | 98.6 | 95.1 |  |
| `websockets/cookies` | 0.0 | 0/3 | 95.2 | 20/21 | 95.2 | 95.2 |  |
| `websockets/interfaces` | 18.6 | 22/118 | 100.0 | 291/291 | 100.0 | 81.4 |  |
| `websockets/keeping-connection-open` | 0.0 | 0/1 | 100.0 | 3/3 | 100.0 | 100.0 |  |
| `websockets/multi-globals` | 0.0 | 0/3 | 100.0 | 3/3 | 100.0 | 100.0 |  |
| `websockets/opening-handshake` | 0.0 | 0/4 | 100.0 | 14/14 | 100.0 | 100.0 |  |
| `websockets/security` | 0.0 | 0/1 | 100.0 | 4/4 | 100.0 | 100.0 |  |
| `websockets/stream` | 0.0 | 0/159 | 0.0 | 0/636 | 0.0 | done |  |
| `websockets/unload-a-document` | 0.0 | 0/4 | 63.6 | 7/11 | 63.6 | 63.6 |  |
| `webstorage` | 92.5 | 1164/1259 | 99.4 | 1283/1291 | 99.4 | 6.9 |  |
| `workers` | 3.2 | 4/124 | 95.1 | 427/449 | 95.1 | 91.9 |  |
| `workers/baseurl` |  |  | 100.0 | 7/7 | 100.0 |  |  |
| `workers/constructors` | 0.0 | 0/41 | 100.0 | 146/146 | 100.0 | 100.0 |  |
| `workers/examples` |  |  | 100.0 | 10/10 | 100.0 |  |  |
| `workers/interfaces` | 0.0 | 0/11 | 99.1 | 213/215 | 99.1 | 99.1 |  |
| `workers/modules` | 0.7 | 1/139 | 95.4 | 226/237 | 95.4 | 94.6 |  |
| `workers/multi-globals` | 0.0 | 0/1 | 100.0 | 1/1 | 100.0 | 100.0 |  |
| `workers/same-site-cookies` | 0.0 | 0/6 | 33.3 | 2/6 | 33.3 | 33.3 |  |
| `workers/semantics` | 5.9 | 1/17 | 99.6 | 519/521 | 99.6 | 93.7 |  |
| `xhr` | 6.6 | 69/1049 | 98.3 | 2113/2150 | 98.3 | 91.7 |  |
| `xhr/formdata` | 0.0 | 0/71 | 100.0 | 119/119 | 100.0 | 100.0 |  |

Aggregate (do not quote): us 107394/481764 (22.3%), Firefox 1603521/1636915 (98.0%)

