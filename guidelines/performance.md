# Performance Guide

## Quick Scan

- Correctness, security, and privacy come first; speed is the main optimization target after them.
  A security fix that costs performance is right. Say so in the commit.
- CPU before memory, especially idle CPU and the redraw path.
- **Idle CPU is zero.** This is the single most fragile property in the project.
- Measure before and after. The counters and scopes exist so you do not have to guess.
- Prefer deleting redundant work over adding speculative caching.
- **Microbenchmarks go in `bench/`, run under the perf preset, and report the minimum of several
  rounds.** A scratch program that means fifty iterations of a debug build is not a measurement, and
  `docs/performance/m1-rasterizer.md` records what that cost the first time.

## The Zero-Idle-CPU Invariant

A browser sitting on a static page should be indistinguishable from a sleeping process. Most are
not, and the reasons are always incremental: a 60 Hz compositor tick, an animation timer that never
stops, a caret blink, a poll for network progress, a "just check if anything changed" heartbeat.
Each costs a fraction of a percent. Together they are why a browser with ten idle tabs drains a
laptop battery.

The rule here is that **the process sleeps in exactly one place**: the platform event wait, in
`platform::SdlWindow`. Everything else follows:

- `app::IdleWaitStrategy` decides *how* to wait, as a pure function of `IdleWaitState`. It blocks
  indefinitely unless there is a specific, named reason not to.
- Work that must happen later does not get a poll loop. It sets `IdleWaitState::next_deadline_ms`,
  and the loop sleeps exactly that long.
- Work that is waiting on *something else* does not get a poll loop either. It hands the loop a
  `util::WaitDescriptor` and the wait watches it, which is how ADR 0011 made loading asynchronous
  without making it a poll. A browser with nothing outstanding hands over no descriptors and the
  wait is exactly what it was before.
- VSync is off. A frame is presented because something changed, never because the display refreshed.
- SDL is initialized with `SDL_INIT_VIDEO` only. A browser that has not been asked to play media has
  no business opening the sound device.

`IdleWaitStrategyTests` pins the policy — in particular that a fully idle state chooses `Wait` and
that a zero deadline still yields one sleep rather than a spin. If that test starts failing in the
"must block" direction, something is spinning.

**Any feature that wants a wakeup must justify it.** CSS animations, `setTimeout`,
`requestAnimationFrame` and a blinking caret are all legitimate; each must arrive as a deadline,
must stop when it is no longer needed, and must not wake the loop while the tab is not visible.
`bindings::AnimationFrames` is the worked example of "stops when it is no longer needed": a page
with no pending frame schedules no frame, which is the line ADR 0011 drew and where a browser
normally starts burning a core on an idle page.

## When To Measure

Collect evidence when work touches:

- startup or first paint
- redraw invalidation, damage tracking, or display-list construction
- rasterization, text shaping, or glyph caching
- HTML/CSS parsing, style resolution, layout, or paint
- network transfer, decompression, or the filter engine
- anything that adds a thread, a timer, or a background task

## Tools

### Microbenchmarks

```bash
cmake --preset microbrowser-perf
cmake --build --preset microbrowser-perf --target microbrowser_bench
./build/microbrowser-perf/microbrowser/microbrowser_bench          # or a substring filter
```

Two rules the harness enforces rather than documents, both learned from getting them wrong while
measuring the span blitter:

- **It refuses to print timings from a build without `NDEBUG`.** The default build directory has no
  `CMAKE_BUILD_TYPE`, so it has no optimization flags; timings from it were forty times too slow and
  the ratio they implied was wrong. A printed warning gets pasted into a document without the
  warning, so this is an exit code instead.
- **It reports the minimum of several rounds after a warm-up, not the mean.** Every source of noise
  on a shared machine makes a run slower and none makes it faster, so the minimum is the closest
  available estimate of the work itself. The same code measured by mean-of-fifty and by
  minimum-of-five differed by 60% in the reported speedup.

When a benchmark's point is a comparison, register *both* implementations, run them against the same
fixture in the same process, and keep the loser in the report. A ratio between two numbers produced
by two different harnesses on two different days is not a ratio.

### Live instrumentation

All off by default:

```bash
# Ranked scope table at exit. The tool to reach for first.
MICROBROWSER_PERF_SUMMARY=1 ./build/microbrowser/microbrowser

# Free-running event counters. Answers "how many times did this actually run?"
MICROBROWSER_PERF_COUNTERS=1 ./build/microbrowser/microbrowser

# Per-frame redraw decisions: full vs partial, rect count, coverage.
MICROBROWSER_TRACE_REDRAW=1 ./build/microbrowser/microbrowser

# Startup phases.
MICROBROWSER_STARTUP_SUMMARY=1 ./build/microbrowser/microbrowser

# Streaming per-scope lines. A firehose, and it distorts what it measures --
# the write and flush happen inside the parent scope. Use the summary instead
# unless you specifically need ordering.
MICROBROWSER_PERF_TRACE=1 MICROBROWSER_PERF_TRACE_MIN_MS=0.5 ./build/microbrowser/microbrowser
```

**Read the main-thread column of the summary first.** Self time ranks CPU cost; main-thread time
ranks what the user actually waits on, and they routinely disagree. In microide, a background tree
walk and twenty git subprocesses were the top two rows of a startup profile and cost zero frames
between them — optimizing the top of the self-time table would have been wasted work.

## Adding Instrumentation

- Scopes go through `util::PerformanceTrace::Scope`. Counters go in the X-macro list in
  `util/PerformanceCounters.h` — id and wire name in one row, so an id cannot exist without its name
  or drift out of position.
- Build labels that carry the path or index with `PerformanceTrace::ScopeLabel`, never a hand-rolled
  `if (Enabled())` around string concatenation. A missed guard is a heap allocation per call in
  production.
- **Scope at the granularity the cost actually has.** Per request, per document, per frame, per
  layout pass — not per byte, per glyph, or per DOM node. A counter on a path that runs millions of
  times costs more than it can reveal.
- Label by the stable part, not the operands: `net::Fetch(scheme=https)`, not the full URL. A label
  minted per call blows the 4096-label cap and turns the ranked table back into a log.
- If a subsystem already measures itself in a shape a scope cannot wrap, feed the number in with
  `PerformanceTrace::RecordSampleNs` rather than adding a second measurement.

## Redraw And Interaction Rules

- Typing, scrolling, pointer movement, and resize are latency-sensitive. Treat them accordingly.
- Damage must be explicit and reviewable. `gfx::DirtyRegion` accumulates it;
  `app::DirtyRegionPolicy` decides whether it is worth a partial repaint.
- **Partial repaint is not free.** Each dirty rect is a separate texture upload with its own driver
  round trip, and rasterizing N rects re-walks the display list N times. Past roughly 60% coverage
  or 12 rects, one full upload beats many partial ones. Those thresholds are currently reasoned, not
  measured — replace them with measured values once the perf harness has a scroll scenario.
- Watch for hidden full-frame repaint regressions. `MICROBROWSER_TRACE_REDRAW=1` makes them visible.

## Optimization Heuristics

- Remove redundant work before reaching for a cache.
- Keep caches scoped, explicitly invalidated, and justified by a measured win.
- Push expensive work off the interaction path when that does not compromise correctness.
- Prefer focused data-flow cleanup over cleverness that obscures ownership.
- A correctness fix that costs performance is usually right. Say so in the commit rather than
  quietly trading one for the other.

## Memory

Memory is the lowest optimization priority, but not zero — it is where a browser's reputation is
made. Two rules while the engine is being built:

- **Per-node types carry `static_assert(sizeof(T) <= N)`.** Types allocated per DOM node, layout
  box, display command, or JS value have their size multiplied by the document, so growth is a
  compile error rather than a memory-profile mystery six months later.
- **Measure resident set, not allocations.** See `docs/performance/m0-baseline.md` for the M0
  numbers and for what turned out to dominate them, which was not what it looked like.
