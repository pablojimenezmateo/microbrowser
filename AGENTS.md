# AGENTS

## Mission

Build `microbrowser` as a native, low-footprint web browser from scratch in C++20: own HTML parser,
own CSS engine, own layout, own software rasterizer, own HTTP client, own JavaScript engine. No GPU
requirement. Privacy-respecting by construction rather than by preference.

Ladybird is the reference for *engine shape* — library separation, spec-literal parsers, pixel
reference tests. LibreWolf is the reference for *defaults* — blocking, sanitization, partitioning,
and zero telemetry are engine-level invariants here, not an extension bolted on top.

Avoid marketing claims in code, commits, or docs. There are internal regression baselines but no
comparative benchmarks against other browsers. Do not write "fastest" or "lighter than X" — say
"native, low-footprint, responsive" and let measurements speak.

Do not optimize for keeping old boundaries alive. If the correct fix breaks compatibility, touches
many files, or requires a broader refactor, prefer the better design.

## Priority Order

When tradeoffs conflict, use this order:

1. **correctness**
2. **privacy & security defaults** — a browser that is fast and wrong about privacy has failed
3. **speed**
4. **low CPU usage** — especially idle CPU and the redraw path
5. **low memory usage**
6. **maintainability and simplicity**
7. **compatibility**, only when explicitly required

Compatibility is not a default constraint. Internal APIs, temporary abstractions, and stale call
patterns can be broken or removed if that is the cleanest way to improve the system.

## The Module Contract

This is the load-bearing structural rule of the project, and the answer to "how does a codebase
stay coherent for a year". Every directory under `src/` has a `MODULE.deps` manifest declaring:

| Field | Meaning | Borrowed from |
|---|---|---|
| `purpose:` | one line justifying the module's existence | — |
| `allow:` | modules this one may include from | Chromium `DEPS` `include_rules` |
| `public:` | headers other modules may include; anything unlisted is module-private | Gecko `moz.build` `EXPORTS` |
| `extern:` | third-party groups this module may include | — |
| `max_tu_lines:` | per-file line cap | microide |
| `budget:` | per-class header lines / public methods / data members | ours |

`tests/ArchitectureInvariantsTests.cpp` enforces every field. `CMakeLists.txt` mirrors `allow:` as
per-module static libraries so the build graph tells the same story (Ladybird's model).

**Why budgets rather than review discipline:** a class does not become a god object in one commit.
It grows three lines at a time, and every individual step looks reasonable. A declared budget makes
the cumulative growth land in the diff — raising a limit is an edit to `MODULE.deps` that a reviewer
sees, instead of drift nobody can point at. A class large enough to need a budget that has none is
also a failure, so new god objects cannot appear un-budgeted.

Full documentation: `guidelines/architecture.md`. Rationale: `docs/adr/0002-growth-budgets.md`.

## Hard Invariants

Enforced by the architecture lint. Each ships with a clean and a dirty control fixture, and the
suite fails if any rule lacks them — **a rule that has never been observed to fail is not a rule.**
That check earned its keep on its first run by catching a fan-out rule that was silently checking
nothing.

- **Module include rules.** A project include must name a module in `allow:` and a header in that
  module's `public:`. This subsumes several rules that would otherwise be hand-written: "gfx must
  not include SDL" and "SDL lives only in platform" both follow from `gfx` declaring no `extern:`
  groups and `platform` declaring `SDL3`.
- **Third-party headers** appear only in modules declaring their group. The sanctioned list is in
  `docs/adr/0001-third-party-dependencies.md`; adding to it requires an ADR.
- **Class and TU budgets** are not exceeded, and every class over 25 header lines has one.
- **Class fan-out**: no class holds data members from more than 4 modules. That is the mechanical
  definition of a god object, and it is far easier to detect than to argue about.
- **No throwing numeric parses** (`std::stoi` and friends). They throw *and* read the decimal
  separator from the process locale, which SDL changes behind our back. Use `util/Parse.h`.
- **No banned C functions.** `strcpy`, `strcat`, `sprintf`, `strncpy`, `alloca`, `strtok`, `atoi`,
  `rand`, `mktemp`, `system`, and the rest of the list in `CheckNoBannedCFunctions` — each has a
  safe-looking call site and no safe behavior on attacker-influenced input.
- **No manual heap ownership.** No owning `new`/`delete`, no `malloc`/`free`. A raw owning pointer
  becomes a use-after-free the first time an early return is added above it, and in a browser a
  use-after-free is an RCE primitive rather than a crash. Placement new and `operator new` are
  outside the rule; `= delete` is not affected.
- **No mutable state at namespace scope.** Function-local statics are fine.
- **Headers use `#pragma once`.**
- **Object-size budgets** (`static_assert(sizeof(T) <= N)`) still exist on the types that are
  allocated per node, per box, per command, or per value.

## Rules Not Yet Enforced

Written down now so they are not rediscovered later. Each becomes a lint when the code it governs
exists.

- **Descriptor creation is close-on-exec** — `SOCK_CLOEXEC` / `O_CLOEXEC` on the creating call
  itself, never a follow-up `fcntl`. A browser spawns helper processes; an unflagged descriptor is
  inherited by all of them. Lands with `src/net` in M2. *(Deliberately not written as a vacuous
  rule today: with zero call sites it would pass while checking nothing, which is the exact failure
  the control fixtures exist to prevent.)*
- **No network request without a `privacy::Verdict`** — `net::Fetch` takes one by value and there is
  no overload without it. Lands with `src/privacy` in M2.
- **Spec-literal parsers stay spec-literal** — `src/html` and `src/css` name their states exactly as
  the specs do and carry the spec section in a comment. Divergence from the spec text is a bug, not
  a style choice. Lands with M3.
- **Paint TUs do not materialize strings** in hot paths. Lands with M6.

## Default Engineering Stance

- Prefer correct behavior over minimal diffs.
- Prefer cohesive refactors over local patches that preserve bad architecture.
- It is fine to touch many files when the change crosses ownership boundaries in reality.
- Delete dead code, stale docs, and compatibility shims as soon as the new path is established.
- Keep ownership narrow and obvious. No singletons, no global service locators.
- If a coordinator grows because a subsystem lacks a real API, add the API and move the logic out.

## Privacy Rules

Not a feature area — a constraint on every other area. See `guidelines/privacy.md`.

- No network request the user did not cause. No telemetry, no crash reporting, no remote config,
  no update pings, no prefetch, no search suggestions.
- Every request passes `src/privacy` before `src/net` sees it.
- Every cookie, cache entry, and storage key is partitioned by `(top-level site, origin)`.
  Total Cookie Protection by construction, not by policy flag.
- HTTPS-only by default; downgrading is an explicit per-site act.
- Nothing persists to disk unless the user opted in. The HTTP cache is memory-only by default.
- A feature that cannot be built without weakening one of these does not get built.

## Performance Rules

- Speed is the main optimization target after correctness and privacy.
- CPU before memory, especially idle CPU and the redraw path.
- **Idle CPU is zero.** The process sleeps in exactly one place — the platform event wait. Any
  feature that wants a timer must justify it and must go through `IdleWaitState::next_deadline_ms`,
  never a poll loop. This is the property most likely to be lost by accident, so
  `IdleWaitStrategyTests` guards the policy and the M0 baseline records the measurement.
- Measure before and after performance-sensitive changes. See `guidelines/performance.md`.
- Prefer deleting redundant work over adding speculative caching.

## Validation Expectations

- Every meaningful bug fix adds or tightens regression coverage.
- Run `tools/run-checks.sh tests` before considering work complete; sanitizers for anything touching
  memory, threads, or untrusted input.
- Every parser that touches network bytes gets a fuzz target on the commit it lands.
- Pixel reference tests for anything that changes rendered output.
- Update docs when a durable architecture decision or shipped capability changes.
