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

## TD-0005 — Collecting background images resolves the whole cascade a second time

`Page::CollectImages` ends with `resolver_.ForEachStyledElement(...)` purely to read
`style.background.image` off every element. That is a full cascade over the document, and it runs
immediately before `BuildBoxTree` resolves the same cascade again for the same elements.

`CLAUDE.md` already names this among the known-crude spots. What is new is the number.

**Measured.** `engine::CollectImages`, a scope added for this: **1.58s over 3 calls** on
en.wikipedia.org/wiki/CSS, against 1.25s for `BuildBoxTree` itself. It is the single largest
non-JavaScript item left on that page — larger than laying the page out.

Before the copy-on-write custom properties landed it was 4.66s, so it shrank with the cascade; it
is still an entire duplicate pass.

**End state.** Collect background images *during* the cascade that builds the box tree, since that
pass already has every element's resolved style in hand.

**Two routes were considered on 2026-08-06 and both have a catch worth knowing before starting.**

*Collect from the box tree instead.* Every `Box` already carries its resolved `ComputedStyle`, so
reading `background.image` off a walk of `boxes_` costs no cascade at all. The catch is timing, and
it is not the one the old call-site comment names: the requests would go out after the first
*layout*, and the first layout does not happen until every render-blocking resource has landed. That
is precisely the 375ms regression `55f7b40` removed from Hacker News, where `triangle.svg` was named
by a stylesheet that arrived at 726ms and was not requested until 1104ms.

*Cache the box tree.* `Page::Layout`'s own comment proposes this, and most of the machinery is
already there — `boxes_` is a member, and eight call sites already `boxes_.reset()` as the
invalidation signal. It is the better route, because the box tree is rebuilt five to six times per
load for a document that never changes. The catch is that `LayoutEngine` takes the `ImageProvider`
as an input, so an image *arriving* changes a replaced box's intrinsic size: every path that changes
an input has to be audited for invalidation before the cache can be trusted. Getting that wrong
renders a stale page, which is priority 1 against this entry's priority 4.

**Measured again on 2026-08-06 in a Release build** (the 1.58s above is Debug): 142ms over 3 calls,
against 272ms for `BuildBoxTree` over 5 and 367ms for `LayoutBoxes` over 5. `css.styles_resolved` is
**84,731** for one load of a document with roughly 10,600 elements — eight full cascade passes —
and `css.candidates_tested` is 3,053,593, which is 36 full selector evaluations per element.
`bench/CssBenchmarks.cpp` is the instrument for the per-element half of that.

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
paint so that something is on screen before the script runs at all. Worth an ADR of its own before
anything is attempted: "a script yields" is a change to the execution model, not an optimisation.

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

## TD-0010 — Six concurrent requests per partition, on a connection built for a hundred

`kMaxConnectionsPerPartition` is six, and the name is now wrong twice over: it
bounds *requests*, not connections, and six was the number the web assumed for a
decade of HTTP/1.1 because six was how many sockets a polite client opened. Over
one multiplexed HTTP/2 connection the equivalent number is what the server's
`SETTINGS_MAX_CONCURRENT_STREAMS` says, which is typically a hundred.

So this browser now opens one connection where it used to open six, and then
uses it six requests at a time.

**Measured**, old.reddit.com, Release build:

```
net.fetches             53
net.requests_started    53
net.requests_deferred   91     <- turns on which something was held back by the bound
net.h2_sessions          6
net.h2_streams          55
```

Ninety-one deferrals for fifty-three requests, against six sessions that between
them would have taken every one of the fifty-three at once.

**End state.** The bound has to become two bounds, because it is answering two
questions that used to have one answer: how many *sockets* may a partition open
(still about six, and still per partition for the ADR 0005 reason — a global
limit is a cross-site interaction the starved site can time), and how many
*requests* may be in flight (the sum over that partition's sessions of what each
peer permits, and six for anything still on HTTP/1.1).

The reason it is written down rather than fixed is that raising it is only safe
once a request's memory cost is bounded — a hundred concurrent streams is a
hundred response bodies accumulating, each bounded individually by
`HttpLimits::max_body` at 64MB and not at all in aggregate. That is a second
decision and it wants its own measurement.

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
from the ones this session used. A counter that says "an image was decoded" and
a counter that says "an image was painted" would make the gap a subtraction
rather than a difference between two eyeball counts, and there is no such pair
today — which is the `font.lookup_hits` lesson again: the counters that exist
measure the half that is working.

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
before it throws.

---

## Closed

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
