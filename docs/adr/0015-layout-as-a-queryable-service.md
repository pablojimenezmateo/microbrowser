# ADR 0015 — Layout as a queryable service, and the seam script asks it through

**Status:** accepted · **Date:** 2026-08-04

## Context

`docs/surveys/2026-08-04-reddit-youtube-plex.md` counts what 16.2MB of application script asks the
platform for. The largest single category is not events, not networking and not storage. It is
geometry:

| | occurrences |
|---|---|
| `clientWidth` / `clientHeight` | 226 |
| `scrollTop` | 254 |
| `offsetWidth` / `offsetHeight` | 158 |
| `getBoundingClientRect` | 152 |
| `getComputedStyle` | 101 |
| **total** | **891** |

Every one of those is script asking a question that only layout can answer, and there is currently
no way for it to ask. `CLAUDE.md` already names this as the awkward part of the next milestone:

> `src/bindings` may see `js` and `dom` and **not `layout`**, which is a security boundary rather
> than an oversight (ADR 0008).

That boundary is real and this ADR does not move it. ADR 0008 put it there because `src/bindings` is
the module that runs attacker-supplied code against our data structures, and the layout tree holds
raw pointers into the box tree, the fragment list and the display list. A binding that can walk
layout can walk all of it. Widening `MODULE.deps` would be a one-line change that deletes the reason
the module exists.

There is a second problem underneath, and it is the one that actually decides the design. These
queries are not reads of stored values. `getBoundingClientRect` is defined to return the position a
box *would have* if layout were up to date, so calling it after a style mutation forces layout to
run. A framework that writes a style, reads a rect, writes another style and reads another rect
makes layout run twice — the pattern the web calls layout thrashing, and 891 call sites is enough
for it to be the dominant cost of a page rather than a curiosity.

So there are two decisions, and taking only the first is how this goes wrong: **who may ask**, and
**what asking costs**.

## Decision

### Geometry is a service the engine offers, not a module `bindings` includes

`src/engine` already sees both `bindings` and `layout`; it is the module whose job is to own a
document and everything computed from it. **The engine publishes a narrow, typed geometry interface;
`src/bindings` holds a reference to that interface and nothing else.** `MODULE.deps` for
`src/bindings` gains no new `allow:` entry, because the interface is a header the engine publishes
and bindings already depends on the engine seam.

The interface answers in **values, never in pointers**:

```
struct BoxGeometry {          // CSS pixels, relative to the initial containing block
  gfx::RectF border_box;
  gfx::RectF padding_box;
  gfx::RectF content_box;
  gfx::SizeF scrollable_overflow;
  gfx::PointF scroll_offset;
};

class GeometrySource {
 public:
  virtual std::optional<BoxGeometry> QueryBox(const dom::Node&) = 0;
  virtual std::optional<std::string> QueryUsedValue(const dom::Element&, std::string_view property) = 0;
  virtual void SetScrollOffset(const dom::Node&, gfx::PointF) = 0;
};
```

A node with no box — `display: none`, a detached subtree, a node from another document — answers
`nullopt`, which the binding turns into the all-zero rect the specification requires. That is not a
stub in ADR 0012's sense: an element with no box genuinely has no geometry, and zero is the honest
answer rather than an evasion.

**The value-not-pointer rule is the whole security content of this ADR.** A returned `BoxGeometry`
is a copy that outlives nothing. Script may hold it forever, and it cannot become a dangling
reference into a box tree that a later layout rebuilt. Compare what the alternative buys: a
`LayoutBox*` behind a binding is a use-after-free the first time a script keeps a rect across a
reflow, and in a browser a use-after-free is an RCE primitive rather than a crash.

### Layout is either clean or it is made clean, and the query says which

The engine tracks a **layout-clean flag**. `QueryBox` and `QueryUsedValue` check it, and if layout
is dirty they **run layout synchronously before answering**. There is no third option: returning a
stale rect is a wrong answer, and refusing to answer is a wrong answer that also breaks every
framework.

The cost of that is a forced layout in the middle of script execution, and it is bounded by making
it *visible* rather than by making it cheap:

- A counter, `layout.forced_by_script`, increments on every synchronous layout a query causes. It
  goes in `MICROBROWSER_PERF_COUNTERS` alongside the ones that already exist.
- A scope, `layout/forced`, so `MICROBROWSER_PERF_SUMMARY` attributes the time to script rather than
  burying it in the frame.

`guidelines/performance.md` says measure rather than guess, and this is the case where a page can
make the browser do arbitrary work through an innocuous-looking property read. Knowing it happened
is the difference between diagnosing a slow page and speculating about one.

**Batching is deliberately not decided here.** Real engines eventually grow a read/write phase split
so a `rAF` callback's reads all happen before its writes. That is a real optimisation and it is
premature: it needs the counter above to say whether it would pay, and the counter does not exist
yet. The decision is to build the honest slow version, instrument it, and revisit with a number.

### `getComputedStyle` returns used values, and that is a different question

`getComputedStyle` is misnamed in the specification: for a resolvable subset of properties it
returns the **used** value — `width` in pixels after layout, not `auto` as authored. So it is a
layout query, not a style query, for exactly the properties script cares about most.

The split is: properties whose used value equals their computed value are answered by
`src/css` without touching layout; the layout-dependent subset (`width`, `height`, the `inset`
properties, `margin`, `padding`, `border-*-width`, `transform`) goes through `QueryUsedValue` and
forces layout like any other geometry read. Which properties are in which set is a table, and the
table is tested against the specification's "resolved value" definitions rather than inferred.

The dishonest implementation — answering every property from the computed style and calling it
`getComputedStyle` — is refused on ADR 0012's rule. It is present, plausible and wrong, and a page
that lays itself out from a returned `"auto"` fails somewhere else entirely.

### `scrollTop` is a write as well as a read, and it belongs to the engine

254 occurrences, and roughly half of them assign. That makes the scroll offset a piece of state
layout consults rather than a piece of state layout owns, which is why `SetScrollOffset` is on this
interface and why the scroll model itself is ADR 0018 rather than this one. What is decided here is
only that the offset is reached through the same seam and by the same rule: a value in, a value out,
no pointer.

### Geometry is a fingerprinting surface, and the answer is not to lie

Text metrics reveal which fonts are installed; viewport and device-pixel-ratio reveal the window and
the display. That is real, and the mitigation is **not** to perturb the numbers: a browser that
returns a jittered rect breaks every layout that reads one, and 891 call sites means it breaks
everything.

The defence belongs where ADR 0029 puts it — controlling what the font stack and the viewport can
*be*, not what geometry reports about them. A page that can only see the fonts we ship learns
nothing from measuring them. That is written here so the next person to notice the leak finds the
decision instead of adding noise to a rect.

## Consequences

- **`src/engine` grows the geometry interface and its budget will fire.** That is the mechanism
  working; the interface is a new kind of thing the engine owns and the manifest should say so.
- **Layout gains a clean/dirty flag with real consequences.** Anything that mutates the DOM or a
  style must mark it dirty, and missing one produces a stale rect — a silent wrong answer of exactly
  the kind this project ranks worst. It needs a test that mutates and immediately queries, for each
  kind of mutation.
- **A page can now make the browser do unbounded work synchronously.** A loop of write-then-read
  runs layout every iteration. That is true of every browser and it is not a vulnerability, but it
  is a new denial-of-service shape and the counter is what makes it diagnosable rather than
  mysterious.
- **`IntersectionObserver` and `ResizeObserver` become tractable.** Both are geometry over time
  (53 and 96 occurrences in the survey), and both are the same `QueryBox` driven by the frame
  deadline from ADR 0011 rather than by script.
- **This is the last structural blocker in ADR 0012's list.** Everything that ADR left unbuilt
  either needed this or needed shadow DOM (ADR 0019).

## Alternatives considered

**Add `layout` to `src/bindings`' `allow:` list.** Rejected, and it is worth saying why bluntly:
it is one line, it makes every one of these bindings trivial to write, and it hands the module that
executes attacker-supplied code a direct reference into the box tree. ADR 0008's boundary exists for
this exact request.

**Push geometry through `src/ipc` as a message, like everything else the UI asks.** Rejected as the
wrong axis. IPC is the UI↔engine seam, and this is a within-engine call from a binding to layout —
routing it through a serializable message would add a copy, a variant arm and a round trip to a call
that happens 891 times per page load, and it would still not answer the question of who may make it.

**Answer from a cached geometry snapshot taken at the end of layout, and never force.** Attractive:
no synchronous layout, no thrashing, one predictable cost per frame. Rejected because it is wrong
whenever script mutates and then measures, which is the single most common pattern in the surveyed
code. It converts a slow correct browser into a fast one that silently disagrees with itself.

**Return opaque handles that the engine resolves later.** Rejected as a worse version of the same
thing. It keeps the lifetime problem — the handle table has to be invalidated on relayout — and adds
an indirection, in exchange for deferring a cost the counter has not yet shown to matter.
