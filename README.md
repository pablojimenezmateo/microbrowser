# microbrowser

A native, low-footprint web browser written from scratch in C++20.

Own HTML parser, own CSS engine, own layout, own software rasterizer, own HTTP client, own
JavaScript engine. No GPU requirement. Privacy-respecting by construction.

**Status: early. Milestone M0 (foundation) is complete — it opens a window and paints a placeholder
page. There is no HTML parser, no CSS, no network, and no JavaScript yet.** See the roadmap below
for what exists and what does not.

## Why

Three references, for three different things:

- **[Ladybird](https://github.com/LadybirdBrowser/ladybird)** for engine shape — clean library
  separation, spec-literal parsers, pixel reference tests, and the process split into a renderer, a
  network service, and an isolated image decoder.
- **[LibreWolf](https://librewolf.net/)** for defaults — content blocking, URL sanitization, storage
  partitioning, and zero telemetry as engine-level invariants rather than an extension.
- **Chromium's site isolation** for containment — the unit of isolation is a site, not a tab,
  because a tab hosts cross-origin iframes and those are the part an attacker controls.

And one stance, inherited from [microide](../microide): correctness first, then security, then
privacy, then speed, then CPU, then memory. Measured, not asserted.

## Building

```bash
sudo apt install libsdl3-dev            # M0 needs only SDL3
# later milestones add: libfreetype-dev libharfbuzz-dev libssl-dev zlib1g-dev libbrotli-dev

cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)

ctest --test-dir build --output-on-failure -j$(nproc)
./build/microbrowser/microbrowser
```

Optional but recommended: `ccache` and `lld` are used automatically when installed.

## Architecture

```
src/app/        main loop, idle-wait policy, dirty-region policy    ── UI side
src/ui/         tabs, omnibox, chrome, settings          (M7)
src/ipc/        typed serializable messages + transport            ── THE SEAM
src/engine/     navigation, lifecycle, page ownership              ── Engine side
  src/html/  src/dom/  src/css/  src/layout/  src/paint/  src/js/   (M3–M8)
src/net/        URL, DNS, TLS, HTTP/1.1, cookies, cache  (M2)
src/privacy/    filter engine, sanitizer, partitioning   (M2)
src/gfx/        Canvas, Path, Rasterizer, GlyphAtlas, Color
src/platform/   SDL window/input/present, app dirs
src/util/       parse, strings, tracing, counters
```

Single process today, with the UI↔Engine boundary defined as a serialized message protocol from the
first commit, so moving the engine into a sandboxed process stays a scheduling decision rather than
a rewrite. See `docs/adr/0003-ipc-seam-before-the-process-split.md`.

Every directory under `src/` carries a `MODULE.deps` contract — allowed dependencies, public header
surface, permitted third-party libraries, and per-class growth budgets — enforced by a lint in the
test suite. See `guidelines/architecture.md` and `docs/adr/0002-growth-budgets.md`.

## Security

A browser downloads code written by strangers and runs it. The containment model is decided up front
even though it does not ship until M7, because it constrains every interface written before then —
see `docs/adr/0004-process-model-and-site-isolation.md` and `guidelines/security.md`.

- **Four processes**: a trusted browser process; one sandboxed **WebContent** process per *site
  instance*; a network process holding the sockets, TLS, cookies, and the privacy layer; and an
  isolated image decoder, because image decoders are historically the most productive source of
  browser RCE and their interface is small enough that isolating them is nearly free.
- **Per site, not per tab.** A tab hosts cross-origin iframes. Per-tab isolation sounds like the
  answer and leaves the attacker-controlled part in the same address space.
- **The renderer is assumed compromised.** Policy runs where the attacker is not: CORS in the
  network process, cookie access in the browser process, file access behind a browser-process dialog
  that returns a descriptor rather than a path. A site identity is never a message field — the
  browser process knows it from having created the connection.
- **What ships today** is the enforcement that costs nothing now and a retrofit later: a
  hostile-input-hardened IPC wire format, banned unsafe C functions, no manual heap ownership, no
  mutable namespace-scope state, non-throwing locale-independent parses, and ASan/UBSan/TSan clean.
  The full enforced-versus-scheduled table is at the end of `guidelines/security.md`.

## Roadmap

| | | |
|---|---|---|
| **M0** | Foundation: build, test harness, architecture lint, gfx core, IPC seam, main loop | **done** |
| M1 | Rasterizer: paths, analytic AA, SIMD blitters, FreeType + HarfBuzz text, images | |
| M2 | Network + privacy: URL, TLS, HTTP/1.1, cookies, filter engine, HTTPS-only | |
| M3 | HTML parsing + DOM | |
| M4 | CSS: parsing, selectors, cascade, computed style | |
| M5 | Layout: block, inline, line breaking, floats, flexbox, grid | |
| M6 | Paint: display-list building, stacking contexts, incremental repaint | |
| M7 | Browser UI: tabs, omnibox, history, downloads, plus the process split and sandbox. **First usable browser.** | |
| M8 | JavaScript: lexer, parser, bytecode VM, GC, builtins | |
| M9 | Integration: DOM bindings, events, forms, fetch, dynamic relayout | |

## Performance and Benchmark Methodology

There are internal regression baselines but no comparative benchmarks against other browsers, and
none are claimed. What is actually measured is in `docs/performance/m0-baseline.md`.

The one property tracked from day one is **idle CPU**, currently 0.017% over 60 seconds — the
process sleeps in exactly one place and is not woken by timers, vsync, or polling. Note also that
the largest component of resident memory today is the system display stack, not the browser; that
document says so plainly rather than quoting a flattering number.

## License

MIT. See `LICENSE`.
