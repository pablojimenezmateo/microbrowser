# ADR 0043 — SVG is a document, and this browser draws it as a picture

**Status:** accepted · **Date:** 2026-09-01 · **Supersedes:** nothing · **Amends:** nothing

## Context

`docs/wpt-plan.md` task F7 says "this browser renders SVG as an image, and the suite tests it as a
document. Decide the scope in an ADR before writing code." That sentence is exactly right and it
understates the gap. Here is the state, measured rather than described.

**What exists.** `src/gfx/SvgDecoder.cpp` parses an SVG byte stream and rasterizes it: paths,
shapes, groups, transforms, paint servers, and the presentation attributes those need. It is
reached from two places, and both of them hand it *text*:

- `<img src="logo.svg">`, through the ordinary image path; and
- an **inline `<svg>` in an HTML document**, which `engine::PageImages` handles by
  **serializing the element back to markup and giving it to the same decoder**
  (`src/engine/PageImages.cpp`, "Inline `<svg>`: serialize the element and rasterize at the used
  size"). `src/layout/ReplacedBoxes.cpp` treats the element as replaced content with an intrinsic
  size, like an `<img>`.

**What that costs, probed directly.** For `<style>rect { fill: green }</style>` around
`<svg><rect id=r …/></svg>`:

| asked | answered |
|---|---|
| `r.constructor.name` | `HTMLUnknownElement` |
| `r.getBoundingClientRect()` | `0, 0, 0, 0` |
| `getComputedStyle(r).fill` | `""` |
| `typeof r.getBBox` | `undefined` |
| `typeof r.getCTM` | `undefined` |

So: an SVG element has no interface, no box, no computed style and no geometry. The subtree is a
string that happens to live in the DOM. Every mutation a script makes to it is re-serialized and
re-rasterized, and nothing about it can be asked a question.

**And until this ADR's first stage, an SVG *document* could not run its own tests at all.** 263 of
`svg/`'s 1,243 files timed out, and 214 of those were one line: an SVG document writes its external
scripts as

```xml
<h:script src="/resources/testharness.js"/>
```

with `h` bound to the XHTML namespace. `PageScript::Collect` matched `TagName() == "script"`, and
the qualified name is `h:script`, so testharness.js was never fetched — the file's own inline
script then threw `ReferenceError: test is not defined` and the page reported nothing. The
neighbouring bug was the same shape: `engine::ParseDocumentFor` never called
`Document::SetHtmlDocument(false)` on a document it had just parsed with the *XML* parser, so
`tagName` came back `H:SCRIPT`, ASCII-uppercased as though the document were HTML.

**Where the area stands, 2026-09-01, after those two lines:**

| | before | after |
|---|--:|--:|
| reported subtests | 3,778 | **5,242** |
| subtests passing | 265 (7.0%) | **479 (9.1%)** |
| files whose harness never reported | 263 | **51** |
| Firefox-gap files | 810 | **769** |
| `blocked` in `docs/wpt-firefox-gap.md` | 228 | **51** |

Reftests are 298 of 526 — **but 187 of those are two blank pages agreeing**, so the honest figure
is 111. That ratio (36% of the area's reftest "passes" prove nothing) is the highest in the suite
and is itself a fact about how little of SVG is drawn from the document tree.

**And the gap is not evenly spread.** Of the 769 files:

| where | files | what it is |
|---|--:|---|
| `svg/animations` | 280 | SMIL: `<animate>`, `<set>`, `<animateMotion>`, syncbase timing |
| `svg/types`, `svg/painting`, `svg/geometry` | 224 | the SVG DOM and the SVG presentation properties |
| everything else | 265 | a long tail, most of it downstream of the two rows above |

One file, `svg/idlharness.window.html`, carries **1,661 failing subtests** — twelve times the next
largest in the area. It is the whole SVG IDL surface asked at once.

## Decision

**SVG becomes a real subtree of the document: styled by the cascade, boxed by layout, and reachable
from script. It stops being a string handed to an image decoder. SMIL is refused.**

Four parts, in the order they unblock each other. Only the first has landed.

### 1. The document half — landed 2026-09-01

An XML document is not an HTML document, and a script element is `(namespace, local name)` rather
than a qualified name. `dom::IsScriptElement` is one predicate in `src/dom/Namespaces.h` with four
callers, for the reason `CanHostShadowRoot` is one predicate: four copies of the question are four
chances to answer it differently.

This is plumbing and it is worth 212 files of the 263 that were silent. **It is also the reason the
rest of this ADR could be written from measurements instead of from guesses** — before it, 228 of
`svg/`'s Firefox-gap files contributed no information in either direction.

### 2. The SVG presentation properties are CSS properties

`fill`, `fill-opacity`, `fill-rule`, `stroke` and its seven relatives, `stop-color`, `stop-opacity`,
`paint-order`, `clip-rule`, `color-interpolation`, `shape-rendering`, `text-anchor`, `dominant-baseline`.
They go in `css::ComputedStyle` and through `ApplyDeclaration` like every other property, and a
presentation *attribute* maps to one exactly as HTML's presentational attributes already do
(`src/css` owns that table today).

This is first because everything else depends on it: `getComputedStyle(rect).fill` returning `""`
is what makes the painter's job undefined, and `svg/styling/` (20 files), `svg/painting/` (73) and
the `inheritance.svg` family are its direct tests.

**It is also the one part that does not need the box tree**, so it can land alone and be measured
alone.

### 3. The SVG DOM: element interfaces, then geometry

`SVGElement` under `Element`, and the per-tag interfaces under it — the same shape
`src/bindings/NodeInterfaces.cpp` already builds for HTML. Then the geometry methods that need a
box: `getBBox`, `getCTM`, `getScreenCTM`, `getTotalLength`, `getPointAtLength`.

**The animated-value pairs (`SVGAnimatedLength`, `SVGAnimatedNumber`, `SVGLengthList`,
`SVGTransformList`, `SVGPathSegList`) are in scope only as far as their *base* value.** They are a
1990s reflection API whose `animVal` half exists to expose SMIL, and SMIL is refused below — an
`animVal` that always equalled `baseVal` would be the stub ADR 0012 forbids. `baseVal` is a real
reflection of a real attribute and is what `svg/types/` (83 files) actually asks about.

### 4. SVG boxes in the layout tree

The `<svg>` element establishes an SVG formatting context; its children get boxes with geometry in
the local coordinate system; `viewBox`, `preserveAspectRatio` and `transform` are the mapping to the
parent. Painting reads the box tree rather than a re-serialized string.

This is the largest piece and it is what retires `PageImages`'s serialize-and-rasterize path for
inline SVG. `<img src="x.svg">` keeps the decoder — a document referenced as an image is a document
this browser must not run script in, and the decoder is what makes that structural rather than
remembered.

### Refused: SMIL animation

`svg/animations/` is 280 of the 769 gap files — **36% of the area, and the largest single row in
it**. It is refused, and the refusal is the point of this ADR rather than an omission from it.

Three reasons, in order:

1. **It is a second animation timeline.** This browser will have CSS animations and transitions
   (task G4 and the `web-animations/` area), and those share one model. SMIL is a different one,
   with its own document timeline, its own `begin`/`end` attribute grammar, its own event set
   (`beginEvent`, `endEvent`, `repeatEvent`), and **syncbase timing** — an animation whose start is
   defined by another animation's start, which is a dependency graph with cycles a document can
   write (`svg/animations/cyclic-syncbase-2.html` is a test that the cycle is *detected*). Two
   timelines is not twice the work; it is twice the work plus the interactions between them, and
   the interactions are what `svg/animations/animate-currentcolor-in-visited-link.html` is about.
2. **It is a wakeup source the user did not cause.** A `<animate repeatCount="indefinite">` in a
   document starts a clock on load and never stops it. The zero-idle-CPU invariant is not a
   performance target here, it is the one thing `src/app`'s main loop is built around, and every
   feature that has needed a timer has needed a design against it (ADR 0011,
   `bindings::AnimationFrames`). A declarative always-on timeline needs that design, and it should
   be written once for *the* animation model rather than first for the one that is not it.
3. **The cost is not repaid by the rest of the suite.** Nothing outside `svg/animations/` depends
   on SMIL — the other 489 gap files are the DOM, the properties and the box tree, and all of them
   are also what `css/`, `html/canvas/` and real pages want. SMIL buys 280 files and nothing else.

**The condition to revisit it**, stated so the refusal is checkable rather than permanent: when the
Web Animations timeline exists (task G4) and one of ADR 0007's compatibility targets is measured to
use SMIL. `grep -rn '<animate' ` over a fetched copy of the five targets is the measurement; nobody
has run it, and this ADR does not claim its result.

Until then `svg/animations/` gets expectation lines and no code, and this file is the comment those
lines name.

### What stays refused for a different reason

- **SVG fonts** (`svg/fonts/`, 3 files) — removed from SVG 2, implemented by no shipping engine.
- **`svg/print/`** (1 file) — there is no print path at all; this is not an SVG decision.

## Consequences

**The target.** With SMIL refused, `svg/`'s reachable gap is **489 files of 769**. The target for
the area is therefore stated against what is in scope:

> `svg/` — 300 of the 489 non-SMIL Firefox-gap files, and `svg/animations/` recorded as a refusal.

`docs/wpt-tasks.json` carries that as F7a (the properties, §2), F7b (the DOM, §3) and F7c (the box
tree, §4). F7 itself is this document.

**The reftest number in `svg/` is not comparable with any other area's until §4 lands.** 187 of its
298 reftest passes are two blank pages. A session that moves that number should say which half it
moved; `microbrowser_wpt --reftests-only svg/` prints both.

**`src/gfx/SvgDecoder.cpp` does not go away and must not grow.** It is the `<img src="x.svg">` path
and the reason an SVG referenced as an image cannot run script — the same argument ADR 0013 makes
for keeping a demuxer unable to decode. §4 gives the *document* path its own implementation over the
box tree; the two will share the rasterizer in `src/gfx` and nothing above it.

**Two bugs found on the way here are not about SVG at all**, and both are recorded because the next
agent will meet them elsewhere:

- `Document::SetHtmlDocument` was never called by the engine's own parse, so **every**
  `application/xhtml+xml` and `image/svg+xml` document this browser has ever loaded believed it was
  an HTML document. `tagName` was the visible half; `createElement`, `createCDATASection`,
  `createAttribute` and selector case-sensitivity all branch on the same flag. `DOMParser` had it
  right from the day it landed, which is why it went unnoticed: the only XML documents anybody had
  looked at were the ones a page made for itself.
- A qualified-name comparison is wrong in an XML document, always. `TagName()` is the *qualified*
  name; `LocalName()` and `Namespace()` are the question. `grep -rn 'TagName() == "' src/` still
  finds several dozen, and each is a latent version of this bug.
