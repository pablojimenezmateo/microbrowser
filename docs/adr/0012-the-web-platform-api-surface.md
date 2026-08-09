# ADR 0012 — The web platform API surface

**Status:** accepted · **Date:** 2026-08-04

## Context

ADR 0008 decided how a binding is built — the seam, the same-origin checks, and the rule that
`src/bindings` is the only module that sees both `js` and `dom`. It did not decide *which* bindings
exist, and that has been answered so far by whatever the page in front of us needed.

That worked while the page was Hacker News. It stops working now, because the failure mode changes.
Hacker News needs a dozen bindings and degrades gracefully without them. An application framework
does not degrade: it probes for a capability, takes a branch, and if the probe answers wrongly it
fails somewhere else entirely, three files later.

Running youtube.com's fourteen scripts with errors surfaced makes the shape of that concrete. After
this session's engine fixes, every remaining failure is a missing binding, and they are not exotic:

    ReferenceError: Image is not defined
    ReferenceError: HTMLElement is not defined
    TypeError: undefined is not a function        (x3)

`HTMLElement` is the one worth reading twice. It is not a function anybody calls — it is the base
class a custom element extends. Its absence is not a missing method, it is a missing *type
hierarchy*: today `document.createElement` returns an object with the right properties and no
constructor to be an instance of, so `class X extends HTMLElement` cannot even be written.

The page also tells us what it expects the platform to have, by what it ships polyfills for:
`webcomponents-all-noPatch.js`, `fetch-polyfill.js`, `intersection-observer.min.js`,
`web-animations-next-lite.min.js`. Those four are a statement of the minimum modern baseline — and a
useful one, because a polyfill needs *primitives* rather than the feature itself. The web components
polyfill needs `MutationObserver`; the fetch polyfill needs `XMLHttpRequest` or a real `fetch`.

The risk this ADR exists to name is the one that comes from the middle of that list. A binding that
is **absent** throws at the call, which is a stack trace pointing at the problem. A binding that is
**present and wrong** returns a plausible answer and fails later — and feature detection makes this
much worse than usual, because `if (window.IntersectionObserver)` is a promise that the thing behind
it works. A half-implemented `IntersectionObserver` is worse than none at all: none takes the
polyfill path, half takes the native path into a wall.

That is the repository's own priority order — correctness before completeness — pointed at bindings,
and it is the rule this ADR is built around.

## Decision

### The governing rule: whole, or absent, and never announced falsely

**A binding is either implemented to the point where a page's feature detection is telling the
truth, or it is not present at all.** No stubs that return empty, no constructors that exist and do
nothing, no methods that accept their arguments and ignore them.

Where a partial implementation is genuinely the right trade, the partiality must be **invisible to
detection**: it may be missing an option, but it may not be missing the behaviour the presence of
the name promises. A deviation like that is written where the code is and listed in the roadmap, the
way the JS engine's deviations already are.

This has a corollary that will feel wrong in the moment and is the point of writing it down: when a
page fails on a missing binding, adding a stub to get past it is *the wrong fix*. It converts a
diagnosable failure into an undiagnosable one.

### The order, and why

Ordered by what unblocks the most per unit of work, not by tier of prestige.

**1. The element type hierarchy — `Node`, `Element`, `HTMLElement` and the per-tag subclasses.**

First because it is structural rather than additive, and everything after it assumes it.
`instanceof` has to work, `class X extends HTMLElement` has to be writable, and the prototype an
element gets from `createElement` has to be the one its tag names. Retrofitting a hierarchy under
bindings that already hand back plain objects means revisiting every one of them, so it goes first
even though nothing visibly improves when it lands.

**2. `MutationObserver`.**

The highest leverage single binding on the list. It is what the web components polyfill is built on,
which means it is the difference between custom elements working through a polyfill and not working
at all. It is also self-contained: a queue of records, delivered as a microtask — and the microtask
queue is already done and already respects zero-idle-CPU.

**3. `XMLHttpRequest`, then `fetch`.**

Both wait on ADR 0011; neither can be honest without it. `fetch` is the better API and `XHR` is the
one the polyfill wants, so `XHR` first and `fetch` over the same machinery. Both go through
`privacy::Verdict` — there is no overload without one and there will not be — and both are subject
to the partition key. **CORS is enforced in the network process** (ADR 0004), not here: a check that
runs where the attacker is, is not a check.

**4. `requestAnimationFrame`, `IntersectionObserver`.**

Both are frame-timing shaped, and both need the frame deadline from ADR 0011.
`IntersectionObserver` additionally needs layout to answer a geometry question about a box, which is
the same capability `getBoundingClientRect` needs — so those two arrive together or not at all.

**5. Custom elements, natively.** Then Shadow DOM, separately and later.

Originally this said "last, because the polyfill covers it". That turned out to be wrong, and the
amendment at the end of this ADR records what was measured and what the order became.

### What is refused, and why it is refused rather than deferred

- **`document.write`.** Re-entering the tokenizer mid-parse, for a feature the web is removing. See
  ADR 0011.
- **`eval` and `Function(source)`.** Present, gated by CSP `'unsafe-eval'`
  (ADR 0039). Refusing them outright hung youtube's WebPO / BotGuard path
  (TD-0024). Direct-eval scope chaining is still approximate (global only).
- **Storage APIs without a partition key.** `localStorage`, `sessionStorage`, IndexedDB and the
  Cache API are all per-site state, so every one of them is keyed by ADR 0005's key or does not
  exist. This is the row people forget, so it is written here as well as there.
- **`Atomics` / `SharedArrayBuffer`.** Needs the process model and cross-origin isolation first.
- **Anything that reports on the user.** The privacy contract is a constraint on this list like any
  other. Where a capability's honest answer is a fingerprinting surface, the decision is made in
  `guidelines/privacy.md` terms and not by what a page expects — `:visited` matching nothing is the
  precedent, and it is a decision rather than a gap.

## Consequences

- **`src/bindings` grows a lot, and its budgets will fire.** That is the mechanism working. The
  growth pressure is real, and the answer is more files in the module rather than bigger ones — the
  split by subject that `DomBindings` / `EventBindings` / `StyleBindings` / `TreeMutation` already
  follows.
- **The type hierarchy costs memory per element** — a prototype chain where there is currently a
  flat object. It is measurable and it is the price of `instanceof` telling the truth.
- **Deciding this makes "how far are we?" answerable.** The list is a checklist, and a checklist can
  be wrong in public. That is the same argument ADR 0007 makes for naming the target sites.
- **The polyfill path is a supported strategy, not a fallback.** Shipping `MutationObserver` so that
  somebody else's web components implementation runs is a deliberate way to buy a large capability
  cheaply, and it is worth saying so, because from the inside it looks like not doing the work.

## Alternatives considered

**Implement bindings on demand, as pages need them.** This is the status quo, and it is what got the
engine here. Rejected now for one reason: it optimises for the next failure rather than the next
class of failure, and with an application framework the next failure is rarely where the problem
is.

**Stub broadly, so pages get further.** Rejected, and it is the alternative worth arguing with,
because it demonstrably does get pages further in the short run. It fails on the project's first
rule. A stubbed `IntersectionObserver` makes a page take the native path and silently never fire a
callback; the same page with no `IntersectionObserver` at all loads a polyfill and works. Stubbing
does not defer the work, it *hides the absence of it* — and it hides it from us, not just from the
page.

**Follow the specification's inheritance graph exactly, everywhere.** Rejected as the opposite
error. The full graph is enormous and most of it is unreachable from any real page. What is needed
is that the parts a page can *observe* — `instanceof`, the prototype an element gets, the base a
custom element extends — are right.

## Amendment, 2026-08-04: the polyfill is not the cheap path

That reasoning was tested by running youtube.com's copy of
`webcomponents-all-noPatch.js` directly against the bindings and fixing whatever it stopped on. It
got materially further each time — `document.createEvent`, then `Event.prototype`, then
`DocumentFragment`, then `CharacterData` — and each fix was worth having on its own merits. But the
list it still wants is:

    Window, ShadowRoot, SVGElement, XMLHttpRequest, MutationObserver, customElements,
    NodeFilter, Range, TreeWalker, NodeIterator, DOMTokenList, NamedNodeMap, Attr,
    HTMLCollection, NodeList

That is not a polyfill filling a gap in the platform. It is a polyfill **reimplementing the DOM**,
and to run it the engine has to provide almost everything a native implementation would have needed
anyway — plus `Range`, `TreeWalker` and `NodeIterator`, which nothing else on the roadmap wants.

So the prediction in this ADR was wrong in a specific and useful way: it treated a polyfill as
buying a capability cheaply, when a *deep* polyfill is closer to a second implementation that has to
be hosted. A shallow polyfill — `fetch` over `XMLHttpRequest` — really is the cheap trade this ADR
described. A polyfill that patches `Node.prototype` and `Element.prototype` is not.

**The revised order is to implement `customElements` natively** — a registry, the upgrade
lifecycle, and the four reactions — and to leave Shadow DOM where it was. Native custom elements
need none of `Range`, `TreeWalker`, `NodeIterator` or `ShadowRoot`, which makes it the smaller of
the two paths as well as the honest one.

The general lesson is worth keeping separately from the decision: **"a polyfill exists" is not by
itself evidence that a capability is cheap.** What decides it is how deep the polyfill reaches, and
that is answerable in an afternoon by running the thing rather than by reasoning about it.
