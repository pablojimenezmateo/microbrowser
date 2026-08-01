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

Not yet wired (`-DMICROBROWSER_FUZZ=ON` exists; there are no targets, because there are no parsers).
The rule for when there are: **every parser that touches network bytes gets a libFuzzer target on
the same commit it lands.** HTML tokenizer, CSS parser, URL parser, HTTP response reader, content
decoders, image decoders, filter-list parser. Corpora under `tests/fuzz/corpora/`.

Fuzz-target link breaks are silent — no default build flow compiles them — so check the fuzz build
after touching shared code.

## Current Coverage

76 tests. Honest about what they do and do not cover:

- `Geometry`, `Canvas`, `DirtyRegion`, `DisplayList` — well covered, including degenerate inputs.
- `Ipc` — every message round-trips; truncation, trailing bytes, unknown tags, version mismatch, and
  a hostile length prefix are all rejected.
- `IdleWaitStrategy`, `DirtyRegionPolicy` — the policy functions are pure, so coverage is thorough.
- `ArchitectureInvariants` — nine rules, each with controls.
- `ReferenceImage` — the harness is tested; there are no goldens yet, because there is nothing
  interesting to render until M1.

Not covered: `Application`'s loop body, `SdlWindow`, and `SdlPresenter` have no automated tests —
they need a window system. The pure policy was deliberately extracted out of them so that what
remains is thin glue, but "thin" is not "zero", and this is a real gap. A headless present path
would close it and is worth doing before the loop grows.
