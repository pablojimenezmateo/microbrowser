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
- **web-platform-tests is now the primary correctness signal** (ADR 0040).
  `tools/wpt/fetch.sh` once, then `./build/microbrowser-perf/microbrowser/microbrowser_wpt dom/`.
  Real pages are still the primary *discovery* signal; both stay, they answer different questions.
  `docs/wpt-firefox-gap.md` is where it stands **against Firefox**, `docs/wpt-baseline.md` is
  where it stands against its own last run, `docs/wpt-plan.md` is the work, `docs/wpt-tasks.json`
  is its state. **The unit is a test file Firefox passes and we do not** — a percentage of
  subtests is not comparable to another browser or to another area, because its denominator is
  what we managed to report (2026-08-17; the whole argument is in the plan's §The target).
  **Before writing code for an area, rank its test files by failing-subtest count** — one
  `python3` over `tests/wpt/expectations/<area>.txt`. C3 spent an afternoon on the thing its title
  named and got its 20 points from four types that ranking made visible; the ability a task names
  is rarely where its subtests are.
- **If an area's data is a checked-in table, build a runner that reads it directly first.**
  `tools/urlconf` runs 3,900 pinned URL vectors against `src/url` in one second and names the
  field that differed; the same vectors through the browser take three minutes and say
  "subtest failed". `url/` went 37% → 98% in one session because of it. `encoding/`,
  `mimesniff/` and `css/parsing/` all qualify and none has one yet.
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
file and prints what it said and what it threw; **`-l` lexes only, which is the only way to ask
whether a slow parse is the scanner or the tree builder** (on youtube's 5.86MB bundle: 0.38s to lex,
1.56s to parse, so it is the tree); and **`-p` parses only and reports each error by
*offset* with the source around it** — a minified bundle is one line of 200KB, so a line number
locates nothing. Every JavaScript bug in the youtube.com pass was found with it in minutes.
`MICROBROWSER_JS_TREEWALK=1` selects the tree-walker here too, so running a file twice and diffing
is a differential test.

**A thrown error now says *where*.** Every compiled function carries the source offset each
instruction came from, so a stack reads `at HS (@1814415)` rather than `at <anonymous>` — offsets
rather than line numbers, for the same reason `-p` reports by offset. Paste the number into
`python3 -c "print(open('bundle.js').read()[N-200:N+200])"` and the fault is on the screen. Eleven
of the twelve fixes in the youtube pass of 2026-08-06 were located that way, in minutes each; the
twelfth was the change that made it possible.

What exists:

| Module | State |
|---|---|
| `src/util` | Parse, StringUtil (including the one ASCII lower-caser five modules used to own a copy of), Env, tracing, counters, DEFLATE, **base64 both alphabets, and SHA-256/384/512** — the digest is here rather than reached out of the TLS stack because `src/csp` may see only `util` and `url`, and CSP's hash-sources need one (ADR 0020 §4) |
| `src/gfx` | Geometry, transforms, Color and its text form, **`letter-spacing` and `word-spacing`, applied to an already-shaped run rather than folded into the cache key** — shaping does not depend on either, and both travel on `FontRequest` so that measurement and paint cannot answer differently, Canvas, DirtyRegion, DisplayList + its two-frame diff, Path, analytic-AA rasterizer, stroker, Painter, FreeType/HarfBuzz text, font catalog + font-stack matching, glyph and shaped-run caches, PNG decoding, **JPEG decoding — baseline and progressive, in two halves: a container of marked segments and an entropy-coded bit stream** — SVG rendering (paths, shapes, groups, transforms), bilinear image scaling and triangle chroma upsampling, **the compositor surface and the display-list hole that names one** (ADR 0013). SDL-free. |
| `src/ipc` | Typed, versioned, serializable UI↔Engine messages, including display lists with text on them. Images cross in a **per-frame resource table** rather than inline per command; a surface crosses as a **name**. Display-list encoding is its own translation unit. |
| `src/url` | WHATWG URL parser, Origin, Site, PartitionKey, public-suffix list. **All 891 of the standard's own parse vectors and all 278 setter vectors pass** (`tools/urlconf`, which runs them against this module directly in one second). A host is *nullable* — "no host" and "the empty host" are different states and serialize differently — and `UrlParser` is its own header because the standard defines every setter as "basic URL parse with X state as **state override**", so the states are this module's vocabulary rather than one file's detail. **IDNA**, through `text::UnicodeToAscii`, plus the URL Standard's two web-compat rules around it: an all-ASCII domain is lowercased whatever UTS #46 thinks of it (`xn--a` is invalid Punycode and reaches a real host), and forbidden domain code points are checked *after* the mapping |
| `src/privacy` | Blocking engine, HTTPS-only, referrer trimming, tracking-parameter removal, Verdict |
| `src/csp` | **The page's own policy on what it may load and run**, at the seam `privacy` occupies and answering the other half of the same question (ADR 0020 §3-4). Content-Security-Policy: `default-src`, `script-src`, `style-src`, `img-src`, `connect-src`, `form-action`, `base-uri`; `'self'`, `'none'`, `'unsafe-inline'`, nonces, sha256/384/512 hash-sources, scheme- and host-sources with wildcard subdomain, port and path. Two policies are two policies and **both** must allow. Plus **Subresource Integrity**, because it is the same kind of thing. `allow: util url` and deliberately not `net`: a policy engine that could see the network stack would be one line from acting on its own decision. **Nothing is reported** — a violation report is an outbound request the user did not cause — so `report-uri`/`report-to` are unknown directives and `Report-Only` has no entry point at all. `frame-src` is absent because there are no nested browsing contexts and a directive that decides nothing reads as enforcement. |
| `src/net` | HTTP/1.1 **and HTTP/2**, cookies, cache, non-blocking sockets, TLS. **CORS (ADR 0020 §2): the check on the response, inside this module, with the response discarded rather than marked** — an opaque response is an empty one, a cross-origin `cors` response keeps only the headers the server exposed, and the preflight `OPTIONS` plus its grant cache are keyed by the partition key. `Fetch` takes a `privacy::Verdict` and has no overload without one, and **starts** a request rather than returning a response. `RequestQueue` runs them concurrently, bounded **per partition key**, and drops them all on a navigation. `Content-Encoding: gzip`/`deflate`, undone under a **double bound** — ceiling and expansion ratio, failing rather than truncating. **`ConnectionPool` keeps connections between requests, keyed by the partition key rather than by host**, with an idle timeout that goes through `next_deadline_ms`; `Fetch` takes the pool for the same reason it takes a Verdict. **HTTP/2 (ADR 0032)**: ALPN offers `h2`, `Http2Session` carries many requests on one socket, and the pool owns the sessions and hands out `shared_ptr`s -- so a connection is no longer something a request *owns*. The part that actually mattered is the **coalescing**: ALPN answers only after a socket is open, so without one-connect-at-a-time-per-unknown-origin six images would open six sockets and negotiate `h2` six times, which is the burst wearing a new protocol. No server push (a response to a request the user never made), no `PRIORITY` (there is nothing to schedule against), trailers decoded-then-discarded (HPACK is stateful; a skipped block desynchronises the connection forever). `FetchRequest::ChooseProtocol` is the **only** place the two protocols diverge. |
| `src/media` | Containers only, never codecs (ADR 0013). Fragmented-MP4 demux: `ftyp`, the `moov` track hierarchy, `moof` sample tables, into tracks and **byte ranges rather than bytes**. Bounds-checked, sticky-failing reader; fuzzed. May name `util` and nothing else, so a demuxer that started decoding would not compile. |
| `src/text` | Unicode character properties, and the algorithms over them: line breaking (UAX #14), the bidirectional algorithm (UAX #9), **NFC (UAX #15)** and **IDNA (UTS #46) with Punycode**. Tables generated by `tools/unicode/generate.py` (15.1.0) and `tools/unicode/generate_idna.py` (17.0.0, because that is what the web-platform-tests vectors are generated against — four code points moved between the two and each is the difference between reaching a host and refusing it). `src/url` sees this module for IDNA and for no other reason |
| `src/dom` | Node, Element, Text, Document, **the namespace an element or attribute is in** — `NamespaceRef`, a counted handle rather than a URI string on every one of a hundred thousand elements, plus a prefix *length* so `tagName`, `localName`, `prefix` and `namespaceURI` are four answers instead of two guesses at one field (the reference counting is not tidiness: an append-only intern table is a leak a page can drive with `createElementNS('u' + i, 'a')`) — a `<template>`'s **contents** — allocated only for that tag, and deliberately not its children, so nothing that walks the document reaches them — a **mutation version** the five mutation primitives mark — what makes "is what I derived from this tree still describing it?" a question a box tree or an invalidation index can ask — and the **dynamic state bits** a selector matches on (`:hover`, `:active`, `:target`, `:checked`, `:disabled`, `:required`, `:placeholder-shown`), written only by the engine. Focus is **not** one of them: it is one element on one document, and `Element::SetState` refuses to write it. |
| `src/html` | Spec-literal tokenizer and tree construction, including the table insertion modes and **the fragment parsing algorithm** (§13.2.6): `ParseFragment(markup, context)`, where the context element is the whole of it — `<td>x</td>` is a cell in a `tr` and bare text in a `div`, and it picks the tokenizer state too. A fragment parse's root is **unpoppable** (`stack_floor_`), because both inputs are a page's and the pair a page finds is the one whose end tags unbalance the stack. **`<template>`** is a real insertion mode now, and its contents are a `DocumentFragment` on the element rather than its children. Form-control predicates and form ownership. |
| `src/css` | Tokenizer, parser, selectors — including the **functional pseudo-classes** `:not()`, `:is()`, `:where()` and the four `:nth-*()` with the whole `An+B` grammar, nested to a bounded depth, and the specificity rule that makes `:where()` worth zero — cascade, computed style, user-agent sheet, HTML presentational attributes, backgrounds including images, the flex properties, `position`/`inset`, `overflow`, min/max sizing, **custom properties and `var()`** — inherited, nested, with fallbacks and the invalid-at-computed-value rule — **`calc()`** with one relative term plus an absolute offset, **`@supports`** answered by *applying* the declaration rather than by a list of names, and `aspect-ratio`, and a **media query evaluator** (`MediaQuery.h`) for width, height, orientation and resolution with `and`/`or`/`not` — which `srcset` and `<source media>` use and which **`@media` does not yet**: a prelude with a parenthesis in it still drops its whole block, which is ledger session 49. Matching lives in `SelectorMatch.cpp` and is a pure function of (element, selector) that never sees a token — including the **dynamic pseudo-classes**, which read a bit `src/engine` wrote rather than asking anything about a mouse. **`StyleInvalidation`** is the index of ADR 0016 §3: which dynamic states any rule in the cascade depends on, and which of those can move a box, so a state change nothing is filed under costs a bitmask test. It is per-*state* rather than per-element — one `pre:hover { overflow: auto }` on Hacker News makes every hover a full relayout. `PropertyAffectsLayout` is the table behind it and defaults to *layout*, so an unclassified property is slow rather than wrong. |
| `src/layout` | Box tree, block box model, line boxes with a shared baseline and `vertical-align` against it (CSS 2.1 §10.8.1, including the two edge values that cannot be resolved until the line box exists), **a non-replaced inline box's horizontal margin, border and padding** — a run entry with no box and an advance, emitted going into the box and coming out, because the start edge belongs to the first fragment and the end edge to the last rather than to every line the box wraps onto — line breaking and `<br>`, text alignment, auto margins, min/max-content widths, per-line text fragments, replaced elements, floats and clearance, automatic table layout, **flexbox** (both axes, grow/shrink/basis, wrap, justify/align, gaps, order), **positioning** (relative/absolute/fixed with a containing-block chain), **`display: inline-block` and `inline-flex`** as real atomic inlines — block inside, one unbreakable rectangle outside, with a CSS 2.1 §10.8.1 baseline — min/max sizing, overflow clipping (and *not* on inline boxes, which it does not apply to), display-list building |
| `src/engine` | Page (one document), image selection (`srcset`, `sizes`, `<picture>`, against the viewport and the device pixel ratio, and **`loading="lazy"`, which is a box near the scrollport rather than a box at all**), PageScript (its interpreter, bindings, timers and animation frames), Loader (everything network, started/completed), PendingLoad (one navigation in flight), Engine (routes messages, drives the load), **`DocumentPolicy` — this document's Content-Security-Policy plus the origin `'self'` names and the base its relative URLs resolve against, which is where `<base href>` lands and where every CSP question resolves a URL, once**. Hit testing for links, form controls and event targets; form submission; navigation from a click. **Geometry as a service** (ADR 0015): the `bindings::GeometrySource` a page's `getBoundingClientRect` and `getComputedStyle` are answered through, laying out synchronously when the document changed under the last one and counting it as `layout.forced_by_script`. **Dedicated workers** (ADR 0022 §1): `Workers` owns the threads, `WorkerScope` owns what a script standing in one can see -- a `DedicatedWorkerGlobalScope` with events, timers, `location`, a **synchronous** `importScripts` (the one place a thread blocks on the main loop), and the document-free half of the binding layer, so `URL`, `TextEncoder`, `crypto`, `Blob`, `fetch` and `XMLHttpRequest` in a worker are the page's own implementations reached through a `DomBindings` with a **null document**. Fetches a document's subresources **concurrently** and runs its scripts at the three points `defer`, `async` and `type=module` actually mean -- and now fetches an image **after** the navigation that carried the document is over, which is the first request this browser makes outside a load. |
| `src/bindings` | **The namespaced half of the DOM** — `createElementNS` keeps what it validates and does not fold case, the six `…NS` attribute operations match on (namespace, local name) while `getAttribute` matches a qualified name in any namespace, `getElementsByTagNameNS`, and `lookupNamespaceURI`/`lookupPrefix`/`isDefaultNamespace`. **`innerHTML`, `outerHTML`, `insertAdjacentHTML` and `template.content`** (ADR 0020 §6) — one call into `html::ParseFragment`, because the *context element* is the whole algorithm and four call sites deciding it independently is four chances to build a tree no other browser would. A `<script>` inserted that way does not run. **`fetch`, `Headers`, `Request`, `Response`, `AbortController` and `XMLHttpRequest`** (ADR 0020 §1) — the last of those a *shim* over the first, in the same pending table, so `DeliverFetchResponse` settles a promise or drives a readyState machine and there is no second request path; a synchronous `open()` throws, because this browser has one loop, over a `NetworkSource` this module *declares* and the engine implements — no `net` on its `allow:` line, and no same-origin comparison anywhere in it, because every such decision was made before the answer arrived. `response.body` and `fetch` itself are **absent** rather than stubbed when there is nothing behind them. **`requestAnimationFrame`**, which schedules a frame only while something has asked for one. The seam between script and the document, and the only module that sees both `js` and `dom`. **`IntersectionObserver` and `ResizeObserver`** (ADR 0018 §5) — geometry sampled once per frame at the one place a frame is produced, and delivered only when the answer changed: never from inside the scroll that caused it, sampled in full before anything is delivered, and scheduling nothing, so a page with no observer costs a pointer comparison per frame. **HTML's reflected IDL attributes, as a table and twelve algorithms** — `Reflection.h` is the vocabulary, `ReflectionTable.cpp` is ~430 rows of (interface, property, attribute, kind), and `ReflectedAttributes.cpp` is each algorithm once, including transcriptions of HTML's own integer and floating-point parsing rules (which are *not* `util::ParseInt`: that one rejects trailing garbage and these must stop at it). The fifty hand-written accessor pairs this replaced are the shape that guarantees `td.colSpan` clamps and `col.span` does not, from the same paragraph of the same specification. `nonce` reflects one way only, which is CSP's nonce-hiding rather than a quirk, and every `aria-*` and `role` is a **nullable** string -- three states, because `aria-checked` absent means "not a checkbox" and `aria-checked=""` means "a checkbox in no state". **A real type hierarchy** — Node/CharacterData/Element/HTMLElement and the per-tag interfaces, so `instanceof` answers and a class can extend HTMLElement; methods live on prototypes rather than on every wrapper. **Custom elements** — the registry, upgrade in place, and the connected/disconnected/attributeChanged reactions. **MutationObserver**, batched and delivered as a microtask, with `observe`'s argument checking and the full record -- `previousSibling`/`nextSibling`/`attributeNamespace`, and **one** record for a replacement rather than one per half, which is the DOM's "suppress observers" flag threaded through the four insertion primitives. **The ParentNode/ChildNode mixins** (`append`, `prepend`, `replaceChildren`, `before`, `after`, `replaceWith`) are `src/bindings/NodeMixins.cpp`: one specification section rather than six methods, because each begins with "converting nodes into a node" and ends in a validated pre-insertion -- before which `el.append(el)` built a **cycle**, which is a hang rather than a wrong tree. **`NodeFilter`, `createTreeWalker` and `createNodeIterator`** — a cursor over a tree a page's own filter may change under it, so every step re-reads the tree and a throw out of the filter stops the walk rather than becoming a "reject". **`document.implementation.createHTMLDocument`, and with it a `document` that is no longer *the* document**: the whole `document.*` surface lives on `Document.prototype` and every query resolves against its **receiver**, which is the inversion same-origin iframes need and the reason `DOMParser` was absent. **`MessageChannel`/`MessagePort`**, delivered as a *task* through `TimerQueue::QueueTask` — a microtask would starve exactly the work a page uses a channel to yield to. **`matchMedia`**, through the geometry seam rather than a media context of its own, so it cannot disagree with the cascade or with `innerWidth`; `change` fires from the one place per frame that already samples geometry. **`Range`** — two boundary points and one ordering function behind `collapsed`, `commonAncestorContainer`, `compareBoundaryPoints` and `toString`; the content-mutation half is absent rather than approximate. **The event hierarchy**, and a constructed event that is an instance of its own constructor: UIEvent under Event, Mouse/Keyboard/Focus/Input under UIEvent, Wheel/Pointer/Drag under Mouse. `window` is an event target. **Events** a page makes and dispatches, untrusted by construction. `DocumentFragment`. Element-scoped queries and the element-only walk. **Geometry** — `getBoundingClientRect`, `offsetWidth`/`offsetHeight`, `clientWidth`/`clientHeight`, `window.innerWidth`/`innerHeight` and `getComputedStyle`, over an interface this module *declares* and the engine implements, so no `layout` appears in its `allow:` line; absent entirely when there is no layout behind them. **`URL`, `URLSearchParams`, `FormData`, and the URL decomposition attributes on `<a>` and `<area>`** — `src/bindings/UrlObject.cpp`, all of them over the one parser in `src/url`, which is on this module's `allow:` line for that reason and does not widen ADR 0008's model because `src/url` cannot see `js` or `dom` either. What it replaced was a *string cut* that had no idea what a special scheme is and disagreed with the real parser for every input it did not consider; deleting it moved four thousand web-platform-tests subtests. A `URL`'s state **is its href**, re-parsed on every access, because the standard's serializer round-trips and a second representation is a second thing to fall out of step. `<base href>` is answered from the tree on every call rather than remembered, because a script can rewrite it between two link resolutions. `window`/`location`/`navigator`, element lookup and the simple selectors, attributes, `classList`, `style` (via `Proxy`), `dataset`, tree walking, creation, removal and reordering, `textContent`, event listeners with click dispatch and bubbling, and the timer queue. **Focus** — `document.activeElement`, `focus()`, `blur()` and the four focus events, over the one copy of focus that lives on `dom::Document`; the engine's click and Tab reach the same algorithm, because two ways to change focus is how `activeElement` ends up disagreeing with where the next keystroke goes. Where every same-origin check will live — ADR 0008. |
| `src/platform` | The only module that knows what a window is, and the only place the process sleeps. SDL, the system font database, and the descriptor wait live here. |
| `src/js` | JavaScript, and as near complete as the language gets here. Lexer, parser, a bytecode compiler and machine (names resolved to slots, calls that cannot leak a scope keeping bindings in the frame, the tree-walker kept as the differential engine behind `MICROBROWSER_JS_TREEWALK=1`), mark-sweep heap with an ephemeron pass. **Realms** (ADR 0042) — many globals in one interpreter and one heap, each with its own `js::Intrinsics`, because `frames[0].Array === Array` answering false is the observable a page detects a second global with; the well-known symbols are *shared*, because a `for...of` compiled in one realm has to find the same `Symbol.iterator` as an array made in another or spreading a cross-realm array silently produces nothing. A callable records its realm and the running realm follows the **callee**, so a builtin allocates from the realm its function came from; the stamp is applied by `Heap::AllocateObject` rather than at the six places a callable is made, because a function with the wrong realm is a child frame's code against the parent's global rather than a wrong answer. Bounded at 64 — the count is page-controlled. **Modules** — every `import`/`export` form, `import.meta`, `import()` — with the host supplying the resolver. Classes with accessors, `super`, private fields and methods, static blocks, `new.target`, the brand check. `Proxy` with every trap, and subclassing a builtin. Full `ToPrimitive`. **UTF-16 string indexing over UTF-8 storage.** Property attributes and integrity levels. `ArrayBuffer`, the nine typed arrays and `DataView`. A real `Date` with a computed calendar and a parser. `JSON` with replacer, reviver, indent and `toJSON`. A backtracking regular expression engine with `/u` code points and `\p{...}`. Symbols, iteration, `Map`/`Set`/`Weak*`/`WeakRef`, Promises and the microtask queue, and **every form of suspending a call** — `async`/`await`, generators, `yield*` with real delegation, async generators, `for await`. No `eval` and no `Function(source)`, and a test says so. **A compiled function carries the source offset of every instruction**, so an error's stack names a place (`at HS (@1814415)`) rather than only a fault — offsets rather than lines, because the scripts this is read against are minified. The compiler's instruction bound is a *ratio* against source length, not a flat cap: a 10.7MB bundle is a large program and not blowup, and refusing one silently handed it to the tree-walker. Knows nothing about the DOM. Deviations are listed in `docs/js-conformance-roadmap.md`, each with its reason. |
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

**A page can now make its own requests.** `fetch` goes out through the same `RequestQueue`, the
same `privacy::Verdict` and the same connection pool as an image, plus CORS; an `AbortController`
cancels one (`RequestQueue::Cancel`, `Loader::Cancel`), and a navigation cancels them all. What a
fetched fragment becomes nodes through `innerHTML`, `outerHTML` and `insertAdjacentHTML`, which
are the fragment parsing algorithm with a context element (session 14). **A `<script>` that arrives
that way does not run**, because `PageScript::Collect` gathers a document's scripts once, when the
document is parsed — which is the specification's behaviour and the property that stops
`el.innerHTML = userText` from being code execution. `DOMParser` is deliberately absent: it returns
a *Document*, and `document.getElementById`/`body`/`head` are bound to the binding layer's one
document rather than to their receiver, so a second one would answer about the main page.

**Loading is asynchronous and the loop still blocks in one place** — ADR 0011 landed. A request is
started and completes on a later turn; the platform wait takes sockets alongside the deadline it
already took; concurrency is bounded per partition key. Name resolution is the one call left that
blocks, and it is one per host rather than one per resource. A display list carrying an image still
serializes the bitmap inline rather than naming it in a resource table. Roadmap in `README.md` and
`AGENTS.md`.

## Where To Pick Up

**Read this first: `main` and `master` were merged on 2026-08-15, and one whole subsystem was
decided by that merge rather than by a session.** Two machines worked from the same base for three
days without either seeing the other, so several things were built twice. What survived:

- **The WPT `.py` handler table is the path-keyed one** (`wpt::RunHandler`, dispatch on the whole
  repo-relative path). The name-keyed table built in parallel was dropped, and with it ~17
  transcribed handlers, the `raw` response a malformed status line needs, and
  `MICROBROWSER_WPT_HANDLER_REPORT=1` — the ranked list of handlers a run actually asked for and
  did not get. **That report is the first thing worth rebuilding**; it is what told a session which
  handler to write next, and picking from grep instead is the mistake TD-0061 records.
- **The worker global scope** (`src/engine/WorkerScope.{h,cpp}`) and **realms plus the frame host
  half** (ADR 0042, `engine::RealmBoundScript`) both landed and are both in this tree. They were
  written against different copies of `PageScript`, so read the sections below together: every host
  entry into a document's script now goes through the realm guard, including the worker paths.
- **Seven expectation files are unmeasured against this tree** — `cors`, `dom`, `encoding`,
  `fetch`, `resource-timing`, `url`, `xhr`. Each side re-recorded different areas from the same
  base, so neither side's numbers describe the merged binary. **Re-record before planning against
  any of them.** This is the exact failure that cost 217,000 subtests once already (TD-0058's
  closing note): *re-record every area a merge touched, not just the ones its branch named.*


**Superseded 2026-08-17 — the table below was ranked by tests-blocked, which was the best signal
available then and is not the ranking now.** The current one is `docs/wpt-firefox-gap.md` and the
plan's §2 "The order", ranked by test files Firefox passes and we do not.

**Gate 0 (F2, then F9) landed the same day and it changed that ranking too — read the plan's §2
before trusting any milestone order you remember.** All 20,998 reftest files — 48% of the suite —
are recorded now, and with them in the number **M-E (layout) is first at 9,285 files, nearly double
M-F**, where the ranking-without-reftests had it third. `css/CSS2/` alone (task E1) went from 40
files to **3,907**, the largest single task in the tree. The aggregate went 11.6% → **29.3% of what
Firefox passes**, which is not a change in the browser: it is half the suite entering the
measurement. **E1, F6, O1, O2 and O4 depend on nothing** and are 5,574 files between them.

**Gate 0 is B6 and nothing else** as of 2026-08-31: F10 landed, so `ctest` runs the pixel half
too. What kept it out was eight intermittents; they were nine, and seven of them were one bug in
the browser — a font-stack resolve cache that was never dropped when an `@font-face` arrived, so
whether a web font applied at all was a race between the fetch and the first layout.
Two of the four rows below have since landed. Keep the table for the caveats under it, which still
hold.

**What is actually blocking the WPT score, ranked, measured 2026-08-12.** 8,417 tests report *no
subtests at all* — the harness never reached `done()`, so none of them is visible in the pass rate.
That bucket is the score, and it is three projects rather than a long tail:

| tests | needs | where |
|--:|---|---|
| 2,446 | a worker global that can run testharness (`.any.worker.html`) | ADR 0022 §1, task G5 |
| 1,083 | an `<iframe>` | ADR 0027 + **ADR 0042 §5** — steps 1-4 landed 2026-08-13; see below for what is left |
| ~2,214 files | **our own server's `.py` handlers** — the path-keyed table answers ~26 paths; the rest are still 501 | ADR 0040, task **A2** |
| 226 | the module loader | — |

Read that table with two caveats. **Workers are bigger by count and smaller by information**: a
`.any.worker.html` is the same assertions as its `.any.html` twin, and the twin mostly passes
already. And **the third row is the only one that needs no browser feature at all** — `tools/wpt/Server.cpp`
answers 501 to every `.py` handler *and* to every method that is not GET or HEAD, which is probably
the cheapest points in the tree and had not been written down before A2.

**The `.py` handlers are a path-keyed table and there is no report to rank them by.** ADR 0040 §2's
condition was met on 2026-08-14: `tools/wpt/Handlers.{h,cpp}` is a closed list keyed by
repo-relative path, because three different `redirect.py` files exist with three different
behaviours and a basename would apply one of them to all three. `?pipe=` landed with it and is the
larger half — hundreds of tests ask for a status or a header on an *ordinary static file*, and a
server that ignored the query served the file as itself, which is a wrong answer rather than a
missing one. The server also reads a request **body** now, which it never did.

**Picking the next handler is currently a guess, and that is the debt to close first.** The obvious
ranking — how many test files *name* a handler, counted with grep — is wrong, and knowing why is
what the dropped report bought: it puts `gentest.py` first at 3,766 files, `generate.py` at 2,085
and `build-compute-kind-widget-fallback-props.py` at 802, and **none of those three is ever
fetched**. They are the generator scripts that produced the tests, named in a header comment in
every file they generated. A reference is not a request. Nor is a request a value: a handler a test
polls inflates its own count, and `stale-script.py` at 806 requests belongs to a directory of
**six tests**. Check `--list <dir> | wc -l` before writing one.

`tools/wpt/Handlers.cpp` is compiled into `microbrowser_tests`, so a transcription is checkable in
milliseconds rather than in a suite run. Add a test with each handler — a wrong transcription cost
a day to find once.


**`fetch/metadata/`'s 87 tests are not a handler problem, and this is the worked example of
the above.** `record-headers.py` landed and the directory went 3.8% → 4.0%. What blocks it is three
features: `window.open()` returns null so every generated `element-*.sub.html` dies at
`win.document` (task **J6**, wanting ADR 0042 §5 underneath); `helper.sub.js` builds its origins from
`{{ports[https][0]}}` and this server binds HTTP only (task **H9**); and `Sec-Fetch-*` request
headers, which this browser never sends — `grep -rn Sec-Fetch src/` is empty. The third is what the
tests assert on and is therefore *last*, not first: it gains nothing until the other two land.

**A2's first 21 handlers landed 2026-08-12, with POST**, and the arithmetic is the argument for
the rest: xhr 135 → 120 harness failures, cors 7 → 8, fetch 204 → 192, and **672 recorded subtest
failures became passes**. They also found a browser bug no page had ever hit — a response to HEAD
carries no body whatever its `Content-Length` says, and `net::ResponseParser` waited for one, so
**every HEAD request hung the browser forever**. That is the shape to expect from this task: the
handlers are not the point, what they let the suite reach is.

**The expectation files are stale in the *pessimistic* direction.** 599 tests sampled from the
harness-failure bucket and re-run against the current tree produced 8,283 subtests with 6,327
passing. Re-measure before planning against any number in `docs/wpt-baseline.md` not dated today.

**`<iframe>`'s lifecycle landed 2026-08-12, and its realm landed later the same day — the *language*
half of it.** A script-appended frame loads, `iframe.src = other` re-navigates, `srcdoc` parses
inline, a 404 still fires `load`, and `load` fires from a *task* — 31 `dom/` tests went TIMEOUT → OK.
Then **ADR 0042** gave `js::Interpreter` many realms: each with its own global, global scope and
`js::Intrinsics`, a callable recording which realm it belongs to, and the running realm following the
*callee* so a builtin allocates from the realm its function came from. `frames[0].Array === Array` is
false, the well-known symbols are shared so a protocol still crosses, and the collector walks every
realm. Nine tests in `tests/JsRealmTests.cpp`; cost +2.9% on the JS microbenchmarks, measured by
interleaving and recorded as TD-0060.

**The host half landed 2026-08-13 (ADR 0042 §5 steps 1-4), and the *guard* is the part to read
before touching any of it.** §5 said a missed realm guard is a same-origin escape rather than a bug,
and said not to solve it with a `RealmScope` at each of ~40 entry points. So it is a type:
`engine::RealmBoundScript` is `PageScript`'s only friend, `PageScript`'s constructor is private, and
its `operator->` hands back a proxy holding the scope for the full expression. **A method added to
`PageScript` tomorrow is guarded because there is no other way to call it.** Do not add a second
route to that object.

What works now: a same-origin child runs its own script in its own realm of the embedder's
interpreter (a cross-origin one gets its own interpreter, which *is* the isolation);
`iframe.contentWindow` is the child's real global and `contentDocument` is that window's own
`document`, so the two are one object; `parent`, `top`, `window[i]` and `window.length` answer; a
frame's window exists the instant the element is inserted, because the suite reads it in the same
script turn; and `window.location = x` navigates and throws a `SyntaxError` on a URL that does not
parse. `url/` went **97.9% → 99.8%** on that last pair alone (188 subtests, all of
`url/failure.html`'s third case), `custom-elements/` + `shadow-dom/` + `domparsing/` gained **141
subtests with none lost**, and `dom/` lost its one crash.

**Two findings from it are worth more than the feature.** First, a `DomBindings` can now be
destroyed while the heap holding its natives lives on -- `f.src = other` does exactly that -- so the
old `kOwnerSlot` *address* was a use-after-free three lines of script could reach, found as a
segfault in `dom/events/scrolling/scroll-cross-origin-iframes.html`. It holds a never-reused serial
now (`bindings::OwnerIdentity`); a liveness check on the address would **not** have been enough,
because the allocator reuses addresses and a stale native would then find a live layer belonging to
another document. Second, `js::kMaxRealms` was bounding realms *ever made* rather than realms alive:
`url/failure.html` appends, reads and removes one `<iframe>` 188 times, and past the 64th none of
them ran script. `Interpreter::RetireRealm` gives the slot back.

**What is left, and the first two are the same shape: a child has script but not the engine's
services.**

- **No `NetworkSource`/`HistorySource`/`StorageSource`/`CookieSource` reaches a child `Page`**, so a
  frame's `fetch` is undeclared (`fetch/api/abort/keepalive.html` times out on exactly that). Wiring
  them is not a one-liner: `Engine::StartFetch` resolves against `page_.Policy()`, so a child's
  relative URL would resolve against the *embedder's* base and its `connect-src` would be checked
  against the embedder's policy.
- **A form inside a frame fires `submit` and never navigates.** `Engine::RunFrameScripts` drains a
  child's queued activations -- without that a child's `element.click()` recorded an activation
  nobody performed and every promise waiting on it hung, which is how three
  `fetch/security/dangling-markup/` tests went from OK to a 20-second TIMEOUT. The submission is
  dropped, because a child navigation the engine drives is not built.
- **`postMessage` between realms** (§5 step 5) and `frameElement` are absent.
- **A wrapper made before an adoption keeps the realm it was made in.** `WrapperFor` delegates to
  the node document's layer and cross-document `appendChild` works at all now (the node's
  `unique_ptr` was parked in the layer that made it, so the insertion silently did nothing), but
  `dom/nodes/node-creation-realm.html` and `node-realm-mixed-across-adoption.html` still fail on it.
  Those two were passing before **only** because `contentWindow.document` was the embedder's own
  document, so nothing ever crossed a realm.

**2026-08-14 was a long session and its lesson is one sentence: three of the five largest causes in
the WPT baseline were capabilities this browser already had that the tests could not reach, and two
more were tables the code's own comments said were incomplete.** Nothing in this paragraph was a
missing feature nobody had thought of. Before reading an area's failures as a specification gap,
check whether the harness can reach the feature at all -- one `--verbose` run and a look at the
*server's* log lines for 501s, and a grep of the output for `not implemented` and `is not defined`
harness errors rather than subtest failures.

| what it was | worth |
|---|---|
| a worker had **no global scope**, with a complete thread-and-heap implementation behind it | 1,763 files |
| the input path existed and nothing exposed it to `testdriver.js` | 1,158 tests |
| ADR 0040 §2's `.py` condition had been met, and ten files were most of it | 512 tests |
| the named character reference table was **42 of 2,231** | 2,189 subtests |
| no `@@toStringTag`, so every platform object was `[object Object]` | 438 in one file, `idlharness` everywhere |
| `hsl()` computed to **black**, and `rgb(1 2 3 / 0.5)` was four components | `css/css-color/` 11.2% -> 45.4% |
| CSSOM stored unparsed values and never canonicalised | `colors-007.html` 0 -> 100% |

**And three tests in `tests/` asserted what the code did rather than what the specification says** --
`element.click()` being trusted, `aria-checked=""` reflecting as `""`, `&notareference;` coming back
untouched. When a WPT subtest disagrees with a local test, read the local test first, and look at
whether its comment argues from the specification or from the old behaviour.

**Two process rules learned the hard way, both in `docs/session-log.md`:** a re-record measures the
binary it *started* with, so nothing that changes behaviour may land while one is in flight; and
`grep -rl <data-file> third_party/wpt` before implementing from a WPT data table, because one of
them turned out to be included only by a `.tentative` file that contradicts the shipped test.

**Dedicated workers gained a global scope on 2026-08-14, and it is the largest single unblock the
suite has had.** `engine::Workers` had owned a thread and a heap since session 38 and every test of
it passed; what a script standing in that heap could *see* was `postMessage`, `self`, `name` and
`onmessage`. **1,763 of web-platform-tests' 42,185 files are a `.any.worker.html` variant** -- the
same assertions as the `.any.html` file beside them -- and every one was a twenty-second timeout:
1,726 of the suite's 7,981 recorded timeouts, in one cause. `src/engine/WorkerScope.{h,cpp}` is the
surface; `docs/session-log.md`'s 2026-08-14 entry is what it found. Three things to know before
touching it:

- **A subsystem with a complete implementation and no surface is invisible to every test of the
  subsystem.** That is the transferable lesson and it is worth checking against the other seams in
  this file before trusting one.
- **`src/url` is safe to call from a second thread and `src/dom` is not.** One is a pure parser over
  generated const tables with no lazy initialisation; the other has a process-wide namespace intern
  table. A worker's `DomBindings` is constructed with a **null document**, which is what makes it
  structurally unable to reach the tree -- so `URL`, `TextEncoder`, `crypto`, `Blob`, `fetch` and
  `XMLHttpRequest` in a worker are the *page's* implementations rather than copies.
- **Both test tools were driving the engine wrong**, and the same shape had been found once before:
  `if (Advance() || HasRunnableWork()) continue;` with `RunDueWork()` only on the else, while
  `RunDueWork` is what drains a worker's outbox and `HasRunnableWork` is true precisely when there is
  something in it. `Application::Turn` calls both every turn; a tool loop that is not that will
  diverge from it again.

What is left in a worker, in the order it blocks tests: **`WebSocket`** (`websockets/` is 532 tests
and `bindings::SocketSource` is the same shape `NetworkSource` was), `IndexedDB`/`localStorage`, and
`OffscreenCanvas` (889 tests, task F6). `SharedWorker` stays refused -- ADR 0022 §1.

**Five parallel worktrees were merged into master on 2026-08-12** — `url/`, declarative shadow DOM,
reflected IDL attributes, `dom/`, and the legacy multi-byte encodings. All five branched from the
same commit, so most of what a merge had to decide was not textual. Four decisions are worth
knowing before trusting anything in this file:

- **Two branches implemented the DOM's NodeIterator pre-removing steps.** `NodeIterators.h` (from
  the `dom/` branch) is the one that survived; the copy in `LiveRanges.h` is gone and that header
  says where it went. Verified: `dom/traversal/` is 1,584 of 1,608 with **0 unexpected results**.
- **Two branches implemented `a.href` and its decomposition attributes**, and each got a different
  half right. The merge kept the URL branch's — the real parser, base re-read from the tree — so
  the query is resolved as UTF-8 where a browser would use the document's charset. **TD-0058** has
  the whole argument and the order the fix has to happen in. Neither branch's tests cover the
  other's case, so *the suite is green either way*.
- **`DomBindings` got three features and shrank.** `InstallReflections`,
  `InstallHyperlinkElementUtils` and `InstallFrameElement` all left the class (to `Reflector`,
  `UrlObject.cpp` and the new `FrameBindings.h`), which is the cut `src/bindings/MODULE.deps` has
  been asking for across six cap raises. `NodeInterfaces.cpp` split too: the tag→interface table is
  `TagInterfaces.h`.
- **Six tech-debt entries collided on TD-0052/TD-0053** — three branches numbered from the same
  base. They are now TD-0052 through TD-0057; check `docs/tech-debt.md` rather than a number you
  remember.

**The WPT checkout had to be re-fetched** and the per-area baselines have *not* all been re-measured
against the merged tree. `docs/wpt-baseline.md` carries five separate paragraphs dated 2026-08-12,
each true of the branch that wrote it and none measured with the other four in place. The head of
`tests/wpt/expectations/shadow-dom.txt` names the one directory that is a *guess* rather than a
measurement. Re-record before planning against any of those numbers.

**Read this first: `build/` is a *Debug* build.** `build/microbrowser-perf/` is Release+LTO and is
between four and seven times faster on every page. Every number in the table below and in
`docs/tech-debt.md` up to 2026-08-06 is a Debug number -- wikipedia is 6.36s in one build and 1.13s
in the other -- so **say which build a measurement came from**, and use the perf preset for anything
you intend to compare against a real browser. The perf preset had also rotted and did not compile
at all until `566f7d9`, which is probably why nobody noticed.

**A latency pass on 2026-08-06 (`566f7d9`..`343e6a4`) found that these pages are not CPU-bound at
all, and built the instrument that says so.** Hacker News spends 1.21s of a 1.41s load blocked on a
socket, against 58ms of scoped CPU. A ranked scope summary cannot see that, because the interesting
quantity is not a duration -- it is *when* each milestone happened and how long the gaps are.

**`MICROBROWSER_LOAD_TIMELINE=1` is the answer and is the first thing to reach for on a real page.**
One navigation, one clock, printed in the order things happened with a **gap** column: the row after
a long gap is what the browser was waiting for. It stamps the navigation, every request start and
finish *with its status*, the document parse, every script, every layout and every paint. It found
four separate bugs within minutes of existing, three of which are fixed:

- **The snapshot tool never ran a due timer.** `RunLoadToCompletion` called `Advance()` and not
  `RunDueWork()`, so no page timer ever ran inside it -- and a page that armed one made
  `NextDeadlineMs()` answer zero, which span the loop **376,522 times** on youtube's front page.
  The worse half is that a snapshot was showing a document whose timers had never fired, which is
  not the page the browser draws. Fixed; youtube now lays out 73 times rather than 17.
- **A name was resolved once per connection rather than once per page.** `getaddrinfo` is the one
  call in the network stack that blocks the loop, and nothing cached it: Hacker News resolved one
  host four times, youtube thirteen times, old.reddit about thirty. Now once each. The cache is
  keyed by the **ADR 0005 partition key** and `Transport::StartConnect` grew a partition parameter
  to make that structural -- a warm name answers in microseconds and a cold one in tens of
  milliseconds, so a host-keyed cache is a "has this browser been to that site?" oracle.
- **The first paint waited for every image.** Hacker News painted at 1116ms and the last thing it
  waited for was `s.gif`, a spacer. Images no longer hold the frame (they still hold `load`, and
  ones arriving after it are decoded in a *batch*), and a background image named by a stylesheet is
  now requested when the sheet lands rather than at the next paint -- worth 375ms on Hacker News.
- **`upload.wikimedia.org` answers 429 to a burst of six parallel HTTP/1.1 connections**, which is
  why wikipedia rendered between 4 and 17 of its images at random. Reproduced with `curl` outside
  this browser and cleared of the `User-Agent`. **Fixed on 2026-08-06 by HTTP/2 (ADR 0032):
  nothing fails any more -- `engine.images_failed` is zero and `engine.images_loaded` is 21 in all
  thirty of thirty runs, over 3 connections rather than 13.** TD-0008 keeps the measurement, because it was the first
  *rendering correctness* cost anybody measured for a missing transport.

**Two benchmark files landed and are the durable half of this.** `bench/CodecBenchmarks.cpp` and
`bench/CssBenchmarks.cpp` are the first benchmarks for anything outside `gfx` and `js`, and both
exist because the alternative -- timing a page load -- is worthless on a shared machine: the same
binary read three times slower while something else was linking, which is larger than any change
either file measures. Inflate came out 2.5x (TD-0006, closed) with `inflate_fuzzer` clean over
108,781 runs. Add to these rather than timing pages.

**An older performance pass on 2026-08-06 (`299a08f`..`1121b3b`) took every target page between 6x
and 41x faster, and fixed the reason youtube rendered a white page.** Where things stand (Debug
build):

| page | before | after |
|---|---|---|
| news.ycombinator.com | 9.68s | **1.47s** |
| old.reddit.com | 34.41s | **5.06s** |
| en.wikipedia.org/wiki/CSS | 258.97s | **6.36s** |
| www.youtube.com | 82.4s | **13.65s** |

Four of the five fixes were one shape: **a question with a handful of distinct answers, asked once
per element or once per text run.** The cascade tested every rule against every element; a font
stack was resolved from scratch for every run's width, line height and ascent; custom properties
were copied into every element on inherit; a punctuator was matched against all 57 of them. Each is
now indexed or memoised. `docs/session-log.md` has the numbers and `docs/tech-debt.md` has what was
*not* fixed, with measurements.

**The white screen was a custom element's prototype being applied after its constructor ran.** A
derived class's `this` comes from its base and must already carry `new.target.prototype` when
`super()` returns, because the next line of the constructor calls the class's own methods --
Polymer's base begins `this._initializeProperties()`. Twenty-nine of thirty-two upgrades threw on
that line. It survived a previous session because the throw was *swallowed*: the early return
carried a comment claiming it was reported and nothing reported it. Five call sites had the same
`(void)CallFunction(...)` and all five now go through `Interpreter::ReportUncaught`, which carries
the stack.

**Read `docs/tech-debt.md` before optimising anything here.** Seven entries, each with its
measurement. The three that matter next:

- **TD-0005** — `engine::CollectImages` resolves the whole cascade a second time, purely to read
  `background-image` off every element, immediately before `BuildBoxTree` resolves it again. 1.58s
  on wikipedia, which is *larger than laying the page out*. The largest non-JavaScript item left.
- **TD-0003** — 1.33M individually allocated AST nodes, three quarters of the parse.
  `microbrowser_jsshell -l` (new) lexes without building a tree, which is how that was split.
- **TD-0007** — the loop still runs a page's script to completion in one turn, so youtube is a
  single 9.7-second uninterruptible call. Six times faster, **same shape**: this is the "app is not
  responding" the user reported and it is only half fixed. Wants an ADR before anything is
  attempted -- "a script yields" is a change to the execution model.

**Where youtube is now.** The bundle runs, upgrades succeed (throws 30 → 1, upgrades 32 → 59), and
it still does not render. The one remaining throw is its dependency-injection container saying it
has no provider for a key, at `EhE (@1323410)`:

    if(!V.providers.has(J)){if(P)return;throw Error("nd`"+J);}

`Wpt.prototype.resolve` picks between throwing and returning undefined with two `instanceof` checks,
so that is the first thing to verify -- this engine has had `instanceof` bugs twice now. That is a
question about the page rather than about layout, and it is readable only because a reported error
now carries its stack.

**Two instruments were added and both earned their place immediately.** `js::Parse`/`js::Compile`/
`js::Execute` split what was one row; `engine::BuildBoxTree` and `engine::LayoutBoxes` split what
was `engine::Page::Layout`, and that split *was* the first diagnosis -- 29,097ms against 22ms. Also
`js::RunScript`, `html::ParseDocument`, `engine::CollectImages`, `css::ParseStyleSheet`,
`net::DecodeContentEncoding` and `engine::DecodeImage`, each labelled with what it ran on.

**The counter lesson from this pass is worth more than the fixes.** `font.lookup_hits` read 985,000
and looked healthy -- it was counting the sized-`Font` cache, which was working, while the three
full passes above it went unmeasured. **A counter on the cheap half of an operation is worse than no
counter**, because it reads as evidence the operation is fine. `font.faces_ranked` is the
replacement: it counts the product the expensive pass actually costs.

And: `perf` and `ptrace` are both blocked in this sandbox, so there was no sampling profiler for any
of this. It did not matter. Scopes and counters answer "how many times, and in which half".

**Read `docs/roadmap-to-any-page.md` first.** It sequences ADRs 0015–0030 into sessions with a
runnable check on each, and it supersedes the ordering of this list wherever the two disagree. The
measurement behind it is `docs/surveys/2026-08-04-reddit-youtube-plex.md`, which found that
`GET https://www.reddit.com/` returns an 8KB JavaScript challenge rather than a page: reddit is not
a layout problem, and the seven small bindings that open its front door were on nobody's roadmap.

The list below is what was queued before that survey. It is still right about the next two items.

Ordered by value, not by milestone number. `docs/adr/0007-compatibility-targets.md` is the
reasoning; this is the queue.

0. **`@media` is wired (session 49).** old.reddit.com, wikipedia and Hacker News now render
   *differently* at 1280 and at 500 CSS pixels, which none of them did before, and none regresses
   at 1280. What is left of it is the design rather than the feature: the prelude is evaluated at
   parse time, so a resize re-parses the author sheets. Keeping the condition on the rule and asking
   during the cascade is the end state.

1. **The CSS math functions and the viewport units are *done*, and this entry was stale for
   several sessions.** `min()`, `max()`, `clamp()` and nested forms are in `src/css/Calc.h`;
   `vw`/`vh`/`vmin`/`vmax` resolve through `AbsoluteLengthFromUnit` against the cascade's
   `MediaContext`. Custom properties, `var()`, `calc()`, `@supports` and `aspect-ratio` were
   already done in session 4. `@supports` answers by *applying* the declaration to a scratch
   style and reporting whether it took -- there is no table of supported names to drift, and
   `CSS.supports` is the same two functions, so a page probing cannot disagree with a
   stylesheet's `@supports`. Verified 2026-08-14 by probing seventeen property/value pairs.

   **What is actually left in `css/` is a long tail of unimplemented properties**, and the
   measurement says so plainly: the largest remaining files are `*-interpolation.html`, and
   they fail on `CSS.supports(property, from)` returning an *honest* false for `translate`,
   `box-shadow`, `shape-outside`, `grid-template-rows` and the rest. There is no systemic
   harness bug behind them -- each is one property.

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

3. **`fetch`, fragment parsing, CSP, SRI and `XHR` are done (sessions 13–15). The module loader is
   not, and neither is `PerformanceObserver` — and those two are what Gate B actually needs.** A page's own request
   goes out through `bindings::NetworkSource` — declared in `src/bindings`, implemented by
   `src/engine`, the same inversion ADR 0015 used for geometry — and **CORS is enforced inside
   `net`, on the response, with the response *discarded* rather than marked**: an opaque
   `no-cors` answer is empty because the bytes were thrown away in the network half, not because
   a getter refuses them. The preflight cache is keyed by the ADR 0005 partition key. On
   www.reddit.com the challenge script's own `POST /svc/shreddit/client-errors` completes, which
   is the first request this browser has ever made because a page asked for one.

   A fetched fragment becomes nodes through the **fragment parsing algorithm** (session 14):
   `html::ParseFragment(markup, context)`, reached by `innerHTML`, `outerHTML`,
   `insertAdjacentHTML` and `<template>`. **`<template>` stopped rendering its own contents**,
   which is correct and cost www.reddit.com its sidebar — the page puts 2400 nodes inside two
   templates for its own `<suspense-replace>` to hoist, and before this they were being rendered
   because the parser dropped the tags. That page still stops at `PerformanceObserver`, so the
   roadmap's "the feed fills in" check for session 14 measures something several sessions away;
   `docs/session-log.md` session 14 has the numbers.

   `XMLHttpRequest` landed in session 15, as a shim over those objects rather than a second
   request path: it is a `bindings::ScriptRequest` like any other, sits in the *same* pending
   table as a `fetch`, and `DeliverFetchResponse` decides whether to settle a promise or drive a
   readyState machine. A synchronous `open()` throws `InvalidAccessError`, because there is one
   loop; `timeout`, `upload`, `overrideMimeType` and the binary response types are **not defined
   at all**, because each is a property a page sets and then trusts.

   **CSP and SRI landed with it.** `src/csp` is a new module at the seam `privacy` occupies, and
   the five enforcement points are all *before* the request rather than after the response:
   `script-src` in `PageScript::Collect`, `style-src` in `CollectStyleSheets`, `img-src` in the
   one `want` lambda every image URL passes, `connect-src` in `Engine::StartFetch`, and
   `form-action` in `Engine::Navigate(FormSubmission)`. `<base href>` landed too, because
   `base-uri` had no enforcement point without it.

   **What Gate B still needs, and nobody has written it down.** www.reddit.com's bundle stops at
   `ReferenceError: PerformanceObserver is not defined` — plus `performance.mark`, `measure` and
   `getEntriesByName`, which the same file calls — and past that it needs the module loader.
   `Interpreter::SetModuleResolver` is synchronous, so wiring it means either a pre-pass that
   fetches the graph or a resolver that can answer later. Decide that before writing code.
   **`.formData()` is deliberately absent** — there is no `FormData`
   class yet and a `.formData()` that answered with something else is the stub ADR 0012 forbids.

   What is *not* done from ADR 0011 and is worth knowing: **`getaddrinfo` still blocks**, one
   call per host; the wait watching sockets caps itself at 16ms because SDL exposes no
   descriptor for its own event queue (see `SdlWindow::WaitEventOrDescriptors`, which names what
   removing the cap would cost); and there is still no incremental parse or paint, so a page
   appears when it is finished rather than as it arrives. The ADR is explicit that the last of
   those is enabled by this work rather than performed by it.

4. **Transport — ADR 0010 is done, all three sections.** `Accept-Encoding` says
   `gzip, deflate` and `Connection: close` is gone. On old.reddit.com that is **697KB on the wire
   where 2.35MB would have been** (3.37x) and **20 connections and 20 TLS handshakes for 40
   fetches**, from 40 and 40. Every decompression is bounded twice — an absolute ceiling and an
   expansion ratio against what arrived — and a gzip bomb is refused from its declared ISIZE
   before a byte is produced. The pool is keyed by the **ADR 0005 partition key**, which is the
   whole privacy content of it: two top-level sites loading the same CDN host get two connections.

   **§3 landed 2026-08-06 — ADR 0032.** ALPN, framing, multiplexing, flow control and HPACK.
   Hacker News is now **1 connection and 1 TLS handshake for 6 fetches**; old.reddit.com is
   **6 and 6 for 53**, from 20 and 20 for 40. The rendering win is wikipedia, where the network
   half is now exactly deterministic: 24 fetches over 3 connections, 21 images loaded and **zero
   failed**, in thirty runs out of thirty, against 4 to 10 images drawn with 15 failures over 13
   connections on HTTP/1.1 (TD-0008, closed). Two of those thirty still *draw* less than they
   loaded, which is **TD-0011** and is a different layer.

   **Two things a next session should know before touching it.** First, the fix for TD-0008 was
   *not* HTTP/2 — it was the coalescing, because ALPN answers after the socket is open and six
   requests would otherwise have opened six sockets and negotiated `h2` on each. A protocol
   upgrade that left the pool alone would have changed nothing. Second, **`kMaxConnectionsPerPartition`
   is now the wrong number**: it bounds requests rather than connections, and six was the HTTP/1.1
   figure. old.reddit defers 91 times for 53 fetches over sessions that would each have taken all
   of them. That is **TD-0010** and it is the obvious next thing here.

   What is left of transport after that is HTTP/3, which is QUIC, which is a dependency question
   for ADR 0001 before it is a parser problem.

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
see it. Two engines agreeing is evidence, not proof — and the 2026-08-06 pass proved it again, with
a **named function expression that could not see its own name** in either engine.

**Check `MICROBROWSER_PERF_COUNTERS=1` for `js.compile_bailouts` before believing anything about a
big script.** youtube's 10.7MB bundle was running on the tree-walker for a whole session because it
blew a flat instruction cap, and every symptom looked like a scoping bug in the language. The bound
is a ratio now (`js.compiled_source_bytes` against `js.compiled_instructions`), but a bailout is
still invisible from the outside and still means the wrong engine took the program.

Known remaining gaps on old.reddit.com, which renders as of 2026-08-04: the subreddit header bar
overlaps itself — **`#header-bottom-right` is `position: absolute; bottom: 0` inside a 66px
`#header` and is placed 7px *below* its containing block's bottom edge, which is what puts the
search field under the `.side` sidebar and makes it unclickable**; the right sidebar's float is
wrong. Its scripts fail with a **masked** error —
`reddit-init.js` wraps itself in `try { … } catch (err) { r.sendError(…) }` and defines `r` inside
the try, so anything that throws early makes the catch handler throw `ReferenceError: r is not
defined` and every later script that expects `r` fails too. The reported error is never the real
one; unmasking it needs a way to evaluate a prelude before a page's own scripts, which
`microbrowser_snapshot` cannot yet do. **`www.reddit.com` is a separate problem** — a JavaScript
challenge, unaffected by the `User-Agent`; see `docs/roadmap-to-any-page.md` Phase A.

**`url/` is 9,884 of 9,903 web-platform-tests subtests (99.8%) as of 2026-08-13**, up from 97.9%:
the 188 that were `failure.html`'s `frame.contentWindow.location = badUrl` third case now pass, and
what remains is 19 subtests plus 30 timeouts of which 24 are `*.any.worker.html` (ADR 0022 §2).
Nested browsing contexts was the highest-value unbuilt feature on the previous measurement and it
is now partly built -- see the ADR 0042 §5 paragraphs above for which parts.

Known remaining gaps on Hacker News itself: `<select>` is laid out and submitted but not clickable,
`cellspacing` is not mapped because there is no `border-spacing`; and `:visited` deliberately
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
tools/run-checks.sh wpt     # -> /tmp/microbrowser-wpt.log
tools/run-checks.sh all
```

## web-platform-tests

**Read `docs/wpt-firefox-gap.md` first if the question is *what to work on*, and
`docs/wpt-baseline.md` first if the question is *did this area move*.** They are different
questions and only one of them a percentage can answer.

`docs/wpt-firefox-gap.md` counts **test files Firefox passes and we do not** — the same rule on
both sides, and a file counts only when every subtest in it passes. `docs/wpt-baseline.md` counts
subtests over subtests *we reported*, which is the right way to watch one area move between two
runs of this binary and **not** a comparison with anything: a test that dies before `done()`
contributes zero subtests to our denominator and its full count to Firefox's, so that rate rises as
we fail worse. Firefox reports 1,128,812 subtests that never enter our denominator at all, against
the 481,764 that do. `url/` reads "us 97.9%, firefox 89.9%, **done**" on 9,909 subtests where
Firefox has 15,420. Do not read `docs/wpt-firefox-ceiling.md`'s `gap` column as a distance.

Where this browser is, per test file, 2026-08-17, **both halves of the suite in the number**:
**10,900 of the 37,171 files Firefox passes — 29.3%.** 14,915 testharness files are ones Firefox
passes and we fail, **6,152 of them because our harness never reported at all**, and 11,356 are
reftests. Then `docs/adr/0040-web-platform-tests.md` before touching `tools/wpt/`, and
`docs/wpt-plan.md` before deciding what to work on — it is ranked by `firefox_gap.files` now, and
so is `docs/wpt-tasks.json`.

**That 29.3% was 11.6% the day before and the browser did not change: task F9 recorded the reftest
half.** All 20,998 of them, in **105 seconds** — the plan had projected six hours, from before the
`WaitOnDescriptors` spin fix, and that projection kept 48% of the suite out of every number for a
week. **7,394 pass, and 757 of those are two blank pages agreeing** — a reference that fails to
load renders white and matches any test that also drew nothing. The runner counts them rather than
deducting them (wptrunner compares screenshots without asking what is on them, and an invented rule
would make the two sides incomparable), and `microbrowser_wpt --reftests-only` closes with the
figure. **8,005 pass as of 2026-08-31 and they are in the `ctest` gate** (task F10). The eight
intermittents that had kept them out were nine, and the note describing them was wrong in every
clause: it said the `css/css-text/text-spacing-trim/` fonts were ones "this browser never loads",
and in fact it loaded them and then never *used* them — `platform::SystemFontProvider` memoises
what a font stack resolves to and dropped that cache only when a face was loaded from the system
index, never when one arrived from the network. A page laid out before its font arrived cached the
fallback, and the relayout the font triggered was handed the same answer. That is also why the
directory *passed more* under `--jobs 64` than under `--jobs 1`, which had been read as an
oversubscription lesson and was a font race. **An intermittent is a measurement, not an
explanation:** thirty renders of one page and `md5sum` found this in a minute where three sessions
of reading the pass count had not.

**A reftest has a tolerance and can leave a picture (task F2, 2026-08-17), and the picture is the
half that matters.** `<meta name=fuzzy>` is read at enumeration time and applied by a transcription
of wptrunner's own rule — not an invented one, because the whole point of the gap document is that
our numbers are comparable with Firefox's. 686 enumerated reftests carry an annotation and **six**
pass on it, which is the size of the effect: the tolerance was never what was failing these tests,
and F9's full run confirmed it.
What it bought is `--reftest-artifacts DIR`, which writes `<stem>.{test,ref,diff}.ppm` for each
failure — the reference washed out with every differing pixel painted yellow at one level and red
at 255. **Use it before writing layout code for a reftest area.** The first three images it ever
produced showed `css/CSS2/backgrounds/background-003.xht` missing the reference's green stripe
entirely: a paint bug, told apart from an antialiasing difference by eye in one second, where the
pixel count says only "different". Opt-in and bounded (`--reftest-artifacts-limit`, default 64),
because three 1.4MB images over a full reftest run is 90GB. A *passing* reftest reports how much
room it had left, under `--verbose`.

```bash
python3 tools/wpt/firefox-gap.py --cache /tmp/firefox-wpt-summary.json  # regenerate the ranking
python3 tools/wpt/firefox-gap.py --list-gap css/selectors               # the actual file names
```

```bash
tools/wpt/fetch.sh                                       # once; ~600MB, pinned and sparse
./build/microbrowser-perf/microbrowser/microbrowser_wpt --list | wc -l   # 44,144 in scope: 23,146 testharness, 20,998 reftest
./build/microbrowser-perf/microbrowser/microbrowser_wpt dom/             # check an area
./build/microbrowser-perf/microbrowser/microbrowser_wpt --verbose dom/nodes/Node-appendChild.html
./build/microbrowser-perf/microbrowser/microbrowser_wpt --update-expectations dom/
./build/microbrowser-perf/microbrowser/microbrowser_wpt --serve --port 8010  # browse by hand

# The pixel half. All 20,998 in under two minutes, so there is no reason to sample it --
# and `--reftest-artifacts` before writing layout code, because a pixel count cannot
# tell an antialiasing difference from a missing feature and three images can.
./build/microbrowser-perf/microbrowser/microbrowser_wpt --reftests-only css/CSS2/
./build/microbrowser-perf/microbrowser/microbrowser_wpt --reftests-only \
    --reftest-artifacts /tmp/refs css/CSS2/floats/

# Re-measure an area into the baseline document. `--summary-state` is what makes a
# sharded run add up: an area this run measured replaces what the file said about it,
# and every other row comes from the state. **The state is committed** at
# tests/wpt/summary-state.tsv -- 134 of the 297 areas as of 2026-08-17, and
# `tools/wpt/baseline.sh` is the sharded, resumable run that fills the rest (task
# B6, still open). Point at it and commit both files: a run without it rewrites the
# document down to the areas it measured, which is what the writer's row-count
# refusal is for. Until the state is complete the writer will refuse to regenerate
# docs/wpt-baseline.md at all, which is the refusal doing its job.
./build/microbrowser-perf/microbrowser/microbrowser_wpt --update-expectations \
    --testharness-only \
    --summary docs/wpt-baseline.md --summary-state tests/wpt/summary-state.tsv dom/

# The URL half of it, against `src/url` directly and in about a second. Four vector sets, all
# pinned in third_party/wpt: 891 parses, 278 setters, 87 toascii, 2,671 IdnaTestV2.
cmake --build --preset microbrowser-perf --target microbrowser_urlconf
./build/microbrowser-perf/microbrowser/microbrowser_urlconf            # all four
./build/microbrowser-perf/microbrowser/microbrowser_urlconf --show 30 setters
```

**`--long-timeout` shortens rather than lengthens, and it used to be in the line above.** The
default is 60,000 ms and a test marked `timeout=long` is the biggest one in its area. Passing
20,000 recorded 39,837 subtests for `dom/` where the default recorded 46,530 -- ~6,700 subtests
silently deleted from the measurement, reading exactly like a slow machine. **Compare the subtest
count against the previous record before committing a re-record**; that check has now caught two
different causes of the same failure (see `docs/wpt-baseline.md`).

**`--update-expectations` prints `0 unexpected` and it is not a measurement.** The counter is
guarded by `&& !options.update_expectations` (`tools/wpt/main.cpp`), so a recording run *cannot*
report anything else -- it is recording, so nothing it sees is unexpected by construction. A
re-record that scrolls past with `860/860 tests, 0 unexpected` is saying only that it finished.
This was read as evidence twice on 2026-08-15 before anyone opened the source, and it is the same
shape as the `font.lookup_hits` lesson further down this file: **a number on the half of an
operation that cannot fail is worse than no number, because it reads as proof the operation is
fine.** The real check is a second run over the same areas with `--update-expectations` *omitted* --
that is the mode `ctest` uses, it is the mode that counts, and its exit status is non-zero when the
count is not zero. Re-record, then verify; the two are different runs.

**Use the perf build.** The expectations were recorded there, and a WPT result is
timing-sensitive in one specific way — a page that has not reported inside testharness.js's own
ten seconds is a `TIMEOUT` whatever the reason, and the Debug build is four to seven times
slower on every page. `tools/run-checks.sh wpt` builds the perf preset for that reason; `ctest`
in a Debug tree passes `--timeout-multiplier 6` to compensate, which is a mitigation and not a
proof (ADR 0040 §6).

**Do not run the whole suite. The unit of work is one area, and it is under two minutes.**
This is the single most expensive mistake a fresh session can make, because the full run looks
like the thorough choice and is nearly always the wrong one. What each thing actually costs:

| what | when you run it | cost |
|---|---|---|
| `ctest -E microbrowser_wpt` | the inner loop, constantly | **4.5s**, all 24 unit shards |
| `microbrowser_wpt <area>/` | every WPT session | `css/css-text/` 51s, `dom/` 146s, `encoding/` 211s |
| `microbrowser_wpt --reftests-only` | after layout or paint work | ~90s for all 20,998 |
| the full `ctest` gate | before a push | tens of minutes |
| a **full** `--summary-state` baseline | ~never; task B6 did it | hours |

**A per-area re-record regenerates the whole `docs/wpt-baseline.md`**, because
`tests/wpt/summary-state.tsv` is committed and is that document's memory: the area this run
measured replaces what the file said about it, and every other row comes from the state. That is
what B6 bought and it is why the full run is not the price of a measurement. Pass
`--summary-state tests/wpt/summary-state.tsv` and commit both files. A run *without* it rewrites
the document down to the areas it touched, which is what the writer's row-count refusal exists to
catch — it has caught three sessions already.

`--update-expectations` writes only when the run finishes, so an interrupted full run records
nothing at all. One more reason to go an area at a time and commit each.

**`--jobs` is a budget of *CPU-active* tests, not a count of processes, and you should not need to
touch it** (2026-08-31). 6,231 of the 23,146 testharness files are already expected to TIMEOUT and
such a test sits in `poll` — twelve of them in flight hold a 24-core machine at a load average of
0.29 — so one is admitted against 0.15 of a job rather than a whole one, and the runner self-tunes
per area from the expectations it already has. That is why `referrer-policy/4K+1/` is 120s where it
was 300s, with a byte-identical result, while `encoding/legacy-mb-japanese/` correctly stays at
twelve processes and keeps all 442,614 of its subtests.

**The ceiling on processes is the test server, not the cores, and the load average will not warn
you.** Our server is single-threaded and forked once for the whole run. A first version of the
budget capped at 4× and took `content-security-policy/` from 1,652s to 64s — which is a collapse,
not a speedup: crashes 4 → 223, reported subtests 3,868 → 389, every child killed by SIGPIPE. It is
invisible below full scale (the directory that crashed is byte-identical at both settings when run
alone, because it is too small to reach the ceiling). The cap is 2× and the argument is in
`tools/wpt/main.cpp`; raising it is a question about the server, and the machine is idle either way.

Every test runs in **its own process**, so a hang is one `TIMEOUT` line and a crash is one
`CRASH` line rather than the end of the run. The server is ours and single-threaded, forked
before anything else exists — no Python, no `/etc/hosts`, no WebDriver. Cross-origin comes
from `*.localhost`, which glibc resolves to loopback and `url::Host::IsLoopbackOrLocalhost`
already treats as local.

`tests/wpt/expectations/*.txt` records **only failures**; PASS is the default. A newly
passing subtest fails the run exactly like a newly failing one, and the diff of those files
is what a session delivers.

**The first thing it found was in the harness path itself**: testharness.js runs its
completion callbacks in one loop with no `try`/`catch`, so a throw inside `show_results` eats
every callback after it — and `show_results` calls `insertAdjacentText`, which this browser
does not implement. A page whose tests had all run reported nothing at all. That is the
argument for the whole thing in one bug: four missing lines, invisible on every real page,
silently destroying a reporting path.

**After a run, READ `/tmp/microbrowser-<target>.log` instead of rebuilding and rerunning.**

## Working The Roadmap One Session At A Time

`docs/roadmap-to-any-page.md` is the argument; `docs/roadmap-sessions.json` is its state — one
entry per session, with the check that finishes it. `/next-session` reads that ledger, picks the
lowest unfinished session, implements it, runs its check, commits, records what it found in
`docs/session-log.md`, and stops.

```bash
tools/agent-loop.sh -n 5                      # five roadmap sessions, each a fresh process
tools/agent-loop.sh -c /next-wpt-task -n 5    # five WPT tasks, ranked by the Firefox gap
```

**Two ledgers, and picking the wrong command works silently.** `/next-session` reads
`docs/roadmap-sessions.json` and sequences by which page a session unblocks; `/next-wpt-task` reads
`docs/wpt-tasks.json` and ranks by test files Firefox passes and we do not. "Continue with the WPT
work" means the second one. `docs/wpt-plan.md` supersedes the roadmap wherever the two disagree.

The process boundary is the point: a long conversation fills with the debris of work already
committed and starts reasoning about its own transcript instead of the repository. Each iteration
starts from nothing and rebuilds from the three things that survive — the git log, the session log,
and the ledger. **A session that cannot hand off through those three files has not finished.**

`status: done` means the session's `check` was *run* and *passed*. Nothing else may set it; a
session wrongly marked done costs the next agent a whole session to discover.

**Build the *whole* sanitizer preset, not just `microbrowser_tests`.** `DecoderClient/ConfigureFlushRoundTrip`
spawns `microbrowser_decoder` from its own build tree, so a preset where only the test binary was built
fails that one test with `configure` — which looks exactly like a real regression in the IPC path and is
not. `cmake --build --preset microbrowser-asan` with no `--target` is the fix. (Cost me two
false-positive investigations on 2026-08-12.)

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
MICROBROWSER_LOAD_TIMELINE=1   # one navigation on one clock, in order, with a gap column
MICROBROWSER_JS_TREEWALK=1     # run script on the tree-walker instead of the bytecode machine
```

**A ranked summary ranks *work*; it cannot see *waiting*.** On a page whose cost is round trips it
adds up to a tenth of the wall clock and says nothing about the other nine. `MICROBROWSER_LOAD_TIMELINE=1`
is the instrument for that half, and `wait::Network` is the row that says the loop was blocked
rather than busy -- the `wait::` prefix is the convention, and a `wait::` row must never be read as
a hotspot.

**Read `MICROBROWSER_PERF_SUMMARY=1` before believing anything about where time goes, and split any
scope that covers two jobs.** Every fix in the 2026-08-06 performance pass came from one such split;
none came from reading code. The seams that are scoped today are `engine::BuildBoxTree` /
`engine::LayoutBoxes` (split from the single `engine::Page::Layout` that hid a 29,097ms-against-22ms
difference), `js::Parse` / `js::Compile` / `js::Execute`, `js::RunScript`, `html::ParseDocument`,
`css::ParseStyleSheet`, `engine::CollectImages`, `engine::RebuildAuthorStyleSheets`,
`net::DecodeContentEncoding` and `engine::DecodeImage` -- the last five labelled with the file or
byte count they ran on, because a page that serves twenty-six scripts is not diagnosed by a row that
says "script".

**A counter on the cheap half of an operation is worse than no counter.** `font.lookup_hits` read
985,000 and looked healthy: it was counting the sized-`Font` cache, which was working, while the
three full passes over the machine's fonts above it went unmeasured and cost 227 seconds.

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
- `docs/adr/0031` — the codec decision: which decoders, from where, and in what process
- `docs/adr/0032` — HTTP/2: sessions, connection coalescing, and what a *shared* connection changes
- `docs/adr/0033` — privacy first, correct, and very fast: Ladybird's shape, LibreWolf's defaults, from scratch
- `docs/adr/0034` — the JavaScript heap is a bounded, collectable resource
- `docs/adr/0035` — request concurrency is not connection concurrency
- `docs/adr/0040` — web-platform-tests as the primary correctness signal, and why the server,
  the runner and the expectation format are all ours
- `docs/adr/0041` — the Web Crypto subset behind `crypto.subtle` (renumbered from a second 0037)
- `docs/adr/0042` — **realms**: many globals in one interpreter, what is per-realm and what is shared,
  and §5's hazard — a missed realm guard in the host is a same-origin escape, not a bug
- `docs/wpt-baseline.md` — **generated**: where this browser is per WPT area, and the
  failures ranked by how many tests each cause costs. Read before picking a task
- `docs/wpt-plan.md` — the whole road from here, in milestones and parallelizable tasks
- `docs/wpt-tasks.json` — the same tasks as state: claimed, done, refused, with the check
- `docs/roadmap-to-any-page.md` — the above, sequenced into sessions with a check on each
- `docs/roadmap-sessions.json` — the same sessions as state: what is done, what the check is
- `docs/tech-debt.md` — shapes that are wrong by design, each with the measurement that says
  what it costs today. Read it before optimising anything.
- `docs/session-log.md` — what each session found that a diff does not say
- `docs/surveys/2026-08-04-reddit-youtube-plex.md` — every number those ADRs cite
- `docs/performance/m0-baseline.md` — the measurements M0 established
- `docs/performance/m1-rasterizer.md` — where paint time actually goes, and what is not hot
- `docs/performance/m6-damage.md` — what incremental repaint saves, and what it does not
- `docs/performance/m8-bytecode.md` — the machine against the tree-walker, and where the time still goes
