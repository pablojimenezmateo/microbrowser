# The WPT baseline

**Generated**, by `microbrowser_wpt --summary docs/wpt-baseline.md`. Do not edit it: the
next run overwrites it, and that overwrite is the point -- the diff of this file is
what a session moved. The argument for the instrument is `docs/adr/0040`; the work it
sequences is `docs/wpt-plan.md`.

WPT revision: `4120ac0deb573634d8b7cd74c38ae9d647eebdb5`

9 of 1960 subtests pass (0.5%) over 25 tests.

**Do not quote that number.** Subtests are not comparable across areas: `mimesniff/mime-types` alone is 99% of every subtest here, because a suite that tests one index table per code point
counts differently from one that tests an algorithm. The per-area column is the
measurement; the aggregate is an artefact of how the suite is written.

A test with no subtests at all -- a reftest, or a testharness page whose harness
died before it ran anything -- contributes nothing to that percentage, so the
harness columns below are the ones to read first. A `TIMEOUT` is not a slow test;
it is a page that never reported, which almost always means something threw before
`done()`.

## Per area

| area | tests | ok | error | timeout | crash | subtests | passed | % |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| `hr-time` | 15 | 8 | 0 | 7 | 0 | 13 | 4 | 30.8 |
| `mimesniff/media` | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0.0 |
| `mimesniff/mime-types` | 3 | 1 | 0 | 2 | 0 | 1939 | 2 | 0.1 |
| `mimesniff/sniffing` | 3 | 3 | 0 | 0 | 0 | 7 | 3 | 42.9 |
| `png` | 3 | 0 | 0 | 3 | 0 | 1 | 0 | 0.0 |

## Why the harness never reported

Ranked by tests affected. One line here is worth more than a page of the table
above: a test whose harness failed reports *no* subtests, so these are invisible in
the pass rate and are the largest block of unrealised coverage in the suite.

| tests | cause | example |
|--:|---|---|
| 10 | TIMEOUT: the page never reported | `hr-time/basic.any.worker.html` |
| 2 | TIMEOUT:  | `mimesniff/mime-types/charset-parameter.window.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: DocumentTimeline is not defined ReferenceError: DocumentTimel... | `hr-time/raf-coarsened-time.https.html` |

## Why subtests fail

Ranked by *distinct tests* affected rather than by subtests, because that is the
number a fix unblocks. Digits are collapsed to `N`; quoted values are not, because
`expected "block" but got "inline"` and `expected "Npx" but got "Npx"` are
different bugs and a bucket labelled `assert_equals` is not actionable.

| tests | subtests | message | example |
|--:|--:|---|---|
| 1 | 1 | NOTRUN (no message) | `png/exif-chunk.html` |
| 1 | 40 | Test timed out | `mimesniff/mime-types/charset-parameter.window.html` |
| 1 | 1 | assert_equals: Blob expected (string) "!#$%&'*+-.^_'\|~Nabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz/!#$%&'*+-.^_'\|~Nabcdefghijklmnop... | `mimesniff/mime-types/parsing.any.html` |
| 1 | 376 | assert_equals: Blob expected (string) "" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "N/N" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 5 | assert_equals: Blob expected (string) "text/html" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;N=x;charset=gbk" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;c=bar" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset='" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset='gbk" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset='gbk'" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 3 | assert_equals: Blob expected (string) "text/html;charset=GBK" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset=\" \\\"gbk\\\"\"" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 3 | assert_equals: Blob expected (string) "text/html;charset=\" gbk\"" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset=\"()\"" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset=\";charset=GBK\"" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset=\"\"" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset=\"gbk \"" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset=\"gbk(\"" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset=\"gbk\\\"\"" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset=\"{gbk}\"" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 14 | assert_equals: Blob expected (string) "text/html;charset=gbk" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;charset=gbk'" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;foo=bar" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;test=\"U+ff\";charset=gbk" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;valid=\";\";foo=bar" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "text/html;x=\"(\";charset=gbk" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 3 | assert_equals: Blob expected (string) "x/x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 241 | assert_equals: Blob expected (string) "x/x;bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "x/x;test=\"\\\\\"" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"(\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\")\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\",\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"/\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\":\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"<\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"=\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\">\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"?\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"@\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 40 | assert_equals: Blob expected (string) "x/x;x=\"U+N\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 4 | assert_equals: Blob expected (string) "x/x;x=\"U+Na\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 4 | assert_equals: Blob expected (string) "x/x;x=\"U+Nb\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 4 | assert_equals: Blob expected (string) "x/x;x=\"U+Nc\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 4 | assert_equals: Blob expected (string) "x/x;x=\"U+Nd\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 4 | assert_equals: Blob expected (string) "x/x;x=\"U+Ne\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 4 | assert_equals: Blob expected (string) "x/x;x=\"U+Nf\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 20 | assert_equals: Blob expected (string) "x/x;x=\"U+aN\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+aa\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ab\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ac\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ad\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ae\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+af\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 20 | assert_equals: Blob expected (string) "x/x;x=\"U+bN\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ba\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+bb\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+bc\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+bd\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+be\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+bf\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 20 | assert_equals: Blob expected (string) "x/x;x=\"U+cN\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ca\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+cb\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+cc\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+cd\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ce\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+cf\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 20 | assert_equals: Blob expected (string) "x/x;x=\"U+dN\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+da\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+db\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+dc\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+dd\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+de\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+df\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 20 | assert_equals: Blob expected (string) "x/x;x=\"U+eN\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ea\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+eb\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ec\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ed\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ee\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ef\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 20 | assert_equals: Blob expected (string) "x/x;x=\"U+fN\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+fa\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+fb\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+fc\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+fd\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+fe\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"U+ff\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"[\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Blob expected (string) "x/x;x=\"\t !\\\"#$%&'()*+,-./N:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_'abcdefghijklmnopqrstuvwxyz{\|}... | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"]\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"{\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 2 | assert_equals: Blob expected (string) "x/x;x=\"}\";bonus=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 4 | assert_equals: Blob expected (string) "x/x;x=x" but got (undefined) undefined | `mimesniff/mime-types/parsing.any.html` |
| 1 | 1 | assert_equals: Document cross-origin isolated value matches expected (boolean) false but got (undefined) undefined | `hr-time/timing-attack.html` |
| 1 | 1 | assert_equals: Document cross-origin isolated value matches expected (boolean) true but got (undefined) undefined | `hr-time/cross-origin-isolated-timing-attack.https.html` |
| 1 | 1 | assert_equals: crossOriginIsolated is properly set expected (boolean) false but got (undefined) undefined | `hr-time/clamped-time-origin.html` |
| 1 | 1 | assert_equals: crossOriginIsolated is properly set expected (boolean) true but got (undefined) undefined | `hr-time/clamped-time-origin-isolated.https.html` |
| 1 | 1 | assert_equals: expected "function" but got "undefined" | `hr-time/performance-tojson.html` |

109 distinct subtest messages and 3 distinct harness messages behind these numbers.
