# ADR 0003 — An IPC Seam Before There Are Two Processes

**Status:** accepted · **Date:** 2026-08-01

## Context

A browser eventually wants renderer processes: one sandboxed process per tab, so a memory-safety bug
in the HTML parser cannot read your cookies. Chrome, Firefox, and Safari all do this. Ladybird added
it after the fact and found it painful.

The problem is that multi-process is not a feature you add. It is a constraint on every interface
between the UI and the engine, retroactively. Any interface that passes a pointer, shares a mutable
object, or returns a value synchronously has to be redesigned — and by the time you have layout and
script, there are hundreds of them.

Building multi-process from day one has real costs too: every debugging session spans two processes,
memory per tab goes up immediately, and none of it earns anything until there is content worth
sandboxing. At M0 there is no HTML parser to contain.

## Decision

Ship one process. Define the boundary as if there were two.

*What the processes actually are — how many, what each contains, and what the sandbox denies — is
`docs/adr/0004-process-model-and-site-isolation.md`. This ADR decides only that the boundary exists
and what shape it has.*

- `src/ipc/Message.h` holds the complete vocabulary crossing the boundary: a closed set of typed
  messages, versioned, each with `Serialize`/`Deserialize`.
- `src/ipc/Transport.h` is a non-blocking interface. `InProcessTransport` (two queues) is what ships;
  `SocketTransport` (length-prefixed frames over a `socketpair`) drops in later.
- The module contract forbids the two sides from naming each other's types. `src/app` cannot include
  a `dom/`, `css/`, `layout/`, or `js/` header; `src/engine` cannot include `platform/` at all, so it
  has no window, no renderer, and no way to acquire one.
- Painting crosses as a `gfx::DisplayList` — data, not draw calls.
- **Every message round-trips through serialization in the test suite on every build**, even though
  the in-process transport never encodes anything.

## Why Serialize What Is Never Serialized

This is the part that looks like waste, and is the whole point.

A message that cannot be serialized is a message that quietly holds a pointer or a reference to
shared mutable state. That is exactly the defect that makes a process split expensive, and it is
invisible while both sides are in one address space — the code compiles, the tests pass, and nothing
is wrong until the day it all is.

`IpcMessageTests` catches it on the commit that introduces it. The cost is a few hundred
microseconds per test run.

The wire format is also already written to a hostile-input standard: bounds-checked reads, a sticky
failure flag, length prefixes validated against the bytes that actually remain, trailing bytes
rejected rather than ignored, explicit tag constants rather than variant indices. Doing that now is
free. Doing it once the seam carries output from a renderer running untrusted content is a security
retrofit under pressure.

## Consequences

- Small ongoing tax: messages are data, so the UI cannot ask the engine a synchronous question. This
  is a genuine constraint and occasionally awkward. It is also the constraint that makes the design
  work, and discovering it now is much cheaper than discovering it at M9.
- The engine is testable headlessly, today: drive it with messages, assert on the display list. No
  window required.
- The split becomes schedulable — a `SocketTransport`, a `fork`/`exec`, and a sandbox policy —
  rather than a rewrite.
- A shared-memory surface will be needed when display lists get large; the `PaintFrame` message
  becomes a handle plus damage rects. That is a change to one message, not to the architecture.

## Note

The constraint immediately found a real violation. `src/app` was including `<SDL3/SDL.h>` to call
`SDL_WaitEvent` — reasonable-looking, and exactly wrong: it put the window system into the layer
that is supposed to run without one. Event waiting moved into `platform::SdlWindow`, and `src/app`
now declares no external dependencies at all, so it cannot come back.

That is one violation found in a codebase four days old, of the precise kind that becomes
unfixable at scale.
