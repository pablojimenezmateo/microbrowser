# ADR 0022 — Workers, and what this browser will not run in the background

**Status:** accepted · **Date:** 2026-08-04

## Context

The survey found three different things that all look like "run script off the main path", and they
have very different costs:

| | occurrences |
|---|---|
| `postMessage` | 47 |
| `serviceWorker` | 20 (youtube only) |
| `new Worker(` | 2 |

Two `new Worker(` calls across 16.2MB is close to nothing. Twenty `serviceWorker` references is also
not much — but a service worker is not a feature that scales with its call count, because a single
registration changes what the browser *is*: a script that persists past the page, that runs when no
page is open, and that intercepts every request the origin makes.

`AGENTS.md` has two rules that meet here and they point in the same direction:

> **Idle CPU is zero.** The process sleeps in exactly one place — the platform event wait.

> **A thread** — justify what it owns, what it borrows, and who joins it before `main` returns.

And ADR 0013 already established the precedent for the one legitimate exception, when it decided the
audio thread was a thread and required it to state its ownership rather than inherit an exemption
from the fact that the platform hands you a callback.

There is a fourth thing in this area worth deciding at the same time, because the reasoning is
identical: **Push, Background Sync, Background Fetch and Periodic Background Sync**, all of which are
built on service workers and each of which is a request the user did not cause, made while the user
is not looking.

## Decision

### 1. Dedicated workers: yes, and they are the model for every thread after them

`new Worker(url)`, `postMessage`, `onmessage`, `terminate()`, and the structured clone algorithm
between them.

A worker is a **separate JavaScript heap on a separate thread**, with no shared objects — which is
what makes it tractable in a codebase whose data structures are, in ADR 0011's words,
"single-threaded by assumption throughout". The seam is a message queue, and messages cross it by
value.

Per `AGENTS.md`, stated here so it is not restated in a commit message:

- **It owns** its `js::Interpreter`, its heap, its microtask queue, and its half of the message
  queue. Nothing else may reach into any of them.
- **It borrows** nothing. Not the DOM, not the document, not a font, not the loader. A worker that
  needs a resource asks through a message.
- **It is joined** when its document dies, before the document's objects are destroyed, and before
  `main` returns. `terminate()` is the same path taken early.

It does not violate zero-idle-CPU: a worker with no message pending blocks on its queue, and a
browser with no workers has no threads.

`SharedWorker` is **not** implemented — it is shared mutable state across documents, which is the
one thing the model above exists to avoid, and no target site uses it.

`SharedArrayBuffer` and `Atomics` stay refused, as ADR 0012 already decided: they need cross-origin
isolation, which needs the process model.

### 2. Service workers: no, and the refusal is the decision

**`navigator.serviceWorker` is not defined.** Not a stub, not a registration that resolves and does
nothing — absent, so that `if ('serviceWorker' in navigator)` is false and every page's fallback path
runs. youtube.com's 20 references are all behind that guard.

Three reasons, in the project's own priority order.

**Correctness.** A service worker is a network proxy written by the page, with its own cache, its own
lifecycle, its own update algorithm, and `fetch` interception that has to be correct for every
request the origin makes including the navigation that would load the page. Getting it *almost*
right produces a site that serves the user stale content from a cache we implemented badly, with no
way for the user to tell. That is the worst failure shape in this document.

**Privacy.** A service worker is durable, scriptable, origin-scoped state that outlives the page,
which is a supercookie by construction. It would have to be partitioned by ADR 0005's key like
everything else, and a *partitioned* service worker registration is close to useless to the site
that installed it, which means the compatibility argument for building it largely evaporates on this
browser specifically.

**Idle CPU.** A registered service worker is a script the browser is expected to start in response to
events that arrive when no page is open. That is a background wakeup source with a page-controlled
trigger, and there is no version of it that respects "the process blocks in one place".

With it go **Push**, **Background Sync**, **Periodic Background Sync**, **Background Fetch** and
**Notification triggers**. Each is a request or a wakeup the user did not cause; `AGENTS.md`'s
privacy rules forbid the shape, not the feature name.

**The cost is real and is stated rather than minimised.** No offline. No installed web apps that
work without a network. A site whose only content path is a service worker — increasingly common
for e-mail clients — will not work here. That is a durable limitation of this browser and belongs in
whatever the user-facing documentation eventually is.

### 3. The Cache API without service workers

The Cache API (`caches`, 14 occurrences) is *usable* from a page without a service worker, and
ADR 0021 already scheduled it late for that reason. This ADR is why: every one of the 14 sites is on
a code path guarded by a service worker check that will be false, so the API is unreachable in
practice. It stays scheduled last and its absence is honest.

### 4. Web Workers are also where the answer to "the engine is too slow" is not

Worth writing down because it will be proposed. A future temptation is to move parsing, layout or
JavaScript compilation onto threads to make pages faster. That is a legitimate design and it is
**not** authorised by this ADR: the worker model above is a message-passing boundary for *page*
script, and internal parallelism is a different decision with a different risk profile — shared
access to the engine's own structures. It needs its own ADR, made against a measurement, the way
ADR 0011 rejected threads for loading and said what would change its mind.

## Consequences

- **The first real thread in the engine arrives here** (or with audio, per ADR 0013, whichever lands
  first), and it arrives with an ownership statement rather than after one. The joining discipline is
  the part to test: a document destroyed while a worker is mid-message is a use-after-free if the
  join is wrong.
- **Structured clone has to exist**, and it is more than `JSON.parse(JSON.stringify(x))`: cycles,
  `Map`, `Set`, `ArrayBuffer` transfer, `Date`, `RegExp`. It is also what `structuredClone()`
  (4 occurrences) and IndexedDB (ADR 0021) both need, so it is built once and used three times.
- **A whole class of sites will not work**, and we will not be able to tell the user why from inside
  the page. Offline-first applications fail in a way that looks like a network error.
- **`navigator.serviceWorker` being absent has to stay absent.** The pressure to add "just a
  registration that resolves" will recur every time a page's console shows an error; ADR 0012's rule
  is the answer and this is the case it was written for.
- **Nothing in this ADR is on the critical path for reddit, youtube or Plex.** All three degrade
  correctly without service workers, and none of them needs a dedicated worker. It is written now
  because the refusal has to be a decision before it is a gap.

## Alternatives considered

**Implement service workers, carefully.** Rejected on cost and on all three priority rules at once.
It is one of the largest specifications on the platform, it is a network proxy on the hostile side
of the trust boundary, and its whole purpose is to run when nothing is looking. There is no
"carefully" that changes those.

**Implement registration and the `install` event, but never intercept `fetch`.** Rejected as the
canonical ADR 0012 stub: `serviceWorker` in `navigator` is true, the page takes the native path,
its offline strategy silently never activates, and the failure surfaces as missing content much
later. Absent is strictly better.

**Implement service workers without persistence, so they die with the session.** Tempting, because
it removes the supercookie and the background wakeup at once. Rejected because it is a service
worker that does not do the thing service workers are for, announced as one — the update algorithm,
the `waiting` state and the cache all become theatre, and pages that rely on a worker surviving a
reload get a browser that disagrees with itself.

**Skip dedicated workers too, since only two sites use them.** Rejected. It is small, it is the
honest shape for the threading this project will eventually want, and having the ownership discipline
written down before the audio thread arrives is worth more than the two call sites.
