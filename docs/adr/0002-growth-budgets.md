# ADR 0002 — Growth Budgets

**Status:** accepted · **Date:** 2026-08-01

## Context

This project is expected to take twelve to eighteen months. Over that span the failure mode is not
a wrong decision — it is the absence of decisions: classes that grow three lines at a time until
one object knows about everything, and per-node structs that gain a field per feature until a large
document does not fit in memory.

Both are invisible in code review. Every individual step is small and defensible, and by the time
the result is obviously wrong, no single commit is to blame.

Prior art solves half of it. Chromium's `DEPS` `include_rules`, Gecko's `moz.build` `EXPORTS`, and
Ladybird's per-library targets all constrain *coupling* — who may depend on whom. None of them
mechanically bounds *size*. They rely on review culture and owner files, which works with hundreds
of reviewers and does not work with one.

## Decision

Three complementary budgets, all mechanically enforced.

### 1. Class budgets, in `MODULE.deps`

```
budget: Application header_lines=80 public_methods=6 members=12
```

Enforced by `CheckClassBudgets`. Two halves, and the second matters more:

- **Exceeding a budget fails.**
- **A class over 25 header lines with no budget also fails.** Without this the rule only constrains
  classes someone already thought about, and a brand new god object sails past it.

### 2. Fan-out budget

`CheckClassFanOut`: no class holds data members whose types come from more than four project
modules. That is the mechanical definition of a god object, and it is far easier to detect than to
argue about. Four allows a legitimate coordinator — its own module plus three collaborators — and
excludes the eight-plus that characterizes a shell object.

### 3. Object-size budgets, as `static_assert`

```cpp
static_assert(sizeof(Color) == 4, "Color is stored per pixel; it must stay one word");
static_assert(sizeof(DisplayCommand) <= 24, "DisplayCommand must stay small");
```

For types allocated per pixel, per display command, per DOM node, per layout box, or per JS value,
size is multiplied by the document. A `sizeof` assert makes growth a **compile error** rather than a
memory-profile mystery six months later. `CheckObjectSizeBudgetsArePresent` verifies the asserts
still exist, so they cannot be quietly deleted to make a change compile.

## Why This Works

**It converts drift into an event.** When `Application` needs a thirteenth member, the build fails.
The fix is either "this belongs somewhere else" or an edit to `MODULE.deps`. Both are fine. The
third option — nobody notices — is the one that is eliminated.

**Raising a limit is visible.** It is a line in the diff, in a file whose entire purpose is to hold
decisions like that one. A reviewer sees `members=12 → members=16` and can ask why. Nobody can see
the equivalent growth spread across nine commits.

**The numbers are not the point.** They are set at roughly each class's natural size when written,
not at an aspirational target and not with generous headroom. A budget with slack in it does
nothing. The point is the forcing function, not the number.

## Consequences

- New classes need a budget entry. Mild friction, deliberately placed at the moment the class is
  designed rather than after it has grown.
- Budgets will be raised, sometimes for good reasons. That is the mechanism working.
- The scanner in `tests/architecture/SourceScan.cpp` is **not** a C++ parser. It understands the
  style this codebase uses — one type per header, no class bodies inside macros, no preprocessor
  conditionals splitting a definition — and the lint enforces that style, so the two hold each other
  up. It is written to under-count when unsure, so its error direction is a missed violation, never
  a false failure.
- `tools/budget-report.sh` prints headroom sorted by how close each budget is to its limit, so the
  class about to blow one is visible before it does.

## Alternatives Considered

**Review discipline alone.** What everyone does, and what fails on a solo long-running project.
There is no second reader to notice the trend.

**Cyclomatic complexity or maintainability metrics.** Correlate poorly with the actual failure and
produce numbers nobody acts on. Member count is crude but names the real thing: a class that knows
about too much.

**A clang plugin** (Gecko's approach) for compile-time enforcement. More precise, and much more
machinery — a build dependency on a specific Clang version, for a project that is warning-clean on
both GCC and Clang today. Revisit if the text scanner's limits start to bite.

## Note

The first thing this system did was catch a bug in itself: `ClassFanOut` could not trip its own
dirty fixture, because the fixture manifest defined three modules against a threshold of four. The
rule was inert. That is why every rule ships with a clean *and* a dirty control fixture, and why the
suite fails if a rule lacks them — see `guidelines/testing.md`.
