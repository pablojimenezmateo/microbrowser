# ADR 0019 — Shadow DOM, and the tree that layout actually sees

**Status:** accepted · **Date:** 2026-08-04

## Context

ADR 0012 put Shadow DOM last, and its amendment revisited half of that: custom elements moved to
native because the polyfill turned out to be a second DOM implementation rather than a gap-filler.
Shadow DOM was left where it was, on the reasoning that native custom elements need none of it.

That was correct about custom elements and wrong about the target sites. The survey counts, in
youtube.com's application bundle:

| | occurrences |
|---|---|
| `attachShadow` | 15 |
| `.shadowRoot` | 34 |
| `adoptedStyleSheets` | 16 |
| `customElements.` | 21 |
| `assignedNodes` / `assignedElements` | 4 |
| `new CSSStyleSheet` | 2 |

youtube.com's document is **909KB containing 421 tags, of which two are custom elements**:
`<ytd-app>` and `<ytd-masthead>`. The entire visible page is constructed by script into shadow trees.
There is no version of "render youtube.com" that does not include this.

reddit contributes zero `attachShadow` calls and is not therefore exempt: its markup carries
`slot="…"` **70 times**. Those are light-DOM children addressed to slots in shadow trees that its
components create at runtime. A browser without slots renders them, but renders them in the wrong
place and unstyled, because the styles are in the shadow tree they never reached.

The reason this is an ADR rather than a feature is that Shadow DOM is not an API. It is a change to
what "the tree" means, and four subsystems each have to be told which tree they are looking at:

- **layout** builds boxes from a tree where a slot is replaced by its assigned nodes
- **the cascade** matches selectors in a scope, and a rule in a shadow tree must not reach out
- **event dispatch** walks a path that is retargeted at each shadow boundary
- **the DOM API** answers `parentNode` one way and `assignedSlot` another

Getting any one of those wrong produces a page that is subtly, expensively wrong rather than one
that fails.

## Decision

### 1. Open shadow roots only, and say so

`attachShadow({mode: "open"})` works. `{mode: "closed"}` **also returns a root, but `element.shadowRoot`
returns `null` for it**, as the specification requires.

Closed mode is not a security boundary and this ADR will not pretend otherwise: the engine holds the
root either way, and any script that could have called `attachShadow` could have kept the reference.
It is implemented because pages use it and detect it, not because it protects anything.

Declarative shadow DOM (`<template shadowrootmode="open">`) is implemented in the tree builder at the
same time. It costs little once the runtime form exists, and it is how a server-rendered page gets a
shadow tree before its script runs — which is the direction the platform is moving and the direction
that suits a browser with a slow JavaScript engine.

### 2. The flattened tree is a separate traversal, computed on demand

Two trees exist and they are not the same:

- the **node tree**, which is what the DOM API answers about — `parentNode`, `childNodes`,
  `querySelector` within a root
- the **flattened tree**, which is what layout and the cascade walk — a shadow host is replaced by
  its shadow root's children, and a `<slot>` is replaced by its assigned nodes

The flattened tree is **not materialised as a second set of nodes**. It is a traversal — a
`FlatTreeIterator` in `src/dom` that layout and the cascade use instead of the child list. Building a
parallel tree of real nodes would double the memory per element and create two things that can
disagree, and disagreement between them is the bug class that would eat weeks.

Slot assignment is the state that traversal needs: each `<slot>` holds an ordered list of assigned
nodes, recomputed when the host's children change, when a slot's `name` changes, or when a slot is
added or removed. That recomputation fires `slotchange`, and it is the one piece of eager state in
this design because computing it lazily means the flat traversal has cost that varies with when you
walk it.

### 3. The cascade gains scopes, and `src/css` learns that a tree has an owner

Style resolution today takes a document and its sheets. It becomes: **resolve against the ordered
list of scopes an element is in**, innermost first.

The rules that come with it, each of which is a place to be wrong:

- A selector in a shadow tree's stylesheet **cannot match outside it**. Descendant combinators do
  not cross the boundary upward.
- A selector in the document **cannot match inside** a shadow tree, except through the explicit
  crossings below.
- **`:host`** matches the shadow host from inside. **`:host(sel)`** matches it conditionally.
  **`::slotted(sel)`** matches assigned light-DOM children from inside the shadow tree — and it
  matches them *at the slot's position*, which is the only selector in CSS whose subject lives in a
  different tree than the rule.
- **`::part()`** is the sanctioned way out, and it lands with the rest because a component that
  cannot be themed from outside is a component pages route around with `!important` and
  `adoptedStyleSheets`.
- **Inheritance crosses boundaries; matching does not.** A colour set on the host inherits into the
  shadow tree. That asymmetry is the entire ergonomic point of the feature and it is the thing an
  implementation most often gets backwards.

Cascade order at the boundary is decided by the specification's rules on scoping proximity, not by
document order, and it is tested against the specification's examples rather than against a page.

### 4. `adoptedStyleSheets` and constructable stylesheets

`new CSSStyleSheet()`, `sheet.replaceSync(text)`, and `root.adoptedStyleSheets = [sheet]` — 16
occurrences, and every Lit-based component on both reddit and youtube uses them for the same reason:
one parsed stylesheet shared by a thousand component instances instead of a `<style>` element per
instance.

That sharing is a memory decision as much as an API. **A `CSSStyleSheet` object is a shared, parsed,
immutable-after-`replaceSync` sheet**, referenced by every root that adopts it, and the cascade holds
it by reference. Implementing it as "clone the text into each root" would work and would multiply
youtube's stylesheet by its component count, which is how a browser with a good rasterizer runs out
of memory.

`replace()` — the asynchronous form that can `@import` — is **not implemented**, and its absence is
honest under ADR 0012's rule: it returns a promise, so a page that needs it gets a rejection it can
see rather than a sheet that silently lacks its imports.

### 5. Event retargeting, and the two properties that make it work

An event dispatched inside a shadow tree has a **composed path**. At each shadow boundary, `target`
is retargeted to the host, so a listener on the document sees the component rather than the
component's internals. `event.composedPath()` returns the full path, and `composed: false` — the
default for most events — stops the event at the boundary entirely.

This is the property that makes components encapsulating: a page's click handler on `<ytd-app>` must
not receive a target inside `<ytd-masthead>`'s shadow tree. Implementing dispatch without it is not
a partial implementation, it is a leak of the thing the feature exists to prevent.

It lands as part of ADR 0017's single dispatch algorithm rather than as a second one, which is why
that ADR insists on there being only one.

### 6. Ordering: slots before styling, styling before parts

1. `attachShadow`, `shadowRoot`, and the flat-tree traversal for layout — a shadow tree renders
2. `<slot>`, assignment, `assignedNodes`, `slotchange` — reddit's 70 `slot=` attributes land
3. retargeting and `composedPath` — events behave
4. scoped cascade, `:host`, `::slotted`, `adoptedStyleSheets` — it looks right
5. `::part`, declarative shadow DOM, `:host-context`

Step 1 alone makes youtube's page appear. Steps 1–3 make it usable. That ordering is chosen so that
each step is visible on a real page, per ADR 0007's method.

## Consequences

- **`src/dom` gains the shadow root type and slot assignment state**, and every module that walks
  children has to decide which tree it means. That decision is not defaultable: a walk that picks
  the wrong tree is a rendering bug with no error attached. Expect the flat-tree iterator to be
  threaded through more places than this ADR predicts.
- **`src/css`'s `StyleResolver` takes a scope chain rather than a document.** Its signature changes
  everywhere, which is a wide mechanical diff and the right kind.
- **`querySelector` stops being able to answer questions about the whole page**, by design. Anything
  in the engine that used a document-wide query to find something — hit testing did, historically —
  has to be re-expressed as a flat-tree walk.
- **This mostly finishes ADR 0012's list.** What was left of it was geometry (ADR 0015) or this.
- **Memory per component instance is the number to watch.** A shadow root per element is an object
  per element on a page that has thousands; the sharing decision in §4 is what keeps the stylesheet
  from being another one. It deserves a measurement on youtube rather than an assumption.

## Alternatives considered

**Ignore shadow roots and render the light DOM.** This is the status quo and it is what makes
youtube.com a blank page with two elements in it. Rejected on the measurement.

**Flatten shadow trees into the node tree at `attachShadow` time.** Rejected. It makes layout and the
cascade trivial and makes the DOM lie: `parentNode` would answer with a node from another tree,
`querySelector` would find shadow internals, and encapsulation — the only reason the feature exists —
would be gone. It also cannot express `<slot>`, because a slot's assigned nodes stay children of the
host in the node tree.

**Materialise the flattened tree as real nodes alongside the node tree.** Rejected on the cost of
keeping two trees in agreement. Every mutation would have to update both, and every place that
forgot would produce a page that renders differently from what the DOM says it contains — which is
the hardest class of bug to even notice.

**Support `mode: "closed"` by refusing it.** Rejected as a stub in ADR 0012's sense. A page that
calls `attachShadow({mode:"closed"})` and gets a throw fails at the call; a page that gets a root
whose `shadowRoot` is null behaves exactly as it does in every other browser, which is what the
specification asks for and what the page was written against.

**Wait for `::part` and `:host-context` before shipping any of it.** Rejected on the ordering
argument in §6. A shadow tree that renders and slots correctly but cannot be themed from outside is
a page that looks slightly wrong; a shadow tree that does not render is a blank page.
