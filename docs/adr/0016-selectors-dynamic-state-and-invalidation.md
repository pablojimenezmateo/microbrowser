# ADR 0016 — Selectors, dynamic state, and what a hover costs

**Status:** accepted · **Date:** 2026-08-04

## Context

`src/css/StyleSheet.cpp` matches type, id, class, universal and attribute selectors, the four
combinators, and nine structural pseudo-classes. Its pseudo-class chain ends like this:

```cpp
} else {
  // A pseudo-class we do not implement must not match. Matching would
  // apply a rule the author scoped to a state we cannot observe.
  return false;
}
```

That is the right call. It is also, on reddit's stylesheet, catastrophic:

| Selector feature | Uses in `styles-css-CSasxzfw.css` |
|---|---|
| `:not(` | **136** |
| `:is(` / `:where(` | **52** |
| `:hover` | 17 |
| `:focus` | 7 |

212 declarations that the author intended to apply, silently not applying. And the failure is not
"missing effect" — `.btn:not(.disabled) { color: white }` failing means the button keeps whatever
colour a lower-specificity rule gave it, so the page renders in colours no rule intended. This is
the font-stack bug from the Hacker News run and the unresolvable-`var()` bug from ADR 0014, a third
time: **one unimplemented mechanism making everything downstream of it wrong at once, and invisible
until a real page is on screen.**

`:is()` and `:where()` are worse than their count suggests, because they are how modern stylesheets
*compress*. One `:where(a, button, [role=button]):hover` is three rules. Failing to match it does
not lose one rule out of 52; it loses the rule that was standing in for a dozen.

Then there is the second half, which is a different kind of problem entirely. `:hover`, `:focus`,
`:active`, `:checked` and `:disabled` are not selectors the engine cannot *parse* — they are
selectors whose truth changes without the DOM changing. Supporting them means style can become stale
because a pointer moved, and that lands directly on the invariant in `AGENTS.md`: idle CPU is zero,
and the redraw path is the thing most worth protecting. A naive implementation restyles the document
on every mouse move.

## Decision

### 1. The functional pseudo-classes, because they are the ones that break pages

`:not()`, `:is()`, `:where()`, `:has()`, and `:nth-child()` / `:nth-of-type()` with the full `An+B`
grammar.

The selector model changes shape to hold them: a `SelectorPart` gains a kind that owns a **nested
selector list**, and matching recurses. Three rules come with it, and each is a place
implementations get it wrong:

- **Specificity.** `:is()` and `:not()` take the specificity of their most specific argument;
  `:where()` contributes **zero**. That is the entire point of `:where()`, and getting it wrong
  produces a page where the cascade order is subtly inverted — a bug that looks like a rendering
  bug and is not.
- **Nesting depth is bounded.** A selector list inside a selector list is attacker-controlled input
  from a stylesheet, and recursion over it is the same hazard ADR 0009 bounded for script. The bound
  is measured against real stylesheets and written down where it is enforced, per ADR 0009's
  precedent.
- **`:has()` is a descendant query and is priced as one.** It is the only selector whose match
  depends on a subtree rather than an ancestor chain, which inverts the invalidation direction
  below. It lands last of the five, behind a measurement, and if it is expensive it stays behind
  one.

### 2. Dynamic pseudo-classes, with the state on the element and not in the selector

`:hover`, `:active`, `:focus`, `:focus-visible`, `:focus-within`, `:checked`, `:disabled`,
`:enabled`, `:required`, `:default`, `:placeholder-shown`, `:target`.

Each is a bit on the element, set by the engine, read by the matcher. The matcher stays a pure
function of (element, selector) — it does not consult the input system, and it does not know a mouse
exists. That keeps `src/css` free of `src/engine`, which its `MODULE.deps` requires anyway, and it
keeps the matcher testable by setting a bit rather than by simulating a pointer.

Which bits exist and who sets them is ADR 0017's problem (input, focus, and the events that move
them). What is decided here is that they are **element state**, not matcher state.

`:visited` stays absent, matching nothing, and `:link` keeps matching every link. That is already
written where the code is and it is a privacy decision, not a gap. Every mechanism for reading the
difference back — painted colour, layout size, timing — is a history leak, and this ADR does not
reopen it.

### 3. Invalidation is a set of rules, computed once per stylesheet

This is the part that protects the invariant, and it has to be designed rather than grown.

When a stylesheet is parsed, build an **invalidation index**: for each selector, the *last compound*
determines what element it can match, and the features in it — a class name, an id, an attribute
name, a pseudo-class bit — are the keys under which the rule is filed. A change to an element then
asks the index which rules could newly match or stop matching, rather than asking every rule.

Concretely, and these are the four cases that cover everything:

| What changed | What is restyled |
|---|---|
| a pseudo-class bit on element E | E, plus the subtree reachable by the combinators in rules keyed on that bit |
| a class or attribute on E | the same, keyed on the class/attribute name |
| a DOM insertion or removal | the parent's structural-pseudo dependents and the inserted subtree |
| a stylesheet added or removed | everything |

**A hover that no rule mentions costs nothing.** That is the property to test, and it is the one
that decays silently: a page with no `:hover` rules must not restyle, must not relayout, and must
not repaint when the pointer crosses it. `IdleWaitStrategyTests` guards the policy for timers; this
needs the equivalent, and it belongs next to it.

The second property is that **a hover that changes only paint does not run layout.** A colour change
is a repaint of one box's damage rectangle through the existing `DirtyRegion` path. Separating "this
declaration affects layout" from "this declaration affects paint" is a table over properties, and it
is the same table `transition` will need in ADR 0014's step 5, so it is built once here.

### 4. Ordering, and why `:not()` outranks `:hover`

`:not()`, `:is()` and `:where()` first, because they are 188 of the 212 misses and they need no
invalidation machinery at all — they are pure matcher work over a static tree, testable the day they
land. `:nth-child()` next, for the same reason.

The dynamic set follows, and it arrives with the invalidation index rather than before it. Shipping
`:hover` on a restyle-the-world implementation would be a working feature that costs the project its
central property, and it would be very hard to take back once pages depended on it.

## Consequences

- **`css::Selector` grows a recursive shape**, and `SelectorPart` stops being a flat POD. Its object
  budget will fire, correctly: a nested selector list is a new kind of thing to store.
- **Style resolution gains an invalidation index built per stylesheet.** On reddit's 111KB sheet
  that is a one-time cost proportional to the rule count, paid to make every subsequent mouse move
  approximately free. It is a measurement to take rather than assume.
- **The engine gains the obligation to set state bits**, and every bit it forgets is a rule that
  never applies — the same silent failure this ADR is fixing, relocated. Each bit ships with a test
  that sets it and asserts a matching rule now applies.
- **Specificity becomes a thing that can be wrong without anything looking broken.** It gets its own
  tests against the specification's examples, not against pages.
- **`:has()` may turn out to be too expensive and stay out.** That is an acceptable outcome and it is
  recorded here so that leaving it out later reads as a decision rather than an omission.

## Alternatives considered

**Parse the functional pseudo-classes and match them permissively — treat `:not(x)` as always true.**
Rejected on the project's first rule and on the same argument ADR 0012 makes about stubs. It gets
more rules applying and applies some of them to the wrong elements, which is harder to diagnose than
the current failure, not easier.

**Implement `:hover` by restyling the document on pointer move.** Rejected on the zero-idle-CPU
invariant. It works, it is a hundred lines, and it turns every mouse movement over a page into a
full cascade — the exact class of change `AGENTS.md` says is most likely to be lost by accident.

**Skip the invalidation index and recompute style for the hovered element's subtree only.**
Rejected as wrong rather than slow: a sibling combinator (`li:hover + li`) and `:focus-within` both
affect elements outside the subtree, so a subtree-scoped restyle produces a page that is *usually*
right, which is the worst available outcome.

**Wait for `:has()` before designing invalidation, since it inverts the direction anyway.** Rejected
as backwards. Designing the index against the four cases above and then discovering `:has()` needs a
fifth is a normal extension; designing it around `:has()` first optimises for the selector that may
not ship.
