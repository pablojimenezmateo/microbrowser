# Agent Guide

First-stop operating guide for agents working in this repository.

## Quick Scan

- `microbrowser` is a from-scratch native web browser in C++20, CMake, SDL3 for windowing only.
- Priority order: correctness → security → privacy → speed → CPU → memory → simplicity.
- **Every byte from the network and every message from a renderer is attacker-controlled.**
  Isolation is per *site*, not per tab. `guidelines/security.md` is the short version; ADR 0004 is
  the model.
- **Idle CPU is zero and must stay zero.** The process blocks in one place: the platform event wait.
- Every `src/` directory has a `MODULE.deps` contract enforced by the architecture lint. Read the
  manifest before adding a file, an include, or a class member.
- `AGENTS.md` owns repo policy. `guidelines/` owns the durable how-to. `docs/adr/` owns decisions.
- Build with `cmake`, test with `ctest`, and prefer `tools/run-checks.sh` so results are readable
  afterward without rerunning.

## Project Status

**The browser renders Hacker News and old.reddit.com, and runs a page against them. The JavaScript
engine is complete** — see `docs/js-conformance-roadmap.md` for what is done, what is
deliberately approximate, and the short list of what is left. External and inline scripts,
DOM reads and writes, `style`, event handlers, and timers — a click reaches the page's own
handlers, and `preventDefault` stops the navigation it would otherwise have caused. `./build/microbrowser/microbrowser <url>` fetches a document,
parses it, resolves its cascade, lays it out, and draws it — text, tables, images and all. The
front page and a comments page both render, and clicking a story navigates to it.

**old.reddit.com renders too**, as of 2026-08-04, and what it took is the reason to keep using the
method rather than the roadmap: it was not the layout work ADR 0007 predicted. It was a
`User-Agent` header we had never sent (reddit's edge blocks a request with none, and served a page
titled "Blocked"), `overflow` being applied to inline boxes it does not apply to, and
`display: inline-block` having been cascaded and stored and then laid out as `inline` — so every
inline-block on the page had no geometry at all. Three sessions running, the named target found
things no milestone list contained.

`./build/microbrowser/microbrowser_snapshot <url> -o out.ppm` does the same with no window, which
is how to look at a page from a machine with no display. `-v` dumps every display list command,
`-click x,y` clicks before the snapshot, **`-hover x,y` moves the pointer without clicking**, `-type` and `-key` drive the keyboard, `-y` scrolls, `-dpr` sets the device pixel ratio an `<img srcset>` is selected against (it does *not* scale painting — nothing does, yet), and it always prints any script that threw. **When anything was driven at the page it also prints a `focus:` line** — the tag plus an id, a name or an href, and whether the keyboard put it there. Focus is the input router, so a click that focused the wrong element renders identically to one that worked; there is no other way to see that from outside.
**Use it.** Every layout and paint bug listed in the git log of the last session was found by
rendering a real page and looking at it; none of them failed a test first.

`./build/microbrowser/microbrowser_jsshell <file.js>` is the same argument for `src/js`: it runs one
file and prints what it said and what it threw, and **`-p` parses only and reports each error by
*offset* with the source around it** — a minified bundle is one line of 200KB, so a line number
locates nothing. Every JavaScript bug in the youtube.com pass was found with it in minutes.
`MICROBROWSER_JS_TREEWALK=1` selects the tree-walker here too, so running a file twice and diffing
is a differential test.

What exists:

| Module | State |
|---|---|
| `src/util` | Parse, StringUtil, Env, tracing, counters, DEFLATE |
| `src/gfx` | Geometry, transforms, Color and its text form, Canvas, DirtyRegion, DisplayList + its two-frame diff, Path, analytic-AA rasterizer, stroker, Painter, FreeType/HarfBuzz text, font catalog + font-stack matching, glyph and shaped-run caches, PNG decoding, **JPEG decoding — baseline and progressive, in two halves: a container of marked segments and an entropy-coded bit stream** — SVG rendering (paths, shapes, groups, transforms), bilinear image scaling and triangle chroma upsampling, **the compositor surface and the display-list hole that names one** (ADR 0013). SDL-free. |
| `src/ipc` | Typed, versioned, serializable UI↔Engine messages, including display lists with text on them. Images cross in a **per-frame resource table** rather than inline per command; a surface crosses as a **name**. Display-list encoding is its own translation unit. |
| `src/url` | WHATWG URL parser, Origin, Site, PartitionKey, public-suffix list |
| `src/privacy` | Blocking engine, HTTPS-only, referrer trimming, tracking-parameter removal, Verdict |
| `src/net` | HTTP/1.1, cookies, cache, non-blocking sockets, TLS. `Fetch` takes a `privacy::Verdict` and has no overload without one, and **starts** a request rather than returning a response. `RequestQueue` runs them concurrently, bounded **per partition key**, and drops them all on a navigation. `Content-Encoding: gzip`/`deflate`, undone under a **double bound** — ceiling and expansion ratio, failing rather than truncating. **`ConnectionPool` keeps connections between requests, keyed by the partition key rather than by host**, with an idle timeout that goes through `next_deadline_ms`; `Fetch` takes the pool for the same reason it takes a Verdict. |
| `src/media` | Containers only, never codecs (ADR 0013). Fragmented-MP4 demux: `ftyp`, the `moov` track hierarchy, `moof` sample tables, into tracks and **byte ranges rather than bytes**. Bounds-checked, sticky-failing reader; fuzzed. May name `util` and nothing else, so a demuxer that started decoding would not compile. |
| `src/dom` | Node, Element, Text, Document, a **mutation version** the five mutation primitives mark — what makes "is what I derived from this tree still describing it?" a question a box tree or an invalidation index can ask — and the **dynamic state bits** a selector matches on (`:hover`, `:active`, `:target`, `:checked`, `:disabled`, `:required`, `:placeholder-shown`), written only by the engine. Focus is **not** one of them: it is one element on one document, and `Element::SetState` refuses to write it. |
| `src/html` | Spec-literal tokenizer and tree construction, including the table insertion modes. Form-control predicates and form ownership. |
| `src/css` | Tokenizer, parser, selectors — including the **functional pseudo-classes** `:not()`, `:is()`, `:where()` and the four `:nth-*()` with the whole `An+B` grammar, nested to a bounded depth, and the specificity rule that makes `:where()` worth zero — cascade, computed style, user-agent sheet, HTML presentational attributes, backgrounds including images, the flex properties, `position`/`inset`, `overflow`, min/max sizing, **custom properties and `var()`** — inherited, nested, with fallbacks and the invalid-at-computed-value rule — **`calc()`** with one relative term plus an absolute offset, **`@supports`** answered by *applying* the declaration rather than by a list of names, and `aspect-ratio`, and a **media query evaluator** (`MediaQuery.h`) for width, height, orientation and resolution with `and`/`or`/`not` — which `srcset` and `<source media>` use and which **`@media` does not yet**: a prelude with a parenthesis in it still drops its whole block, which is ledger session 49. Matching lives in `SelectorMatch.cpp` and is a pure function of (element, selector) that never sees a token — including the **dynamic pseudo-classes**, which read a bit `src/engine` wrote rather than asking anything about a mouse. **`StyleInvalidation`** is the index of ADR 0016 §3: which dynamic states any rule in the cascade depends on, and which of those can move a box, so a state change nothing is filed under costs a bitmask test. It is per-*state* rather than per-element — one `pre:hover { overflow: auto }` on Hacker News makes every hover a full relayout. `PropertyAffectsLayout` is the table behind it and defaults to *layout*, so an unclassified property is slow rather than wrong. |
| `src/layout` | Box tree, block box model, line boxes with a shared baseline, line breaking and `<br>`, text alignment, auto margins, min/max-content widths, per-line text fragments, replaced elements, floats and clearance, automatic table layout, **flexbox** (both axes, grow/shrink/basis, wrap, justify/align, gaps, order), **positioning** (relative/absolute/fixed with a containing-block chain), **`display: inline-block` and `inline-flex`** as real atomic inlines — block inside, one unbreakable rectangle outside, with a CSS 2.1 §10.8.1 baseline — min/max sizing, overflow clipping (and *not* on inline boxes, which it does not apply to), display-list building |
| `src/engine` | Page (one document), image selection (`srcset`, `sizes`, `<picture>`, against the viewport and the device pixel ratio, and **`loading="lazy"`, which is a box near the scrollport rather than a box at all**), PageScript (its interpreter, bindings, timers and animation frames), Loader (everything network, started/completed), PendingLoad (one navigation in flight), Engine (routes messages, drives the load). Hit testing for links, form controls and event targets; form submission; navigation from a click. **Geometry as a service** (ADR 0015): the `bindings::GeometrySource` a page's `getBoundingClientRect` and `getComputedStyle` are answered through, laying out synchronously when the document changed under the last one and counting it as `layout.forced_by_script`. Fetches a document's subresources **concurrently** and runs its scripts at the three points `defer`, `async` and `type=module` actually mean -- and now fetches an image **after** the navigation that carried the document is over, which is the first request this browser makes outside a load. |
| `src/bindings` | **`requestAnimationFrame`**, which schedules a frame only while something has asked for one. The seam between script and the document, and the only module that sees both `js` and `dom`. **`IntersectionObserver` and `ResizeObserver`** (ADR 0018 §5) — geometry sampled once per frame at the one place a frame is produced, and delivered only when the answer changed: never from inside the scroll that caused it, sampled in full before anything is delivered, and scheduling nothing, so a page with no observer costs a pointer comparison per frame. **A real type hierarchy** — Node/CharacterData/Element/HTMLElement and the per-tag interfaces, so `instanceof` answers and a class can extend HTMLElement; methods live on prototypes rather than on every wrapper. **Custom elements** — the registry, upgrade in place, and the connected/disconnected/attributeChanged reactions. **MutationObserver**, batched and delivered as a microtask. `window` is an event target. **Events** a page makes and dispatches, untrusted by construction. `DocumentFragment`. Element-scoped queries and the element-only walk. **Geometry** — `getBoundingClientRect`, `offsetWidth`/`offsetHeight`, `clientWidth`/`clientHeight`, `window.innerWidth`/`innerHeight` and `getComputedStyle`, over an interface this module *declares* and the engine implements, so no `layout` appears in its `allow:` line; absent entirely when there is no layout behind them. `window`/`location`/`navigator`, element lookup and the simple selectors, attributes, `classList`, `style` (via `Proxy`), `dataset`, tree walking, creation, removal and reordering, `textContent`, event listeners with click dispatch and bubbling, and the timer queue. **Focus** — `document.activeElement`, `focus()`, `blur()` and the four focus events, over the one copy of focus that lives on `dom::Document`; the engine's click and Tab reach the same algorithm, because two ways to change focus is how `activeElement` ends up disagreeing with where the next keystroke goes. Where every same-origin check will live — ADR 0008. |
| `src/platform` | The only module that knows what a window is, and the only place the process sleeps. SDL, the system font database, and the descriptor wait live here. |
| `src/js` | JavaScript, and as near complete as the language gets here. Lexer, parser, a bytecode compiler and machine (names resolved to slots, calls that cannot leak a scope keeping bindings in the frame, the tree-walker kept as the differential engine behind `MICROBROWSER_JS_TREEWALK=1`), mark-sweep heap with an ephemeron pass. **Modules** — every `import`/`export` form, `import.meta`, `import()` — with the host supplying the resolver. Classes with accessors, `super`, private fields and methods, static blocks, `new.target`, the brand check. `Proxy` with every trap, and subclassing a builtin. Full `ToPrimitive`. **UTF-16 string indexing over UTF-8 storage.** Property attributes and integrity levels. `ArrayBuffer`, the nine typed arrays and `DataView`. A real `Date` with a computed calendar and a parser. `JSON` with replacer, reviver, indent and `toJSON`. A backtracking regular expression engine with `/u` code points and `\p{...}`. Symbols, iteration, `Map`/`Set`/`Weak*`/`WeakRef`, Promises and the microtask queue, and **every form of suspending a call** — `async`/`await`, generators, `yield*` with real delegation, async generators, `for await`. No `eval` and no `Function(source)`, and a test says so. Knows nothing about the DOM. Deviations are listed in `docs/js-conformance-roadmap.md`, each with its reason. |
| `src/ui` | Browser chrome: toolbar, omnibox with editing, navigation history. No dom/css/layout — the chrome is not a page. It no longer scrolls: the arrow and page keys are a *keydown's default action* and run in the engine after the page's handlers, because the chrome taking them meant a page never saw an ArrowDown. |
| `src/app` | Main loop: idle-wait policy fed by the page's soonest deadline **and the sockets it is waiting on**, bounded event drain, dirty-region policy, **surface damage from generation counters** (a playing surface damages its rect every frame; a paused one damages nothing), composites chrome over page, present. **`KeyRouting.h` decides chrome-or-page for every key** — before it becomes an ipc message, and therefore before it could cross into a sandboxed renderer. While the omnibox has focus the chrome takes *everything*; otherwise only ctrl+L and ctrl+R, deliberately the whole list. |

Not yet started: grid (rest of M5), stacking contexts (rest of M6), tabs, downloads,
the process split and the sandbox (rest of M7), integration (M9). M8 is done. **The collector runs during evaluation**, at every loop back edge and every call: the
machine's operand and frame stacks are data, so a script that recurses while allocating is collected
through rather than starved.

**Nothing the parser accepts is handed back to the tree-walker any more.** Every remaining reason
`Compile` can return null is a bound — nesting too deep, too many instructions, more block-scoped
names than a slot index holds — or a guard against a bug in the compiler itself, and a test says so.
Suspending a call is what the machine was built for and every form of it has landed: `Await` files
the running frame and every slice of the machine's stacks it owns, and `Yield` is the same two
halves with a `next` as the trigger instead of a settled promise. An async generator is both at
once, which is why its promises are one per request rather than one per call. **The tree-walker
refuses an async function or a generator rather than running one wrong** — a wrong answer three
lines later is worse than a refusal at the call — and that refusal is now the only thing it is asked
to do that the machine does not.

One thing still delegates rather than compiles: a class is *built* by the tree-walking
`EvaluateClass`, reached through an opcode. Its method bodies are compiled; the computed keys, the
static initializers and the per-instance field initializers are walked, which is right for things
that run once per class or once per instance rather than once per call.

**Loading is asynchronous and the loop still blocks in one place** — ADR 0011 landed. A request is
started and completes on a later turn; the platform wait takes sockets alongside the deadline it
already took; concurrency is bounded per partition key. Name resolution is the one call left that
blocks, and it is one per host rather than one per resource. A display list carrying an image still
serializes the bitmap inline rather than naming it in a resource table. Roadmap in `README.md` and
`AGENTS.md`.

## Where To Pick Up

**Read `docs/roadmap-to-any-page.md` first.** It sequences ADRs 0015–0030 into sessions with a
runnable check on each, and it supersedes the ordering of this list wherever the two disagree. The
measurement behind it is `docs/surveys/2026-08-04-reddit-youtube-plex.md`, which found that
`GET https://www.reddit.com/` returns an 8KB JavaScript challenge rather than a page: reddit is not
a layout problem, and the seven small bindings that open its front door were on nobody's roadmap.

The list below is what was queued before that survey. It is still right about the next two items.

Ordered by value, not by milestone number. `docs/adr/0007-compatibility-targets.md` is the
reasoning; this is the queue.

0. **Wire `@media` to the evaluator that now exists.** Ledger session 49, and it is here at the
   top because it is four lines of wiring over a bug that has been dropping rules on every page:
   `MediaListItemMatches` accepts one Ident token, so `@media (min-width: 600px) { … }` has never
   applied. ADR 0014 counts it at 791 occurrences and calls it supported. `css::MediaQueryListMatches`
   landed with `srcset` in session 6 and answers exactly this grammar.

1. **`min()`, `max()` and `clamp()`.** Custom properties, `var()`, `calc()`, `@supports` and
   `aspect-ratio` are all **done** (session 4). `calc()` holds one relative term plus an
   absolute offset, so `calc(100% - 20px)` keeps both and `calc(100% - 1em)` is dropped rather
   than rounded. `@supports` answers by *applying* the declaration to a scratch style and
   reporting whether it took — there is no table of supported names to drift, which is what
   ADR 0014 §3 asks for.

   What is left of that family is the other math functions: on wikipedia's stylesheet, 45 of
   54 `calc` declarations now apply and **eight of the nine failures need `max()`**. The ninth
   needs `vh`, and the viewport units need a viewport size in the cascade, which is not there.

2. **The scroll model — `scrollTop`, and a scroll that is a paint.** `getBoundingClientRect`,
   `offsetWidth`/`offsetHeight`, `clientWidth`/`clientHeight` and `getComputedStyle` are **done**
   (session 7): a page asks through a `bindings::GeometrySource` that `src/engine` implements, a
   query on a mutated document runs layout before it answers, and `layout.forced_by_script`
   counts every time it does. ADR 0012's list is otherwise done too — the element type hierarchy,
   `MutationObserver`, custom elements, events a page makes and dispatches, element-scoped
   queries, `DocumentFragment`, `CharacterData`, `Image`.

   The seam is worth knowing before adding to it: `src/bindings` may see `js` and `dom` and **not
   `layout`**, which is a security boundary rather than an oversight (ADR 0008). ADR 0015's own
   sketch has the dependency backwards — it says the engine publishes the header — so the
   interface is declared in `src/bindings` and implemented by `src/engine`, and every answer is a
   copy rather than a pointer.

   The scroll model itself is **done** (session 8) and so is input, events and focus (sessions
   9–10): `scrollTop`/`scrollWidth`/`scrollTo`/`scrollIntoView`, a scroll that damages a band
   rather than the window, `position: sticky`; then the two input messages, the full dispatch
   algorithm, default actions after dispatch, and the focus model — `document.activeElement`,
   `focus()`, `blur()`, Tab order, and one copy of focus on `dom::Document`. So is the dynamic
   state and the invalidation index (session 11): `:hover`, `:active`, `:target`, `:checked`,
   `:disabled`, `:enabled`, `:required`, `:optional`, `:placeholder-shown` as bits on
   `dom::Element`, `:focus`/`:focus-visible`/`:focus-within` **derived** from the one copy of
   focus rather than stored, and `css::StyleInvalidation` — so a pointer crossing a page whose
   rules never say `:hover` does not hit-test, restyle, lay out or paint. What is left of
   ADR 0012 is now (3) below: `IntersectionObserver`, `ResizeObserver` and `loading="lazy"`
   landed in session 12, and reddit's front page went from 26 images fetched to 10.

   **Two things a next session should know before touching input.** `microbrowser_snapshot`
   prints a `focus:` line whenever `-click`, `-type` or `-key` drove the page — the tag plus an
   id, a name or an href, and `-hover x,y` moves the pointer without clicking. Use them: a click
   that focused the wrong element renders identically to one that worked, and a `:hover` rule
   that never applies looks like a page with no `:hover` rule. And **old.reddit's search
   box can no longer be clicked**, which is correct rather than a regression in focus: a click
   now focuses the nearest focusable ancestor of the *topmost* element, and `.side` at
   `975,66 300x191.6` covers the field at `975,73 300x21.6`. The cause is a layout bug that
   predates the focus model — `#header-bottom-right` is `position: absolute; bottom: 0` inside a
   66px `#header` and lands 7px *below* its containing block's bottom edge. Links elsewhere on
   the page are unaffected; clicking a story still navigates.

   **ADR 0012's rule stays the important part: a stub is worse than an absence**, because
   feature detection sends a page down the native path into a wall where a missing name would
   have sent it to a polyfill that works. The amendment at the end of that ADR is the other half:
   a *deep* polyfill is not the cheap path it looks like.

3. **`fetch`, `XHR`, and wiring modules to the loader.** The seam exists now: session 12's
   lazy images made `Engine` fetch something with no navigation behind it, and `late_images_`
   plus the completion route through `Advance()` is the shape a `fetch` response arrives on.
   ADR 0011 is **done** — a request is
   started and completes later, the loop waits on sockets, and Hacker News went from 4.3s to
   2.1s — so all three are now bindings over machinery that exists rather than structural work.
   The module half needs one thing the others do not: `Interpreter::SetModuleResolver` is
   synchronous, so wiring it means either a pre-pass that fetches the graph or a resolver that
   can answer later. Decide that before writing code.

   What is *not* done from ADR 0011 and is worth knowing: **`getaddrinfo` still blocks**, one
   call per host; the wait watching sockets caps itself at 16ms because SDL exposes no
   descriptor for its own event queue (see `SdlWindow::WaitEventOrDescriptors`, which names what
   removing the cap would cost); and there is still no incremental parse or paint, so a page
   appears when it is finished rather than as it arrives. The ADR is explicit that the last of
   those is enabled by this work rather than performed by it.

4. **Transport — ADR 0010 §1–2 is done; §3, HTTP/2, is not.** `Accept-Encoding` says
   `gzip, deflate` and `Connection: close` is gone. On old.reddit.com that is **697KB on the wire
   where 2.35MB would have been** (3.37x) and **20 connections and 20 TLS handshakes for 40
   fetches**, from 40 and 40. Every decompression is bounded twice — an absolute ceiling and an
   expansion ratio against what arrived — and a gzip bomb is refused from its declared ISIZE
   before a byte is produced. The pool is keyed by the **ADR 0005 partition key**, which is the
   whole privacy content of it: two top-level sites loading the same CDN host get two connections.
   What is left is ALPN and HTTP/2, which is mostly a *parser* problem — framing, multiplexing,
   flow control, and HPACK, whose CVE history is state confusion between the two ends rather than
   buffer overruns.

5. **`transform`, and with it stacking contexts.** 1391 uses. `AffineTransform` and path
   transforms already exist in `src/gfx`; what is missing is the property, the computed value and
   the display-list command. `transform` creates a stacking context, which is what finally pulls
   M6's remainder in. Animations come *after* it — animating a property that does not apply gains
   nothing — and must not leave a 60Hz loop running on a settled page.
   `bindings::AnimationFrames` is the model for that: it schedules a frame only while something
   has asked for one, and four tests say so.

6. **Grid, and scrolling an overflow container.** Real, and sixth on the measurement. Scrolling
   needs a scroll offset per box and an input path to move it, which is engine work rather than
   layout's; `position: sticky` parses as relative because there is no offset to compare against.

7. **The language itself is done.** What is left is in `docs/js-conformance-roadmap.md` and is small:
   Annex B block-function hoisting, the two BigInt typed arrays, `Intl`, and the Unicode tables
   `normalize` and the rest of `\p{...}` need. Unhandled rejections still get a console line and
   nothing more.

**Use `tools/jsshell`.** It runs one JavaScript file, and `-p` reports a syntax error by *offset*
with the source around it — a minified bundle is one line of 200KB, so a line number locates
nothing. Every engine bug in the youtube.com pass was found with it in minutes. Its lesson is worth
keeping: `var` was block-scoped and un-hoisted in **both** engines, so the differential could not
see it. Two engines agreeing is evidence, not proof.

Known remaining gaps on old.reddit.com, which renders as of 2026-08-04: `vertical-align` does not
exist at all, so its `vertical-align: middle` flair sits on the baseline; the subreddit header bar
overlaps itself — **`#header-bottom-right` is `position: absolute; bottom: 0` inside a 66px
`#header` and is placed 7px *below* its containing block's bottom edge, which is what puts the
search field under the `.side` sidebar and makes it unclickable**; the right sidebar's float is
wrong; and **`background` on an inline box is not
painted**, which is why a spoiler span is now invisible text on white rather than the grey bar
`.md-spoiler-text:not(.revealed)` asks for. Its scripts fail with a **masked** error —
`reddit-init.js` wraps itself in `try { … } catch (err) { r.sendError(…) }` and defines `r` inside
the try, so anything that throws early makes the catch handler throw `ReferenceError: r is not
defined` and every later script that expects `r` fails too. The reported error is never the real
one; unmasking it needs a way to evaluate a prelude before a page's own scripts, which
`microbrowser_snapshot` cannot yet do. **`www.reddit.com` is a separate problem** — a JavaScript
challenge, unaffected by the `User-Agent`; see `docs/roadmap-to-any-page.md` Phase A.

Known remaining gaps on Hacker News itself: `<select>` is laid out and submitted but not clickable,
`cellspacing` is not mapped because there is no `border-spacing`, and `:visited` deliberately
matches nothing.

Known-crude spots, each with the reasoning written where the code is: a page appears when its load
finishes rather than as it arrives, because there is no incremental parse or paint; `getaddrinfo`
is the one call in the network stack that still blocks; an image crosses the IPC seam once per
frame in a resource table rather than once per *session* in a cache, because a cross-frame cache
makes the receiver's memory part of the protocol and that is a protocol decision (ADR 0013);
scrolling an overflowing document repaints in full because there is no scroll blit in
the presenter; a background image is re-rasterized per element rather than shared; and collecting
background images resolves the cascade a second time, before layout resolves it again.

## Development Workflow

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)

# Inner loop: test binary only.
cmake --build build --target microbrowser_tests -j$(nproc)

# Full suite, sharded across cores.
ctest --test-dir build --output-on-failure -j$(nproc)

# Benchmarks. Only built under the perf preset, and the binary refuses to print
# timings from a build without NDEBUG.
cmake --preset microbrowser-perf && cmake --build --preset microbrowser-perf --target microbrowser_bench
./build/microbrowser-perf/microbrowser/microbrowser_bench blit

# Focused, by substring.
./build/microbrowser/microbrowser_tests Canvas
./build/microbrowser/microbrowser_tests ArchitectureInvariants
./build/microbrowser/microbrowser_tests --list
```

The build auto-uses **ccache** and **ld.lld** when present; both are no-ops if absent.

Prefer the logging wrapper — it tees to a deterministic file so a result can be read back instead of
rerunning:

```bash
tools/run-checks.sh tests   # -> /tmp/microbrowser-tests.log
tools/run-checks.sh asan    # -> /tmp/microbrowser-asan.log
tools/run-checks.sh ubsan   # -> /tmp/microbrowser-ubsan.log
tools/run-checks.sh tsan    # -> /tmp/microbrowser-tsan.log
tools/run-checks.sh all
```

**After a run, READ `/tmp/microbrowser-<target>.log` instead of rebuilding and rerunning.**

## Working The Roadmap One Session At A Time

`docs/roadmap-to-any-page.md` is the argument; `docs/roadmap-sessions.json` is its state — one
entry per session, with the check that finishes it. `/next-session` reads that ledger, picks the
lowest unfinished session, implements it, runs its check, commits, records what it found in
`docs/session-log.md`, and stops.

```bash
tools/agent-loop.sh -n 5    # five sessions, each in a brand-new agent process
```

The process boundary is the point: a long conversation fills with the debris of work already
committed and starts reasoning about its own transcript instead of the repository. Each iteration
starts from nothing and rebuilds from the three things that survive — the git log, the session log,
and the ledger. **A session that cannot hand off through those three files has not finished.**

`status: done` means the session's `check` was *run* and *passed*. Nothing else may set it; a
session wrongly marked done costs the next agent a whole session to discover.

TSan needs ASLR cleared (`setarch -R`); `run-checks.sh` does this automatically. Running `ctest` on
the tsan preset by hand without it fails with "unexpected memory mapping" — that is the environment,
not a bug.

## Observability

All off by default, all read once on first use:

```bash
MICROBROWSER_PERF_COUNTERS=1   # non-zero event counters, dumped at exit
MICROBROWSER_PERF_SUMMARY=1    # per-label scope table ranked by self time
MICROBROWSER_PERF_TRACE=1      # one stderr line per scope (a firehose; distorts what it measures)
MICROBROWSER_STARTUP_SUMMARY=1 # same, for startup scopes
MICROBROWSER_TRACE_REDRAW=1    # one line per presented frame: full/partial, rects, coverage
MICROBROWSER_JS_TREEWALK=1     # run script on the tree-walker instead of the bytecode machine
```

`MICROBROWSER_JS_TREEWALK=1` is the differential switch, not a debug print: the two engines
answering the same suite is the only way to know they agree. Thirty-four tests are expected to fail
under it and the list is at the top of `tests/JsVmTests.cpp`; anything else appearing there is a
difference nobody decided on. Three tree-walker bugs were found this way rather than by reading it —
the third was a `for...of` that read `done` off a step and never wrote it back to its cursor.

**Read the main-thread column of a summary first.** Self time ranks CPU cost; main time ranks what
the user waits on, and the two routinely disagree. See `guidelines/observability.md`.

## Before You Add Anything

- **A new file** — check the module's `max_tu_lines` and whether the file belongs in this module at
  all. A file that does not fit the module's `purpose:` line wants a different module.
- **A new include** — check `allow:` and the target module's `public:`. If the header you want is
  private, decide whether it should be published or whether you are reaching across a boundary.
- **A class member or public method** — check the class's `budget:`. If you must raise it, say why
  in the commit message. That is the mechanism working, not a hoop.
- **A third-party dependency** — write an ADR. `docs/adr/0001-third-party-dependencies.md` lists the
  sanctioned set and why each earns its place.
- **A timer, poll, or background wakeup** — justify it against the zero-idle-CPU invariant, and
  route it through `IdleWaitState::next_deadline_ms`.
- **A network request** — it must be user-caused and must pass the privacy layer. See
  `guidelines/privacy.md`.
- **A parser, a decoder, or an IPC message** — the input is hostile. Bounds-check every read,
  saturate every size computed from input, and land the fuzz target on the same commit. See
  `guidelines/security.md`.
- **A thread** — write down what it owns, what it borrows, and who joins it before `main` returns.

`tools/budget-report.sh` prints headroom against every budget, sorted by how close each is to its
limit. Run it before a refactor to see what is about to blow.

## Agent Best Practices

- Narrow the problem with fast repo inspection: `rg`, `rg --files`, targeted reads. Avoid broad dumps.
- Match the repo's design direction instead of preserving stale boundaries. Broad refactors are fine
  when they improve correctness or ownership.
- Prefer RAII, explicit ownership, value semantics. Inheritance only for a durable polymorphic
  boundary (`ipc::Transport` is one; most things are not).
- Keep deterministic logic out of event glue and paint code — that is what makes it testable.
- Treat performance as measurable engineering. Do not guess; the counters and scopes are there.
- When a lint fires, fix the design it is pointing at. Raising a budget to silence it is sometimes
  right, but it should be a decision, not a reflex.

## Related Docs

- `AGENTS.md` — repo policy, priority order, invariants
- `SECURITY.md` — vulnerability reporting, scope, disclosure
- `guidelines/architecture.md` — the module contract, layering, separation of concerns
- `guidelines/security.md` — trust boundaries, the process model, hostile input, memory safety
- `guidelines/privacy.md` — the privacy contract every feature is held to
- `guidelines/cpp.md` — ownership and implementation guidance
- `guidelines/performance.md` — measurement workflow and the zero-idle-CPU rule
- `guidelines/observability.md` — counters, scopes, and how to read a summary
- `guidelines/testing.md` — test strategy, reference tests, control fixtures
- `docs/adr/` — durable decisions and their reasoning
- `docs/adr/0007-compatibility-targets.md` — the five sites that must eventually work, and what they cost
- `docs/adr/0009` — the parse depth bound, and the measurements it comes from
- `docs/adr/0010` — transport: content coding, connection reuse, HTTP/2
- `docs/adr/0011` — asynchronous loading and the event loop, against zero-idle-CPU
- `docs/adr/0012` — which web APIs get built, in what order, and why a stub is worse than an absence
- `docs/adr/0013` — media, the video surface, and the codec dependency
- `docs/adr/0014` — the CSS features a real page actually uses, counted
- `docs/adr/0015` — layout as a queryable service: geometry from script, without widening `MODULE.deps`
- `docs/adr/0016` — selectors, dynamic state, and what a hover costs
- `docs/adr/0017` — input, the event model, and focus
- `docs/adr/0018` — scrolling, the viewport, and why a scroll is a paint
- `docs/adr/0019` — shadow DOM, and the tree layout actually sees
- `docs/adr/0020` — the network a page asks for itself: fetch, CORS, CSP, SRI, WebSocket
- `docs/adr/0021` — client-side storage, partitioned, and what is allowed to survive
- `docs/adr/0022` — workers, and what this browser refuses to run in the background
- `docs/adr/0023` — the image formats a page actually sends, and who decodes them
- `docs/adr/0024` — web fonts, WOFF2, and brotli
- `docs/adr/0025` — encodings, bidi, line breaking, and the Unicode data question
- `docs/adr/0026` — navigation, session history, and the URL bar as a security surface
- `docs/adr/0027` — nested browsing contexts, and the isolation they were the reason for
- `docs/adr/0028` — the media element, MSE, and the refusal of DRM
- `docs/adr/0029` — canvas, WebGL, permissions, and the fingerprinting surface
- `docs/adr/0030` — incremental parsing, and showing a page before it is finished
- `docs/roadmap-to-any-page.md` — the above, sequenced into sessions with a check on each
- `docs/roadmap-sessions.json` — the same sessions as state: what is done, what the check is
- `docs/session-log.md` — what each session found that a diff does not say
- `docs/surveys/2026-08-04-reddit-youtube-plex.md` — every number those ADRs cite
- `docs/performance/m0-baseline.md` — the measurements M0 established
- `docs/performance/m1-rasterizer.md` — where paint time actually goes, and what is not hot
- `docs/performance/m6-damage.md` — what incremental repaint saves, and what it does not
- `docs/performance/m8-bytecode.md` — the machine against the tree-walker, and where the time still goes
