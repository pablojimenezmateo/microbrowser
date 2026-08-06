# ADR 0033 — Privacy first, correct, and very fast

**Status:** accepted · **Date:** 2026-08-06

## Context

`AGENTS.md` already states a priority order and names two references: Ladybird for *engine
shape*, LibreWolf for *defaults*. Those lines are load-bearing and easy to dilute under the
pressure of a compatibility target — especially `youtube.com`, which rewards every missing API
with a white page and every privacy refusal with a working polyfill path that phones home.

This ADR exists so the three goals are a *decision* rather than a slogan, and so a later session
that wants to trade one for another has to say so in public.

The references are deliberate and incomplete:

| Reference | What we take | What we do not |
|---|---|---|
| **Ladybird** | Spec-literal parsers, library separation, pixel reference tests, process-shaped seams (`WebContent` / network / image decoder) | Their dependency set, their schedule, their willingness to ship stubs |
| **LibreWolf** | No telemetry, partitioned state, HTTPS-only, blocking as engine policy, zero remote config | Firefox's compatibility surface, its extension model as the privacy story |
| **Chromium (site isolation)** | Isolation unit is a *site*, not a tab | The rest of the browser |

Building from scratch is the third reference, and it is a constraint rather than a boast: every
byte of HTML, CSS, layout, raster, HTTP, and JavaScript is ours, so a wrong default cannot be
"turned off in `about:config`" and a wrong dependency cannot be "someone else's CVE". ADR 0001 is
the dependency half of that; this ADR is the product half.

## Decision

### 1. The order is the product

When goals conflict, this is the order, and it is not rearranged for a named site:

1. **Correctness** — a wrong parse, a wrong same-origin answer, or a wrong layout geometry is a
   bug, including when the bug is also a security hole.
2. **Security** — every network byte and every renderer message is attacker-controlled;
   isolation is per site (ADR 0004).
3. **Privacy** — what leaves the machine; no request the user did not cause; everything
   partitioned (ADR 0005); LibreWolf's *defaults* as structure rather than flags.
4. **Speed** — the main optimisation target once the three above hold. Measured, not claimed.
5. **Idle CPU, then memory, then simplicity.**
6. **Compatibility** — required for the targets in ADR 0007, never as a reason to weaken 1–3.

Compatibility is last on purpose. A page that needs a fingerprinting surface, a tracking
prefetch, or a stub that feature-detection will trust (ADR 0012) does not get one. The remedy is
to implement the honest API, or to leave the name absent so the page's own fallback runs.

### 2. Ladybird's shape, not Ladybird's pace

Module `MODULE.deps` contracts, fuzz targets on every network parser, and the IPC seam before the
process split (ADR 0003) are non-negotiable. A feature that cannot say which module owns it, or
which process will own it after M7, does not land.

Speed work follows the same shape: counters and scopes before speculation
(`guidelines/observability.md`), and a ranked summary is never mistaken for a wait diagnosis —
`MICROBROWSER_LOAD_TIMELINE` exists because youtube and Hacker News were socket-bound while the
CPU table looked quiet.

### 3. LibreWolf's defaults are not a preference pane

The privacy contract in `guidelines/privacy.md` is enforced by types and by the architecture lint
(`privacy::Verdict` on `Fetch`, partition keys on every store). A future "strictness" UI may
*tighten* further; it may not offer a mode that sends telemetry, shares connections across
top-level sites, or turns `:visited` into a cross-site oracle.

`:visited` matching nothing is the canonical example: it costs a cosmetic difference on Hacker
News and buys a class of history leak. That trade is accepted and does not get revisited because
a target site styles visited links.

### 4. From scratch means we own the failure

When youtube.com's ShadyDOM polyfill fights the native shadow DOM, the fix is the native surface
(ADR 0012's amendment, ADR 0019) — not a second DOM hosted for the polyfill's convenience. When
the JS heap ceiling is hit during a custom-element reaction, the fix is correct collection and a
measured limit (ADR 0034), not an unbounded heap "because Chrome has one".

A deep polyfill is not a shortcut. Running one until it stops, then implementing what it needed,
is how this browser found `importNode`, `NamedNodeMap.getNamedItem`, iterable `classList`, and
`Node.DOCUMENT_FRAGMENT_NODE` — each a native gap, each cheaper than hosting the polyfill's
replacement forever.

### 5. Named targets measure the product; they do not redefine it

ADR 0007's five sites remain the checklist. A session that makes youtube.com paint while weakening
partitioning, adding a network request the user did not cause, or shipping a stub ADR 0012 forbids
has failed this ADR even if the snapshot looks right.

## Consequences

- **Positive:** Reviewers and agents share one sentence for tradeoffs. New ADRs can cite this one
  instead of restating LibreWolf/Ladybird. Speed work stays legitimate and ordered — it is not
  "premature" when a counter shows a real cost under a target page.
- **Positive:** Privacy features that look like missing compatibility (`:visited`, no speculative
  prefetch, no `report-uri`) have a home to point at.
- **Negative:** Some pages will need more native surface than a Chromium-skinned fork would.
  That cost is paid in sessions (youtube's Polymer path is the current bill), not in principle.
- **Negative:** "Very fast" is a claim this project refuses in marketing copy; this ADR still
  commits to speed as a ranked goal. The resolution is measurement in `docs/tech-debt.md` and
  the load timeline, never a benchmark against another browser in the README.

## Alternatives considered

**Compatibility-first, privacy as a mode.** Rejected. A mode can be off; a partition key cannot.
LibreWolf's lesson is that defaults which are optional are not defaults.

**Match Chromium's API surface until youtube works, then trim.** Rejected. ADR 0012 already
forbids stubs; a temporary stub becomes a permanent ABI the moment a page feature-detects it.

**Defer all speed work until M9.** Rejected. Idle CPU and the redraw path are invariants now
(ADR 0011); the 2026-08-06 passes showed target pages were waiting on the network and on
duplicate cascade work, not on "more milestones".
