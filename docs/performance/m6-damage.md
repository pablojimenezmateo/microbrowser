# Incremental repaint: what the two-frame diff actually saves

Measured with `tests/DisplayListDiffTests.cpp` for correctness and with a driver that runs the
engine through real interactions for the numbers. Viewport 800×600 = 480,000 device pixels.

## The numbers

| Interaction | Frames sent | Pixels damaged | Share of the viewport |
|---|---|---|---|
| First resize (nothing on screen yet) | 1 | 480,000 | 100% |
| Navigate to a small document | 1 | 105,840 | 22% |
| Resize to the size it already is | **0** | 0 | — |
| Reload, identical content | **0** | 0 | — |
| Scroll a document that fits | **0** | 0 | — |

The three zero-frame rows used to be a full-viewport repaint *and* a texture upload each. They are
the cases that matter most, because they are the ones a browser hits constantly and gets nothing
for: a window manager echoing a configure event, a user pressing reload on a static page, a scroll
wheel at the end of a short document.

Navigating repaints 22% rather than 100% because the background fill is identical between frames
and only the document's own commands differ.

## What is *not* saved

**Scrolling a document that overflows still repaints everything.** Every command moves, so every
command differs, and the diff correctly reports that. Narrowing it needs a scroll blit in the
presenter — copy the overlapping region up or down, repaint only the newly exposed strip — which is
a change to `platform::SdlPresenter`, not to the diff. Until that exists, reporting full damage is
the honest answer rather than a missed optimization.

**A changed clip forces a full repaint.** A clip is state that every later command reads, so an
identical command after a moved clip draws somewhere else entirely. Tracking that would mean
replaying the clip stack during the diff, at which point the diff has become a second renderer.
The fallback is stated in the return value: `ComputeDamage` returns false and asks for everything.

## Why this shape

The alternative — every piece of layout and style code reporting what it invalidated — is the
design that produces stale-pixel bugs forever, because the one call site that forgot to report is
invisible until a user sees a smear. Diffing two frames has no such site. It costs one pass over
two command vectors, which is nothing next to rasterizing the commands.

Two comparisons had to resolve side-table indices rather than compare them:

- A `FillPathCommand` holds an index into *its own list's* path table. Two lists both using index 0
  for different geometry compare equal under `operator==`, and the diff would skip repainting a
  shape that moved across the page.
- The same for text runs and font requests.

`tests/DisplayListDiffTests.cpp` asserts the property rather than the rect list: start from the
previous frame's pixels, repaint only the damage, and the result must be pixel-identical to
repainting everything. A test that asserted "damage is these three rects" would pass just as
happily on a diff that under-reports.
