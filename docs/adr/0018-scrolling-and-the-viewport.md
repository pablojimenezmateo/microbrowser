# ADR 0018 — Scrolling, the viewport, and what a scroll costs

**Status:** accepted · **Date:** 2026-08-04

## Context

Scrolling is currently a wheel delta that reaches `Engine` as `ScrollMessage{delta_x, delta_y}` and
moves the document. Nothing else scrolls, nothing observes it, and `CLAUDE.md` records the cost:

> scrolling an overflowing document repaints in full because there is no scroll blit in the
> presenter

The survey says script does not treat scrolling as a viewport affordance. It treats it as an API:

| | occurrences |
|---|---|
| `scrollTop` | **254** |
| `scrollIntoView` / `scrollTo(` | 44 |
| `IntersectionObserver` | 53 |
| `ResizeObserver` | 96 |

`scrollTop` at 254 outranks `getBoundingClientRect` at 152. These applications read and write scroll
offsets constantly — a feed restores its position, a menu scrolls an item into view, a virtualised
list computes which rows exist from the offset. A browser where only the document scrolls, and only
because a wheel moved, cannot run any of it.

Three other things depend on this and are currently blocked behind it:

- **`position: sticky` parses as `relative`**, recorded in ADR 0014 as a deliberate approximation
  with a named blocker: there is no scroll offset to compare against.
- **`overflow: auto|scroll` clips but does not scroll.** Layout already computes the clip; what is
  missing is an offset and a way to change it.
- **`loading="lazy"`** — 27 uses on reddit's front page alone — and `IntersectionObserver` behind it,
  which is "is this box in the scrollport" asked once per frame.

And there is the invariant. Scrolling is the single most common continuous interaction in a browser,
and it is the one where a naive implementation burns a core: a scroll that recomputes style, reruns
layout and repaints the window at 60Hz is a browser that feels bad and drains a battery, and it is
the default outcome if the design is not decided.

## Decision

### 1. A scroll offset is layout state, owned per box

Every box with `overflow` other than `visible` — and the **viewport**, which is the document's
scrolling box — gets a scroll offset and a scrollable-overflow size. Layout computes the overflow
size; the offset is state layout consults and does not own, clamped to `[0, overflow - client]`
whenever layout changes it.

The offset is reached from script through ADR 0015's `GeometrySource` — `scroll_offset` is already a
field of `BoxGeometry` and `SetScrollOffset` is already on the interface — by the same rule as every
other geometry query: a value in, a value out, no pointer to a box.

`scrollTop` / `scrollLeft` read and write it. `scrollWidth` / `scrollHeight` report the
scrollable-overflow size. `scrollTo` / `scrollBy` / `scrollIntoView` are the same write with the
arithmetic done for the caller, and `scrollIntoView` is the one worth implementing carefully because
it has to walk *every* scrolling ancestor, not just the nearest one.

### 2. A scroll is a paint, not a layout

**Changing a scroll offset does not invalidate style and does not invalidate layout.** It changes
where the display list is sampled from. This is the load-bearing sentence of the ADR and everything
about the cost of scrolling follows from it.

The presenter gains a **scroll blit**: the overlap between the old and new scrollport is copied
within the framebuffer, and only the newly exposed band is painted. That is the fix `CLAUDE.md`
already asks for, and it is what makes a scroll cost proportional to the exposed strip rather than
to the window.

Two things break that and both are known in advance:

- **`position: fixed`** does not move with the scroll, so a fixed element intersecting the scrollport
  damages its own rectangle every frame. It is subtracted from the blit region.
- **`position: sticky`** moves *sometimes*, which is the whole feature. A sticky box's position is a
  function of the scroll offset of its nearest scrolling ancestor, computed at paint time from the
  offset rather than at layout time — which is precisely why it could not be implemented before
  there was an offset, and why it becomes straightforward now. It stops being a deviation and
  becomes a feature; ADR 0014's note is superseded on the day it lands.

### 3. The scroll event is throttled to the frame, and fires only if something asked

`scroll` fires on the scrolling element and bubbles to the document, **at most once per frame**, and
only if the offset actually changed. It is not fired synchronously from the input handler: a page
with twelve `scroll` listeners must not run them twelve times per wheel notch.

The scheduling rides ADR 0011's frame deadline, and it obeys the same rule `requestAnimationFrame`
established: **a document with a settled scroll offset and no pending frame schedules nothing.** The
loop wakes because a wheel event arrived, does the work, and goes back to blocking. There is no
scroll animation loop, and smooth scrolling — `scroll-behavior: smooth`, and the smooth flag on
`scrollTo` — is an animation that runs while it is running and then stops, registered through the
same mechanism `bindings::AnimationFrames` uses. Four tests already say that mechanism does not leak
a frame; this gets the equivalent.

### 4. Hit testing and input routing follow the offset

A pointer position is in viewport coordinates; hit testing (ADR 0017) subtracts the accumulated
scroll offsets down the box chain. A wheel event is routed to the deepest scrolling box under the
pointer that can still scroll in that direction, and **chains to its ancestors when it cannot** —
which is what makes scrolling inside a menu stop at the menu's end instead of scrolling the page
behind it, and what makes it continue when the menu is not scrollable.

That chaining rule is one line of specification and the difference between a browser that feels
right and one that does not.

### 5. `IntersectionObserver` and `ResizeObserver` land here, not with the frame work

Both are geometry sampled once per frame against a threshold, and both need exactly two things this
ADR provides: a scrollport to intersect against, and a frame at which to sample. Both deliver their
records as a task at the end of the frame, never synchronously from a scroll.

`IntersectionObserver` first, because `loading="lazy"` and every infinite feed on the target sites
are built on it. `ResizeObserver` second despite its higher count (96 against 53), because its
delivery has a re-entrancy rule — an observer callback that resizes an element must not loop forever
— and the specification's answer is a depth counter that is worth implementing after the simpler one
is working.

**Neither is shipped in a form that fires approximately.** ADR 0012's rule applies with full force
here: an `IntersectionObserver` that exists and never fires sends a page down the native path into a
wall, where its absence would have sent it to a polyfill that works.

## Consequences

- **`src/layout` gains per-box state that is not derived from style.** That is a genuine change to
  what a box is, and the box object-size budget will fire. It is the right place for the budget to
  fire, because it is a new kind of thing rather than a bigger version of an old one.
- **The presenter gains a scroll blit and the dirty-region policy gains a case.**
  `docs/performance/m6-damage.md` measured what incremental repaint saves; this is the second such
  measurement and it should be taken the same way, before and after, on a real page.
- **Fixed and sticky elements make scrolling more expensive, in proportion to how many there are.**
  A page with a fixed header damages that header's rectangle on every scroll frame. That is
  unavoidable and it is worth knowing, because it means "scrolling is cheap" is a statement about
  the page as well as about the browser.
- **`position: sticky` stops being a lie.** Until it lands, it keeps parsing as `relative`, and the
  note in ADR 0014 stays accurate.
- **Overflow scrollbars are chrome drawn inside a page**, which is the first thing this browser
  paints that is neither page content nor browser chrome. Where that code lives is a real question
  and the answer is `src/layout` plus `src/gfx` — a scrollbar is a box the user agent stylesheet
  creates, not a widget `src/ui` owns, because `src/ui` cannot see a document at all.

## Alternatives considered

**Keep document-only scrolling and add `scrollTop` on the document alone.** Rejected on the
measurement. The 254 `scrollTop` sites are overwhelmingly on elements — virtualised lists, feeds,
menus — and a document-only offset makes them all read zero, which is a wrong answer rather than a
missing feature.

**Treat a scroll as a relayout, and rely on layout being fast.** Rejected on arithmetic and on
principle. Layout on a page like reddit's is not free, 60Hz of it while a finger is on a wheel is
the CPU cost the project exists to avoid, and "rely on X being fast" is the shape of assumption
`guidelines/performance.md` says to replace with a measurement.

**Composite scrolling layers, like a GPU-accelerated browser.** Rejected for now, not forever. It is
the right long-term design and it presupposes a layer tree, which presupposes stacking contexts
(ADR 0014's step 4). The scroll blit gets most of the win with none of that, and it does not
foreclose layers later — a blit is what a single-layer compositor does.

**Fire `scroll` synchronously, as the older specifications did.** Rejected. It is observably
different only in that it is slower and more re-entrant, and every engine converged on the
frame-throttled behaviour because the synchronous one made pages janky in a way authors could not
fix.
