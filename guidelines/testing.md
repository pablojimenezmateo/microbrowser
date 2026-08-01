# Testing Guide

## Quick Scan

- Own harness, no gtest. One registration function per suite, listed in `tests/TestMain.cpp`.
- The suite shards across cores; `ctest -j$(nproc)` runs 24 slices in parallel.
- Pixel reference tests are the highest-value test a browser can have. They work because the
  rasterizer is deterministic software.
- Every architecture rule ships with a clean *and* a dirty fixture. A rule that cannot fail is not
  a rule.
- Every parser that touches network bytes gets a fuzz target on the commit it lands.

## Running

```bash
ctest --test-dir build --output-on-failure -j$(nproc)   # sharded, all cores
./build/microbrowser/microbrowser_tests                 # everything, one process
./build/microbrowser/microbrowser_tests Canvas Ipc      # substring filters
./build/microbrowser/microbrowser_tests --list          # names only
./build/microbrowser/microbrowser_tests --shard-index=3 --shard-count=24
```

Sharding is round-robin rather than contiguous blocks, so shards stay balanced even when one suite
is far slower than the rest. A filter that matches nothing exits non-zero — a typo in a filter or a
suite missing from `CollectTests()` must not report success for zero work.

## Adding A Suite

1. Write `tests/FooTests.cpp` with `void RegisterFooTests(std::vector<TestCase>& tests)`.
2. Declare it and call it in `tests/TestMain.cpp`.
3. Add the file to `microbrowser_tests` in `CMakeLists.txt`.

Explicit registration rather than static-initializer self-registration: the order is deterministic,
the linker cannot silently drop a suite whose only reference is a global constructor, and finding
every test is a grep for one list.

Assertions are `Expect`, `ExpectEqInt`, and `ExpectEqString`. Deliberately few. A good message from
the call site beats a generated one, and a matcher library is a maintenance surface that pays for
itself only at a scale this project will not reach for years.

## What A Good Test Looks Like Here

Name the *property*, not the method. `Canvas/ClipStackOnlyEverShrinks` says what must be true;
`Canvas/TestPushClip` says which function was called and will still pass when the behavior is wrong.

Test the boundary that actually breaks. The interesting Canvas tests are the degenerate ones — a
fill that overhangs every edge, a pop on an empty clip stack, a resize to zero, a negative extent.
The happy path is usually right; the edges are where a rasterizer corrupts memory.

Say why in the message when the reason is not obvious:

```cpp
ExpectEqInt(canvas.Row(0)[5], 0xFF000000,
            "outside the damage rect must be untouched; otherwise a partial repaint is "
            "no cheaper than a full one");
```

## Pixel Reference Tests

The most valuable kind of test a browser can have, and the direct payoff of choosing a deterministic
software rasterizer: identical input produces identical bytes on every machine, so a golden file
means something.

```cpp
const ComparisonResult result = CompareAgainstGolden(canvas, "block/margin-collapse");
Expect(result.matches, result.message);
```

Goldens live in `tests/ref/` as P6 PPM — eight lines of code to write, no library, diffs
meaningfully as binary. On mismatch the harness writes `<name>.actual.ppm` beside the golden so the
failure can be *inspected*, not just described.

**A missing golden fails.** It writes the actual output and tells you to look at it, but it never
creates the golden silently — a silently-created golden records whatever bug was present when the
test was written, and then defends it forever.

As coverage grows, seed the corpus from the Web Platform Tests reftests rather than writing cases
by hand.

## Spec Conformance Suites

The parsers are written against specs, so they get tested against the specs' own suites, wired in as
parameterized cases (one ctest case per fixture file):

| Suite | Covers | Arrives |
|---|---|---|
| `html5lib-tests` | HTML tokenizer + tree construction | M3 |
| `css-parsing-tests` | CSS tokenizer | M4 |
| `urltestdata.json` | WHATWG URL parser | M2 |
| `test262` | JavaScript | M8 |

## Control Fixtures

Every architecture rule is run twice: once over the real tree (must produce no violations) and once
over a hand-written fixture known to violate it (must produce at least one). The suite fails if a
rule has no fixtures.

This is not belt-and-braces. microide shipped a close-on-exec lint whose pattern matched `openat`
but never plain `open(`, so it passed green for months while checking nothing. A lint nobody has
watched fail is indistinguishable from no lint.

It paid off immediately here: on its first run, `ClassFanOut` could not trip its own dirty fixture —
the fixture manifest defined three modules against a threshold of four, so the rule was silently
inert. That is a bug that would otherwise have been found by a god object appearing.

When adding a rule, write the dirty fixture first and watch it fail.

## Sanitizers

```bash
tools/run-checks.sh asan
tools/run-checks.sh ubsan
tools/run-checks.sh tsan
```

Required for anything touching memory, threads, or untrusted input — which, in a browser, is most
things. TSan needs ASLR cleared; `run-checks.sh` wraps it in `setarch -R` automatically. Running
`ctest` on the tsan preset by hand without that fails with "unexpected memory mapping", which is the
environment, not a bug.

## Fuzzing

**Every parser that touches bytes written by a stranger gets a libFuzzer target on the commit it
lands.** Eight exist: `inflate_fuzzer`, `png_fuzzer`, `font_fuzzer`, `http_fuzzer`, `cookie_fuzzer`,
`url_fuzzer`, `html_fuzzer`, `css_fuzzer`. Still to come: the filter-list parser.

Three check a *property* rather than the absence of a crash, and those are the valuable ones.

Two of them check a *property*, not just the absence of a crash, and those are the valuable ones.
`url_fuzzer` requires that serializing a parsed URL and reparsing it yields the same string — a
parser whose output does not reparse to itself is one two components can disagree about, which is
where origin-confusion bugs come from. `cookie_fuzzer` requires that no storable cookie can produce
a `Cookie:` header containing CR or LF. `html_fuzzer` requires that tokenizing *any* input
terminates and reaches EOF, traps if it ever emits more tokens than the input had bytes, and
requires that tree construction yields a document with an html element for every input — HTML has
no failure mode, so a state machine that can loop on malformed markup is a denial of service
reachable by anyone who can serve a page.

```bash
cmake -S . -B build/fuzz -G Ninja -DMICROBROWSER_FUZZ=ON \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/fuzz
./build/fuzz/microbrowser/png_fuzzer tests/fuzz/corpora/png -print_funcs=0
```

`-print_funcs=0` is not optional on a machine without `llvm-symbolizer`: without it libFuzzer stalls
trying to symbolize the first newly-covered function, and the run looks like a hang in the decoder.

**Check the counter count on the first line of output.** It reads like noise and it is the single
most important line:

```
INFO: Loaded 1 modules   (1352 inline 8-bit counters)
```

That number must be in the hundreds or thousands. The first version of this build instrumented only
the fuzz harness and not the libraries it linked, so it reported **8** counters — libFuzzer spent
thirteen million executions exploring the eight branches of its own entry point and reported a clean
run, having never once executed an instrumented parser. It is the same failure the control fixtures
exist to prevent, in a place where nothing was watching for it. `MICROBROWSER_FUZZ` now puts
`-fsanitize=fuzzer-no-link` on `microbrowser_settings`, so instrumentation reaches every module.

It paid for itself immediately: eight seconds into the first correctly instrumented run, the inflate
fuzzer found a stack buffer overflow in the Huffman table builder — the fixed-Huffman alphabet has
288 symbols and the table was sized for the 286 a dynamic block can name. Twelve unit tests covered
that code path and none of them noticed, because the two-byte write landed in stack padding. (An
ASan run of the suite would have caught it too; nobody had run one since the decoder landed, which
is its own lesson.)

Corpora live under `tests/fuzz/corpora/`, minimized with `-merge=1`. Fuzz-target link breaks are
silent — no default build flow compiles them — so check the fuzz build after touching shared code.

## Security Tests

`guidelines/security.md` is the model; these are the tests that hold it up.

**A security fix ships with a test that fails without it.** Not a test that exercises the area — the
specific malformed input, committed as a case or a corpus entry. Otherwise the only record of the
bug is a commit message, and the next refactor reintroduces it.

**Decoders are tested on the inputs that are hostile, not the inputs that are valid.** Truncation
mid-field, a length prefix larger than the remaining bytes, a length prefix that overflows when
multiplied, zero and negative dimensions, and trailing garbage. `IpcMessageTests` already does all
five for the wire format; every parser that lands gets the same treatment, because a decoder tested
only on well-formed input has been tested on the case the attacker will not send.

**Allocation must be bounded before it happens.** The interesting assertion is not "the decode
failed", it is "the decode failed *without* attempting a 4 GiB reserve". That needs the allocation
counting that `MICROBROWSER_PERF_HARNESS_BUILD` does not yet arm; until it does, the bound is
asserted by reading the code, and that is a real gap rather than a decision.

**Sanitizers are a gate, not a chore.** ASan and UBSan for anything touching memory or untrusted
input; TSan for anything touching threads. They are the only automated thing standing between a
memory-safety bug and a shipped exploit until the sandbox exists.

## Current Coverage

390 tests. Honest about what they do and do not cover:

- `Geometry`, `Canvas`, `DirtyRegion`, `DisplayList` — well covered, including degenerate inputs.
- `Path`, `PathFlattener`, `Rasterizer`, `Stroker`, `AffineTransform` — the strongest assertions in
  the tree are here, and they are analytic rather than pictorial: a triangle and a circle must cover
  their closed-form area, a half-covered pixel must read exactly 128, a butt-capped line must cover
  length times width, a mitered rectangle outline must equal the difference of two rectangles.
  Shapes are also checked where the two fill rules disagree, which only happens where a path
  overlaps itself — a suite that draws only rectangles passes with the rule ignored entirely.
- `Blitter` — the vector path is compared against the scalar reference across all 256 source alphas,
  every tail length, and eight start offsets, plus an assertion that a vector path is compiled in at
  all, without which the comparison would be scalar against itself.
- `PaintPipeline` — engine to display list to wire to pixels, with a golden. Every link in that
  chain is unit-tested; a chain of tested links is not a tested chain.
- `Font`, `TextShaper`, `GlyphCache` — against a TrueType font the harness builds in memory, whose
  glyphs have closed-form areas. Covers the y-flip out of font space, the cmap, contour direction
  (one glyph is a square with a reversed inner square, so a decoder that ignored winding would come
  back with the wrong area), quadratic contours, and clusters through multi-byte UTF-8.
- `Url`, `Origin`, `PublicSuffix`, `Site`, `PartitionKey` — the WHATWG standard's own cases, plus
  the four spellings of a loopback address, opaque-origin identity, and PSL wildcards and
  exceptions. `PartitionKey` covers Total Cookie Protection directly: one third party under two
  top-level sites must produce two keys.
- `Blocking`, `Policy` — filter matching including exceptions and `$important`, and the policy's
  *ordering* (block before upgrade, sanitize after, referrer from the final URL).
- `Http`, `Cookie` — framing ambiguity is the theme: `Content-Length` with `Transfer-Encoding`,
  duplicate lengths, obsolete line folding. Cookie tests cover the label-boundary rule that stops
  `notevil.com` matching `evil.com`, and partitioning at the storage layer.
- `Tokenizer`, `TreeBuilder` — the HTML suites. The tokenizer's cases are the ones that are
  normative rather than tidy: first-of-a-duplicate-attribute wins, an unterminated reference inside
  an attribute is not expanded (`?x&copy=1` must survive), RCDATA ends only on its own end tag. The
  tree builder's are the four behaviours that separate a parser from a tree builder — implied
  elements, implied end tags, void elements never entering the open-element stack, and an end tag
  for something not open being dropped.
- `Inflate`, `PngDecoder` — the hostile-input suites. Reference streams produced by zlib itself
  rather than by this project, so the decoder is checked against an independent implementation of
  the format; then truncation at every length, a flipped byte at every third offset, and every way
  a header can lie about its own dimensions. PNG fixtures use *stored* DEFLATE blocks so a failure
  points at one decoder rather than two.
- `Ipc` — every message round-trips; truncation, trailing bytes, unknown tags, version mismatch, and
  a hostile length prefix are all rejected.
- `IdleWaitStrategy`, `DirtyRegionPolicy` — the policy functions are pure, so coverage is thorough.
- `ArchitectureInvariants` — twelve rules, each with controls. The newest three
  (`NoBannedCFunctions`, `NoManualHeapOwnership`, `EnvironmentReadsAreCentralized`) carry clean
  fixtures built out of near misses rather than obviously-fine code: `snprintf`, `pool_.Free()`,
  `= delete`, placement new. A banned-name lint dies from false positives, so the cases that would
  produce them are the ones worth pinning. `EnvironmentReadsAreCentralized` additionally has a
  fixture for its own vacuity — a tree where the file it exempts has been renamed away, which
  without the check would pass while matching nothing.
- `Env` — the flag parser, including that unset and empty mean the same thing.
- `AppDirectories` — profile relocation and permissions. First coverage of `src/platform`.
- `ReferenceImage` — the harness, plus five goldens: a circle, a rounded rectangle with four
  different radii, a star rendered under both fill rules, a sheet of joins and caps, and a frame the
  engine actually produced.

Not covered: `Application`'s loop body, `SdlWindow`, and `SdlPresenter` have no automated tests —
they need a window system. `AppDirectories` is now covered because it deliberately takes its paths
from the environment, which is the general shape of the fix: the part of a platform class that has
no window in it can usually be reached if it was written to be given its inputs. The pure policy was deliberately extracted out of them so that what
remains is thin glue, but "thin" is not "zero", and this is a real gap. A headless present path
would close it and is worth doing before the loop grows.
