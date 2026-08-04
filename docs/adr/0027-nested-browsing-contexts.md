# ADR 0027 — Nested browsing contexts, and the isolation they were the reason for

**Status:** accepted · **Date:** 2026-08-04

## Context

`<iframe>` is a tag the HTML parser accepts and the layout tree treats as an empty replaced element.
There is no second document behind it.

ADR 0004 decided the process model against precisely this element, and its central sentence is:

> **Isolation is per site, not per tab.** A tab hosts cross-origin iframes; a process hosts one site.
> Per-tab isolation leaves the part the attacker controls unprotected.

That decision has been correct and untested for the whole project, because there has never been a
cross-origin iframe to isolate. This ADR is where it becomes real.

The demand is modest and unavoidable. youtube.com's document contains one `<iframe>`; the target
sites reference the string 282 times across their bundles, mostly for embeds, ad slots and OAuth
flows. Reddit's page loads Google and Apple sign-in scripts. For the stated goal — any page — the
count does not matter: embedded video, embedded maps, payment forms, and every "sign in with"
button on the web are iframes, and a browser without them shows a hole where each of them should be.

The reason it is hard is not rendering. It is that an iframe is a **second document, with its own
origin, its own event loop obligations, its own script, and its own view of the same window**, and
almost every subsystem this project has built assumes there is exactly one document.

## Decision

### 1. A browsing context is the unit, and `Page` becomes a tree of them

`src/engine`'s `Page` owns one document. It becomes a **browsing-context tree**: a top-level context
with child contexts, each owning a document, each with its own script interpreter, its own loader
state, its own history entries, and its own lifetime.

The tree is what makes the rest expressible:

- **Layout**: the parent's layout gives the iframe element a content box; the child lays out into a
  viewport of that size and produces a display list; the parent's display list carries the child's
  as a **nested list with a transform and a clip**. That is a shape the display list already has the
  vocabulary for, and it means an iframe repaints without repainting its parent.
- **Input**: hit testing descends into the child at the iframe's box, translating coordinates.
  Focus is per-context, with one focused context in the tree (ADR 0017).
- **Navigation**: a child has its own current entry, and a navigation in a child creates a joint
  session history entry in the top-level context — which is what makes the back button do the
  surprising-but-correct thing of undoing an iframe navigation.
- **Teardown**: a context dies with its parent, children first. ADR 0026 already made document
  teardown the most safety-critical routine in the engine, and this multiplies it by the tree.

### 2. Cross-origin means cross-origin, everywhere, from the first commit

The origin checks are not a later hardening pass. They are what the feature *is*, and a
same-origin-only first version that "adds checks later" would be a browser with a universal
cross-origin read for however long later takes.

What a page may do to a cross-origin child, and nothing else:

- read `frame.contentWindow` and get a **`WindowProxy`** exposing only the cross-origin-allowed
  surface: `postMessage`, `close`, `closed`, `focus`, `blur`, `frames`, `length`, `top`, `opener`,
  `parent`, `self`, `window`, and `location` **write-only**
- `postMessage`, with an origin argument that is checked on delivery and an `origin` on the received
  event that is not forgeable
- navigate it by assigning `location`, subject to the sandbox rules below

Everything else throws or is absent: no `contentDocument`, no reaching into its DOM, no reading its
`location.href`, no seeing its history length.

`document.domain` is **not implemented**. It exists to relax the same-origin policy and every engine
is removing it; implementing it would mean the origin of a document can change at runtime, which
makes every check in this ADR a check against a moving value.

`src/url` already has `Origin` and `Site` and the public-suffix list behind them. This ADR is mostly
an exercise in calling them from places that never needed them before.

### 3. `sandbox`, `allow`, and the headers that say who may frame you

- **`sandbox`** on the iframe element, with the token set implemented rather than approximated:
  `allow-scripts`, `allow-forms`, `allow-same-origin`, `allow-popups`, `allow-top-navigation`,
  `allow-downloads`. A sandboxed frame without `allow-same-origin` gets an **opaque origin**, which
  means it is same-origin with nothing, including itself.
- **`X-Frame-Options`** and CSP's **`frame-ancestors`** are honoured on the response: a document that
  refuses to be framed is not framed. This is a protection for *other* sites against ours, which is
  a category of check a browser must implement even though no local user benefits directly — it is
  the clickjacking defence, and a browser that ignores it makes every site it renders exploitable.
- **`frame-src`** from the embedding page's CSP gates the load (ADR 0020).
- **`allow`** (Permissions Policy) is parsed and used to deny; it is not used to grant, because
  nothing on this browser's permission surface is granted by default (ADR 0029).

### 4. Third-party frames meet the partition key, and the result is not neutral

Every request a child context makes is keyed by ADR 0005's key — `(container, top-level site,
origin)` — where the **top-level site is the embedder's**, not the frame's. That is the entire point
of the key and it is what Total Cookie Protection means in practice.

The consequence, stated plainly because it will look like a bug: **a "sign in with Google" iframe on
reddit does not see the cookies Google set on google.com.** Federated sign-in through third-party
frames does not work, on purpose, and it is the same decision ADR 0021 records for storage. Where
that breaks a flow, the breakage is a decision this project has already made twice.

The blocking engine (ADR 0006) also applies to frame loads, and a blocked frame is an empty frame
rather than an error page.

### 5. The process split arrives with this, or immediately after it

ADR 0004 is written and unimplemented. This ADR does not make the split happen — that is M7 — but it
is the feature that makes it necessary rather than principled, and it constrains the design so the
split is an extraction rather than a rewrite:

- **A browsing context communicates with its parent only through messages that would survive being
  serialized.** Display lists already serialize; input already crosses `src/ipc`; `postMessage` is a
  message by definition. Nothing in this ADR requires a child to hold a pointer into its parent.
- **A child's display list is a nested list, not a shared framebuffer.** So a child in another
  process delivers the same thing an in-process child delivers.

Get those two right and moving a context into its own process is a transport change. Get them wrong
— a child that walks up to its parent's DOM for one convenience — and the split becomes the rewrite
ADR 0003 was written to avoid.

### 6. Order

1. **Same-origin iframes**: the context tree, nested layout, nested display lists, hit testing. No
   new security surface, and it is where every structural bug will be found.
2. **Cross-origin iframes**: `WindowProxy`, `postMessage`, the origin checks, `X-Frame-Options` and
   `frame-ancestors`.
3. **`sandbox`** and Permissions Policy.
4. **The process split**, which is ADR 0004 finally landing.

`<frame>` and `<frameset>` are **not implemented**. They are removed from the specification, and the
pages that use them are old enough that they were already broken.

## Consequences

- **`Page` stops meaning "the document" across the whole engine.** Every place that says "the page's
  document" has to say which context it means. That is a wide, mechanical, error-prone change and it
  is better done as one refactor than discovered incrementally.
- **Memory and CPU scale with frame count**, and an ad-heavy page has a lot of frames. Each is a
  document, a style resolver, a layout tree and a JavaScript heap. This is the first feature where a
  page can multiply the browser's per-document cost at will, and it is a reason lazy frame loading
  (`loading="lazy"` on iframes) is worth having early.
- **`about:blank` and `srcdoc` frames inherit their embedder's origin**, which is the one place the
  origin of a document is not derived from its URL. It is a small rule and it is the source of a
  recurring vulnerability class, so it gets its own tests.
- **This is the payoff for four years of `MODULE.deps` discipline**, or the point at which it turns
  out not to have been enough. The origin checks are only credible if `src/bindings` cannot reach a
  document it was not given, and that is exactly what the module contract has been enforcing.
- **Nothing on the three target sites strictly requires this** — youtube's single iframe is an
  embed, not the page. It is here because "any page" cannot be reached without it and because
  deferring the design until after the process split would invert the dependency.

## Alternatives considered

**Render iframes as empty boxes and move on.** The status quo. Rejected for the goal, accepted as the
interim: a hole where an embedded video should be is honest, and it is what the browser does today.

**Implement same-origin iframes only, and refuse cross-origin ones.** Genuinely tempting — it is most
of the structural work with none of the security surface, and it would make same-site embeds work.
Rejected because the useful iframes are all cross-origin, so it delivers the entire cost of the
context tree for approximately none of the benefit.

**Run iframes as separate documents in one interpreter, sharing a heap.** Rejected. It makes the
origin checks a matter of discipline inside a shared object graph rather than a structural property,
and it forecloses the process split entirely, since a shared heap cannot be split.

**Wait for the process split, then build iframes on top of it.** Rejected on ordering. The split is
motivated by iframes; building the isolation machinery with nothing to isolate means designing
against an imagined client. Building the context tree first, with the two constraints in §5, gives
the split a real client and a real test.
