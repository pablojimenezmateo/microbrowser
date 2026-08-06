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
pass already has every element's resolved style in hand. The cost is timing: the requests would go
out after the first box tree rather than before it. The comment at the call site argues that would
"show the page twice" — worth re-examining, because images already arrive asynchronously and
already cause a second paint.

---

## TD-0006 — Inflate runs at roughly a tenth of the speed it should

`net::DecodeContentEncoding` scope, on youtube.com: seven gzip responses of 780KB–920KB, at
**85–119ms each**, which is on the order of 10MB/s. A production inflate manages 200–400MB/s on this
hardware. Brotli is fine by comparison — 2.1MB in 22ms — which points at `util::Inflate` rather than
at the framing around it.

**Measured** but not diagnosed: nothing here has looked at why. The likely candidates are a
bit-at-a-time Huffman decode and a byte-at-a-time match copy, both of which are the textbook first
implementation.

**End state.** Table-driven Huffman with a multi-bit peek, and a match copy that moves words. Both
are local to `util/Inflate.cpp` and both are exactly the kind of change that wants the existing
fuzz target run against it.

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

## TD-0008 — No HTTP/2, so a burst of six connections gets rate-limited and the page loses images

`en.wikipedia.org/wiki/CSS` renders **between 4 and 17 of its 19 images, at
random, from run to run**. The cause is not in this browser's loop, its decoders
or its layout: `upload.wikimedia.org` answers **HTTP 429** to a burst of six
parallel HTTP/1.1 connections, and this browser opens six because
`kMaxConnectionsPerPartition` is six and it has no other way to fetch six images
at once.

**Measured**, with the load timeline added in `55f7b40`, five consecutive runs of
the same page:

| run | images drawn | `engine.images_failed` |
|---|---|---|
| 1 | 14 | 2 |
| 2 | **4** | 15 |
| 3 | 14 | 5 |
| 4 | **4** | 15 |
| 5 | 14 | 5 |

`net.fetches` is 24 every time: the requests are made, and the responses are
429s. Reproduced outside this browser -- six parallel `curl --http1.1` requests
to that host return `200 200 200 429 200 429` -- so it is the concurrency and
not the `User-Agent`, which was the first suspicion and was tested and cleared.

A real browser never sees this, because it speaks HTTP/2 to that host: one
connection, multiplexed, no burst to rate-limit. This is the first *rendering
correctness* cost anybody has measured for the missing transport, as opposed to
a latency cost, and it is the argument ADR 0010 §3 did not have.

**End state.** ALPN and HTTP/2 -- ADR 0010 §3, mostly a parser problem: framing,
multiplexing, flow control and HPACK. Lowering the concurrency bound is the
tempting cheap fix and is the wrong one: it slows every page that is not being
rate-limited to work around one that is, and the number that would work is a
guess about somebody else's edge.

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
