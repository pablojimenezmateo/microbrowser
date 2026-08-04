# ADR 0026 — Navigation, session history, and the URL bar as a security surface

**Status:** accepted · **Date:** 2026-08-04

## Context

Navigation today is: a click on a link, or a form submission, produces a new load, which replaces the
document. `src/ui` keeps a history list for the back and forward buttons. That is the entire model,
and it is the model of a browser from 2004.

Every one of the three target sites navigates without loading a document:

| | occurrences |
|---|---|
| `history.pushState` | 23 |
| `popstate` | 11 |

And reddit does something sharper than a count. Its front page dynamically imports
`apply-polyfill-BcpMVdvg.js`, which is `@virtualstate/navigation` — **a polyfill for
`window.navigation`, the Navigation API**. Reddit's routing is built on it. That is the same signal
ADR 0012 read off youtube's polyfill list: a page shipping a polyfill is telling you what it expects
the platform to have.

Two other measured facts bear on this:

- **reddit's front door is a navigation.** The interstitial's script calls `form.requestSubmit()` on
  a `method="GET"` form, and the resulting navigation — with the cookie from the first response —
  is what returns the real page. A browser that cannot navigate from a scripted form submission
  cannot load reddit at all.
- **reddit's content arrives as HTML fragments** from 31 `/svc/shreddit/…` endpoints, swapped into
  the tree. From the user's point of view that is a page change. From the document's point of view
  nothing was navigated. The URL bar has to be told, by script, what to say.

Which is where the security question enters, and it is the reason this is an ADR rather than a
feature. **`pushState` lets a page change what the URL bar displays without loading anything.** That
is the mechanism, and it is by design: the specification restricts the new URL to the same origin,
and that restriction is the only thing standing between a page and a perfect address-bar spoof. A
browser that implements `pushState` and gets the origin check slightly wrong has shipped a phishing
primitive with a nice API.

## Decision

### 1. Session history is the engine's, not the chrome's

The history list moves out of `src/ui` and into `src/engine`, where the documents are.

`src/ui` cannot hold it correctly any more, and the reason is precise: with `pushState`, a history
entry is not "a URL that was loaded". It is a URL **plus a state object owned by a document**, and
the chrome cannot see a document — `src/ui` has no `dom`, no `css`, no `layout`, and that separation
is right. So an entry becomes:

```
struct HistoryEntry {
  url::Url url;
  js::SerializedValue state;   // structured-clone bytes, not a live object
  gfx::PointF scroll_offset;   // restored on traversal
  DocumentId document;         // which document this entry belongs to
};
```

The chrome keeps what it needs to draw buttons — can-go-back, can-go-forward, the current URL — and
receives it over the existing IPC seam. That is a strict narrowing of what `src/ui` knows, which is
the right direction.

**The state object is stored as structured-clone bytes, not as a live JavaScript value.** A history
entry outlives the document that created it; holding a live object would keep a dead document's heap
alive and hand a later document a reference into it. This is the same value-not-pointer rule
ADR 0015 applies to geometry, for the same reason.

### 2. `pushState` and `replaceState`, with the origin check as the load-bearing line

`history.pushState(state, "", url)` and `replaceState`, with:

- **the new URL parsed against the document's base URL and required to be same-origin.** Not
  same-site, not same-host — same **origin**, by `url::Origin`'s own comparison, which already
  exists and is already tested. A cross-origin URL throws `SecurityError` and the URL bar does not
  move.
- `history.state`, `history.length`, `history.go/back/forward`.
- **`popstate` fires on traversal within a document**, never on the initial load, and carries the
  stored state.
- **`hashchange`** for fragment-only navigations, which are the one case that has always been able
  to change the URL without a load.

The origin check gets its own tests, including the ones that look like they should pass and must not:
a URL with a different port, a `javascript:` URL, a `data:` URL, a URL that parses to the same host
through a different scheme. Those are the shapes address-bar spoofs are built from.

**And the rule underneath it, which no API can be allowed to violate: the URL bar shows the origin
of the document that is displayed.** Every feature that touches it — `pushState`, redirects,
`about:blank`, an iframe (ADR 0027), a download — is checked against that sentence.

### 3. Navigation becomes an algorithm with cancellation points

Today a navigation happens. It becomes a thing with a defined sequence, because scripts now
participate in it:

1. a navigation is **initiated** — link click, form submission, `location` assignment, `back`
2. the current document may **cancel** it — `beforeunload`, and only for a document that has seen a
   user interaction, because otherwise it is a dialog every page can trap the user with
3. the request is made, redirects are followed, the response's origin and `Content-Type` decide
   whether it is a navigation or a download
4. the old document is **unloaded**: its requests are dropped, its timers stop, its workers are
   joined, its bindings are torn down before the document itself — which the commit
   `Drop the binding layer before the document it points at` already established as the order
5. the new document is created and the entry is pushed

ADR 0011 already requires that a response arriving for a document that is gone is dropped by
construction rather than by a check. Step 4 is where that construction lives, and every asynchronous
thing added since — `fetch` (ADR 0020), a WebSocket, a worker (ADR 0022), a pending geometry
query — has to be attached to the document so that it dies there. **A subsystem that outlives its
document is a use-after-free**, and the list of subsystems is now long enough that "remember to
cancel it" is not a strategy.

**No back/forward cache.** Keeping a document alive after navigating away means keeping its timers,
its connections and its heap, and the resulting lifetime rules are where every browser that has one
has found bugs. It is a performance feature, it is explicitly out of scope, and saying so now
prevents a future "just keep the document around" patch.

### 4. Form submission, properly

reddit's door requires it and it is currently partial. What lands:

- `form.submit()` and **`form.requestSubmit()`** — which differ in a way that matters here:
  `requestSubmit()` fires the `submit` event and runs validation; `submit()` does neither. Reddit
  calls `requestSubmit()`, and its own `onsubmit` handler appends the query parameters. A browser
  that implements `requestSubmit` as an alias for `submit` gets a form submitted **without the
  fields the handler was going to add**, and reddit's challenge fails with no error anywhere.
- `GET` with the query string built from the form data set, and `POST` with
  `application/x-www-form-urlencoded` and `multipart/form-data`.
- `form.elements`, indexed and `namedItem()` — reddit reads `elements.namedItem("solution")`.
- `document.forms`.
- the `submit` event with a working `preventDefault`, which already exists for clicks and generalises.
- `form-action` from CSP (ADR 0020) checked at submission.

### 5. The Navigation API is the target, and `pushState` is the step before it

`window.navigation` — `navigate()`, `back()`, `forward()`, the `navigate` event with
`intercept()`, and `navigation.entries()` — is what reddit polyfills and what the platform is moving
toward. It is strictly better than `pushState` for the thing both are used for: a same-document
navigation that the page handles, with the browser still able to see that a navigation happened.

It is **not** first, because `pushState` is what the polyfill is implemented in terms of, and
`pushState` is what every other page uses. Building `pushState` correctly makes reddit's polyfill
work and makes the Navigation API a layer over machinery that exists — which is the same relationship
ADR 0020 sets up between `fetch` and `XMLHttpRequest`.

### 6. Refused, with reasons

- **`window.open` returning a controllable window, and `window.opener`.** There are no tabs yet, so
  this is not a gap. When tabs arrive, `opener` is a cross-document reference that crosses an
  isolation boundary and it gets decided then, against ADR 0004.
- **`document.write`.** Still refused, still for ADR 0011's reason.
- **Automatic `<meta http-equiv="refresh">` with a non-zero delay.** A page that navigates the user
  somewhere else after a pause, without an action, is a redirect the user did not cause. Zero-delay
  refresh is treated as a redirect and honoured; a delayed one is not, and it is a decision rather
  than an omission.

## Consequences

- **`src/ui` gets smaller**, which is unusual and good. It stops owning history and starts displaying
  what the engine tells it.
- **The IPC seam grows navigation state messages**, and they are the messages a compromised renderer
  would most want to lie in — "the URL is now `bank.example`". The browser process, not the renderer,
  decides what the URL bar shows; the renderer's `pushState` is a *request* that the browser process
  validates against the document's origin. That is ADR 0004's "policy runs where the attacker is not",
  applied to the one pixel of UI that users are taught to trust.
- **Document teardown becomes the most safety-critical routine in the engine.** Every ADR from here
  on adds something that must die with the document, and the ordering established by
  `Drop the binding layer before the document it points at` is the pattern each one follows.
- **Session history is per-tab state that does not exist yet**, so this lands as a single list and
  grows a tab dimension when tabs do. Designing it as a list-per-browsing-context now costs nothing.
- **reddit becomes reachable.** `requestSubmit`, `document.forms`, `elements.namedItem` and
  `DOMContentLoaded` are the front door, and they are four small bindings rather than a milestone.

## Alternatives considered

**Keep history in `src/ui` and send state objects across the IPC seam.** Rejected. It puts a
document-owned value in a module that cannot see documents, and it means serializing the state object
on every `pushState` rather than on entry creation. The chrome does not need it and should not have
it.

**Implement the Navigation API and let `pushState` be a shim over it.** Rejected on ordering, not on
merit. Reddit's polyfill implements `navigation` *using* `pushState`, so shipping only the newer API
makes the polyfill fail while the native API sits unused — the page has already branched.

**Allow `pushState` to any URL and rely on the user reading the origin.** Rejected outright, and it
is worth recording as considered-and-refused because it is the shape of a shortcut somebody might
take while testing. It is a complete address-bar spoof.

**Add a back/forward cache, since navigation is otherwise slow.** Rejected for now. The right answer
to slow navigation is ADR 0030's incremental rendering and ADR 0010's connection reuse, both of which
make every navigation faster rather than making the second visit to a page faster at the cost of a
lifetime model nobody can hold in their head.
