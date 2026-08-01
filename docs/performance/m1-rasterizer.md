# M1 Rasterizer Measurements

Measured 2026-08-01. Release build (`--preset microbrowser-perf`, `-O3 -DNDEBUG`), GCC 13.3,
24-core x86-64.

Internal regression baselines. Not benchmarks, and not comparable to other browsers.

## Read this first: measure the build you ship

The first pass at these numbers was taken against the default build directory, which has no
`CMAKE_BUILD_TYPE` and therefore **no optimization flags at all**. It reported the scalar blend at
11.6 ns/px and the vector one at 4.5 ns/px — a 2.6x speedup, and both figures roughly forty times
too slow. In a release build the same comparison is 0.83 vs 0.21 ns/px.

The conclusion happened to survive; the numbers did not, and the ratio changed by half. Use
`cmake --preset microbrowser-perf` for anything that will be written down.

## Where the time goes

Rasterizing and painting forty translucent rounded boxes into a 1280x800 surface — roughly the shape
of a page of text blocks:

```
rasterize only (path -> coverage spans)     0.061 ms
+ scalar blend                              0.772 ms
+ SSE2 blend                                0.233 ms
```

**Blending was 92% of the frame, and rasterization 8%.** This was measured before the blitter was
written, and it is the whole reason the blitter exists: the intuition going in was that curve
flattening and the cell sort would dominate, and the intuition was wrong by an order of magnitude.

The cell sort in particular is *not* hot. The page above produces 2400 cells and a 760px circle
produces 3150; `std::sort` over a few thousand 16-byte structs is not measurable next to half a
million blended pixels. The per-row cell lists that `ftgrays` uses to avoid that sort are a real
optimization for a real workload, and this is not yet that workload. Recorded here so the next
person does not spend a day on it without measuring first.

## Span blending

Isolated, one million pixels, source-over with a translucent source:

```
scalar (Color.h reference)      0.83 ns/px
SSE2 (four pixels per step)     0.21 ns/px      4.0x
```

The scalar loop is not auto-vectorized at `-O3`: the compiler cannot prove the rounding identity
`(t + (t >> 8)) >> 8` stays in 16-bit lanes, which is the fact the hand-written version is built on.

The vector path is bit-identical to the scalar one, not merely close — `BlitterTests` compares them
across all 256 source alphas, every tail length from 0 to 17, and eight start offsets, and asserts
that the vector path is actually compiled in on x86-64 so the comparison cannot pass by comparing
scalar against itself.

## Path fills

```
one 760px circle, coverage spans only       0.063 ms      3150 cells, 3590 spans
same, opaque fill (memset-shaped)           0.116 ms
same, translucent fill (SSE2 blend)         0.196 ms
```

Coverage generation for a 453,000-pixel circle costs 63 microseconds. Curve flattening at the 0.1px
tolerance produces 32 segments per full circle at this radius.

## Reproducing

There is no committed benchmark binary yet — these came from a throwaway harness against the release
static libraries. That is a gap: a number nobody can reproduce is a number that will not be
re-measured. A committed `microbrowser_bench` target is the obvious next piece of instrumentation,
and it should land before the next optimization does.
