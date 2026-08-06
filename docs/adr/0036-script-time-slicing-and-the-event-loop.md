# ADR 0036 — Script time-slicing, and when the loop may yield

**Status:** accepted · **Date:** 2026-08-06

## Context

TD-0007 names the shape that survived the 2026-08-06 performance pass: the wall clock came down
by 6×, but the *structure* did not. `Engine::RunScripts` still runs a page's script to completion
inside one turn of the loop, so youtube.com's 10.7MB kevlar bundle is a single uninterruptible
call during which no input is drained and no frame is presented. The window manager offers to kill
the process because from outside that is indistinguishable from a hang.

**Measured**, per phase, on youtube.com's main bundle (Debug build, 2026-08-06):

| Phase | Wall |
|---|---|
| `js::Parse` | 2.58s |
| `js::Compile` | 3.08s |
| `js::Execute` | 4.07s |
| **Total** | **~9.7s** |

Each phase is one call. `MICROBROWSER_LOAD_TIMELINE=1` on the same page also shows **~14.3s**
inside `page_.Layout()` on two passes after the bundle finishes — a separate problem (TD-0013) —
but the APP_NOT_RESPONDING shape the user reported is the script monolith, not layout.

ADR 0011 made the *network* asynchronous and left the *work* synchronous. ADR 0030 will slice the
*parse* against a deadline so the window stays responsive while a document arrives, and it
explicitly leaves script compilation and execution open. This ADR answers that open question for
the JavaScript phases.

Three constraints are not negotiable:

1. **Zero idle CPU** (`AGENTS.md`). The loop blocks in one place. A script that yields must
   schedule work through `IdleWaitState::next_deadline_ms`, not poll.
2. **Correctness of observable ordering.** Microtasks drain to completion before the host returns
   to the event loop. A yield in the middle of `Promise.then` chains, custom-element reactions, or
   `MutationObserver` delivery would be a web-visible bug.
3. **The bytecode machine already has safepoints.** `MaybeCollect()` runs at loop back-edges and
   at calls (`src/js/Vm.cpp`). ADR 0034 fixed collection during C++→script entry; the safepoint
   story is real, not hypothetical.

## Decision

### 1. Do not time-slice script execution yet — reduce the monolith first

**The next work on youtube.com's unresponsive window is not yielding; it is making the three phases
shorter and showing something before they run.**

| Lever | What it buys | Where |
|---|---|---|
| Arena-allocated AST | ~1.2s of the 2.58s parse (TD-0003) | `src/js` |
| Incremental parse + first paint | A skeleton on screen while subresources load; window responsive during parse (ADR 0030) | `src/html`, `src/engine` |
| Faster cascade / layout | Does not shorten the script call; fixes the post-bundle hang (TD-0013, TD-0005) | `src/css`, `src/engine` |

Time-slicing `js::Execute` while the bundle still takes 9.7s of CPU on a cold load only spreads
the same work across more turns. It fixes the *chrome* freezing, not the page appearing, and it
costs the hardest invariant in this file (microtask atomicity) for a gain ADR 0030 and TD-0003
deliver more cleanly.

**Revisit this ADR when**, after TD-0003 and ADR 0030 land, a measured load still spends more
than one frame budget inside a single `js::Execute` or `js::Compile` call *and* the user-visible
complaint remains. The counter pair is `js.compiled_instructions` against
`js.compiled_source_bytes`; a bailout is a different problem.

### 2. If execution must yield, use bytecode safepoints — not a host timer around `RunCompiled`

When the measurement says slice, the mechanism is:

- **Count instructions or wall time inside `Vm::Run`**, at the back-edges where `MaybeCollect`
  already runs.
- On budget exhaustion, **return a new completion kind** — "suspended, resume at this IP" — rather
  than throwing or pretending the function returned.
- The host (`PageScript::Run` / `Engine::RunScripts`) **requeues the frame** through the same
  path that already runs due work: `IdleWaitState::next_deadline_ms = now`, one `Advance()` turn,
  resume.

This reuses the machine's existing interruptibility. A separate host thread, a `requestIdleCallback`
poll, or slicing only at `await` boundaries are all rejected for this browser today:

| Option | Why not now |
|---|---|
| **`requestIdleCallback`-style host slice** | Inventing a second scheduling primitive beside timers and rAF; easy to violate zero-idle-CPU if "idle" is redefined as "between bytecodes". |
| **Yield only at `await` / `import()`** | youtube's kevlar bundle is overwhelmingly synchronous once it runs; `await` density is not the 9.7s. |
| **Preemptive OS thread** | Rejected in ADR 0011 and ADR 0034: single-threaded heap, no collector barrier. |
| **Slice parse/compile only** | Valuable and *orthogonal* — ADR 0030 slices HTML; `js::Parse`/`js::Compile` may get the same budget later. Does not require execution slicing. |

### 3. Rules any future slice must obey

Written now so the implementation cannot "just add a yield" without answering them:

1. **Microtask checkpoints are atomic.** Drain may run only at host boundaries where the spec
   already allows it — after a script job completes, not mid-job. A sliced `RunCompiled` that
   returns while microtasks are pending must resume *before* input or paint, same as today.
2. **Zero idle CPU unchanged.** A suspended script sets a deadline; a finished script schedules
   nothing. No periodic wakeups to poll "is the script done".
3. **Re-entrancy is forbidden.** While suspended, the same `Interpreter` must not run a second
   script on the same global (navigation, `document.write`, devtools). Navigation cancels the
   suspended frame with the document.
4. **Parse and compile bounds stay** (ADR 0009). Slicing is not a substitute for refusing
   blowup; it is a responsiveness tool for programs that are large but legitimate.
5. **Measure before merging.** `MICROBROWSER_LOAD_TIMELINE=1` must show the loop draining input
   between slices on youtube.com, and `microbrowser_tests` must include an ordering test: the
   same script run sliced and unsliced produces the same heap state and the same DOM.

### 4. Relationship to TD-0007 and ADR 0030

- **TD-0007** remains open. This ADR is its design home; the entry should cite ADR 0036 rather
  than "wants an ADR".
- **ADR 0030** is the higher-value responsiveness work for server-rendered pages. Script slicing
  is the fallback when incremental paint is not enough — exactly youtube.com and Plex, where the
  document is a skeleton and the bytes that matter are JavaScript.

## Consequences

- **Positive:** A later session can implement slicing without re-arguing the invariant set.
  Safepoints, microtask rules, and the rejection of polling are decided.
- **Positive:** Effort stays on TD-0003 and ADR 0030 first, which attack the measured 9.7s rather
  than hiding it behind 600 turns of 16ms.
- **Negative:** Until slicing lands, a 10MB bundle remains one uninterruptible call after the
  network half finishes. That is accepted under ADR 0033: correctness and honest bounds before
  responsiveness hacks that risk ordering bugs.
- **Negative:** `js::Compile` is still one call. If compile time rivals execute time after TD-0003,
  the same safepoint machinery must be extended to the compiler or parse — a separate follow-up.

## Alternatives considered

**Slice immediately at 16ms like ADR 0030's parse budget.** Rejected as the first move. The parse
budget has a clear partial-state story (incomplete tree). A bytecode frame suspended mid-function
has none — every native callback, every DOM mutation, every promise reaction must define what
"partial" means. The machinery exists; the spec surface does not.

**Run script on a worker thread.** Rejected. The DOM, layout, and network completion path are
main-thread and single-threaded by assumption throughout. Moving script alone buys parallelism
at the cost of every binding becoming a cross-thread marshalling problem — ADR 0004's process
split, not a yield patch.

**Do nothing; accept the hang.** Rejected for the targets in ADR 0007. The deferral is
conditional on measured headroom after TD-0003 and ADR 0030, not permanent.
