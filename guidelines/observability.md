# Observability Guide

How to see what the browser is doing, and how to add new visibility without paying for it in
production.

## Principles

**Everything is off by default and costs one predictable branch when off.** A trace scope on the
disabled path does not copy its label, does not allocate, and does not touch anything but a
relaxed atomic load. That is what makes it acceptable to leave instrumentation in hot code
permanently, which in turn is what makes it available when you need it — instrumentation you have to
add before you can debug is instrumentation you do not have.

**Counters stay armed even in release.** One relaxed atomic add is cheap enough that the answer to
"how many times did this actually run in a real session?" is always available. A sampling profiler
tells you what was hot; a counter tells you what happened.

**Observability is not telemetry.** Nothing here leaves the machine. There is no reporting endpoint,
no opt-out, and no aggregation service — output goes to stderr, on request, in this process. That is
a privacy invariant, not a missing feature. See `guidelines/privacy.md`.

## The Three Mechanisms

### Event counters — "how many times?"

Free-running process-wide counters, declared in one X-macro list in `util/PerformanceCounters.h`:

```cpp
X(FramesPresented, "frame.presented")
X(GfxFillRectPixels, "gfx.fill_rect_pixels")
```

The id and its wire name live in the same row on purpose. They used to be two parallel lists in
microide, so inserting an id without inserting its name at the same position still compiled and
silently relabelled every counter after that point, attributing one subsystem's numbers to another.

Naming: `<subsystem>.<event>`. A counter ending in a plural noun counts that noun (pixels, bytes,
rects); everything else counts calls or events.

```bash
MICROBROWSER_PERF_COUNTERS=1 ./build/microbrowser/microbrowser
```

Only non-zero counters print, sorted by name.

### Trace scopes — "how long, and where did it go?"

```cpp
util::PerformanceTrace::Scope scope("engine::Paint");
```

Two independent output modes:

- `MICROBROWSER_PERF_SUMMARY=1` — per-label count / total / self / max / main-thread-self, ranked by
  self time, printed once at exit. **This is the one to use when hunting a hotspot.**
- `MICROBROWSER_PERF_TRACE=1` — one stderr line per scope, indented by nesting depth, filtered by
  `MICROBROWSER_PERF_TRACE_MIN_MS`. A firehose, and the write plus flush happens inside the parent
  scope, so streaming a hot inner scope distorts exactly the number you are reading.

`MICROBROWSER_STARTUP_TRACE` / `MICROBROWSER_STARTUP_SUMMARY` are the same mechanism for startup.

### Targeted flags — "show me this specific decision"

```bash
MICROBROWSER_TRACE_REDRAW=1   # [redraw] full  rects=1 coverage=100.0% surface=1280x800 commands=29
```

Read through `util::PerformanceTrace::FlagEnabled`, cached in a function-local static so the env
read happens once and the class does not grow a member for a debugging flag.

## Reading A Summary

The table has two rankings. **Read the main-thread one first.**

Self time ranks CPU cost. Main-thread time ranks what the user waits on. They disagree constantly,
and the disagreement is the point: a background parse that burns 200 ms of CPU while the UI thread
sleeps costs the user nothing, and a 30 ms style recalc on the main thread drops two frames. A tool
built to find UI stalls that ranks by self time will confidently point at the wrong row.

`MarkTracingMainThread()` is called first thing in `main()`. Without it the main-thread column is
meaningless, and the summary says so rather than printing zeros.

## Adding Instrumentation Well

**Scope at the granularity the cost actually has.** Per request, per document, per frame, per layout
pass — not per byte, per glyph, or per DOM node. A scope on a path that runs a million times costs
more than it reveals. When the layout engine lands, `LayoutBlock` gets a scope and
`MeasureTextRun` does not.

**Label by the stable part, not the operands.**

```cpp
// Good: one label, aggregates meaningfully.
util::PerformanceTrace::Scope scope("net::Fetch(scheme=https)");

// Bad: a new label per call. Blows the 4096-label cap and turns the ranked
// table back into a log.
util::PerformanceTrace::Scope scope("net::Fetch(" + url + ")");
```

**Build dynamic labels with `ScopeLabel`,** which does the string work only when the channel is on:

```cpp
util::PerformanceTrace::ScopeLabel label("css::ParseStylesheet");
label.Field("bytes", static_cast<long long>(source.size()));
util::PerformanceTrace::Scope scope(label.View());
```

**Do not add a second measurement of something already measured.** If a subsystem times itself in a
shape a scope cannot wrap, feed the number in with `RecordSampleNs` so it lands in the same table.

## What Is Missing

Honest gaps, so they are not mistaken for decisions:

- **No perf harness yet.** There is no scenario runner and no recorded baselines, so "did this
  regress?" is currently answered by hand. `microbrowser_perf` is wired into the build but has no
  scenarios. It needs them before M5 layout work starts, or layout will be optimized blind.
- **No allocation counting.** `MICROBROWSER_PERF_HARNESS_BUILD` exists as a build option but arms
  nothing. The counting `operator new` that lets a test assert "this path must not allocate" is a
  microide feature not yet ported; it wants to exist before the paint hot path does.
- **No memory instrumentation.** Resident set is measured externally (`/proc/self/status`), not
  reported by the process. Per-subsystem attribution will matter once there is a DOM to attribute.
