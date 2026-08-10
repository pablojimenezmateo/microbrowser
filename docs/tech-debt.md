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

## TD-0001 — A layout algorithm that measures a subtree then places it walks it twice — **fixed 2026-08-09**

`layout::LayoutFlexChildren` laid every item out to measure it and then laid the same item out
again to place it. A column container did it a third time for a base size. `PlaceFloat` and
atomic-inline placement did the same probe-then-place pair. Nested, these multiplied: a flex
container inside a flex container inside a float walked its leaves eight times.

**Measured before** (Release, youtube search, 2026-08-08):

| metric | value |
|---|---|
| `layout.block_passes` | **189 863 331** over 128 passes |
| `engine::LayoutBoxes` | **127 644 ms** self (101 calls, avg 1.26 s) |
| snapshot wall | **~157–272 s** |

**Fix.** Geometry is already absolute, and `OffsetLaidOutSubtree` already existed for relative /
absolute placement. Wire it through the three probe-then-place sites: flex place when the final
forced sizes match the measuring ones (including stretch that does not change the measured cross
size), float place after the size probe, and atomic-inline place-on-line. Stretch that *does*
change a cross size still re-lays out. Counters `layout.measure_cache_hits` /
`layout.measure_cache_misses` now mean translations vs forced re-layouts.

**Measured after** (Release, same URL, 2026-08-09):

| metric | value |
|---|---|
| `layout.block_passes` | **138 558** |
| `layout.measure_cache_hits` | **62 997** |
| `layout.measure_cache_misses` | **7 583** |
| `engine::LayoutBoxes` | **732 ms** self (103 calls, avg 7 ms) |
| snapshot wall | **~28 s** |

Layout is no longer the wall. Next on that page: `engine::BuildBoxTree` (~3 s) and the JS
compile/execute of the kevlar bundle.

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

**Measured**, www.reddit.com post-challenge (Gate A passes), Release build, 2026-08-08:

| metric | before late-script fix | after (`31e78e4`) | after CSP trust (`46ddc28`) | after connect-src + `whenDefined` (this session) | after BeginTask + challenge settle (this session) |
|---|---|---|---|---|---|
| snapshot wall time | ~726 s | ~608 s | **~731 s** | **~581 s** (challenge interstitial; post-challenge not reached) | **~597 s** (still interstitial; one run) |
| `display_list.commands` (final frame) | 210 | 210 | **3150** | **212** (challenge URL; peak **4026** mid-load) | **210** (challenge URL; peak **4407** mid-load) |
| `csp.violations` | 72 | 95 (`script-src`) | **3** (`connect-src` `data:` only) | **0** | **0** |
| `js.modules_loaded` | — | 3 | **8** | **7** | **8** |
| `js.steps_exhausted` | 2 | 2 | 3 | **2** | **3** |
| small `js/concat` `script.start` | yes | yes | yes |
| large `runtime-concat` `script.start` | no | no | **yes** (concat chain runs) |

Late-script completions that arrive after `load_` clears now run (regression test
`Engine/ALateScriptRunsAfterTheLoadHasFinished`). **CSP transitive trust** propagates from
permitted scripts through promise microtasks, `fetch` continuations, timers, rAF, and idle
callbacks (`TrustedScript` + `Interpreter` microtask tagging). That cleared the 64
`script-src` concat refusals; the three remaining violations were `connect-src` refusals of
`data:text/javascript,…` bootstrap URLs from `fetch()`, which is not a network connection --
`Engine::StartFetch` now skips `connect-src` for `data:` and `blob:` the way `Loader` already
answers them locally. **`customElements.whenDefined`** landed (reddit waits on several names);
`define` batch-upgrade gets a fresh step budget via `BeginTask`. **TD-0018 host-turn gaps:**
`InsertFragmentChildren` custom-element upgrades, `MutationObserver` delivery, and
`PerformanceObserver` callbacks each `BeginTask` so a spent concat-polyfill budget does not
abort the next DOM reaction. Snapshot keeps turning while `js_challenge=1` is in the URL (15 min
cap). Gate B still needs a reliable post-challenge feed snapshot (`article` count > 3,
`display_list.commands` >> 212): one Release run after these fixes still landed on the
interstitial (210 commands, `eval: true:0:undefined`) despite `engine.script_navigations` 1 and
`js.modules_loaded` 8 — the second navigation returned interstitial-sized HTML, so the next
investigation is whether the computed `solution` is wrong or the response is being mishandled.

**Investigated 2026-08-08 (session after `81f476d`).** Verdict: **the challenge algorithm and
form-submission path are correct in isolation; the live failure is the second HTTP response
body (~8 KB interstitial, not ~405 KB feed), so the server is rejecting the submission.**

Evidence:
- `Engine/RedditInterstitialHtmlSubmitsDoubledSeed` and
  `Engine/AsyncDomContentLoadedChallengeSubmitsAfterAwait` pass with the exact inline script and
  form shape from a live `GET https://www.reddit.com/` interstitial (seed doubled via
  `await (async e => e + e)(seed)`, GET `action="/"`, `requestSubmit()` after `onsubmit` copies
  `location.search`).
- `curl -A microbrowser` with the same doubled-seed URL returns **511 KB / 3 `<article>`** — the
  honest UA is not the blocker.
- Interstitial HTML is **one inline script + one hidden form**; there is no concat polyfill on the
  challenge page. The ~600 s wall clock is the **feed** page's scripts, not the interstitial.
- `js_challenge=1` remains in the feed URL too (curl lands there with 3 articles) — do not use
  that query param alone to detect interstitial vs feed; use body size or `article` count.
- `LoadTimeline` now stamps `navigation.form` (submission URL) and `document.arrived` (byte size +
  final URL) for the next live run.

Still open on the wire: why the browser's second request gets ~8 KB again while the scripted test
and curl agree on the correct GET shape. Next checks: `navigation.form` solution param in timeline,
`document.arrived` byte count on nav 2, and whether `edgebucket` from the first response is on the
submission request.

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
`Host()`. Live stamped hosts still strip binding tokens **before** the constructor
(moved 2026-08-09: Polymer's `_initializeProperties` deserializes a present boolean
attribute as true, so `hidden="[[data.hideContents]]"` used to become `hidden=""` via
reflect before the old post-constructor strip ran). Template contents keep tokens for
`_parseTemplate`.

**Companion (not debt).** `document.all` as HTML [[IsHTMLDDA]] landed with the strip
move: without it, polymer_resin's `!Z && Z !== document.all` treated every undefined
sink as `"zClosurez"`, and `HTMLElement.hidden = undefined` stuck the attribute —
blanking youtube search even after UA `[hidden]` and the reflection were correct.

**Status** (2026-08-07, Release). Skip is enabled. A youtube home snapshot finishes
(~3 min Release) with masthead and two-column hosts in the tree; `ytd-rich-item-renderer`
is still **0**. **Update** (2026-08-09): that zero is usually the server's
history-off `feedNudgeRenderer` / `richSectionRenderer` ("Try searching to get
started"), not a failed rich-item stamp — confirm content kinds before blaming
the stamper. Masthead search Enter is closed (TD-0026). **Update** (same day):
`ytd-app { min-height:100% }` now fills the viewport ICB (see session log);
remaining empty-looking home is TD-0028 (page-manager / nested flex height).

**Close when** youtube home applies browse→two-column `data` without `-eval`, rich-grid
stamps *when the response contains `richItemRenderer`s*, and the live-host strip
can be deleted without a hang (related: TD-0018).

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

**Also** (2026-08-09). MSE is the other gap: `appendBuffer` delivers `updateend`
*before it returns*, so script frames are still live and `BeginHostTurn` is a
no-op. youtube's SABR pump (`iuT` → `d9` → `vW`) was aborting mid-link with
`script ran too long`, leaving `d9` empty; the player maps
`this.d9.shift().info` / slicer throws to `fmt.unplayable` (TD-0020). Fix:
`Interpreter::MediaEventBudget` on each SourceBuffer / HTMLMediaElement event
delivery — fresh hang-guard allotment per event, capped at
`5 * kMaxSteps` across the reentrant chain so this cannot reopen the stamp hang.
Counter: `js.media_event_budget_resets`.

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

**Update** (2026-08-08, pointer press/release). The engine now dispatches
`pointerdown`/`mousedown` on press and `pointerup`/`mouseup`/`click` on
release (ADR 0017 §1). User activation moves to press. youtube's
`getPlayer().playVideo()` after a click still returns state `-1` — the stub
player path does not wire through to `<video>.play()` even though a gestured
`video.play()` succeeds (MSE + VP9 paints ~2s). **HTMLImageElement**
`complete`/`naturalWidth`/`naturalHeight` and `load` events on decode landed
the same day; search `loaded` images went from **0 → 4+** in snapshot probes.

**Update** (2026-08-09, NestedHostBudget for CE upgrades). Above-fold search
thumbs were sized (replaced %) and IO sampling ran (`view.intersection_records`),
but `img.onViewportEntered` was never installed: `u5m`/`SE3.observe` runs from
Lit `connectedCallback` during the stamp, and `UpgradeElement`'s `BeginTask`
was a no-op under live rAF frames — first-chunk upgrades shared one spent 20M
allotment and aborted mid-stamp (`hasData` true, `hasEnter` false, empty `src`).
`Interpreter::NestedHostBudget` generalises the MSE media budget: refresh when
the machine is empty, when nesting past the first NestedHostBudget, or when the
shared allotment is already half spent — not on every cheap upgrade (that would
re-open the stamp hang). `ElementUpgradeBudget` in `UpgradeElement`; counter
`js.element_upgrade_budget_resets` (only when a refresh happens under live
frames or nested NestedHostBudget depth). Acceptance: after Accept, no scroll,
in-view thumbs have `onViewportEntered` and a non-empty `src` — **met**
(Release 2026-08-09: `inViewNoEnter:0`, `inViewNoSrc:0`, `js.steps_exhausted`
absent, `js.steps_peak` ≈ 10.3M).

**End state.** Close when `js.steps_exhausted` is 0, search results show
painted thumbnails for every in-view row without `-eval` force, watch plays via
the page's own click handler (not only `-eval video.play()`), and home is
honest about nudge vs UA. Related: TD-0017 (binding-token strip), TD-0007 /
ADR 0036, **TD-0001 closed** (search layout no longer the wall), **TD-0020**
(youtube `playVideo` stub), **TD-0023** (img.src recollect). Inline replaced
`width/height: 100%` now resolves (2026-08-09) — above-fold thumbs are sized
500×281; empty `src` / `visibility:hidden` until IO assigns was the NestedHostBudget
gap above.

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

**Closed 2026-08-08.** `PageVideo` owns the ring and clock, converts decoder
s16 PCM to float, and `Start`s the borrowed `AudioSink` only while unmuted and
playing. `Application` owns `SdlAudioDevice` and injects it; mute/pause/`Clear`
call `Stop`. Counter `media.audio_frames_written`; test
`PageVideo/TheSinkFollowsMuteAndPauseWithoutDecoding`. Headless hosts may still
report `audio.devices_opened` 0 when SDL has no playback device — that is
`AudioDeviceUnavailable`, not this debt.

---

## TD-0021 — BuildBoxTree is still whole-document after every stamp/font turn

After TD-0001, youtube search's wall moved to `engine::BuildBoxTree`: **~2.0 s
self over ~95 calls** (Release, 2026-08-09), against **~0.45 s** of
`LayoutBoxes`. Each rebuild walks the flattened tree, resolves style, allocates
boxes, and rebuilds the element→box index.

**What already landed** (same day, then 2026-08-09 evening):

| change | counter |
|---|---|
| `RunScripts` invalidates only when `MutationVersion` or cascade generation moved | `engine.box_tree_script_skipped` / `_invalidated_by_script` |
| `AddImage` attaches pixels in place when both axes are definite without the bitmap | `engine.box_tree_image_paint_only` / `_invalidated_by_image` |
| Computed-style cache keyed by cascade gen + structure version + attr version + state + parent style id | `css.style_cache_hits` / `_misses` |
| Invalidation provenance at font / due-work / sheet sites | `engine.box_tree_invalidated_by_{font,due_work,sheet}` |
| `Document::StructureVersion` vs attribute-only `MutationVersion` | — |

On youtube search (Release, after the cache): **~1.4 s** BuildBoxTree / 108
calls (was ~2.8 s / 112), with **~57k cache hits** against **~51k misses**. Font
and image rebuilds that keep the same structure now reuse cascade answers;
Polymer attribute stamps miss only the hosts that changed. Whole-tree box
*allocation* remains — dirty-subtree rebuild is still the end state.

**End state.** Dirty-subtree box rebuild keyed on mutation provenance (ADR 0016
selector dependency graph). The style cache is necessary but not sufficient:
unchanged chrome still allocates fresh boxes on every stamp. `due_work` is the
largest remaining invalidation bucket after script/image/font/sheet are counted.

**Update** (2026-08-09, watch). `RunDueWork` treated every CSS-animation frame,
every decoded video frame, and every attribute/`style` write (WAAPI polyfill)
as `InvalidateLayout` — `boxes_.reset()` plus `CollectImages` — so a watch page
paid **~50 full `BuildBoxTree`/`LayoutBoxes` passes per second** (Release turn
trace: 2930 `LayoutBoxes` in 60 s, 1.5 GiB RSS, never finishing the snapshot
drain). Fixed:

| trigger | action |
|---|---|
| structure or cascade change | `InvalidateLayout` (rebuild boxes) |
| attribute / style mutation | throttled `RestyleWithoutLayout` (≥50 ms) + `DueWorkKind::Paint`; during `IsLoading`, Engine does not `PaintAndSend` so the snapshot loop can wait on sockets |
| layout-affecting CSS animation | `RestyleWithoutLayout` + reflow |
| paint-only CSS animation (transform/color) | `RestyleWithoutLayout` only |
| video frame | paint / surface damage only |

`EnsureBoxTree` keys off `StructureVersion` + cascade generation; `Layout` still
keys cleanliness off `MutationVersion` so `offsetWidth` after a style write
restyles then reflows. Counters: `layout.animation_tick_no_box_rebuild`,
`layout.animation_paint_only`, `layout.attr_paint_only`, `layout.video_paint_only`.

**Still open.** Dirty-subtree box rebuild remains the end state for stamp/font/
sheet. Full Web Animations (KeyframeEffect constructor, `getAnimations`,
opacity as a paint property) is still approximate — `new Animation()` is now
constructible (idle/empty) and the prototype carries `reverse` / `finish` /
`playbackRate` / `startTime` so youtube's lite polyfill completeness probe can
skip (2026-08-09). SPA search→watch no longer dies on Illegal constructor;
`#movie_player` stamp after soft nav remains a separate gap.

**Update** (2026-08-10). **False-positive display invalidations (TD-0033, fixed).**
`RestyleWithoutLayout` rebuilt whenever an element "should generate a box but
has none". Two false positives: (1) hidden `<input>`s skipped by `IsHiddenInput`
while UA still made them `inline-block`; (2) children of replaced hosts
(`button`/…) that never get boxes. On `/results` before: display invalidations
**280** ≈ restyles, `BuildBoxTree` **~8.3 s / 390**. After: display **7** against
**278** restyles, `BuildBoxTree` **~5.2 s / 118**. See TD-0033.

**Update** (2026-08-10). **Web-font swap is reflow-only.** `AddWebFont` used to
`InvalidateBoxTree` on every successful registration (`font-display: swap`).
Box generation does not change — only metrics — so an existing tree now clears
intrinsic widths and forces `laid_out_width = -1`
(`engine.box_tree_font_reflow_only`). Cold path (no boxes yet) still drops the
tree (`engine.box_tree_invalidated_by_font`). Release `/watch` after Accept:
**21** `font_reflow_only`, **1** full font invalidation (was **22** full).

**Update** (2026-08-09). **`opacity` as a paint property landed**: cascade +
`getComputedStyle`, skip the whole subtree at 0, multiply command alphas for
(0,1), and treat `opacity < 1` as a stacking context. Youtube's solid-black
`yt-interaction .fill` bars and the opaque consent backdrop were this gap
(`background:#000; opacity:0` / `opacity:0.3`), not TD-0028. Animating opacity
is still discrete (not on `AnimatableProperty`); group offscreen compositing
is still the SVG-style multiply approximation.

**Update** (2026-08-09). **`Element.animate` / `Animation` landed** through
`bindings::AnimationSource` → `engine::Animations` programmatic effects (same
Apply path as CSS `@keyframes`, never `el.style`). Feature detection sees
`Element.prototype.animate` and `window.Animation`, so `web-animations-next-lite`
should skip its style-writing fallback. Counters: `animation.waapi_started` /
`finished` / `cancelled`. Pause leaves `NextDelayMs` empty (idle CPU). What
remains of the watch CPU story is then TD-0020's facade / format path and
dirty-subtree rebuild — not a 60 Hz attribute restyle from the polyfill.

**Update** (2026-08-09, watch probe after WAAPI). Release snapshot of
`/watch?v=jNQXAC9IVRw` completes (~53 s) with:

| probe | value |
|---|---|
| `typeof Element.prototype.animate` | `function` |
| `typeof Animation` | `function` |
| `getPlayerState()` | **3** (buffering) |
| `isError` / `errorCode` | **false** / **null** (no longer `fmt.unplayable`) |
| `layout.animation_tick_no_box_rebuild` | 907 |
| focus after `-click` | `tp-yt-paper-dialog#dialog` (consent UI, not the player) |

Empty/`null` keyframe lists must be accepted (WPT / the lite polyfill's probe);
rejecting them threw and left the polyfill path noisy. Snapshot post-load drain
for non-reddit pages is capped and yields on the rAF clock so perpetual
`requestAnimationFrame` cannot starve `-eval`. Video `readyState` was still 0
on that click — consent dialog first.

**Measured** (Release, `/results?search_query=cats`):

| metric | after TD-0001 guards | after style cache |
|---|---|---|
| `engine::BuildBoxTree` | ~2.0–2.8 s / ~95–112 calls | **~1.4 s / 108** |
| `css.styles_resolved` | ~132k | **~51k** (+57k hits) |
| snapshot wall | ~18–25 s | **~18 s** |

---

## TD-0020 — youtube's `playVideo()` is a stub that never reaches `<video>.play()`

**Update** (2026-08-08). Root cause split in two:

1. **`navigator.userActivation` was missing.** youtube's `playVideo()` reads
   `navigator.userActivation.isActive` before calling `<video>.play()`. With the
   name absent the check read false and `playVideo()` never reached the element
   (`playCalls=0` with a hook). Landed `navigator.userActivation` with
   `isActive`/`hasBeenActive` backed by the document's one activation bit, plus
   `document.hidden`, `document.visibilityState`, `document.hasFocus()`, and
   `document.elementFromPoint` (ADR 0017 survey: 34 `visibilitychange` sites).

2. **Clicks hit `.ytp-error`, not the play handler.** The automatic player path
   leaves a full-size error overlay (`ytp-error` at 880×660) even when the
   `<video>` already has MSE data (`readyState` 4, `duration` ~19s). Clicks
   never reached `playVideo()`. Landed a default action:
   `Page::ToggleMediaPlaybackAt` / `EngineInput` — a trusted click inside
   `#movie_player` toggles the descendant `<video>` through `Page::Play`.
   HTML `muted` now seeds `MediaState` on first touch.

**Update** (2026-08-09). Facade diagnosis with `-eval` on
`/watch?v=jNQXAC9IVRw`:

| observation | value |
|---|---|
| `#movie_player.playVideo` | present (function) |
| `getPlayerState()` | **-1** |
| `getPlayerStateObject().isError` | **true** |
| `getVideoData().errorCode` | **`fmt.unplayable`** |
| `playabilityStatus.status` | `OK` |
| adaptive `canPlayType` / `isTypeSupported` | all `probably` / true for offered itags |
| `response.body instanceof ReadableStream` | **true** (was false — plain object; fixed 2026-08-09) |
| `HTMLMediaElement.currentSrc` | **set** (was missing — empty while `src` held `blob:…`; fixed) |
| `HTMLMediaElement.error` | **`null`** when nothing failed (was `undefined`) |
| `SourceBuffer.prototype.changeType` | **present** (was missing; SABR feature-detects it) |
| empty `<video>` first touch | **no `error` event** (was `FailNoSource` → `error`; fixed via `MarkNoSource`) |
| MSE attach before first buffer | **`ResourceSelected`** leaves `NO_SOURCE` without wiping readiness |
| `HTMLMediaElement.load()` | **resets** (was sync `NotSupportedError` throw) |
| `TextEncoder` / `TextDecoder` | **present** (UTF-8; watch encodes ~48KB / decode 257× on one load) |
| progressive `streamingData.formats` | **[]** (adaptive-only / SABR) |
| click → `Page::Play` | still works (`paused=false`, `currentTime≈2`) |
| `AudioContext` | absent (ADR 0028 §4 — deliberate; not this error code) |
| `crypto.subtle` | **present** (importKey / encrypt AES-CTR / sign HMAC-SHA-256; ADR 0037) |
| `indexedDB` / `BroadcastChannel` | **absent** (Woffle offline store `g.D8` / `plI`; importKey never reached) |

So the facade is not a single missing binding: it is youtube's player stuck in
`fmt.unplayable` while MSE has already buffered a playable stream. `playVideo()`
after a trusted click still leaves the facade in error even when the element is
playing via the click bypass. Closing TD-0020 means finding which player-side
check still sets `fmt.unplayable` — player `base.js` maps MediaError code 4 and
SABR slicer exceptions (`trg:"sabrslicerqt"`) to that code; early `play()` without
activation is `NotAllowedError` (handled as autoplay-blocked, not this code).
Watch config sets `allowWoffleManagement:true`; without `crypto.subtle` the PES
encoder cannot import AES-CTR keys, and without IndexedDB/BroadcastChannel the
offline store never opens — both still open platform gaps (survey: 2 and 7 uses).

**Update** (2026-08-09, late). `TextEncoder`/`TextDecoder` and `crypto.subtle`
(ADR 0037) landed. Watch still reports `Woffle: PES is undefined` and
`fmt.unplayable`. Diagnosis of the Woffle half:

| piece | role | status |
|---|---|---|
| `TextEncoder` | `dW` key material from `DATASYNC_ID` | **done** |
| `crypto.subtle` | `au()` → AES-CTR / HMAC for `W3O` PES encoder at `B[1]` | **done** |
| `indexedDB` + `IDBTransaction` / `IDBObjectStore` / `IDBIndex` / `IDBKeyRange` | `yPS` feature-detect; without them `g.wU` is false and `plI` returns | **done** (ADR 0038) |
| `BroadcastChannel` | `X_` sync channel `PERSISTENT_ENTITY_STORE_SYNC:…` | **done** (ADR 0038; document-scoped) |

`plI` only constructs `W3O` (which installs the PES encoder) after `g.wU()` and
`BroadcastChannel` succeed. Closing the Woffle report means **IndexedDB (ADR 0021
§5) plus BroadcastChannel**, not more media surface. `fmt.unplayable` via
`setmediasrc`/Gal may still be a separate throw once Woffle is quiet.

**Update** (2026-08-09, later still). `BroadcastChannel` and a memory-backed
`IndexedDB` (ADR 0038) landed: `indexedDB`/`IDBDatabase`/`IDBTransaction`/
`IDBObjectStore`/`IDBIndex`/`IDBKeyRange`/`IDBCursor`/`IDBCursorWithValue`/
`IDBRequest`/`IDBOpenDBRequest` and `BroadcastChannel` are all real
constructors now, backed by `storage::PartitionedIndexedDb` (in-memory, 50MiB
per partition) on one side of the seam and `bindings::IndexedDbSource` on the
other, the same inversion ADR 0021 used for `sessionStorage`.

Two bugs kept watch from using them even after the constructors existed:

1. `"objectStoreNames" in IDBTransaction.prototype` was false — the name lived
   as an own property on each transaction instance. `yPS` returns false for the
   whole feature without opening anything.
2. `"IDBTransaction" in self` was false — `MakeInterface` puts constructors in
   the global *scope* only (so a page assigning `window.ShadowRoot = …` cannot
   fork the bare name), but the `in` operator consulted the global object's
   property map alone. Fixed in `Operators.cpp` to match `GetProperty`/
   `SetProperty`'s "one namespace, two spellings" rule.

What is **not** closed: `BroadcastChannel` is document-scoped rather than
partition-scoped; index `keyPath` metadata is bindings-local so a reopen without
`upgradeneeded` cannot repopulate indexes.

**Update** (2026-08-09, after yPS prototype/`in` fixes). Release watch probe
(`/watch?v=jNQXAC9IVRw`, with `-click 456,398`):

| metric | value |
|---|---|
| `idb.opens` / `puts` / `gets` | **10 / 8 / 15** (was 0) |
| `broadcast_channel.constructed` | **4** |
| `Woffle: PES is undefined` | **gone** from console |
| `video.readyState` / `buffered` | **4 / ~19s** |
| `video.paused` / `currentTime` after click | **false / ~4s** |
| `crypto.subtle_import_key` | still **0** (lazy until PES encrypts; construction no longer blocks) |
| `getVideoData().errorCode` | still **`fmt.unplayable`** |
| `getPlayerStateObject().isError` | still **true** |
| MSE `addSourceBuffer` / `appendBuffer` failures (prelude wrap) | **0** of 139 |

**Update** (2026-08-09, after versionchange + btoa). Release
`/watch?v=jNQXAC9IVRw` `-click 456,398`:

| metric | value |
|---|---|
| `encoding.btoa` | **30** (was ReferenceError ×17) |
| IDB upgrade `addEventListener`/`objectStore` of undefined | **gone** |
| `Woffle: PES is undefined` | still **gone** |
| `video` after click | **paused=false, t≈4s, buffered≈19s** |
| `getVideoData().errorCode` | still **`fmt.unplayable`** |
| `js.steps_exhausted` | **23** (`js.steps_peak` ≈ 20 000 022) |
| top remaining throws (MICROBROWSER_JS_THROWS) | `info` of undefined ×23, script-too-long ×23, `B` of null ×13 |

The facade error correlates with player code reading `.info` off an empty
queue / null mediaSource (`@2341091`, `WzT`/`Ty1`) while MSE itself never
fails an `addSourceBuffer`/`appendBuffer`. The step-budget storms (TD-0018)
are the likely reason those queues are empty. Closing TD-0020 still means
`isError === false` and `playVideo()` without the click bypass.

**Update** (2026-08-09, media event budget). `Interpreter::MediaEventBudget` on
SourceBuffer / HTMLMediaElement event delivery gives each sync MSE callback a
fresh hang-guard allotment while appendBuffer's frames remain live (see
TD-0018). Expected effect: SABR `iuT` finishes pushing `d9`, `vW` no longer
throws on `shift().info`, and `fmt.unplayable` via `sabrslicerqt` stops being
the false positive for a spent step budget. Verify with
`js.media_event_budget_resets` / `js.steps_exhausted` on a consented watch.

**Update** (2026-08-09, MSE updateend as macrotask). Sync `updateend` was the
real empty-`d9` bug, not only step budget: player `Ty1` → `wSl`/`DP4` →
`appendBuffer` re-entered `Ty1`/`vW` while the outer call still held the
segment from `OP` (`@2341091` stack showed two `Ty1` frames). MSE says queue a
task for `update`/`updateend`; delivering before `appendBuffer` returns matches
neither the spec nor Chrome. `ScheduleSourceBufferEvents` posts through
`TimerQueue::QueueTask` (same host-task drain as MessageChannel). `updating`
stays true until that task. MediaEventBudget remains for HTMLMediaElement
events flushed from the task.

**Measured**, Release, `/watch?v=jNQXAC9IVRw`, `-click 456,398` (no `-eval`):

| metric | before | after |
|---|---|---|
| `video.paused` | true | **false** |
| `video.currentTime` | 0 | **~2 s** |
| `navigator.userActivation.isActive` | undefined | **true** |

**Update** (2026-08-09, evening). `-prelude` hooks on watch (before page scripts):

| observation | value |
|---|---|
| `HTMLVideoElement.play` rejections | **6× `NotAllowedError`** only (blob `src` set, `networkState=2`, unmuted) |
| `NotSupportedError` from `play()` | **0** (ResourceSelected / `currentSrc` path holds) |
| `HTMLMediaElement.load()` | **23×** with empty `src` (player speculative path) |
| `media.error_events` | **0** on a clean counter run |
| `Error()` constructions of note | `Woffle: PES is undefined` (×3) — Woffle offline path in player, not our demux |

So the facade error is **not** an early `NotSupportedError` from empty `currentSrc` / `NO_SOURCE`, and **not** a fired `HTMLMediaElement` `error` event. Unmuted autoplay correctly refuses; the player maps that to autoplay-blocked. `fmt.unplayable` is still set by a higher SABR/Woffle path while MSE buffers ~19s. Click bypass remains the Gate C watch path.

**Tooling:** `microbrowser_snapshot -prelude '<js>'` runs once before the page's scripts (`Engine::SetScriptPrelude`).

**End state.** Close fully when `getPlayerStateObject().isError` is false for a
loaded watch and `#movie_player.playVideo()` reaches `HTMLMediaElement.play`
without the `#movie_player` click bypass. Until then Gate C watch is satisfied
by the default media click path.

**Update** (2026-08-09, consent cookies). HTTP-date `Expires` parsing
(`util::ParseHttpDate`) landed — youtube's past-dated deletes of
`TESTCOOKIESENABLED` / `PREF` had been left as session cookies, and Accept
reported "error saving your choice". After Accept (plus closing the lightbox):

| observation | value |
|---|---|
| `SOCS` / `PREF` | set; no `PREF=null`, no leftover `TESTCOOKIESENABLED` |
| save-error string | **absent** |
| `<video>` after `playVideo()` | `readyState` **4**, `buffered` **[0, ~19s]**, `blob:` src |
| `getVideoData().isPlayable` | **true** |
| `getPlayerState()` | still flaps **-1** / **3** while `paused===true` |

So the MSE/buffer half of TD-0020 is largely unblocked once consent cookies
round-trip. Remaining: facade state vs element play. Consent UI positioning
(TD-0022) is fixed; Accept is reachable via scroll + trusted `-click`
(`-eval` scrollIntoView then `-click last`).

**Update** (2026-08-09, after MSE macrotask `updateend`). Release
`/watch?v=jNQXAC9IVRw` with Accept scroll+click:

| metric | before (sync updateend) | after |
|---|---|---|
| `getVideoData().errorCode` | `fmt.unplayable` | **`null`** |
| `getPlayerStateObject().isError` | true | **false** |
| `isPlayable` | true | true |
| `video.readyState` / buffered | 4 / ~19s | 4 / ~19s |
| `cannot read property 'info' of undefined` (@2341091) | ×27 | **0** |
| `js.steps_exhausted` | 7–23 | **2** |
| `js.media_event_budget_resets` | 554 (sync nest) | **66** |

TD-0020's facade half is closed for this watch URL: `isError === false` with a
full MSE buffer. `playVideo()` after Accept alone may still leave `paused`
true (`state` -1) until a trusted gesture reaches the player — that is
autoplay policy, not `fmt.unplayable`.

**Update** (2026-08-09, SPA after TD-0025). Search→watch (Release, cats →
Accept → thumb) matches cold: `movie`/`video`/`html5` true, `readyState` **4**,
`buffered` **≈23s**, `isError:false`, `errorCode:null`, `isPlayable:true`,
and `paused:false` after the trusted thumb click. Remaining TD-0020 surface is
home-feed stamp (TD-0017/0018), not watch playback.

---

## TD-0022 — youtube consent dialog fits at 0×0 and never auto-refits

**Symptom.** `tp-yt-paper-dialog#dialog` opens with `top:448px; left:640px`
(viewport centre as if size were zero) while content is ~748×928. Accept all
lands near `y≈1887` — off-screen. `max-height` *is* written (`896px`) once
`getComputedStyle` serializes unbounded max-size as `"none"` (iron-fit's
`sizedBy.height = maxHeight !== "none"`).

**Update** (2026-08-09). Several halves landed or were named:

| piece | status |
|---|---|
| `max-height` / `max-width` serialize as `none` | **done** |
| `getBoundingClientRect` subtracts ancestor scroll | **done** |
| `HTMLElement.click()` | **done** (does **not** grant user activation — correct) |
| cookie `Expires` HTTP-date (consent save / `TESTCOOKIESENABLED` delete) | **done** |
| `box-sizing: border-box` honoured for min/max size | **done** (iron-fit's pair with `max-height`) |
| `getComputedStyle().overflow` shorthand | **done** |
| column flex grow/shrink + `max-height` re-layout | **done** — `#content` is now ~840px inside the 896px dialog (`layout.flex_column_max_height_relayouts`); Accept still needs `scrollIntoView` because it sits at the end of the scrollable policy text |
| `location.assign` / `replace` / `href=` / **`reload`** | **done** (ADR 0026 §3) — deferred through `HistorySource::RequestNavigation`; Accept's Fy8 ends in `location.reload()` after POSTing `savePreferenceUrl` (GET is 405; POST returns 204) |
| Accept → `consent.youtube.com/save` | **done** for the network half — click fires `yt-save-consent-action` → `handleSaveConsent` → `Fy8` (set SOCS, POST `/upgrade_visitor_cookie`, POST save URL); dismiss needs `location.reload()` (above) |
| Accept dismiss (dialog gone after reload) | **done** 2026-08-10 — TD-0032 first-party cookie jar + post-click script-fetch drain; Release home: `dialogs:0`, `SOCS` set |
| auto-refit after stamp | **done** — root cause was not FlattenedNodesObserver: iron-overlay prepares with `style.display=""` then measures, and `RestyleWithoutLayout` could not invent a box for an element that had been `display:none` (box tree skipped when only `MutationVersion` moved). Display none↔box now rebuilds the tree (`engine.box_tree_invalidated_by_display`). Dialog centres at `top:0; left:266` without `-eval` |
| non-scroller `scrollWidth`/`scrollHeight` | **done** — were 0 on any non-scroll-container; now at least the padding box |
| inflated `#content.scrollHeight` (~1e5–4e5) | **done** — was a symptom of the missing box after `display:none` cleared (overflow measured against a stale tree); after the rebuild, `#content.scrollHeight` is ~2.3k for ~1.3k of policy text. Accept still sits below the dialog fold until the content scroller moves (real UX) |
| real `-click` Accept → user activation → play | **done** for the scroll half — see below |

`dialog.refit()` / `resetFit(); fit()` recentres correctly when called. After
Accept sets `SOCS`, MSE buffers the full zoo clip (`readyState` 4, ~19s) once
the overlay is cleared; `play()` still needs a trusted gesture.

**Update** (2026-08-09, wheel → Accept). `#content` is `overflow-y: auto` inside
a `position:fixed` dialog whose host (`ytd-consent-bump-v2-lightbox`) is **0×0**.
`ScrollTargetAt` required every ancestor `BorderBox` to contain the pointer, so
the wheel never reached `#content` while `elementFromPoint` / `scrollTop =`
still could. `Page::ScrollAt` now walks from `ElementAt` up the DOM for a
movable scroller (same elevated-abspos path as clicks). Snapshot gained
`-wheel x,y,dy` and `-y` aims at the viewport centre. Counters:
`scroll.overflow_moved` / `scroll.viewport_fallback`.

Measured (Release, `/watch?v=jNQXAC9IVRw`):
`-wheel 640,400,950` → `#content.scrollTop === 950`, Accept at `y≈471`;
`-click last` (from eval `"click":"x,y"`) → `dialogs:0`, `SOCS` set, MSE `readyState` 4.

**Update** (2026-08-09, ghost click). Accept over search results was navigating
to `/watch`: press hit the button, the dialog left the hit path, release
re-hit-tested the point and fired `click` + link default action on the result
underneath. UI Events: remember `pointer_down_target_`, fire `click` at the
common ancestor of press and release, and resolve default actions from that
element (`ResolveClickActivation`) — never a fresh `LinkAt(point)`. Counter
`input.click_retargeted`. Test
`Engine/ClickDoesNotActivateLinkUnderDismissedOverlay`. After Accept on
`/results?search_query=cats`: stay on search, `dialogs:0`, `SOCS` set, ~7
`ytd-video-renderer` at 500×281 (thumbnail `src` still lazy — TD-0018).

**Close when.** After the consent bump stamps, the dialog's border box is
inside the viewport without `-eval` fit/scroll, Accept is hit-testable by
`-click`, and Accept leaves `opened===false` (or navigates) without a scripted
property write. **Done** for scrollport + trusted click via `-wheel` then
`-click` (optional eval only to discover Accept's post-scroll coordinates),
for not activating whatever was under the dialog, and for dismiss after
save→reload (TD-0032, 2026-08-10: `dialogs:0` on home/results/watch).

---

## TD-0023 — `img.src` assigned by script never entered the image fetch list

**Symptom.** youtube search thumbnails are gated by `IntersectionObserver`, which
assigns `img.src` without inserting nodes. Attribute-only mutations bumped
`MutationVersion` and took the paint-only due-work path (TD-0021), which
restyled but never called `CollectImages`. `StartImageRequests` then had an
empty pending list for URLs the DOM already named. Observation delivery could
also set `src` *during* `PaintAndSend`, after the last collect and before
`StartImageRequests`.

**Landed** (2026-08-09). `CollectImages` on attribute-only due work;
`RecollectDocumentImages` after `DeliverObservations` when callbacks ran
(`engine.images_recollected_after_observation`). Test
`Page/RecollectsImagesAfterScriptAssignsSrc`.

**Still open.** Close when a set `src` always fetches. Empty-`src` reliability
on first paint is closed under TD-0018's NestedHostBudget (2026-08-09): in-view
search thumbs get `src` without scroll.

---

## TD-0024 — SPA search→watch can leave `ytd-player` without `#movie_player`

**Symptom.** Clicking a search thumbnail navigates to `/watch?v=…` but
`document.querySelector('video')` stays null while cold loads of the same URL
reach `readyState` 4 after Accept. Soft nav finishes `IsLoading` before the
player modules stamp; the snapshot tool's generic **2s** post-load drain then
stopped (and `-eval` saw chrome-only `ytd-player`).

**Landed** (2026-08-09). Snapshot `RunLoadToCompletion` treats youtube watch
URLs like a longer settle: up to 90s or until `video` / `#movie_player`, and
keeps waiting on sockets when there is no timer deadline. Constructible
`Animation` removed a separate SPA abort (`Illegal constructor: Animation`).

**Landed** (2026-08-09, Symbol `in`). Lit brands signal getters with a Symbol and
gates writes on `SSn in getter` (`gvU`). `BinaryOp::In` stringified Symbol keys,
so the check was always false and every reactive merge threw `Error: ad` in
observer callbacks. `in` / Proxy `has` now keep Symbol identity; Proxy
`getPrototypeOf` / `Object.hasOwn` also go through the target (Lit's `U3D` is
`getPrototypeOf(o) === Object.prototype`). After the fix, `Error: ad` is gone on
search→watch; `ytd-watch-flexy.data` can still be only `["contents"]` with no
`#movie_player` — the remaining gap is the innertube/player application path,
not Lit signal writes.

**Root cause A — WebPO / BotGuard hang (blocks innertube).** Soft nav runs
async context processor `Wq2` (WEB_PO) before `networkManager.fetch`. When the
integrity service exists but `!isReady`, it waits on `wne()` → `wpc.f()`, which
never settled because BotGuard's challenge script threw
`ReferenceError: eval is not defined` on `(0,eval)(…)`. Cold watch never waits:
`rha({pV: videoId, …})` can mint a short poToken while `!isReady`. **Fix:
ADR 0039** — `eval` / `Function` exist, gated by CSP `'unsafe-eval'`. Also set
`data-loaded` on `<script>` after a successful run so YouTube's `_.VE` loader
does not wait forever for a bit that never appeared.

**Measured** (2026-08-09, Release + `MICROBROWSER_LOAD_TIMELINE=1`, pre-eval).
Soft-nav search→watch updated the URL and started player chunks but showed **no**
`/youtubei/v1/player` / `/next` while WebPO hung. Cold `/watch` needs none of
those because `ytInitialPlayerResponse` is in the HTML.

**Still open (root cause B).** After player JSON returns OK with `streamingData`,
`loadVideoWithPlayerResponse` can still leave `getPlayerPromise` unsettled —
`Application.create` / VE `eue` / `apiResolver` race when the player API is not
ready (`new Promise(function(){})`). Close when search→watch reliably yields a
playable `<video>` without a cold document load.

**Landed** (2026-08-09, post-load scripts). `OptionsForSubresource` no longer
dereferences a cleared `load_.base`; CSP trust is stamped on
`createElement('script')` as well as append; refused/failed late scripts fire
`error`. SPA now fetches and runs `player_ias/.../base.js`, issues
`/youtubei/v1/player` + `/next` (200), and `Application.create` is a function.

**Landed** (2026-08-09, `data-loaded` / `OgC` ordering). Stamping `data-loaded`
*before* firing `load` made YouTube's `P_U` completion (`BzU(el)||(hQn,OgC)`)
skip `OgC`, so `EHT` set `eue` and waited forever while `create` existed from
the script body but was never *called* with a target. `load` fires first;
`hQn` sets the attribute. **Check:** search→watch after Accept:
`movie:true`, `video:true`, `html5:true` (Release). `readyState` can still be 0
until MSE buffers — that is playback (TD-0020), not stamp.

**Close when** search→watch stamps `#movie_player` / `<video>` without a cold
document load, and media reaches a playable `readyState` (or a measured MSE
blocker is filed separately). **Closed** 2026-08-09: stamp (`37448fd`) + SPA
MSE via TD-0025 (`9c6747e`). See Closed.

**Instrumentation.** `MICROBROWSER_LOAD_TIMELINE` kept only 512 rows and truncated
during font cookies on youtube soft-nav — raised to 4096 so innertube/player
rows stay visible.

---

## TD-0025 — HTTP/2 session death fails every open GET with no retry

**Symptom.** On youtube SPA search→watch, `player_ias/.../base.js` and dozens of
`fonts.gstatic.com` SVGs finish together as `FAILED the connection failed`
(~10s after the script started). Innertube `/player` and `/next` later succeed
on a fresh connection, but without `base.js` YouTube never runs `OgC` →
`Application.create` with a target, so `#movie_player` stays missing
(TD-0024 stamp flakiness). Measured Release: `fetch.failed=81` on one soft-nav;
`base.js` was among the blast.

**Cause.** `Http2Session::Read` maps a fatal transport read to
`"the connection failed"` on every incomplete stream. `FetchRequest` retried
HTTP/2 only for `REFUSED_STREAM` / unprocessed GOAWAY — correct for POST, but
a shared-session death then permanently fails every in-flight GET, including
late scripts that have no application-level retry.

**Landed** (2026-08-09). One GET/HEAD retry on session-level failure, forcing
`ConnectionPool::Acquire(... allow_reuse=false)`. Counter `net.h2_retried`.
Test `Http2Fetch/AGetSurvivesADeadSharedSessionOnce`.

**Update** (2026-08-10, TD-0041). Soft-nav SPA watch started 8
`googlevideo.com/videoplayback` SABR POSTs and completed **zero** of them while
`media.source_buffers_created` was 6 and `media.source_appends` stayed 0.
Session-death retry is extended to POST when no response was processed — the
body is still buffered on the client. Test
`Http2Fetch/APostSurvivesADeadSharedSessionOnce`.

**Close when.** SPA search→watch stamps `#movie_player` reliably across network
jitter (retry recovers `base.js`), and cold/SPA watch reach playable
`readyState` without transport-mass-failure orphans. Stamp half measured
closed 2026-08-09; MSE half re-opened under TD-0041 until soft-nav shows
`source_appends > 0`.

---

## TD-0026 — youtube home searchbox preventDefaults Enter but never navigates

**Closed** (2026-08-09). Root cause: youtube's search path (`n0n`) does
`document.createElement('a'); a.href = location.href; a.pathname.startsWith(…)`.
`HTMLAnchorElement` only reflected `href` as text — `pathname` was undefined —
so Enter's handler threw and never called `resolveCommand`. Fixed
HTMLHyperlinkElementUtils (`pathname`/`search`/`hash`/…) via shared
`SplitHref` (also used by `location` / `URL`). Release check: home → Accept →
type `cats` → Enter → `location.pathname==="/results"` with
`ytd-video-renderer` stamped (13).

**Also landed with the dig:** `KeyboardEvent` constructor copies
`KeyboardEventInit` (`key`/`code`/`keyCode`); snapshot `-type`/`-key` set
`code`; `EvaluateScript` follows pending form/`location` navigations; fragment
`location` no longer starves a same-turn `requestSubmit`.

---

## TD-0027 — Binding URL arguments used pure `js::ToString` (no `toString`)

**Closed** (2026-08-09) for URL-taking entry points; **extended** same day.

**Symptom.** youtube home's consent flow requested
`consent.youtube.com/m?continue=https://www.youtube.com/%5Bobject%20Object%5D…`.
Probe: `new URL(location).href === "https://www.youtube.com/[object%20Object]"`
while `new URL(location.href)` was correct. Separately, `fetch({})` (a plain
object with no Request shape) was coerced to the relative URL
`[object Object]`, resolved against the document base, and actually issued
`GET https://www.youtube.com/[object%20Object]` (counter
`fetch.object_object_url`). Chrome rejects that with TypeError before any
request.

**Cause.** `URL` / `fetch` / `Request` / `location.assign` / XHR `open` /
`history.pushState` URL args — and later reflected DOMString attrs /
`setAttribute` / `encodeURI*` / WebSocket / EventSource — used `js::ToString`,
which cannot call into the interpreter and invents `"[object Object]"`.
`ResolveUrl` then treated that as a path against the document base.

**Fix.**

- `CoerceToString` (`Interpreter::ToStringOf`) on URL-taking sites, reflected
  attribute setters, `setAttribute`/`setAttributeNS`, `encodeURI`/
  `encodeURIComponent`, WebSocket and EventSource constructors.
- `fetch` / `Request` / XHR `open` refuse the literal `"[object Object]"` with
  TypeError (Chrome's `fetch({})` behaviour), so a bad coerce cannot become a
  network request.
- Counter `fetch.object_object_url` if one still reaches `Engine::StartFetch`.

**Tests.** `DomBindings/BlobUrlAndWindowPostMessage` (location → URL /
encodeURIComponent / `link.href` / `img.src` / `setAttribute` / `fetch({})` /
`new Request({})`); `JsInterpreter/TheUriFunctionsDifferInWhatTheyKeep`.

**Still open (same class, lower urgency).** Style `setProperty` / storage keys
and other non-URL DOMString sites that still use pure `js::ToString`. Audit
when a page depends on coercing a host object there.

---

## TD-0029 — Event object UAF across `DrainMicrotasks` in `DispatchEventTo`

**Closed** (2026-08-09).

**Symptom.** ASAN heap-use-after-free (Release: SIGFPE in
`Object::GetOwnProperty`) when youtube.com fired script `load` and the
handler's microtasks allocated past the GC threshold. The event from
`MakeEvent` was only a C++ local; `DrainMicrotasks` → `MaybeCollect` freed it;
reading `defaultPrevented` afterwards was UAF. Interactive loads that forced
layout/`getBoundingClientRect` during settle hit it often enough to abort.

**Fix.** `Interpreter::ValueRoot` on the event for the whole of
`DispatchEventTo`, `DispatchAtWindowWith`, and `NotifyScriptElementEvent`
(same root already used for thrown completions and microtask jobs).

**Test.** `DomBindings/APageCanMakeAndDispatchItsOwnEvents` — cancelable
dispatch whose handler queues an allocating `then`; returns `false` after the
drain without crashing.

---

## TD-0028 — youtube home `#content` / page-manager stay content-sized inside a filled `ytd-app`

**Opened** 2026-08-09 after the abspos ICB `min-height:100%` fix.

**Symptom.** With `ytd-app` correctly filling the viewport (~900px),
`#content` / `ytd-page-manager` stayed far shorter; early on the feed-nudge
wrappers measured 0 under `overflow:hidden`, so the window read as masthead +
empty rather than "Try searching…".

**Not.** Missing rich items when `ytInitialData` only has `feedNudgeRenderer`
(history-off): that zero is the response (TD-0017 update).
**Not.** A missing `#content { display:flex; height:100% }` rule — kevlar has
none; `#content` is deliberately `display:block`. `#page-manager`'s `flex:1`
is inert because its parent is not a flex container (same in Chrome).
**Not.** Home (`page-subtype=home`) relying on
`ytd-two-column-browse-results-renderer { min-height: calc(100vh - 120px) }` —
that rule is **channels-only**; playlist/show use
`calc(100vh - var(--ytd-toolbar-height))`. Home is content-sized by design
when the server sends only the history-off nudge.

**Root cause for dead `vh` (2026-08-09).** `Page::ResetResolver()` assigned a
fresh `StyleResolver` and reattached the adjuster, but never restored
`MediaContext`. `RebuildAuthorStyleSheets` always resets first, so every
`vh`/`vw` declaration was refused at apply time (`viewport_height == 0`) even
though `ParseStyleSheet` received the real viewport for `@media`.

**Fixed (2026-08-09).** Flex / paint / media-context:
1. Row flex definite/ForcedSize height → `align-items: stretch`.
2. Row item `height:100%` against that definite cross size.
3. `flex: 1` → `flex-basis: 0%`; `%` basis against indefinite main → `auto`.
4. Main-axis `%` height against indefinite flex main → `auto` (nudge wrappers).
5. Anonymous boxes do not re-paint a copied parent background (white fills
   after nudge text).
6. **`ResetResolver` restores `viewport_` onto the new resolver** so `vh` in
   author sheets survives Load and resize rebuilds (`Page/VhSurvivesAuthorSheetRebuild`).

**Verified after Accept (Release):** nudge ~163px with title text in the
display list and pixels; `#content` ~291 / page-manager ~235 inside a 900px
`ytd-app` — content-sized, matching a history-off home rather than a failed
`100vh` fill.

**Close when** channels/playlist two-column `min-height: calc(100vh - …)`
applies from the author sheet without inline overrides (depends on (6) plus
`var()` already in the cascade), and a non-nudge home with rich items fills
usefully when the server sends them (TD-0017).

---

## TD-0030 — Collected stacking units skip intervening overflow clips — **fixed 2026-08-10**

**Opened** 2026-08-09 with the Appendix E hit-test / paint scroll_delta fix.

**Symptom / gap.** Paint and hit-testing applied intervening *scroll* when a
stacking context collected a positioned unit from under an `overflow:auto`
ancestor (youtube Accept), but not that ancestor's *clip*: a partially scrolled
relative button could paint and hit outside the scroller's padding box.

**Fix.** Collect records each intervening scroll container's padding box plus
the scroll accumulated before entering it (`layout::InterveningClip` on
`StackingUnit`). Paint `PushClip`s those rects (same floor/ceil as the tree
walk); hit-test requires the point inside each before visiting the unit.
Helpers live in `Stacking.h` so paint and hit-test stay in lockstep.
Absolutely positioned units still skip intervening clips
(`SkipsInterveningOverflowClip`) — the existing youtube-thumbnail exception
documented next to `ScrollAdjust` / `Page/LinkAtThroughOverflowHiddenAncestor`.

**Tests.** `Page/HitTestsRelativeInsideOverflowScroller` (outside-clip miss),
`Page/CollectedRelativeUnitClippedByOverflowScroller`.

**Close when.** Done for relative / in-flow collected units. Abspos under a
non-covering `overflow:hidden` padding box remains the documented exception
until thumbnail layout no longer needs it. html/body no longer clip locally
(TD-0034).

---

## TD-0031 — Page `fetch()` / stuck fonts kept snapshot `IsLoading` forever on youtube `/results`

**Opened** 2026-08-09. Session 13 already warned: a long-poll open would hold
`microbrowser_snapshot` for the settle deadline. It showed up — with fonts too.

**Symptom.** `/results?search_query=…` stamped ~10 `ytd-video-renderer`s, then
the load loop spun with `MICROBROWSER_LOAD_TURN_TRACE` reporting
`reason=script_fetches` and/or `font_fetches` and one wait descriptor —
youtube innertube/SABR-style requests and gstatic `@font-face` downloads that
do not complete (related: TD-0015). Home finished; results after Accept/`-eval`
appeared hung for minutes.

**Fix.** `Engine::IsDocumentLoading()` is the navigation / render-blocking half
(document, late images/scripts, modules, classic script fetches). `IsLoading()`
still includes page `fetch`/XHR and font downloads so `RunEngineToIdle` and
Fetch/WebFont tests wait for them. Snapshot `RunLoadToCompletion` asks
`IsDocumentLoading` only; sockets remain on `AppendWaitDescriptors` for the
finite post-load drain.

**Close when** `/results` after Accept finishes a snapshot without a 15‑minute
cap, with video renderers present, and a unit test asserts document-vs-fetch
loading separately.

**Update** (2026-08-09). Release `/results?search_query=cats` with Accept:
finishes (~122 s, peak ~1.8 GiB) with `vids:20`, `SOCS` set; plain `/results`
without Accept ~18 s (was unbounded). `SettleForSnapshot` no longer
unconditionally `InvalidateLayout`s (that was the `bad_alloc`). Consent dialog
node can remain (`dialogs:1`) after Accept — dismiss visibility still open.

**Update** (2026-08-10). Dismiss was blocked by TD-0032 (first-party cookie jar
split + snapshot exiting mid-save). After that fix, Accept's save→reload can
finish; re-measure `opened` / dialog count on home.

---

## TD-0032 — First-party cookie jar keyed by request origin; snapshot dropped Accept mid-fetch — **fixed 2026-08-10**

**Opened** 2026-08-10.

**Symptom.** After trusted Accept on youtube home: `document.cookie` showed
`SOCS`, `upgrade_visitor_cookie` ran with `cookies=7`, but
`consent.youtube.com/save` went out with `cookies=0`, the dialog stayed
`opened===true` / `display:flex`, and the snapshot often exited after
`request.start` for save with no `request.done` / no `location.reload()`.

**Cause (two halves).**

1. **Cookie partition.** `CookieJar` matched `entry.key == key` exactly.
   `document.cookie` stores under `ForTopLevel(www.youtube.com)`; a page fetch
   to `consent.youtube.com` looks up `ForEmbedded(youtube-site, consent-url)` —
   same container and top-level site, different **origin** in the key → empty
   jar. ADR 0005's Total Cookie Protection needs the top-level site so a third
   party on a.com and b.com cannot correlate; it does not ask to split
   first-party jars by same-site host. Domain / host-only matching already
   scopes what each host receives.

2. **Snapshot settle.** TD-0031 correctly stopped `RunLoadToCompletion` from
   waiting on page `fetch`/fonts (innertube hang). After a click that *starts*
   Accept's fetch→save→reload chain, the tool returned immediately because
   `IsDocumentLoading` was false, then exited while save was still on the wire.

**Fix.** `CookiePartitionMatches`: first-party lookups share all first-party
entries under `(container, top_level site)`; third-party still matches the full
key including origin. Snapshot `RunPostInteractionDrain` after click/key waits
up to 20 s for `HasInFlightScriptFetches` (not fonts), runs
`RunLoadToCompletion` when reload starts, then **returns** so innertube cannot
re-open a long settle. Test `Cookie/FirstPartySameSiteOriginsShareJar`.

**Measured** (Release, before): Accept click → save `cookies=0`, no reload,
`propOpened: true`. After: `dialogs:0`, `SOCS` set, home Accept wall **~16 s**.
Watch after Accept: `#movie_player` + `<video>` `readyState` 4 / ~19 s buffer /
`isPlayable`; trusted click on the player (not the pre-play `top:-660px` video
box) reaches `paused:false` / `getPlayerState()===3`.

**Update** (2026-08-10). Post-click drain must **return** after reload's
`RunLoadToCompletion`, not keep waiting on the new document's long-lived
innertube fetches (was burning the full cap after every Accept).

**End state.** Same-site first-party cookie visibility matches browsers;
snapshot Accept completes save→reload without waiting on stuck fonts/innertube.

**Close when.** Unit test green (done); Accept leaves no consent dialog (done);
ADR 0005 notes the first-party jar rule (done).

---

## TD-0033 — Restyle treated non-box DOM as display changes; rebuilt the tree every time — **fixed 2026-08-10**

**Opened** 2026-08-10 while profiling youtube `/results` after Accept.

**Symptom.** Release `/results?search_query=cats`: `engine::BuildBoxTree`
**8343 ms self / 390 calls**, `engine.box_tree_invalidated_by_display` **280**
equal to `RestyleWithoutLayout` call count — every restyle dropped the box tree.

**Cause (two halves).**

1. **Hidden inputs.** `BuildBoxTree` skips `input[type=hidden]` via
   `IsHiddenInput`, but the UA sheet only said `input { display: inline-block }`.
   HTML §15.3.1 requires `display: none !important`. Fixed in the UA sheet.

2. **Replaced hosts.** `button` / `select` / `video` / … are replaced: DOM
   children never get boxes. `RestyleWithoutLayout` saw every inner `span` /
   custom element as "generates a box but has none under a boxed parent" and
   set `box_generation_changed`. Youtube's buttons make that fire on **every**
   restyle. Fixed: skip the check when the parent's box is `Kind::Replaced`.

**Fix.** UA `input[type=hidden] { display: none !important }`; restyle ignores
missing boxes under replaced parents. Tests:
`StyleResolver/AppliesTheUserAgentStyleSheet`,
`Geometry/HiddenInputsDoNotInvalidateBoxTreeOnRestyle`,
`Geometry/ReplacedChildrenDoNotInvalidateBoxTreeOnRestyle`.

**Measured** (Release `/results` after Accept). Before: display **280** ≈
restyles, BuildBoxTree **~8.3 s / 390**. After: display **7** vs **278**
restyles, BuildBoxTree **~5.2 s / 118** (network still dominates wall).

**End state.** Display-change rebuilds only when an element's own box generation
actually flips; dirty-subtree rebuild (TD-0021) remains the larger end state.

---

## TD-0034 — html/body overflow clips to a height-0 padding box instead of the viewport — **fixed 2026-08-10**

**Opened** 2026-08-10 after TD-0030 made youtube search thumbnails unclickable.

**Symptom.** `elementFromPoint` on a visible `a#thumbnail` returns
`ytd-page-manager`. Clicks never navigate. Structure: `body{height:0;
overflow:scroll}` (CSSOM), abspos `ytd-app` (unit, not a stacking context),
`ytd-search{position:relative;z-index:0}` collecting the abspos thumbnail.
TD-0030 recorded body's empty padding box as an intervening clip and rejected
the `ytd-search` unit.

**Fix.** CSS Overflow propagation for continuous media: `html` / `body` never
establish a local overflow clip or scroll container (`Box::ClipsOverflow`).
Document scroll remains the page's viewport offset (`layout_.scroll_y`). The
zero-area `InterveningClipApplies` workaround is removed. Tests:
`Page/HitTestsAbsposUnderNonStackingAbsposAncestor`,
`Page/HtmlBodyOverflowDoesNotClipAsLocalScroller`.

**Still approximate.** Used overflow is forced `visible` on both elements
rather than only the CSS propagator, and the viewport does not yet honour
`overflow:hidden` on html/body as a scroll lock. Good enough that youtube
search hits work; a later pass can store propagated viewport overflow policy.

**Close when.** Done for the hit/paint clip half. Scroll-lock from
`body{overflow:hidden}` remains open if a page needs it.

---

## TD-0035 — Click / focus / activation stopped at shadow roots — **fixed 2026-08-10**

**Opened** 2026-08-10 while chasing youtube `/results` → `/watch`.

**Symptom.** After TD-0034, `elementFromPoint` correctly hit the `<img>` inside
`a#thumbnail`'s `yt-image` shadow. Press/release still produced no click
default: `DispatchPointerReleaseAt` walked `Parent()` only, so a shadow-tree
target was "not in the document" and cleared; `FocusFromClickAt` and
`ResolveClickActivation` stopped at the shadow root the same way. Event
propagation already crossed hosts via `ShadowHostOf` (ADR 0019); the engine's
click router did not.

**Fix.** Composed-parent and in-document walks in `Page.cpp` use
`dom::ShadowHostOf` the same way `EventDispatch` does. Regression:
`Page/ClickOnImgInsideShadowUnderLinkActivatesHref`.

**Close when.** Done for the engine click path. Audit other `Parent()`-only
ancestor walks that decide user input (Tab order already uses focusables;
form ownership is light-DOM by spec).

---

## TD-0036 — Document content height ignored absolutely positioned content — **fixed 2026-08-10**

**Opened** 2026-08-10 after TD-0035: youtube `/results` thumbs hit correctly but
stayed unreachable below the fold.

**Symptom.** `ytd-app` / `#page-manager` report ~10 000px via offsetHeight, while
`document.documentElement.scrollHeight` stayed at the viewport (~900).
`scrollIntoView` on `a#thumbnail` was a no-op; wheel on the document moved
nothing. Root cause: `LayoutEngine::Layout` returned in-flow cursor / floats
only. After abspos against the ICB (the youtube shape — body `height:0`,
`ytd-app { position:absolute; min-height:100% }`), cursor collapses and the
tall content never extended document height.

**Fix.** After `LayoutAbsoluteDescendants`, document height is
`max(cursor, floats, MeasureScrollableOverflow(root))`. The overflow walk
already includes abspos and skips fixed. Regression:
`Scroll/DocumentHeightIncludesAbsolutelyPositionedContent`.

**Close when.** Done for document scroll height. Nested abspos overflow edge
cases stay covered by the same overflow walk used for scroll containers.

**Follow-up** (same day). `scrollIntoView` under `position:fixed` must not
drive the document scroller — once document height was real, Accept's
`scrollIntoView` scrolled the page by the button's layout Y and threw the
dialog off-screen. Fixed in `Page::ScrollIntoView`. Also: fixed subtrees are
viewport-laid-out; `getBoundingClientRect` / hit-testing must not apply
document scroll to them (`AncestorScrollOffsets`, `PointForBox`). See
`Scroll/ScrollIntoViewUnderFixedDoesNotMoveTheDocument`.

**Measured** (Release, 2026-08-10). After Accept on
`/results?search_query=cats`, `scrollHeight≈9k`, `scrollIntoView` on
`a#thumbnail` brings it to the viewport, `-click last` soft-navigates to
`/watch?v=…`. Hit-testing is flaky (TD-0037): sometimes `IMG`/`a#thumbnail`
(and focus is the link), sometimes `YTD-SEARCH`. Soft-nav `#movie_player`
stamp was aborted by a results drain that treated the new `/watch` URL as
"results ready" (TD-0038).

---

## TD-0037 — `elementFromPoint` returns `ytd-search` for in-view thumbnails

**Opened** 2026-08-10 after TD-0036 made result thumbs reachable.

**Symptom.** Release `/results` after Accept + `scrollIntoView(a#thumbnail)`:
thumbnail / `ytd-video-renderer` client rects contain `(x,y)`, but
`document.elementFromPoint(x,y)` is sometimes `YTD-SEARCH` (chain stops there).
Other runs on the same flow hit `IMG` and focus `a#thumbnail`. Paint shows the
thumb either way; SPA navigation can still succeed.

**Live computed style (2026-08-10).** `ytd-app` `position:absolute;z-index:auto`;
`ytd-search` `position:relative;z-index:0;display:flex;
overflow-x:visible;overflow-y:hidden`; light children `#container` / `#survey`;
`a#thumbnail` `position:absolute; overflow:hidden`. Fixtures that copy that
shape — including flex + asymmetric overflow after `scrollIntoView`
(`Page/HitTestsAbsposInFlexOverflowYHiddenAfterScroll`) — still hit the
thumbnail. The live miss is not steady-state CSS; it correlates with early
post-Accept drain / incomplete stamp (self-hit on `ytd-search` after descendant
units miss).

**Harness mitigation (2026-08-10).** `YoutubeResultsLooksReady` now requires
`elementFromPoint` on the first `a#thumbnail` centre to hit the thumb chain
(not stop at `YTD-SEARCH`), in addition to ≥12 renderers. Engine hit-test root
cause still open.

**Close when.** `elementFromPoint` / `ElementAt` agree with the painted topmost
thumb (img or `a#thumbnail`) on `/results` after scrollIntoView, stably across
runs; regression from a minimal fixture once one exists.

---

## TD-0038 — Snapshot results drain aborts watch settle after soft-nav — **fixed 2026-08-10**

**Opened** 2026-08-10 while measuring search→watch `#movie_player`.

**Symptom.** Click on a search thumb updates `location` to `/watch` and can
focus `a#thumbnail`, but the following `-eval` still sees no `#movie_player`.
`RunLoadToCompletion`'s post-load drain captured `youtube_results_drain` from
the URL at entry. After `pushState` to `/watch`,
`YoutubeResultsLooksReady` returns true whenever the URL is not `/results`,
so the loop broke immediately and never applied the 90s watch settle.

**Fix.** Re-read watch/results mode every drain iteration. On a transition
into `/watch`, reset the post-load deadline to the watch budget and count
`snapshot.soft_nav_watch_drain`.

**Close when.** Done for the snapshot tool. Engine soft-nav itself is unchanged.

---

## TD-0039 — Trusted click/key inherited a spent JS step budget — **fixed 2026-08-10**

**Opened** 2026-08-10 after soft-nav search→watch showed `js.steps_exhausted`
and either zero `media.source_appends` or a full href navigation that left a
50-command watch shell.

**Symptom.** A click after a long stamp/rAF turn ran handlers with
`steps_ > kMaxSteps`. Youtube's search handler never reached `preventDefault`,
so `ResolveClickActivation` followed `a#thumbnail` as a document navigation.
Timers/fetch already called `BeginTask`; trusted pointer/key dispatch did not.

**Fix.** `DispatchPointerMouse` and `DispatchKey` call `Interpreter::BeginTask`
before building the event. Regression:
`Page/TrustedClickGetsAFreshStepBudget`.

**Still open.** Soft-nav can still show `js.steps_exhausted` during player
module load (separate from the click task). Zero MSE appends on some soft-nav
runs after the player stamps remain under TD-0020 / step storms.

**Close when.** Done for trusted input tasks.

---

## TD-0040 — Soft-nav `sourceopen` inherited a spent JS step budget

**Opened** 2026-08-10 after SPA search→watch stamped `#movie_player`, created
SourceBuffers (`media.source_buffers_created` 6), and never called
`appendBuffer` (`media.source_appends` 0, `readyState` 0) across a 90s watch
settle. Cold watch on the same machine had reached `rs:4` with dozens of
appends on earlier sessions; soft-nav is the heavier path before
`video.src = blob:…`.

**Symptom.** `sourceopen` fired synchronously from the `src` attribute write
(`ReflectedAttributes` → `DeliverMediaSourceOpenedFor` → `FireOn` with
`MediaEventBudget`). Under live frames / a half-spent outer allotment,
`EnterNestedHostBudget` often does not reset. The handler can
`addSourceBuffer` (cheap) then abort before SABR starts a googlevideo fetch /
first append. `updateend` was already a `TimerQueue::QueueTask` for the same
family of bug (TD-0020); `sourceopen` was not.

**Fix.** `ScheduleMediaSourceOpened` queues delivery like
`ScheduleSourceBufferEvents`. Regression:
`Page/MediaSourceOpenRunsAsAHostTask`.

**Still open until measured.** Soft-nav may still show zero appends from H2
session death on SABR POSTs (TD-0041) or residual step storms after a
fresh `sourceopen` task — confirm with `MICROBROWSER_LOAD_TIMELINE=1` for
`googlevideo` dones and `js.steps_exhausted`.

**Close when.** Soft-nav search→watch reaches `readyState >= 2` with
`media.source_appends > 0` on Release, stably across runs.

---

## TD-0041 — Soft-nav SABR `videoplayback` POSTs never complete after H2 session death

**Opened** 2026-08-10 after Release soft-nav
`/results?search_query=cats` → Accept → thumb: `#movie_player` stamped,
`media.source_buffers_created=6`, `media.source_appends=0`, `readyState=0`.
Timeline: **8** `request.start` for `googlevideo.com/videoplayback?…&sabr=1`,
**0** `request.done` for those URLs; other streams later `FAILED the connection
failed`. `js.steps_exhausted=3`.

**Cause.** TD-0025 retried GET/HEAD on shared-session death but not POST.
YouTube SABR uses POST (buffered body, Content-Type unset). Those streams died
with the session and were never re-asked, so `sourceopen`/`addSourceBuffer`
ran with nothing to append.

**Fix.** Same one-shot session-death retry for POST when no response was
processed (`MayRetry`). Test `Http2Fetch/APostSurvivesADeadSharedSessionOnce`.

**Update** (2026-08-10, after fix). Soft-nav Release remeasure: googlevideo
`request.done` **1**/11 (was 0/8), `net.h2_retried=9`, still
`media.source_appends` absent / `readyState` 0. Retry helps some POSTs;
remaining hang/incomplete SABR responses and `js.steps_exhausted` still block
playback. Counter `media.source_append_attempts` added to separate "never
called" from "called and failed". Follow-on: TD-0042 (per-stream stall +
fair body pump + BeginNetworkTask).

**Close when.** Soft-nav shows `request.done` for googlevideo and
`media.source_appends > 0` / `readyState >= 2` on Release.

---

## TD-0042 — Soft-nav SABR hung on shared H2 progress and spent fetch budgets

**Opened** 2026-08-10 after TD-0041: 11 `videoplayback` starts, 1 done (200),
0 `appendBuffer` attempts, `js.steps_exhausted=1`.

**Causes.**
1. `FetchRequest` treated any `Http2Session::Advance()` progress as *this*
   request's progress, so one live multiplexed stream reset every sibling's
   30s stall timer while POST bodies sat behind the connection send window
   without END_STREAM.
2. `PumpBodies` drained streams in vector order until the window was empty,
   starving later SABR POSTs.
3. `DeliverFetchResponse` called `BeginTask()` / `BeginHostTurn`, which is a
   no-op under live frames — soft-nav delivery from `RunDueWork` left SABR's
   `then` on a spent hang-guard allotment.

**Fix.** Per-stream `AccountingOf` for stall; round-robin one-frame
`PumpBodies`; `Interpreter::BeginNetworkTask` always zeros steps (counter
`js.fetch_delivery_budget_resets`). Tests:
`Http2/TwoPostBodiesShareTheSendWindow`, BeginNetworkTask under live frames.

**Remeasure (Release, 2026-08-10, after fix).** Soft-nav search→watch:
SABR `videoplayback` **12 start / 1 done (200)** (was 8/0); progressive
itag=18 still **403**; `media.source_append_attempts` absent; `js.steps_exhausted=2`;
`net.h2_stream_stalls` 0. A second run with a fetch/`appendBuffer` probe saw
**2 SABR 200s**, **4 appends**, init segments, decoder frames — so TD-0042
unblocks the path when a POST completes, but most SABR streams still end as
`fetch.aborted` without `request.done` (player retry). Watch readiness was also
reading the wrong `<video>` (TD-0043).

**Close when.** Soft-nav `videoplayback` dones ≈ starts (or aborts accounted),
`source_append_attempts` and `source_appends` > 0 stably, player
`readyState >= 2` on Release.

---

## TD-0043 — Watch readiness / eval queried a non-player `<video>`

**Opened** 2026-08-10 after soft-nav runs showed `media.source_appends` /
`media.decoder_frames` / `media.video_sessions` while
`document.querySelector('video').readyState` stayed **0** and `buffered` empty.

**Cause.** Youtube watch can stamp more than one `<video>`. The first in tree
order is often a placeholder without `src` / MediaSource; MSE attaches to
`#movie_player video`. `YoutubeWatchLooksReady` and ad-hoc `-eval` probes used
the first match, so the snapshot's 90s settle never saw HAVE_* and reported a
dead player beside a live decoder session.

**Fix (harness).** Prefer `#movie_player video`, then `video[src]`, then any
`video`. Results drain also waits until `elementFromPoint` on the first thumb
hits the thumb chain (TD-0037), not merely ≥12 renderers.

**Close when.** Soft-nav Release cats search→watch reports player
`readyState >= 2` (or buffered > 0) when appends/decoder counters are non-zero;
document any remaining engine-side multi-`<video>` confusion separately.

---

## Closed

- **TD-0039 — Trusted click/key inherited a spent JS step budget** (2026-08-10).
  `BeginTask` on `DispatchPointerMouse` / `DispatchKey` so preventDefault can
  run after a spent stamp turn.
- **TD-0038 — Snapshot results drain aborted watch settle after soft-nav**
  (2026-08-10). Drain mode follows the current URL; `/results`→`/watch`
  resets the 90s watch deadline (`snapshot.soft_nav_watch_drain`).
- **TD-0036 — Document content height ignored abspos** (2026-08-10).
  Layout return value includes MeasureScrollableOverflow after ICB abspos;
  youtube results become document-scrollable.
- **TD-0035 — Click/focus/activation stopped at shadow roots** (2026-08-10).
  Engine click router walks composed parents via `ShadowHostOf`; youtube
  thumbnail `<img>` under `a#thumbnail` activates href.
- **TD-0034 — html/body overflow clipped as local scroll containers** (2026-08-10).
  Used overflow propagates to the viewport; html/body no longer PushClip or
  reject collected units. See open entry (closed for the clip half).
- **TD-0033 — Restyle treated non-box DOM as display changes** (2026-08-10).
  UA hidden-input `display:none`; skip missing boxes under replaced parents.
  Youtube `/results`: display invalidations 280 → 7; BuildBoxTree ~8.3 s/390 →
  ~5.2 s/118.
- **TD-0030 — Collected stacking units skip intervening overflow clips** (2026-08-10).
  Collect records padding clips with scroll_delta; paint PushClip / hit-test
  Contains. Abspos exception retained for thumbnail layout.
- **TD-0029 — Event UAF across DrainMicrotasks in DispatchEventTo** (2026-08-09).
  ValueRoot on the event; see open entry above (now closed).
- **TD-0027 — Binding URL arguments used pure `js::ToString`** (2026-08-09).
  See above (extended: reflected attrs, setAttribute, encodeURI*, sockets,
  fetch({}) refusal).
- **TD-0026 — youtube home searchbox preventDefaults Enter but never navigates**
  (2026-08-09). See above.
- **TD-0024 — SPA search→watch can leave `ytd-player` without `#movie_player`**
  (2026-08-09). Eval/CSP (ADR 0039), post-load script fetch, `load` before
  `data-loaded`, and TD-0025's GET retry. Release cats search→watch:
  `#movie_player` + `<video>` + `readyState` 4 / ~23s buffer / `isError:false`.
- **TD-0001 — Measure-then-place walked every flex/float/atomic subtree twice** (2026-08-09).
  `OffsetLaidOutSubtree` replaces the second `LayoutBlock` when constraints are unchanged.
  Youtube search: `layout.block_passes` 189M → 139k; `engine::LayoutBoxes` 128 s → 0.7 s;
  wall ~3–10× faster. See open entry above for the full before/after table.
- **TD-0019 — Decoded Opus frames never reach the audio ring** (2026-08-08). See open
  entry above for the measurement; closed by wiring `PageVideo` → `AudioRing` →
  `SdlAudioDevice` per ADR 0028 §4.
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
