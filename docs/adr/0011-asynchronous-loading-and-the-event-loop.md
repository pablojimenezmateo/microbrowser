# ADR 0011 — Asynchronous loading and the event loop

**Status:** accepted, implemented · **Date:** 2026-08-04 · **Implemented:** 2026-08-04

## Context

Loading is synchronous. `Loader` fetches a document, then its stylesheets, then its scripts, then
its images, and the host loop is blocked for the whole of it. `PageScript::Run` then runs every
script in one pass, after the parse has finished.

This was the right first version and it is written down as crude in `CLAUDE.md`. It has now become
the structural blocker, and aiming at youtube.com is what made that concrete:

- **15 sequential round trips** for one page (ADR 0010 has the counters). Every one of them is
  latency the user waits through, serialised, on a page whose resources have no dependency on each
  other.
- **Nothing can arrive after the load.** `fetch`, `XMLHttpRequest` and dynamic `import()` are all
  the same missing shape: a request made *by* script, whose answer arrives *later*. A
  single-page application is defined by that shape. It is why ADR 0007 puts new reddit, Plex and
  YouTube in the hard tiers, and it is why `<script type="module">` parses and links but cannot
  reach the network — the VM half of modules is done and the host half needs a loader that can
  answer a question asked in the middle of evaluation.
- **`requestAnimationFrame` has nowhere to go.** It is not a timer; it is a callback tied to the
  next frame, and there are no frames — there is a paint at the end of a load.

The invariant that constrains every answer here is the one in `AGENTS.md`: **idle CPU is zero, and
the process blocks in one place — the platform event wait.** Anything that turns loading into
polling breaks the project's most-stated promise. This is the reason the design has to be decided
rather than grown: an event loop is exactly the component that gets a `while (true)` with a 1ms
sleep in it, and then never loses it.

The good news is that the two hardest pieces already exist and already respect the invariant. The
microtask queue is done, and it turned out to need nothing from the host loop: a microtask only
exists because something already ran, so the drain rides a wakeup that was already happening. And
`setTimeout` is done and arrived as `IdleWaitState::next_deadline_ms` — a page with nothing pending
still lets the loop block indefinitely. Both are the pattern the rest of this follows.

## Decision

**Loading becomes asynchronous; the loop stays blocking.** Those are not in tension, and the
mechanism that makes them compatible is the one already in use: the loop computes how long it may
sleep from what is actually pending, and sleeps exactly that long in the platform wait.

### The wait gains a second input

Today `IdleWaitState` answers "how long until the soonest timer". It gains "and which file
descriptors am I waiting on". The platform wait becomes a wait on *sockets or input or a deadline*,
whichever comes first — one blocking call, no polling, and an idle browser with no requests
outstanding still blocks forever on input alone. Zero-idle-CPU is unchanged, and it is unchanged for
the same reason it survived timers: the loop is told what to wait for rather than asked to check.

This is the whole design. Everything below is consequences of it.

### Requests become objects with completions, not calls that return

`Fetch` today is a function that returns a response. It becomes a request that is *started*, and
whose completion is delivered on a later turn of the loop. The privacy layer is untouched by this: a
verdict is computed when the request is created, and there is still no way to start one without it.

**The number of concurrent connections per partition key is bounded**, and the bound is per key
rather than global — the key is from ADR 0005 and a global limit would let one site's requests
starve another's, which is a cross-site interaction of exactly the kind the key exists to prevent.

### The script/parse relationship changes, and this is the risky part

A synchronous `<script>` blocks the parser and can call `document.write`. Running scripts after the
parse (the current behaviour) is a deliberate deviation, written down in `PageScript.h`, and it is
correct for the pages reached so far. It cannot survive `defer`, `async`, and module scripts, which
are defined by *when* they run relative to the parse.

The decision is to keep the deviation for classic inline and blocking scripts, and to implement
`defer`, `async` and `type="module"` as what they actually are — three different points in a
document's lifecycle — rather than treating them as attributes to ignore. `document.write` stays
unimplemented and stays written down. It is used by ad tags and by almost nothing else on the target
sites, and supporting it properly means re-entering the tokenizer mid-parse, which is a large amount
of risk for a feature the web is actively removing.

### `requestAnimationFrame` is a frame deadline, not a timer

It joins `next_deadline_ms` as a second kind of "the loop must wake by": a page with a pending
animation frame wakes at the next frame boundary, and **a page with none does not schedule a frame
at all.** This is the point where a browser normally starts burning a core on an idle page, by
running a 60Hz loop whether or not anything asked for one. It is stated here so that a later change
that does that has to argue against this line.

### Ordering is the thing to test, not throughput

The failure mode of asynchronous loading is not slowness, it is *nondeterminism*: resources arriving
in a different order producing a different page. So the tests are ordering tests, driven through the
existing canned-transport seam — the same responses delivered in several arrival orders must produce
the same display list. That is a stronger property than "it loads", and it is the one that decays
silently.

## Consequences

- **The synchronous `Fetch` overload goes away.** Everything that calls it — `Loader`, and the
  tests — moves to the started/completed shape. That is a wide, mechanical change and it is better
  done once than incrementally, because a codebase with both shapes will grow calls that block
  inside a completion.
- **The engine gains a queue that survives between turns**, and therefore gains state that a
  navigation has to cancel. A response arriving for a document that is gone must be dropped, and
  dropped by construction rather than by a check somebody remembers to write — the natural
  expression is that a request belongs to a page and dies with it.
- **This is what unblocks the module loader.** With a loader that can answer later,
  `Interpreter::SetModuleResolver` gets its host half, and `<script type="module">` reaches the
  network. `fetch()` is then a thin binding over the same machinery rather than a second one.
- **It does not by itself make anything faster to paint.** Fifteen serialised round trips become a
  handful of concurrent ones, which is a latency win; incremental parsing and painting is a separate
  piece of work that this enables and does not perform.

## Alternatives considered

**Threads for loading, with the loop joining them.** Rejected for now. It solves latency and
introduces the one class of bug this project is least able to absorb: the engine's data structures
are single-threaded by assumption throughout, and making the network half concurrent means every
one of those assumptions becomes a thing to check. Non-blocking sockets in the existing loop get the
same latency win with no shared mutable state at all. When loading does move off-thread — most
likely with the network process in ADR 0004 — it moves behind the IPC seam, which is where a thread
boundary is already assumed.

**A general "run this later" queue, with polling.** Rejected on the invariant. Polling is what
zero-idle-CPU means, and a design that polls is not repairable later by making the interval longer.

**Keep loading synchronous and special-case `fetch`.** Rejected. It would mean two loading paths,
and the one used by script — the hostile one — would be the newer and less exercised of the two.

## Addendum — what implementing it found

Written after the fact, because two things turned out to be true that this decision did not
anticipate, and a reader who only has the decision would be surprised by the code.

**Name resolution still blocks.** `getaddrinfo` has no non-blocking form. The two ways out are a
thread — rejected above, and the reasons have not changed — or a resolver library, which is a
third-party dependency and therefore ADR 0001's problem rather than this one's. It costs one
blocking call per *host* rather than one per resource, which is why it did not hold the rest up.
It is written down in `SocketTransport.h`, where someone adding a DNS cache will find it.

**"One blocking call" is approximated, and the approximation is bounded.** SDL exposes no
descriptor for its own event queue, so there is no single call that can wait on both window events
and sockets. With requests outstanding the loop waits on the sockets and caps that wait at one
frame, so a socket wakes it the instant it is ready and an input event waits at worst 16ms. It
costs a wakeup every 16ms *while a load is in flight and nothing else is happening*, and nothing at
all when none is — which is the case the zero-idle-CPU invariant is about, and the case the
decision above scopes it to. Removing the cap needs the display connection's descriptor, which SDL
offers only through platform-specific window properties: that would make X11 or Wayland a build
dependency, and so an ADR rather than a patch. `SdlWindow::WaitEventOrDescriptors` says all of this
where the cap is.

**One thing the decision did not mention and should have:** with nothing below the request layer
blocking, nothing below it can time out either. A server that accepts a connection and then says
nothing would hold a descriptor in the loop's wait forever. `RequestQueue` therefore keeps an
inactivity deadline — measured from the last byte that moved, so a large download is not killed for
being large — and feeds it to the same `next_deadline_ms` the timers use. That is not a new
mechanism; it is the one this ADR already relies on, used for the case the ADR created.

Measured on news.ycombinator.com, five requests: 4.3s to 2.1s.
