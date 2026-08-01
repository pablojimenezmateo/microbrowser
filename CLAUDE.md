# Agent Guide

First-stop operating guide for agents working in this repository.

## Quick Scan

- `microbrowser` is a from-scratch native web browser in C++20, CMake, SDL3 for windowing only.
- Priority order: correctness → security → privacy → speed → CPU → memory → simplicity.
- **Every byte from the network and every message from a renderer is attacker-controlled.**
  Isolation is per *site*, not per tab. `guidelines/security.md` is the short version; ADR 0004 is
  the model.
- **Idle CPU is zero and must stay zero.** The process blocks in one place: the platform event wait.
- Every `src/` directory has a `MODULE.deps` contract enforced by the architecture lint. Read the
  manifest before adding a file, an include, or a class member.
- `AGENTS.md` owns repo policy. `guidelines/` owns the durable how-to. `docs/adr/` owns decisions.
- Build with `cmake`, test with `ctest`, and prefer `tools/run-checks.sh` so results are readable
  afterward without rerunning.

## Project Status

**Milestone M0 (foundation) is complete.** What exists:

| Module | State |
|---|---|
| `src/util` | Parse, StringUtil, tracing, counters — ported from `../microide` |
| `src/gfx` | Geometry, Color, Canvas, DirtyRegion, DisplayList. Solid fills only. SDL-free. |
| `src/ipc` | Typed, versioned, serializable UI↔Engine messages + swappable transport |
| `src/engine` | Stub: turns a navigation into a placeholder display list. No DOM/CSS/network. |
| `src/platform` | The only module that knows what a window is. SDL lives here. |
| `src/app` | Main loop: idle-wait policy, bounded event drain, dirty-region policy, present |

Not yet started: `gfx` paths/text/images (M1), `net`/`privacy` (M2), `html`/`dom` (M3), `css` (M4),
`layout` (M5), `paint` (M6), `ui` (M7), `js` (M8). Roadmap in the plan file and `AGENTS.md`.

## Development Workflow

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)

# Inner loop: test binary only.
cmake --build build --target microbrowser_tests -j$(nproc)

# Full suite, sharded across cores.
ctest --test-dir build --output-on-failure -j$(nproc)

# Focused, by substring.
./build/microbrowser/microbrowser_tests Canvas
./build/microbrowser/microbrowser_tests ArchitectureInvariants
./build/microbrowser/microbrowser_tests --list
```

The build auto-uses **ccache** and **ld.lld** when present; both are no-ops if absent.

Prefer the logging wrapper — it tees to a deterministic file so a result can be read back instead of
rerunning:

```bash
tools/run-checks.sh tests   # -> /tmp/microbrowser-tests.log
tools/run-checks.sh asan    # -> /tmp/microbrowser-asan.log
tools/run-checks.sh ubsan   # -> /tmp/microbrowser-ubsan.log
tools/run-checks.sh tsan    # -> /tmp/microbrowser-tsan.log
tools/run-checks.sh all
```

**After a run, READ `/tmp/microbrowser-<target>.log` instead of rebuilding and rerunning.**

TSan needs ASLR cleared (`setarch -R`); `run-checks.sh` does this automatically. Running `ctest` on
the tsan preset by hand without it fails with "unexpected memory mapping" — that is the environment,
not a bug.

## Observability

All off by default, all read once on first use:

```bash
MICROBROWSER_PERF_COUNTERS=1   # non-zero event counters, dumped at exit
MICROBROWSER_PERF_SUMMARY=1    # per-label scope table ranked by self time
MICROBROWSER_PERF_TRACE=1      # one stderr line per scope (a firehose; distorts what it measures)
MICROBROWSER_STARTUP_SUMMARY=1 # same, for startup scopes
MICROBROWSER_TRACE_REDRAW=1    # one line per presented frame: full/partial, rects, coverage
```

**Read the main-thread column of a summary first.** Self time ranks CPU cost; main time ranks what
the user waits on, and the two routinely disagree. See `guidelines/observability.md`.

## Before You Add Anything

- **A new file** — check the module's `max_tu_lines` and whether the file belongs in this module at
  all. A file that does not fit the module's `purpose:` line wants a different module.
- **A new include** — check `allow:` and the target module's `public:`. If the header you want is
  private, decide whether it should be published or whether you are reaching across a boundary.
- **A class member or public method** — check the class's `budget:`. If you must raise it, say why
  in the commit message. That is the mechanism working, not a hoop.
- **A third-party dependency** — write an ADR. `docs/adr/0001-third-party-dependencies.md` lists the
  sanctioned set and why each earns its place.
- **A timer, poll, or background wakeup** — justify it against the zero-idle-CPU invariant, and
  route it through `IdleWaitState::next_deadline_ms`.
- **A network request** — it must be user-caused and must pass the privacy layer. See
  `guidelines/privacy.md`.
- **A parser, a decoder, or an IPC message** — the input is hostile. Bounds-check every read,
  saturate every size computed from input, and land the fuzz target on the same commit. See
  `guidelines/security.md`.
- **A thread** — write down what it owns, what it borrows, and who joins it before `main` returns.

`tools/budget-report.sh` prints headroom against every budget, sorted by how close each is to its
limit. Run it before a refactor to see what is about to blow.

## Agent Best Practices

- Narrow the problem with fast repo inspection: `rg`, `rg --files`, targeted reads. Avoid broad dumps.
- Match the repo's design direction instead of preserving stale boundaries. Broad refactors are fine
  when they improve correctness or ownership.
- Prefer RAII, explicit ownership, value semantics. Inheritance only for a durable polymorphic
  boundary (`ipc::Transport` is one; most things are not).
- Keep deterministic logic out of event glue and paint code — that is what makes it testable.
- Treat performance as measurable engineering. Do not guess; the counters and scopes are there.
- When a lint fires, fix the design it is pointing at. Raising a budget to silence it is sometimes
  right, but it should be a decision, not a reflex.

## Related Docs

- `AGENTS.md` — repo policy, priority order, invariants
- `guidelines/architecture.md` — the module contract, layering, separation of concerns
- `guidelines/security.md` — trust boundaries, the process model, hostile input, memory safety
- `guidelines/privacy.md` — the privacy contract every feature is held to
- `guidelines/cpp.md` — ownership and implementation guidance
- `guidelines/performance.md` — measurement workflow and the zero-idle-CPU rule
- `guidelines/observability.md` — counters, scopes, and how to read a summary
- `guidelines/testing.md` — test strategy, reference tests, control fixtures
- `docs/adr/` — durable decisions and their reasoning
- `docs/performance/m0-baseline.md` — the measurements M0 established
