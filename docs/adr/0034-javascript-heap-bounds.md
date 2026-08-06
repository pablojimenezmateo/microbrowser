# ADR 0034 — The JavaScript heap is a bounded, collectable resource

**Status:** accepted · **Date:** 2026-08-06

## Context

ADR 0009 bounded *parse* depth for attacker-controlled script after youtube.com's bundle sat
just past a round-number guess. The live heap had the same shape of bug and had not been written
down: `Heap::limit_` was a flat 500 000 cells, allocation past it threw `RangeError: out of
memory`, and nothing recorded what a real page's *live* set cost.

Two findings on youtube.com, Debug build, after Polymer could stamp:

1. **`js.heap_live_peak` sat at 500 000** — equal to the ceiling — with `js.heap_oom` non-zero.
   Raising the ceiling without fixing collection would have been the same mistake ADR 0009 names:
   a round number with no measurement beside it.
2. **Collection was not running during custom-element reactions.** `CallFunction` raised
   `call_depth_` around `CallCompiled`. `MaybeCollect` refuses when `call_depth_ != 0`, because
   that flag means "C++ locals hold roots the collector cannot see". Bytecode frames are *data*
   (`GatherVmRoots`); raising the depth around them made every VM safepoint a no-op for the
   whole reaction. The page allocated straight into the ceiling. That is the failure mode the
   bytecode machine was built to prevent (see `src/js/Vm.cpp`'s file comment), defeated by one
   counter around the wrong call.

Speed and privacy meet here. An unbounded heap is a denial-of-service primitive and a fingerprint
of how long the user left a tab open. A heap that cannot collect during the work a modern page
actually does (upgrades, attribute reactions, observers) is a correctness bug that presents as
"out of memory" on a machine with free RAM.

## Decision

### 1. Compiled code entered from C++ may collect

`CallFunction` does **not** raise `call_depth_` when the callee has bytecode. Natives and the
tree-walker still do — their live values sit in C++ frames. The machine's stacks remain the
safepoint story ADR 0009's era wanted and the VM delivered.

### 2. The default live-cell ceiling is 2 000 000, and it is a measured number

Two million cells admitted youtube.com's Polymer upgrades with `js.heap_live_peak ≈ 524 000` and
`js.heap_oom = 0` in the same Debug configuration that previously died at 500 000. The ceiling is
still a *cell count*, not a byte budget — that remains tech debt (TD-0012) — but it is no longer
a guess smaller than one SPA's live set.

Changing the default requires writing the new peak from a target page beside the change. A round
number alone is not a reason.

### 3. The counters are part of the bound

| Counter | Meaning |
|---|---|
| `js.heap_oom` | Hard-limit hits (`AllocateObject` / `AllocateEnvironment` returned null) |
| `js.heap_live_peak` | Max live cells observed at a collection |

A page that sits near the ceiling with `heap_oom` still zero is headroom; a page that throws with
`heap_oom > 0` and a peak glued to the limit is a live-set problem, not a "GC is slow" problem.
`ReportUncaught` appends `CaptureStack` even when `MakeError` had to fall back to a bare string,
because the OOM that cannot allocate an `Error` object is exactly the one that needs a place.

### 4. Refusing remains correct

Past the limit, allocation fails and script sees `RangeError`. The heap does not grow without
bound, does not page forever, and does not start killing other tabs' state. A single document that
cannot fit is a broken page or a too-low ceiling — decided with measurements, not by removing the
ceiling.

## Consequences

- **Positive:** Custom-element reactions, event handlers, and other C++→script entries collect
  like top-level script. youtube.com's upgrades stop dying with a stackless OOM.
- **Positive:** TD-0012 has a design home; a later byte-budget limit replaces the cell count
  without changing the safepoint rules.
- **Negative:** A page whose *true* live set exceeds the ceiling still fails. That is accepted
  under ADR 0033: correctness and a privacy-relevant bound outrank shipping an unbounded heap
  for one target.
- **Negative:** `call_depth_` is now a sharper tool. A new native that calls back into script
  while holding unscanned `js::Value` locals must still raise it — or root those values
  explicitly. Getting that wrong is a use-after-free, not a performance footgun.

## Alternatives considered

**Remove the heap limit.** Rejected. Attacker-controlled allocation without a ceiling is a
process-level DoS; ADR 0009's refusal logic applies to the live heap as much as to parse depth.

**Only raise the limit to 2M, leave `call_depth_` around `CallCompiled`.** Rejected. That treats
the symptom. The next SPA would find the same wall with a larger number, and safepoints would
still not run where the bytecode engine promised they would.

**Collect from a timer or background thread.** Rejected under the zero-idle-CPU invariant and
ADR 0011: the process blocks in one place, and a concurrent collector is a multi-session design
with its own ADR when measurements demand it.
