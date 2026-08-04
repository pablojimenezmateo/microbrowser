# ADR 0014 — The CSS features a real page needs, in order

**Status:** accepted · **Date:** 2026-08-04

## Context

The CSS engine parses, cascades and computes; layout does block, inline, float, table, flex,
`position` and overflow clipping. What is missing has been decided by milestone number — grid is
"the rest of M5", stacking contexts are "the rest of M6" — rather than by what a page uses.

Counting what a page actually uses gives a very different order. youtube.com's stylesheet is 3.5MB;
the features in it, by number of occurrences:

| Feature | Uses | Supported |
|---|---|---|
| `var(--…)` custom properties | **8585** | no |
| flex properties | 17406 | **yes** |
| `transform:` | 1391 | no |
| `@media` | 791 | yes |
| `calc(` | 550 | no |
| `transition:` | 478 | no |
| `@supports` | 425 | no |
| `@keyframes` | 107 | no |
| `grid-template` | 46 | no |
| `display:grid` | 32 | no |
| `position:sticky` | 10 | parses as relative |

The ordering that falls out of this is not the one anybody would have guessed, and two entries are
worth stating plainly.

**Custom properties are the single largest gap in the CSS engine, by a factor of six over the next
one.** 8585 uses is not theming — it is how the entire stylesheet expresses colour and size. A
`var()` that does not resolve is not a slightly-wrong value; it is an *invalid* declaration, so the
property falls back to its initial value. Unsupported, essentially every colour and dimension on the
page is black, transparent, or zero. This is the CSS equivalent of the font-stack bug from the
Hacker News run — one unimplemented mechanism making everything downstream of it wrong at once,
and invisible until a real page is on screen.

**Grid is near the bottom.** It is the marquee missing layout feature and the page uses it 78 times
against flexbox's 17406. It matters, and it is nowhere near the top of this list.

`@supports` deserves its own note because it fails in the direction that is hard to see. It is
feature *detection*, so getting it wrong does not produce a wrong colour — it produces the wrong
branch of the stylesheet, and a page that then styles itself for capabilities the engine does not
have. It is cheap to implement and expensive to omit, which is an unusual and welcome combination.

## Decision

**CSS work is ordered by measured use on target pages, not by milestone number.** The order:

### 1. Custom properties and `var()`

Including the fallback form (`var(--x, 1px)`), inheritance, and — critically — the substitution
model. A custom property's value is a token stream that is *not* parsed until it is substituted,
which is a real change to how the cascade stores declarations: the value has to survive as tokens
rather than as a parsed value, and the guard against a cyclic reference has to exist from the start
rather than be added when a page hangs.

The failure mode is defined by the spec and has to be implemented as such: an unresolvable `var()`
makes the declaration **invalid at computed-value time**, which is not the same as ignoring it.
Getting that wrong is how a page ends up with a colour from the wrong rule rather than no colour.

### 2. `calc()`

Nearly always found next to custom properties and needed for the same values to resolve. Lengths,
percentages, and the mixing rules, with division by zero and unit mismatch producing an invalid
value rather than a guess.

### 3. `@supports`

Cheap, and it fails in the direction that produces a wrong page rather than a missing effect. Its
correctness requirement is unusual: it must answer honestly about what the engine *actually*
supports, which means the condition parser and the property table cannot drift apart. A `@supports`
that claims a property works because the parser accepts the token is worse than no `@supports` at
all — the same trap as a stubbed binding in ADR 0012.

### 4. `transform`

1391 uses, and it is what positions a great deal of modern UI. The rasterizer already has
`AffineTransform` and a path transform; what is missing is the property, the computed value, and
the display list carrying it. This is also the point where **stacking contexts stop being
deferrable**, because `transform` creates one — so M6's remaining work arrives here, pulled in by
what a page uses rather than by its milestone.

### 5. `transition` and `@keyframes`

Deliberately *after* transform, and the ordering is the point: an animation is a value changing over
time, and a page whose animated properties do not apply at all gains nothing from animating them.
Both need the frame deadline from ADR 0011 — and both are subject to the zero-idle-CPU invariant,
which for animation means the loop wakes while something is animating and **not one frame after
everything has settled.** An animation system that keeps a 60Hz loop alive on a static page is the
most likely way this project loses its central property, so it is written here.

### 6. Grid

Last of the layout features, on the measurement, and with no suggestion that it is unimportant —
78 uses is real. It is simply sixth.

### `position: sticky` is a deviation, not a gap

It parses as `relative` today because there is no scroll offset to compare against. That is
recorded here as a deliberate approximation with a named blocker — scrolling an overflow container,
which `CLAUDE.md` already places in the engine rather than in layout — so that it is a decision
rather than a surprise.

## Consequences

- **The cascade stores unparsed token streams.** Custom properties force it, and it is a change to
  `ComputedStyle`'s shape rather than an addition to it. `ComputedStyle` is at 164 of 165 header
  lines, so the budget will fire immediately and correctly: this is a new *kind* of thing to store,
  and the manifest should be made to say so.
- **Substitution happens at computed-value time**, which means the resolver gains a phase. That is
  the natural place for the cycle guard, and it is the place a cost will show up on a stylesheet
  with 8585 substitutions — this is a measurement to take rather than assume.
- **Stacking contexts arrive with `transform`**, not on their own schedule.
- **The known-crude note about background images resolving the cascade twice gets more expensive**,
  because substitution makes a second resolve cost more than a second pass over parsed values. That
  is now a real reason to fix it rather than a tidiness one.
- **This changes what "M5" and "M6" mean.** The milestones described what to build; this describes
  what to build first, and where they disagree, this wins. `README.md`'s roadmap should be read with
  that in mind.

## Alternatives considered

**Do grid next, as the roadmap says.** Rejected on the measurement: 78 uses against 8585. Grid was
scheduled because it is the conspicuous missing feature, which is exactly the bias ADR 0007 was
written to correct — "the things it turned out to need were not the things on the list".

**Implement `var()` by substituting at parse time.** Rejected: it is wrong, and wrong in the quiet
way. Custom properties inherit and cascade, so the value substituted depends on the element being
resolved, and a page that sets `--fg` differently on two subtrees would get one of them everywhere.

**Treat an unresolvable `var()` as "skip this declaration".** Rejected because it is not what the
spec says and the difference is observable: invalid-at-computed-value-time means the property takes
its *inherited or initial* value, not the value from the next rule down. Skipping would let a lower
rule win, and the resulting colour would come from a rule the page did not intend to apply.
