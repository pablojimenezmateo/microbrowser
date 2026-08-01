# M0 Performance Baseline

Measured 2026-08-01. Debug build, GCC 13.3, 24-core x86-64, Wayland session with Mesa.

These are the first numbers, recorded so later regressions have something to be a regression
against. They are not benchmarks and must not be compared to other browsers.

## Idle CPU — the headline property

```
1280x800 window, one placeholder page loaded, no input for 60 seconds

CPU consumed:  1 clock tick (10 ms) over 60 s  =  0.017%
loop.iterations:      5
loop.blocking_waits:  4
loop.polls:           1
```

Effectively zero, and structurally so: the loop entered `SDL_WaitEvent` four times and stayed there.
The single tick is startup settling inside the measurement window.

**This is the number most likely to be lost by accident.** Every future feature that wants a timer,
an animation, or a background poll must justify itself against it. Re-measure with:

```bash
./build/microbrowser/microbrowser & P=$!; sleep 4
T0=$(awk '{print $14+$15}' /proc/$P/stat); sleep 60
T1=$(awk '{print $14+$15}' /proc/$P/stat)
echo "$(( T1 - T0 )) ticks over 60s"; kill $P
```

## Frame accounting

One navigation into a 1280x800 window, from `MICROBROWSER_PERF_COUNTERS=1`:

```
engine.navigations              1
engine.paints_produced          2     navigate + viewport resize
display_list.builds             2
display_list.commands          58
display_list.executions         1     the two paints coalesced into one frame
frame.presented                 1
frame.full_repaint              1
frame.texture_pixels_uploaded   1024000   = 1280 x 800, one full upload
gfx.fill_rect_calls            27
gfx.fill_rect_pixels      1632640
ipc.messages_sent              10
ipc.messages_received          10
```

Two display lists were built and one frame was presented — the coalescing works. 27 fills touched
1.63 M pixels for a 1.02 M pixel surface, i.e. 1.6x overdraw, which is the background fill plus
overlapping content blocks. Worth watching once real content exists; not worth acting on now.

### Where the frame time actually goes

`MICROBROWSER_PERF_SUMMARY=1`, first frame after launch:

```
   self ms    main ms    calls  label
   10.996     10.996         1  Application::PaintAndPresent
   10.971     10.971         1  SdlPresenter::Present
    0.026      0.026         2  engine::Paint
    0.010      0.010         1  engine::Navigate
```

**Rasterizing 27 fills over 1.6 M pixels costs 0.026 ms. Presenting costs 11 ms** — 99.8% of the
frame. That inverts the intuition the project was designed around, and is worth stating plainly.

Two caveats before anyone optimizes against it. This is the *first* frame, so it includes
`SDL_CreateTexture` and the driver's first-use initialization; steady-state present will be far
cheaper. And it is a Debug build. The number is recorded because it is the first measurement, not
because it is a target.

What it does suggest is that once real content exists, the interesting measurement is present cost
per frame in steady state, not rasterizer throughput. That is a perf-harness scenario, and it is
another reason the harness should exist before M5.

### Partial repaint is built but not yet exercised in the app

`MICROBROWSER_TRACE_REDRAW=1` reports `full` for every frame, and that is correct rather than a bug:
with no layout, the engine genuinely cannot narrow damage, so it reports empty damage and the app
honors it. The partial path is covered by unit tests
(`DisplayList/ExecuteRespectsDamage`, `DirtyRegionPolicy/*`) and is wired end to end, but it will
not do real work in the running browser until layout exists in M5/M6.

Recorded explicitly so nobody later reads "partial repaint implemented" and assumes it is in effect.

## Memory — and what actually dominates it

```
VmRSS       115 MB
PSS          53 MB      (most of RSS is shared clean file pages)
Private      39 MB      37 MB dirty + 2 MB clean

Largest mappings:
   61 MB   libLLVM.so.20.1
   14 MB   [anonymous]
   14 MB   [heap]
   12 MB   libgallium-25.2.8.so
    2 MB   libSDL3.so
```

Our canvas is 4 MB. **Roughly 73 MB is Mesa's llvmpipe/gallium stack**, loaded when SDL initializes
its renderer. RSS before any rendering setup is 7.5 MB.

An experiment worth recording so it is not repeated: `SDL_GetWindowSurface` was measured as an
alternative to `SDL_CreateRenderer`, on the theory that avoiding the GL path would avoid loading
Mesa. It does not — the surface path measured 115 MB against the renderer path's 111 MB, because
SDL's Wayland backend initializes EGL either way. **The present-path choice does not affect this.**

Reducing it would mean a different windowing backend or a different SDL configuration, not a
different blit strategy. Not worth chasing at M0, but it should be understood before anyone claims
a memory figure for this browser: the majority of resident memory today is the display stack, not
the browser, and most of it is shared with every other GL client on the machine.

## Build and test

```
Full build (24 cores, cold ccache)   ~9 s
Test suite (24 ctest shards)         0.02 s
Tests                                76, all passing
Warnings                             0
```

Under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor
-Wold-style-cast -Wdouble-promotion -Wformat=2`, on GCC 13.

ASan, UBSan, and TSan all clean. TSan requires `setarch -R` to clear ASLR; `tools/run-checks.sh`
does this automatically.

## Gaps

- **No perf harness.** No scenario runner, no recorded baselines beyond this document, so "did this
  regress?" is answered by hand. `microbrowser_perf` is in the build with no scenarios. It needs
  them before M5, or layout gets optimized blind.
- **No allocation counting.** `MICROBROWSER_PERF_HARNESS_BUILD` exists but arms nothing. The
  counting `operator new` that lets a test assert "this path must not allocate" should exist before
  the paint hot path does.
- **Debug build only.** No Release or LTO numbers yet; the `microbrowser-perf` preset is configured
  but unmeasured.
