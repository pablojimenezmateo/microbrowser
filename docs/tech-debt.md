# Tech Debt

Things that are *wrong by design* rather than merely unfinished, each with the measurement that
says how much it costs today. A milestone that is simply not built yet belongs in
`docs/roadmap-to-any-page.md`; this file is for shapes that are already load-bearing and that a
later session will have to undo rather than extend.

The rule for an entry: it names what is wrong, what it costs *measured*, and what the end state is.
An entry with no number is a complaint, not debt. An entry whose number came from reading the code
rather than running it says so.

Opened 2026-08-06, during the pass that took en.wikipedia.org/wiki/CSS from 259 seconds to 6.4.
Everything below was found by profiling that pass and is written down rather than fixed because
fixing it is a design change, not a faster loop.

---

## TD-0001 — A layout algorithm that measures a subtree then places it walks it twice

`layout::LayoutFlexChildren` lays every item out to measure it (`FlexLayout.cpp`, the "lay each item
out at its resolved main size" loop) and then lays the same item out again to place it. A column
container does it a third time, to get a base size. `PlaceFloat` does the same pair — a detached
probe, then the real placement. Nested, these multiply: a flex container inside a flex container
inside a float walks its leaves eight times.

**Measured.** `layout.block_passes` against `layout.boxes_created`, both added for this:

| page | boxes | block passes | ratio |
|---|---|---|---|
| youtube.com | 5,288 | 53,196 | **10.1x** |
| en.wikipedia.org/wiki/CSS | 19,116 | 13,622 | 0.7x |

Wikipedia is under 1.0 because most of its boxes are text and inline, which are laid out by the line
breaker rather than by `LayoutBlock`. Youtube's 10x is the real number, and it is *not* currently
the bottleneck on that page — its layout is 340ms against 9.7 seconds of JavaScript — which is
exactly why this is written down rather than fixed. It becomes the bottleneck the moment the
JavaScript does.

**End state.** Measurement and placement are different questions and should not both be "run the
whole layout algorithm". Intrinsic sizing already has a memo (`Box::Intrinsic()`); the flex base
size and the float probe want the same treatment, keyed on the same per-pass invalidation.

---

## TD-0002 — `@media` is evaluated at parse time, so a resize re-parses every stylesheet

`css::ParseStyleSheet(text, viewport)` decides there and then whether each `@media` block's rules
exist at all. The consequence is that a parsed stylesheet is only valid for the viewport it was
parsed at, so `Page::SetViewport` throws every parse away and redoes it.

This is also the *only* reason the parsed-sheet cache added in this pass needs a key
(`DocumentResources::author_sheet_parsed`). Without it the cache would be pure: a sheet's text
cannot change once it has arrived.

**Measured.** `css.tokens` is 613,700 per full re-parse on youtube.com and 626,658 before the cache;
one wikipedia sheet takes 26ms to tokenize by itself, and youtube's largest takes 456ms.

**End state.** Named in `CLAUDE.md` already: keep the condition on the rule and evaluate it during
the cascade. Then a resize re-runs the cascade — which it must anyway — and re-parses nothing.

---

## TD-0003 — The JavaScript AST is 1.3 million individual allocations

`ParserImpl::Make` does one `std::make_unique<Node>` per node, and `Node` carries a `std::string`
and a `std::vector<std::unique_ptr<Node>>` inline — so a node with children costs at least two
allocations and a reallocation, and the tree is retained for the life of the page (it has to be:
a function object holds raw `Node*` into it).

**Measured**, with `microbrowser_jsshell -l` added for exactly this question — it lexes without
building a tree, so the parse can be split in two. On youtube's 5.86MB bundle:

```
-l  (lex only)        0.38s, 1,774,373 tokens
-p  (lex and build)   1.56s, 1,330,546 nodes
```

So the scanner is a quarter of the parse and building the tree is three quarters.

**End state.** Arena-allocate the nodes. They share one lifetime already — the whole tree dies with
the program — which is precisely the case an arena is for. This is a change to what `NodePtr` *is*,
so it touches the parser, the tree-walker and the compiler together.

---

## TD-0004 — The style resolver is thrown away and rebuilt whenever a stylesheet arrives

`Page::RebuildAuthorStyleSheets` calls `ResetResolver()` and then `AddStyleSheet` for every sheet,
which deep-copies every rule — selector and declaration list — into a fresh `StyleResolver::Entry`,
and rebuilds the rule index over all of them. A page with ten stylesheets does this ten times as
they land, and the tenth rebuild copies all 17,865 rules.

**Measured.** `css::AddStyleSheet` is 10ms over 51 calls on wikipedia — small, *now*, because the
rule index made the cascade itself 15x cheaper and the copy is no longer hidden behind it. It is
recorded because the shape is quadratic in sheets and nothing bounds it.

**End state.** A resolver that appends rather than one that is rebuilt, with the index appended to
in the same way. The obstacle is that a sheet's rules are currently owned by the resolver rather
than shared with the parsed sheet the cache now holds; those two decisions should be made together.

---

## TD-0006 — Inflate ran bit-at-a-time and byte-at-a-time — **fixed 2026-08-06**

Kept here rather than moved to Closed, because the *measurement* is the part worth keeping: the
entry said inflate ran "at roughly a tenth of the speed it should" and could not be more precise
than that, because the only way to time it was to load a page and the network's variance was larger
than the thing being measured — the same seven gzip responses on youtube.com read anywhere between
12ms and 25ms from run to run.

`bench/CodecBenchmarks.cpp` is the fix for that, and it had to bring its own DEFLATE encoder:
`src/util` deliberately has none. Two corpora, because they measure different halves — a 258-byte
match is two symbol decodes and a `memcpy`, and a three-byte match is two symbol decodes for three
bytes, and real markup is much closer to the second:

| benchmark | before | after | |
|---|---|---|---|
| `codec/inflate-symbols` | 4.286 ns/byte | **1.716 ns/byte** | 2.50x |
| `codec/inflate-copies` | 1.043 ns/byte | **0.485 ns/byte** | 2.15x |

Both halves were the textbook first implementation and both are named in the original entry.
`DecodeSymbol` called `Bits(1)` once per *bit* of every code; it now peeks nine bits and indexes a
direct table built with the canonical code assignment, falling back to the bit walk only for codes
longer than the window, which are by definition uncommon symbols. A match copied one `push_back` per
byte; it now grows once and copies, with `memcpy` when the match does not overlap and a forward
byte loop when it does — the second is not a slower `memmove`, it is the only correct thing, since
`distance` 1 and `length` 200 means two hundred copies of one byte.

The `inflate_fuzzer` target ran 108,781 executions against the new decoder with no crash, and the
suite passes under ASan.

**What is left.** This is now roughly 600MB/s on the copy-heavy corpus and 580MB/s on the
symbol-heavy one, which is within range of production zlib rather than a tenth of it. A further
step exists — a two-level table so long codes are also one lookup, and a 64-bit bit buffer refilled
eight bytes at a time — and is not obviously worth it: at these numbers decoding youtube's seven
gzip responses is under 40ms of a 4-second load.

---

## TD-0007 — The main loop blocks for the whole of a script, and that *is* the "not responding"

This is the one the user reported and it is only half fixed. The wall-clock cost came down by 6x,
but the *shape* is unchanged: `Engine::RunScripts` runs a page's script to completion inside one
turn of the loop, so a 10.7MB bundle is a single 9.7-second call during which no event is drained
and no frame is presented. The window manager offers to kill the process because from outside that
is indistinguishable from a hang.

**Measured**, per phase, on youtube.com's main bundle:

```
js::Parse     2.58s
js::Compile   3.08s
js::Execute   4.07s
```

Each of the three is one uninterruptible call.

**End state.** Not "make the loop preemptible" — the zero-idle-CPU invariant and the single-threaded
design are both deliberate and neither should go. The honest answers are to make the three phases
faster (TD-0003 is a quarter of the first one), and to reach ADR 0030's incremental parse and first
paint so that something is on screen before the script runs at all. **Design home: ADR 0036** — defer
execution slicing until TD-0003 and ADR 0030 are measured; if still needed, yield at bytecode
safepoints with microtask-atomic checkpoints, not a host poll.

---

## TD-0008 — No HTTP/2, so a burst of six connections gets rate-limited — **fixed 2026-08-06**

Kept rather than moved to Closed, because the *measurement* is the part worth
keeping: this was the first **rendering correctness** cost anybody had measured
for a missing transport, as opposed to a latency cost, and it is the argument
ADR 0010 §3 did not have when it was written.

`en.wikipedia.org/wiki/CSS` rendered **between 4 and 17 of its 19 images, at
random, from run to run**. The cause was not in this browser's loop, its
decoders or its layout: `upload.wikimedia.org` answers **HTTP 429** to a burst
of six parallel HTTP/1.1 connections, and this browser opened six because
`kMaxConnectionsPerPartition` is six and it had no other way to fetch six images
at once. Reproduced outside the browser with `curl` and cleared of the
`User-Agent`, which was the first suspicion.

**Fixed by ADR 0032.** Same page, same machine, the same afternoon — five runs
on HTTP/1.1 and **thirty** on HTTP/2, because the first five all drew 19 and
that turned out to be luck:

| | images drawn (of 19) | `engine.images_loaded` | `engine.images_failed` | connections | TLS handshakes |
|---|---|---|---|---|---|
| HTTP/1.1 | 4, 4, 7, 4, 10 | 6 | 15 | 13 | 13 |
| HTTP/2 | 19 in 28 runs of 30; 18 once, 15 once | 21, every run | **0, every run** | 3, every run | 3, every run |

Nothing fails now, and the network half is exactly deterministic: the same 24
fetches over the same 3 connections, and the same 21 images loaded, in all
thirty runs. That is the whole of what this entry was about.

Two runs in thirty still do not *draw* everything they loaded. That is a
different bug in a different layer and it is **TD-0011**; conflating it with
this one would leave a closed entry that never quite closes.

**The fix is not HTTP/2, and that is the lesson.** ALPN settles the protocol
during the handshake, which is *after* a socket is open — so six concurrent
images would have opened six sockets, each discovered independently that the
server speaks HTTP/2, and finished with six sessions carrying one stream each.
The same burst, the same 429. What fixes it is the *coalescing*: at most one
connect per origin while nobody yet knows what it speaks. A protocol upgrade
that did not change the pool would have changed nothing here.

`Http2Fetch/SixConcurrentRequestsShareOneConnection` is the regression test, and
it reports six sockets when the coalescing is commented out — checked, because a
concurrency test that passes either way is worse than none.

---

## TD-0009 — The first paint still waits for every stylesheet and every blocking script

Fixed for images in `55f7b40`; the shape remains for everything else, and on a
page whose CSS and JavaScript are large that is the whole wait. Hacker News is
now gated entirely on `hn.js`: the document is parsed at 553ms and the frame goes
out at 1111ms, of which 535ms is waiting for a 6KB script on a fresh connection.

Some of that is correct and must stay -- a stylesheet *is* render-blocking, and a
synchronous script may write to the document. What is not correct is that there
is no paint at all before it: there is no incremental parse and no first paint of
the part of the document that has arrived, so a slow subresource is a blank
window rather than a partial page.

**Measured** (load timeline, release build):

| page | document parsed | first paint | gap |
|---|---|---|---|
| news.ycombinator.com | 553ms | 1111ms | **558ms** |
| en.wikipedia.org/wiki/CSS | 229ms | 744ms | **515ms** |

**End state.** ADR 0030, which this now has numbers for.

---

## TD-0010 — Six concurrent requests per partition, on a connection built for a hundred — **fixed 2026-08-06**

`kMaxConnectionsPerPartition` was six, and the name was wrong twice over: it
bounded *requests*, not connections, and six was the HTTP/1.1 socket figure.
Over one multiplexed HTTP/2 connection the equivalent number is what the
server's `SETTINGS_MAX_CONCURRENT_STREAMS` says, which is typically a hundred.

So this browser opened one connection where it used to open six, and then used
it six requests at a time.

**Measured**, old.reddit.com, Release build, before the split:

```
net.fetches             53
net.requests_started    53
net.requests_deferred   91
net.h2_sessions          6
```

youtube.com under the same bound deferred 260 times in a Debug load.

**Fix.** Two bounds: `kMaxConnectionsPerPartition` stays six (sockets, privacy),
`kMaxRequestsPerPartition` is 64 (streams on those sockets). The queue counts
requests against the second number. A per-queue byte budget for a hundred
concurrent bodies is still not written — 64 is inside what the pages this
browser loads have been measured to cost.

---

## TD-0011 — An image can load and still not be drawn, twice in thirty runs

Found while closing TD-0008, and separated from it deliberately: with HTTP/2 in
place the network half of `en.wikipedia.org/wiki/CSS` is exactly deterministic
and nothing fails, but the page still does not always draw everything it has.

**Measured**, Release build, thirty consecutive runs:

| runs | images drawn (of 19) | `engine.images_loaded` | `engine.images_failed` | fetches | connections |
|---|---|---|---|---|---|
| 28 | 19 | 21 | 0 | 24 | 3 |
| 1 | 18 | 21 | 0 | 24 | 3 |
| 1 | 15 | 21 | 0 | 24 | 3 |

Every column except the first is identical across all thirty. The bytes arrive,
the decode succeeds, `engine.images_loaded` is 21 every time — and then one to
four of them are missing from the display list.

The load timeline on a bad run does not show the obvious cause. The last four
images complete at 1194.6ms, a layout runs from 1252.9ms to 1374.9ms *after*
them, and they are still not drawn. So it is not "the image arrived after the
last layout"; something between the decode batch and the box tree is the
suspect, and `CLAUDE.md` already names the shape — images that arrive after
first paint are decoded in a **batch**, and a batch is a place where an
ordering can be wrong without being wrong every time.

**Why it is written down rather than fixed.** It needs a different instrument
from the ones this session used. **2026-08-06:** `engine.images_drawn` landed in
`Page::Paint` (one increment per `DrawImageCommand`, so tiled backgrounds count
more than once) alongside the existing `engine.images_loaded`. The gap on a bad
run is now `images_loaded − unique drawn sources` rather than an eyeball count.
The root cause between decode batch and box tree is still open.

**End state.** The two counters first, then the fix. A one-in-fifteen rendering
difference is exactly the kind of thing that decays into "wikipedia sometimes
looks wrong" and is never chased, so it belongs here with its number.

---

## TD-0012 — The JS heap limit is a flat cell count, and a real page fills it

`Heap::limit_` is a hard ceiling on `objects_ + environments_`. Past it,
`AllocateObject` returns null and the interpreter throws `RangeError: out of
memory`. That is the right *shape* of refusal — a page must not grow forever —
but the number is not derived from anything a page costs.

**Measured**, youtube.com, Debug build, after the 2026-08-06 Polymer-upgrade
fixes (so the collector *is* running during custom-element reactions):

```
js.heap_live_peak   500000     <- equal to the then-limit
js.heap_oom              8
```

Raising the default to 2M is the unblock, not the design. The live set of a
single SPA is a property of the page, and a flat constant will be wrong again
the moment the feed fills in.

**Two bugs this measurement found that are fixed, not debt.**

1. `CallFunction` raised `call_depth_` around `CallCompiled`. `MaybeCollect`
   refuses to run when `call_depth_ != 0`, so every VM safepoint during a
   custom-element reaction (entered from C++) was a no-op — the page allocated
   straight into the ceiling. Compiled code's stacks are data; the depth flag
   is for C++ locals the collector cannot see. Removed for the CallCompiled
   path.
2. An OOM `MakeError` falls back to a bare string when the heap cannot hold an
   Error object, so `ReportUncaught` had no `.stack` to print. It now appends
   `CaptureStack` for string errors too.

**End state.** A limit expressed in bytes (or in cells with a per-page soft
target and a hard process ceiling), with `js.heap_live_peak` read against it in
the same way `js.compiled_instructions` is read against the instruction ratio.
A page that sits at 90% of the limit should be visible in the counter dump
before it throws. Design home: **ADR 0034**.

---

## TD-0013 — Real `querySelector` unblocks youtube.js into a CPU hang

`DomBindings::Matches` was a three-form toy (`#id`, `.class`, exact tag). That
was enough for early tests and wrong for every framework: a selector list like
`ytd-app,ytd-masthead`, a combinator like `div > span`, or an attribute like
`link[rel~="stylesheet"]` silently matched nothing, while
`querySelector("ytd-app")` still worked. Wiring `querySelector` /
`querySelectorAll` / `matches` / `closest` to `css::ParseSelectorList` +
`Selector::Matches` is the correct fix (and `document.scripts` /
`document.images` / `document.links` landed beside it).

**Measured**, Debug build, after that wiring (2026-08-06):

| | toy matcher | real selectors |
|---|---|---|
| youtube.com load | ~80s, 56 display-list commands, finishes | **≥5 min at ~99% CPU, never finishes** |
| `MatchesSelectorList` calls | n/a | **not a match storm** (no 50M-call abort) |
| early `querySelectorAll` | misses `script:not([nonce])`, `link[rel~=…]` | finds them, then hangs in a non-selector path |

So the hang is not "matching is too slow". Real answers let the page's own
bootstrap take a branch the toy matcher accidentally skipped, and that branch
spins without allocating (memory stable at ~3.6%) and without running enough
JS bytecodes to hit `kMaxSteps` — which is the shape of a native-heavy loop or
a `RunLoadToCompletion` busy-wait, not a selector walk.

**Update.** `MICROBROWSER_LOAD_TURN_TRACE=1` shows:

1. The 10.7MB kevlar bundle **does** finish (`script.start` / `script.end`).
2. Afterwards `Engine::Advance` calls `LayoutAndPaint` many times in one turn
   (completions / module work), each of which usually completes.
3. The hang that never returns is inside `page_.Layout()` — specifically after
   `BuildBoxTree enter` or `LayoutBoxes enter` with no matching `end` — not in
   the selector matcher and not in a top-level script.
4. Loads are **flaky**: one traced run finished in ~22s with 39 display-list
   commands; three untraced 60s timeouts on the same binary did not. Memory
   stays flat (~3.6%), so this is a loop or a pathological layout, not a leak.

**End state.** Find why layout re-enters or diverges on the post-kevlar tree
(Polymer stamp + real `querySelector` answers), fix it, and close this when
youtube.com finishes reliably under the real matcher with a command count that
is not a white page.

**Instrumentation** (2026-08-06, TD-0013 pass):

| counter | what it measures |
|---|---|
| `layout.forced_by_script` | synchronous `Page::Layout` from a geometry query mid-script |
| `layout.passes` | every `Page::Layout` entry (forced or frame) |
| `layout.pass_boxes` | sum of box counts after each pass; divide by `layout.passes` for tree size |
| `layout.runs` | `LayoutEngine::Layout` calls (one per pass, usually) |
| `layout.boxes_created` | cumulative boxes appended during tree builds |
| `layout.block_passes` | `LayoutBlock` entries; ratio to boxes is TD-0001 |

Scopes: `engine::Page::Layout`, `engine::LayoutBoxes` (inside it),
`layout/forced` (inside `EnsureLayoutClean`). `MICROBROWSER_LOAD_TURN_TRACE=1`
still prints `[load] LayoutBoxes enter/end` when the hang is a missing `end`.

Read with `MICROBROWSER_PERF_COUNTERS=1` on a snapshot. A write-read loop in
script shows `layout.forced_by_script` rising with `layout.passes`; a post-bundle
hang with flat memory shows `layout.passes` climbing while `layout.pass_boxes`
stays high and `layout.block_passes` / `layout.boxes_created` diverge (TD-0001
ratio on a pathological tree).

**Update** (2026-08-07, session 55). Shadow-root / connection fixes landed:

| fix | effect |
|---|---|
| `PrototypeFor` reads live `constructor.prototype` | `attachShadow` wrappers pass `instanceof ShadowRoot` after ShadyDOM replaces `window.ShadowRoot` |
| `NotifyConnection` / upgrade `connectedCallback` use `OwnerDocument()` | custom elements inside a shadow tree get connection reactions |
| `UpgradeSubtree` walks `<template>` contents | custom tags inside a template upgrade when a subtree is inserted |

**Update** (2026-08-07, session 56). Text bindings and attribute surface:

| fix | effect |
|---|---|
| `Node.textContent` setter on Text/Comment sets `data` | Polymer property-effects that write `textNode.textContent` no longer leave literal `[[…]]` in `Data()` |
| `isConnected` uses `OwnerDocument()` | true inside shadow trees (Polymer gates on it) |
| `getAttribute` / `attributes` / `cloneNode` / `setAttribute` keep binding tokens | Polymer can see `items="[[…]]"` to wire effects |
| upgrade still strips `[[…]]`/`{{…}}` before the constructor | leaving them there made youtube hang (unbounded Polymer deserialize/reaction); documented below |

Measured after (Debug, ~75s): **tokens:0** unbound text, **47** non-empty text nodes, **7** `dom-repeat` in the tree, **493** upgrades, **73** display-list commands. Feed still empty: **0** `ytd-rich-item-renderer`, every `dom-repeat.items` is `null`, **0** images drawn. Console warns `Polymer::Attributes: couldn't decode Array as JSON: [[computedBadges]]` / `[[shownItems]]` — tokens reach deserialize on some path after stamp.

**TD-0017** (opened here): stripping binding tokens at upgrade is wrong-by-platform and kept only because full preservation hung youtube; the end state is leave attributes alone and fix whatever reaction loop that hang was.

**Next:** why `dom-repeat.items` stays null despite browse `__data.data.contents.twoColumnBrowseResultsRenderer`; property path from browse data into the grid.

| counter | value |
|---|---|
| `layout.passes` | **114** |
| `layout.pass_boxes` / `layout.passes` | ~32k boxes per pass |
| `layout.forced_by_script` | 17 |
| `engine.paints_produced` | 94 |

Each late layout pass costs **~0.9–1.5s** (fonts arriving one-by-one still
invalidate the box tree; batching within one `Advance` turn is landed but fonts
spread across ~60s of wall clock). **The white page is no longer a layout hang**
— it is Polymer: `ytd-browse.__data` carries a `feedNudgeRenderer` ("Your
YouTube history is off") but **no feed elements are stamped** (`0`
`ytd-rich-item-renderer`, no `feed-nudge` in the tree). The painted text is
literal `[[errorMessage]]` / `[[label]]` — property effects never bound child
templates. `CharacterData.data` / `nodeValue` setters landed (spec gap) but did
not move this needle; next step is why templatize/dom-bind does not run after
`__data` is set.

---

## TD-0014 — Plex's main bundle dies at source offset 370

`app.plex.tv/desktop/` is a 30KB shell — one empty `<div id="plex">`, two SRI-marked scripts totalling
**5.47MB** (survey §4). Session 22 landed `sessionStorage`; the splash inline script runs. The
vendor bundle loads; the **3.5MB main bundle** then throws before React mounts.

**Measured**, Debug build, compatibility pass 2026-08-06:

```
microbrowser_snapshot https://app.plex.tv/desktop/
  3 commands, 0 runs, 0 fonts, 0 images, title "Plex"
  script error: TypeError:  is not a function (@370)
```

Load timeline: ~555ms to main JS start, **~2.9s** inside main-bundle execution, **~4.9s** wall.
The callee name is empty in the reported error — typical of a missing global or a minified import
the engine never bound. Session 22's note had the same shape with different wording; this pass pins
the offset.

**What is not known yet.** The symbol at offset 370 has not been minimised to a name. The survey's
API table (`docs/surveys/2026-08-04-reddit-youtube-plex.md` §5) ranks `MediaSource` (33),
`requestAnimationFrame` (113), and `localStorage` (44) on Plex's bundles; Gate D needs MSE and
decoders, but this failure is earlier — the UI never mounts. `microbrowser_jsshell -p` on a pinned
copy of the main bundle, or stack-at-offset via the youtube method (`python3 -c` around offset 370),
is the next step.

**End state.** Identify the callee at @370, implement the missing primitive honestly (ADR 0012) or
fix the bug if a name exists but throws wrong, and close when the snapshot reports non-zero text runs
and more than the three display-list commands of an empty shell.

---

## TD-0015 — YouTube's gstatic font fetches fail after a long load

Web fonts are not what blocks youtube's white page — session 20 measured **11** registrations where
there were **0**, and the page still drew **16** display-list commands with no text. After the
2026-08-06 compatibility pass, the bundle runs (~13s script, ~87s wall) and the page draws **56**
commands with **0 images** and near-white output; a separate class of failure appears **late** in
the timeline.

**Measured**, Debug build, `MICROBROWSER_LOAD_TIMELINE=1`, 2026-08-06:

| Milestone | Time |
|---|---|
| kevlar bundle execution | ~9.3s |
| layout passes (×2) | ~14.3s each (see TD-0013) |
| gstatic font fetch failures | **~39–58s** |

Console during the run: connect failures / "server stopped responding" on `fonts.gstatic.com`
requests. `gfx.web_fonts_registered` can be non-zero while individual subset fetches still fail —
unicode-range splits Roboto into many files (session 20: 23 `@font-face` blocks → 2 requests when
working). Whether this is **connection exhaustion** after an ~87s load (TD-0010 fixed request
deferral but not aggregate open sockets), **CDN rate limiting** (same shape as TD-0008 on
wikimedia), or a **sandbox/network burst** has not been isolated — it was not reproduced with
`curl` outside the browser in this pass.

**End state.** Counters that pair `gfx.web_fonts_registered` with `gfx.font_load_failures` and
`net.connections` at failure time, then either fix the exhaustion path or document a host limit.
Rendering text in Roboto is necessary for a readable youtube.com but is not sufficient for thumbnails
(Polymer templatize — session 53: **6 fonts load ok**, **0 images**, browse `__data` has feed JSON
but DOM never gets feed/nudge elements; see TD-0013 update).

**Update** (2026-08-07). Session 53: Roboto and YouTube Sans subsets **complete** (timeline shows
200s on gstatic); snapshot reports **6 fonts, all ok**. One unrelated script error:
`google.com/js/th/…` uses `eval` (refused by design). Font load no longer blocks the white page.

---

## TD-0016 — reddit's feed waits on polyfill chain, then `<suspense-replace>` hoisting

Session 14 fixed `<template>` parsing: reddit puts the feed and sidebar inside
`<template for="s_8a5ed_0">` and `<template for="s_8a5ed_1">` (**729** and **1668** nodes) for its
own `<suspense-replace>` custom element to hoist into the light DOM. Before that fix, the parser
dropped template tags and the sidebar rendered by accident — **331 commands** dropped to **143**
when templates became inert, which is correct.

Session 50 landed `PerformanceObserver` and the module loader (ADR 0037). After spread-`super()`
(`9689730`) the page draws **214 commands / 48 script runs / 1 image** — search box, user menu, and
sidebar card render. **Gate B is still not met:** the feed does not fill in.

**Measured**, www.reddit.com, 2026-08-06 (after session 51 template-parser fix):

```
display_list.commands       214
js.modules_loaded           3
js.dynamic_imports          0
js.compile_bailouts         0
performance.observer_callbacks  5
```

**Session 51** fixed the **es-module-shims parse error** (`SyntaxError: expected ';'` at line 370):
`LexTemplate` and `SplitTemplate` ended substitutions at the first `}` inside nested template text
(`with{type:"css"}`) or mis-read `/ '/g` as a single-quoted string. es-module-shims now parses and
executes (**42s** script time, no syntax error).

**Session 52** fixed **`Object.create` with a properties argument when called unbound** (core-js
keeps `Object.create` in a local and calls it as `u(proto, desc)` while wiring
`AggregateError.prototype`). The implementation reached `defineProperties` through `this`, which is
`undefined` on an unbound call — `TypeError: undefined is not a function` at @19450. core-js now
runs to completion (**299s** script time); the full polyfill chain loads (es-module-shims,
regenerator-runtime, intersection-observer, custom-elements-es5-adapter) with **no script errors**.

**Measured**, www.reddit.com, 2026-08-06 (after session 52 `Object.create` fix):

```
display_list.commands       214
js.modules_loaded           3
js.dynamic_imports          0
js.compile_bailouts         0
performance.observer_callbacks  5
```

No `runtime-concat` / `ac-render-template` bundle fetch appears in the timeline yet; **0 feed
articles**. The next blocker is concat `type=module` bundles not fetching or executing after the
polyfill chain completes.

**Session 53** fixed **es-module-shims `initPromise` never settling**: the polyfill builds feature-
detection scripts with `new Blob` + `URL.createObjectURL`, loads them as trusted `<script src=blob:…>`,
and completes feature detection via `window.postMessage` from a hidden iframe. This browser had no
`Blob`, no blob URL loader, no `window.postMessage`, and trusted script inserts were only collected
after the running script returned — so the probe script ran too late and `initPromise` hung.

Landed: `Blob`, per-document `BlobUrlRegistry`, `URL.createObjectURL(Blob)`, loader `blob:` decode,
`window.postMessage` + `message` events, `HTMLScriptElement.noModule`, `HTMLScriptElement.supports`,
synchronous `ProcessDynamicScripts` flush on trusted `<script>` insertion, synthetic `esms` message
after trusted iframe insert, and immediate drain of sync loader completions.

**Measured**, www.reddit.com challenge interstitial, 2026-08-06 (after session 53):

```
display_list.commands       214
js.modules_loaded           3
js.dynamic_imports          0
```

Timeline now shows `blob:null/1` feature-detection script **during** es-module-shims execution (not
after the polyfill chain). The full shreddit feed page (post-challenge, ~299s polyfill run) still
needs a snapshot pass to confirm `runtime-concat` fetches; this environment often lands on the
`js_challenge=1` interstitial instead.

**Session 54** fixed **es-module-shims `initPromise` on the runtime iframe path**: session 53's
synthetic `['esms', …]` message fired only on *trusted* iframe insert (blob probe), but the polyfill's
main feature detection inserts an iframe from ordinary script after `addEventListener('message')`.
This browser has no `iframe.srcdoc` or `contentDocument`, so the real probe never posts back. Now any
`src`-less iframe insert delivers the same synthetic tuple when a `message` listener is registered.

**Measured**, www.reddit.com `js_challenge=1` interstitial, 2026-08-06 (after session 54):

| metric | before (82b109a) | after (session 54) |
|---|---|---|
| snapshot wall time | ~5 s | **>360 s** (killed at timeout; 99% CPU) |
| `js.modules_loaded` | 3 | not yet captured (timeline flushes at exit) |
| `runtime-concat` in timeline | absent | not yet captured |
| feed articles | 0 | 0 (expected until polyfill + hoisting) |

The five-second snapshot was `initPromise` still hung; the multi-minute CPU-bound run is the concat
polyfill chain executing — the expected shape after session 52's **299 s** measurement. Gate B still
needs a completed snapshot (feed articles > 0, `ac-render-template` registered) and
`<suspense-replace>` hoisting.

**End state.** Let the challenge-interstitial polyfill chain run to completion and confirm
`runtime-concat` fetches in the timeline; implement `<suspense-replace>` hoisting for the `for=`
templates; close when the snapshot shows feed posts rather than an empty main region.

---

## TD-0017 — Template contents were upgraded (binding tokens lost before parse)

`getAttribute`, `attributes`, `cloneNode`/`importNode` and `setAttribute` keep `[[…]]` /
`{{…}}` values. `attributeChangedCallback` skips binding-token values so Polymer does not
`_deserializeValue` them as JSON.

**Root cause (2026-08-07).** `template.innerHTML = …` inserts into `template.content`, a
host-less `DocumentFragment`. `InsertFragmentChildren` upgraded custom elements there, and
`UpgradeElement`'s post-constructor strip removed every `data="[[…]]"`. Polymer's
`_parseTemplate` therefore saw bare tags with no host bindings — browse never pushed
`twoColumnBrowseResultsRenderer` into the two-column host.

**Fix.** `DocumentFragment::IsTemplateContent()` marks `<template>.content` so
`InsertFragmentChildren` skips `UpgradeSubtree` there (`dom.template_content_upgrade_skips`).
Do **not** treat every host-less fragment as inert — ShadyDOM stamps into one without
`Host()`. Live stamped hosts still strip binding tokens after the constructor until the
stamp path is cheap enough to leave tokens alone.

**Status** (2026-08-07, Release). Skip is enabled. A youtube home snapshot finishes
(~3 min Release) with masthead and two-column hosts in the tree; `ytd-rich-item-renderer`
is still **0** (server often serves only a `feedNudgeRenderer` when history is off, and
stamp still burns `js.steps_exhausted`). Post-load hang was mostly reflow-on-every-rAF
(see TD-0018 layout skip).

**Close when** youtube home applies browse→two-column `data` without `-eval`, rich-grid
stamps, and the live-host strip can be deleted without a hang (related: TD-0018).

---

## TD-0018 — youtube.com's kevlar turn exhausts the JS step budget mid-stamp

`js::Interpreter::kMaxSteps` (20M) is a hang guard for `while (true) {}`, shared by one
top-level script turn and every nested custom-element reaction / microtask. On
`https://www.youtube.com/` (Debug, 2026-08-07) the kevlar bundle spends that budget before
page attach finishes, then dozens of `connectedCallback`s throw `RangeError: script ran too long`
(~88 console lines; `js.steps_exhausted` **343**, `js.steps_peak` **20 000 343** on one Debug run).

**What the page looks like afterwards.** Browse `__data.data.contents.twoColumnBrowseResultsRenderer`
is present (selected tab has `richGridRenderer`). `ytd-two-column-browse-results-renderer` is in
the tree with empty `#primary` / `#secondary`. **0** `ytd-rich-grid-renderer`. The two-column
host stamps via `YtRendererstamperBehavior` on computed `content` from `data` — that path is
what the aborted reactions never finish.

**What does not fix it.**

| Attempt | Result |
|---|---|
| Raise `kMaxSteps` to 100M+ | Multi-minute 99% CPU hang (unmasks a reaction/layout loop; not "just more budget") |
| Reset `steps_` on every empty-frame `CallCompiled` | Hang: each microtask got a fresh 20M after kevlar spent the first |
| Skip native CE reactions when `usePatchedLifecycles` | Hang: ShadyDOM's `aY` does not replace native connect for these hosts here |

**What did land.** `BeginHostTurn()` resets the budget at top-level script entry
(`RunCompiled` / `RunProgram`) and records `js.steps_peak` / `js.steps_exhausted`.
Timer / event resets were tried and pulled: after kevlar spends the budget, a fresh
per-timer budget lets a post-script storm run forever. Nested `CallCompiled` must
not reset either (same hang via microtasks).

**Update** (2026-08-07). Three hang-guard bugs stacked: (1) the step check ran
*before* instruction fetch, so `UnwindToHandler`'s `ip - 1` pointed outside the
`try` range and `catch` never saw `RangeError: script ran too long`; (2) after a
caught exhaustion, `continue` with `steps_` still past the limit rethrew every
iteration; (3) nested `RunCompiled` (module eval from dynamic `import()` while
frames were still live) called `BeginHostTurn` and reset the budget — youtube's
stamp storm never hit 20M. Check is after fetch; one absorption resets for
`catch`/`finally`; nested RunCompiled shares the outer budget; a second
exhaustion in the same host turn aborts **and abandons frames** — returning with
frames still on the machine left every later `-eval`/`Run` sharing a spent budget
(`BeginHostTurn` skips reset while frames remain).

**Also** (same day). `Page::Layout` early-outs when the box tree, cascade and
laid-out width already match (`layout.skipped_clean`). `LayoutAndPaint` used to
reflow on every rAF after a settled stamp; that was why enabling TD-0017 never
finished a snapshot even after the load loop exited. Snapshot still paints a
sparse chrome (history-off nudge) until feed/stamp complete.

**Also** (same day). `DeliverFetchResponse` calls `Interpreter::BeginTask()` so a
network answer is a fresh host turn. Timers/rAF still must **not** reset — that
hang remains. Guide data (`/youtubei/v1/guide`) and search stamp continuations
were dying on a spent budget after kevlar.

**Also** (same day). Every macrotask `BeginTask`s — MessageChannel, idle
callbacks, and `setTimeout`/`setInterval`. Nested `setTimeout(0)` without the
HTML 4ms clamp after five nestings was the hang that made “timers must not
reset” look true; with the clamp the loop sleeps rather than spins. Host-task
in-turn drain (capped at 64) and mutation-aware `InvalidateLayout` remain.

**Measured** (2026-08-07, Release, `/results?search_query=cats` after the above):

| metric | value |
|---|---|
| `ytInitialData` `videoRenderer` nodes | **20** |
| first `ytd-item-section-renderer.__data.contents` | **16–21** (many `videoRenderer`) |
| `dom-repeat` `items.length` under that section | **2** |
| stamped `ytd-video-renderer` | **2–3** |
| `js.steps_exhausted` | **1** |
| snapshot wall | **~35–50 s** (was hang / multi-minute) |
| display-list commands | **~300–360** |

So the list’s Polymer `contents` arrives, but the `dom-repeat` that should
stamp it only ever sees two items — incomplete binding/observer application,
not “stamp sliced and drained short”. Slot assignment also fixed (only the
first same-named `<slot>` receives light children).

**Update** (2026-08-08). The “2 of 16” reading was wrong about *which* repeater:
section contents use `YtRendererstamperBehavior` + `YtLazyListBehavior`
(`initialCount` 4, autofill via `_.Ot` → rAF → `tryRenderChunk_`), not the
metadata `dom-repeat`. Three platform holes blocked autofill:

1. `children` lived on `Node.prototype`; ShadyDOM noPatch only captures *own*
   `Element.prototype.children` into `__shady_native_children`, so
   `polymerDom(container).children` was `undefined` and
   `stampDomArraySplices_` threw on expand.
2. `characterData` MutationObserver records were never emitted; Polymer’s ASAP
   (`_.Ub` / text-node bump) never ran, so `tryRenderChunk_` never reached rAF.
3. Snapshot’s post-load drain stopped when nothing was due *now*, skipping the
   16ms rAF spacing the chunk chain needs; rAF also lacked `BeginTask`.

After those: **`shownCount` reaches `length_` (~18–20), ~12–14 `ytd-video-renderer`
stamp**, `canShowMore` false. Residual: `js.steps_exhausted` still 1 mid-stamp
on some runs.

**Update** (2026-08-08). Thumbnail hosts were **500×0** because youtube sizes
search thumbnails with

`ytd-video-renderer[use-standard-config-width] ytd-thumbnail…:before {
  content:""; display:block; padding-top:56.11% }`

and this engine **dropped every `::before`/`::after` rule**, then resolved
percentage padding against font size (→ 0). Landed: parse + cascade
`StyleForPseudo`, empty `content:""` generated boxes, and padding `%` of
containing-block width written back as used pixels. Search results now lay out
at **500×280**; above-the-fold lazy imgs get `src` via IntersectionObserver
(observe schedules a frame; zero-height intersection inflate remains for imgs
that are still 0×0 until load; observer callbacks `BeginTask` so they do not
inherit a spent stamp budget). Below-fold stays `src`-less until scrolled —
correct lazy behaviour. Snapshot's post-stamp drain keeps turning after the
observation rAF so thumbnail fetches can finish.

**Update** (2026-08-08, later). Search result clicks now navigate. Three hit-test
gaps stacked on top of the sized thumbnail: (1) closed `tp-yt-app-drawer` covers
the viewport with `visibility: hidden` and we did not implement visibility;
(2) `yt-interaction` ink layers sit above `#thumbnail` and want
`pointer-events: none`; (3) abspos descendants under an `overflow: hidden`
ancestor whose padding box does not cover them were gated out of the hit walk
even though their border boxes covered the point (flat scan found
`covering_links=1` while `LinkAt` answered none). Landed visibility +
pointer-events, elevated-child visit without the clip gate, `LinkAt` /
form hit-tests `EnsureLayoutClean` after script, and `translate3d(x,y,0)` as
2D translate. Check: `-click` on a cats result focuses `a#thumbnail` and the
URL becomes `/watch?v=…`.

**Update** (2026-08-08, watch geometry). The watch player shell is no longer
`880×0`. Three platform gaps stacked:

1. `var()` after `/` or `*` was not recognised (`calc(var(--h)/var(--w)*100%)`),
   so youtube's `#player-container-inner` padding-top never applied even though
   the `/_/ss/` bundle and the default `--ytd-watch-flexy-*-ratio` customs were
   present. Fixed in `SubstituteVarsDepth`.
2. Normal-flow `height: 100%` ignored a parent's definite height, so
   `ytd-player` stayed 0 inside the abspos `#player-container`. Fixed in
   `LayoutBlock`.
3. Non-strict bare calls bound `this` to `undefined`, so the player IIFE's
   `var window=this` left `window` undefined and threw
   `Context has not been set and window is undefined`. OrdinaryCallBindThis
   now uses the global object for non-strict functions; `'use strict'` on a
   function (or enclosing script) still gets `undefined`.

After those, `#player-container-inner` / `#player-container` / `ytd-player` /
`#container` are **880×495**. `setAttributeNS` (namespace ignored, like
`createElementNS`) removed the bootstrap TypeError. Remaining watch gap: the
automatic `preparePlayer` path leaves a stub `player_` API
(`isReady`/`destroy`/`getLastError` only) with an empty `#container`; forcing
`ytd-player.initPlayer_()` stamps `#movie_player` + `<video>` at 880×495 with
empty `src` (playback / MSE / googlevideo still open). `js.steps_exhausted`
still fires in MutationObserver / CE reactions on some watch loads.

**Update** (2026-08-08, watch plays). MSE appends were already succeeding
(UMP + bare WebM clusters). Three playback gaps stacked on top:

1. `PageScript::RunTiming` read `e.stack` after the script `error` event
   drained microtasks and collected the thrown Error — flaky SIGSEGV on
   watch (ValueRoot; same UAF the class already documented for RunCompiled).
2. `canPlayType` still returned `""` while `MediaSource.isTypeSupported`
   returned true; wired to the same allowlist, answers `"probably"`.
3. Decoded VP9 frames were rejected by `Surface::Update` because the
   surface was sized from track metadata (often `0×0` → guessed `640×360`)
   while the bitstream was another size. Recreate from the frame; advance
   `currentTime` from `timestamp_us`.

Measured (Release, `/watch?v=jNQXAC9IVRw`, click player, `muted` + `play()`):

| metric | value |
|---|---|
| `media.video_sessions` | 1 |
| `media.decoder_samples_fed` | ~130 |
| `media.decoder_frames_applied` | 30–60 |
| `video.currentTime` after drain | **~2 s** |
| player-region unique colours in ppm | **~240** (was near-uniform) |

Gate C's "no video plays" is therefore out of date for watch after a gesture.
Remaining: `videoWidth`/`videoHeight` bindings, audio out through the ring,
home-feed stamp (history-off nudge), and `js.steps_exhausted` on some loads.

**End state.** Close when `js.steps_exhausted` is 0, search results show
painted thumbnails for every in-view row without `-eval` force, watch plays
(or honestly shows chrome without a skeleton that never upgrades), and home is
honest about nudge vs UA. Related: TD-0017 (binding-token strip), TD-0007 /
ADR 0036, TD-0001 (`layout.block_passes` still huge on search).

---

## TD-0019 — Decoded Opus frames never reach the audio ring

Watch video paints (TD-0018 update 2026-08-08): VP9 frames blit into ADR 0013
surfaces. The matching Opus `DecoderClient` is configured for youtube's audio
SourceBuffer and samples are pushed, but `PageVideo::AdvancePlayback` discards
audio `PollFrames` results (`sample_count != 0`) and never opens an
`media::AudioSink` / writes an `AudioRing`.

**Measured.** Same Release watch run that applied 30–60 video frames:
`media.video_configure_failures` is typically **1** (the audio track's
configure or the unused path), `audio.devices_opened` is **0**, and playback
is silent even when `muted` is false after a user gesture.

**Why it is written down.** Session 24's ring, clock and SDL sink exist and are
tested; session 27's decoder emits PCM. The missing piece is the engine-side
owner that starts a sink on `play()`, converts decoder PCM into ring frames,
and joins the device before `main` returns — ADR 0028 §4's ownership statement,
not a one-liner in `PageVideo`.

**End state.** A gestured unmuted watch plays sound; an idle page with no media
still has no audio thread; `audio.devices_opened` tracks the sink lifetime.

---

## Closed

- **TD-0005 — `CollectImages` duplicated the cascade** (2026-08-06). Background URLs are queued
  through `ImageProvider::WantImage` during the one `EnsureBoxTree` pass; `CollectImages` walks
  `<img>` tags only. `EnsureBoxTree` caches the box tree by mutation version and
  `StyleResolver::Generation()`, skipping rebuild when neither changed. Debug build,
  `en.wikipedia.org/wiki/CSS`: `engine::CollectImages` **2351 ms / 3 calls → 16 ms / 7 calls**;
  duplicate `ForEachStyledElement` gone. `AddImage` still invalidates the box tree when intrinsic
  sizes change — that audit is separate from the duplicate-cascade problem this entry named.
- **TD-0010 — request concurrency was the HTTP/1.1 socket bound** (`…`). Split into
  `kMaxConnectionsPerPartition` (6, sockets) and `kMaxRequestsPerPartition` (64, streams).
- **The cascade asked every rule about every element** (`299a08f`). 18,360 rules against 686
  elements, per layout, seventeen times: 29.1 of youtube's 29.2 seconds. Rules are now filed under
  the most selective part of their subject compound. 15.4x.
- **A font stack was resolved once per text run** (`2ed98d9`). Three passes over the machine's fonts
  — every file, every loaded face, then the catalog's match — 985,000 times against 490 faces, each
  comparison allocating a string. 227 of wikipedia's 259 seconds. 8.5x.
- **Custom properties inherited by copying** (`d160a6e`). Every element copied its parent's whole
  set; a page that declares its palette on `:root` copied fifty string pairs into each of 19,000
  elements, per cascade pass. Copy-on-write. 4.7x.
- **A punctuator was lexed by walking all 57 of them** (`8e30e3a`). Indexed by first byte. 1.6x on
  the parse.
