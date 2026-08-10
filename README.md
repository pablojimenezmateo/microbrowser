# microbrowser

A native, low-footprint web browser written from scratch in C++20.

Own HTML parser, own CSS engine, own layout, own software rasterizer, own HTTP client, own
JavaScript engine. No GPU requirement. Privacy-respecting by construction.

**Status: early. M0 (foundation) and M1 (rasterizer) are complete — it opens a window, paints
antialiased paths and strokes, shapes and renders text, decodes PNG, parses HTML and CSS, runs a
small JavaScript interpreter, fetches HTTP/1.1, and lays out block, inline, float, image and basic
table content.** See the roadmap below for what exists and what does not.

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
sudo apt install libsdl3-dev libfreetype-dev libharfbuzz-dev libssl-dev
# later milestones add: libbrotli-dev

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
src/net/        DNS, TLS, HTTP/1.1, cookies, cache       (M2)
src/privacy/    filter engine, sanitizer, containers      (M2)
src/url/        Url, Origin, Site, PartitionKey, PSL      (M2)
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

## Privacy

Not a feature area — a constraint on every other one. `guidelines/privacy.md` is the contract;
the two designs that give it teeth are decided in ADRs.

- **One partition key, `(container, top-level site, origin)`,** on every piece of per-site state:
  cookies, storage, HTTP cache, connection pool, DNS cache, TLS session tickets, HSTS entries,
  permissions. Total Cookie Protection because a data structure cannot be switched off the way a
  policy flag can. The two rows people forget are session tickets and HSTS — both are set by the
  *server*, both survive a cookie clear, and both have been used to track in the wild.
- **Contextual identities** — Firefox-style containers — are the first component of that key rather
  than a feature bolted beside it, so a container is a real boundary at every layer: a WebContent
  process serves one `(site, container)` pair, and consolidation under the process cap never crosses
  a container. Ephemeral containers are the same mechanism with persistence off, which is also all a
  private window is. See `docs/adr/0005-contextual-identities-and-the-partition-key.md`.
- **Content blocking** takes uBlock Origin's architecture, not just its filter syntax: lists compile
  once into a flat arena indexed by a hostname trie and selective token buckets, so a request probes
  a handful of buckets instead of testing 300,000 patterns, and the match path allocates nothing.
  Scriptlets and redirect resources are named entries in a table compiled into the binary — a filter
  list supplies a name and arguments, never code, because a list is third-party text and script
  execution on every site is a better position than most browser exploits achieve. See
  `docs/adr/0006-content-blocking-engine.md`.
- **Lists ship compiled in**, so a fresh install blocks with no network at all. Updating them is a
  user action or an opt-in jittered schedule — never a default-on timer, which is the exact failure
  the privacy guide warns about by name.

## Compatibility Targets

What should eventually *work*, which is a different question from what gets
built. In order of what each demands — see `docs/adr/0007-compatibility-targets.md`
for the per-site gap analysis.

| | | |
|---|---|---|
| `news.ycombinator.com` | server-rendered, tables, almost no script | **renders and navigates** — the front page and a comments page both draw, and clicking a story follows it |
| `old.reddit.com` | server-rendered, moderate CSS | needs `position`, overflow scrolling, an event loop |
| `google.com` | real script, UA-varied markup, HTTP/2 in practice | search results before the homepage |
| Plex web | single-page application **and** media playback | needs the JS engine and the media stack |
| `youtube.com` | web-components SPA, DASH over MSE, VP9/AV1 | the hardest; playing a *stream* is a far smaller problem than rendering the site |

**These make the JavaScript engine the dominant cost of the project rather than
a milestone within it**, and the 12–18 month estimate below does not survive
that. The roadmap's ordering still holds; the schedule does not. ADR 0007 says
so in detail rather than leaving it to be discovered at the end.

## Correctness

Since 2026-08-10 the primary correctness signal is
[web-platform-tests](https://web-platform-tests.org/) rather than "load a real
page and look at it". Real pages are still how bugs are *discovered*; WPT is how
we know what is implemented, and whether a fix broke something else.

```bash
tools/wpt/fetch.sh                            # pinned, sparse, ~600MB
./build/microbrowser/microbrowser_wpt dom/
```

Every test runs in its own process, the server is ours and needs no Python, and
`tests/wpt/expectations/` records only failures — so an expectation diff that is
all deletions is a session that made the browser more correct. The reasoning is
in `docs/adr/0040-web-platform-tests.md`; the work is in `docs/wpt-plan.md`.

## Roadmap

| | | |
|---|---|---|
| **M0** | Foundation: build, test harness, architecture lint, gfx core, IPC seam, main loop | **done** |
| M1 | Rasterizer: paths, analytic AA, SIMD blitters, FreeType + HarfBuzz text, images | **done** — text reaches pixels through the display list, with a font database, a glyph cache and a shaped-run cache |
| M2 | Network + privacy: URL, TLS, HTTP/1.1, cookies, containers, filter engine, HTTPS-only | **in progress** — URL, privacy, HTTP/1.1, cookies, cache, non-blocking sockets and TLS done, with requests started rather than called and concurrency bounded per partition key (ADR 0011); connection pooling, content coding and a partitioned DNS cache not |
| M3 | HTML parsing + DOM | **in progress** — tokenizer, DOM, tree construction, ordinary tables and select insertion modes done; foreign content and templates not |
| M4 | CSS: parsing, selectors, cascade, computed style | **done** — tokenizer, parser, selectors, cascade, computed style, user-agent sheet |
| M5 | Layout: block, inline, line breaking, floats, flexbox, grid | **in progress** — box tree, the block box model, line boxes with baseline alignment, line breaking, replaced elements, floats, clearance and basic table row/cell layout done; flexbox and grid not |
| M6 | Paint: display-list building, stacking contexts, incremental repaint | **in progress** — display-list building and incremental repaint from a two-frame diff done; stacking contexts not |
| M7 | Browser UI: tabs, omnibox, history, downloads, plus the process split and sandbox. **First usable browser.** | **in progress** — toolbar, omnibox with editing and keyboard shortcuts, back/forward history done; tabs, downloads, the process split and the sandbox not |
| M8 | JavaScript: lexer, parser, bytecode VM, GC, builtins | **done** — lexer, parser, a bytecode compiler and machine, mark-sweep GC that runs *during* evaluation, classes with private members and static blocks, `new.target`, full `ToPrimitive`, property attributes, UTF-16 string indexing over UTF-8 storage, a real `Date`, `JSON` with replacer and reviver, `Proxy` with every trap, subclassing a builtin, `ArrayBuffer` and the typed arrays, regular expressions with `/u` and `\p{...}`, Promises, `async`/`await`, generators with real `yield*` delegation, async generators, **modules** — every form, with the host supplying the resolver — and **BigInt**. Suspending a call is what the machine was built for: a frame is a record, so a waiting one is filed whole and put back later, which is not a thing C++ stack frames can be. What is left is wiring the module resolver to the loader: the loader can now answer later (ADR 0011), and the remaining question is that `SetModuleResolver` is synchronous. Deviations are listed with their reasons in `docs/js-conformance-roadmap.md`. See ADR 0007. |
| M9 | Integration: DOM bindings, events, forms, fetch, dynamic relayout | **in progress** — DOM bindings, events, forms and asynchronous loading done; `fetch` and `XMLHttpRequest`, which are now bindings over machinery that exists, and incremental parse and paint not |

## Performance and Benchmark Methodology

There are internal regression baselines but no comparative benchmarks against other browsers, and
none are claimed. What is actually measured is in `docs/performance/m0-baseline.md` and
`docs/performance/m1-rasterizer.md`. The second of those records two wrong measurements alongside
the right one, because how a number was produced matters as much as the number.

The one property tracked from day one is **idle CPU**, currently 0.017% over 60 seconds — the
process sleeps in exactly one place and is not woken by timers, vsync, or polling. Note also that
the largest component of resident memory today is the system display stack, not the browser; that
document says so plainly rather than quoting a flattering number.

## License

MIT. See `LICENSE`.
