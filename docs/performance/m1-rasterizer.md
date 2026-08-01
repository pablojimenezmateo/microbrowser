# M1 Rasterizer Measurements

Measured 2026-08-01. Release build, GCC 13.3, 24-core x86-64.

```bash
cmake --preset microbrowser-perf
cmake --build --preset microbrowser-perf --target microbrowser_bench
./build/microbrowser-perf/microbrowser/microbrowser_bench          # or a name filter
```

Internal regression baselines. Not benchmarks, and not comparable to other browsers.

## Two things about measuring, learned the expensive way

**Measure the build you ship.** The first pass at these numbers was taken against the default build
directory, which has no `CMAKE_BUILD_TYPE` and therefore no optimization flags. It reported the
scalar blend at 11.6 ns/px and the vector one at 4.5 ns/px — both roughly forty times too slow, and
the ratio between them wrong. `microbrowser_bench` now refuses to print a table without `NDEBUG`,
because a warning above a table gets pasted into a document without the warning.

**A throwaway harness is not a measurement.** The second pass used a scratch program that took the
mean of fifty iterations, and reported the span blitter 4.0x faster than scalar. The committed
harness, which takes the minimum of five rounds after a warm-up, reports 2.5x for the same code on
the same machine. The mean was measuring the machine's mood; every source of noise makes a run
slower and none makes it faster, so the minimum is the closest available thing to the cost of the
work. **The 4.0x figure in the blitter's commit message is wrong. 2.5x is the number.**

## Where the time goes

A 1280x800 surface, forty full-width translucent rounded boxes — the shape the engine actually
emits, and roughly the shape a page of text blocks has:

```
rasterize only (path -> coverage spans)     0.038 ms
rasterize + scalar blend                    0.788 ms
rasterize + SSE2 blend                      0.237 ms
```

**Before the vector blitter, blending was 95% of the frame and rasterization 5%.** That ratio is the
only reason `gfx/Blitter.cpp` exists. Going in, the expectation was that curve flattening and the
cell sort would dominate; the expectation was wrong by an order of magnitude.

The cell sort in particular is **not** hot. The page above produces 2400 cells and a 760px circle
3150; `std::sort` over a few thousand 16-byte structs is not measurable next to half a million
blended pixels. The per-row cell lists `ftgrays` uses to avoid that sort are a real optimization for
a real workload, and this is not yet that workload. Recorded so the next person does not spend a day
there without measuring first.

## Span blending

One million pixels, source-over:

```
blit/span-srcover-vector            0.238 ns/px
blit/span-srcover-scalar            0.593 ns/px      2.5x
blit/span-srcover-opaque-source     0.051 ns/px      short-circuits to a store
```

In the page workload the same change is worth **3.3x** (0.788 -> 0.237 ms), more than the isolated
2.5x, because page spans are short and stay in cache while the 4 MiB isolated buffer is partly
memory-bound. Both numbers are real; they answer different questions, and the in-page one is the one
a user would feel.

The scalar loop is not auto-vectorized at `-O3`. The vector version rests on the accumulated 16-bit
intermediates never exceeding 65535, which is what allows four pixels per step, and the compiler has
no way to establish that from the scalar source.

The vector path is bit-identical to the scalar one, not merely close: `BlitterTests` compares them
across all 256 source alphas, every tail length from 0 to 17, and eight start offsets, and asserts
that a vector path is actually compiled in on x86-64 so the comparison cannot pass by comparing
scalar against itself.

## Text

A screenful of body text — forty lines of forty glyphs at 16px:

```
text/shape-page                      0.053 ms      33 ns/glyph
text/draw-page (no cache)            1.771 ms    1107 ns/glyph
text/draw-page (glyph cache)         0.352 ms     220 ns/glyph      5.0x
text/glyph-outline                                 209 ns/glyph
```

Shaping is not the problem and never was: 33 ns per glyph, six percent of the frame. Drawing was,
and of the 1107 ns each uncached glyph cost, only 209 was extracting the outline from FreeType — the
rest was re-rasterizing the same handful of shapes tens of thousands of times.

The cache is keyed on face, size, glyph, hinting **and sub-pixel position**. That last one is not
optional: text is positioned in fractional pixels, so a cache keyed without it would return a mask
rendered at the wrong fraction and silently undo sub-pixel positioning. Four horizontal positions is
the usual compromise — a quarter pixel of error is below what 8-bit coverage can express, and it
costs four entries per glyph rather than the hundreds a continuous key would.

**This benchmark is a worst case for the mask blitter, and deliberately so.** The synthetic test
font's glyphs are solid blocks, roughly 100% ink; real glyphs are nearer 15%, and the blitter skips
zero-coverage pixels. Real text should be substantially faster than 220 ns/glyph, so that number is
a ceiling rather than an estimate. It is also why the mask blitter is still scalar: vectorizing it
against a workload this unrepresentative would be optimizing for a font nobody has.

## Path fills and strokes

```
raster/circle-760px                 0.062 ms     0.137 ns/px      3150 cells, 3590 spans
paint/circle-760px-opaque           0.116 ms     0.251 ns/px
paint/circle-760px-translucent      0.196 ms     0.426 ns/px
paint/circle-760px-stroked          0.600 ms                      width 6, round joins
```

Coverage generation for a 453,000-pixel circle costs 62 microseconds. Flattening at the 0.1px
tolerance produces 32 segments for a full circle at this radius.

A stroke costs about ten times its fill. That is expected — the stroker emits a quad per segment
plus a disc per join, so a 32-segment circle becomes a path of 64 pieces — and it is the next thing
worth attacking if strokes ever show up in a profile of a real page. They do not yet, because there
are no real pages.
