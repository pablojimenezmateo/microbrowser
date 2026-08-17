---
description: Pick the highest-ranked unclaimed WPT task, implement it, verify it, commit it, record it, stop.
argument-hint: "[task id, to override the pick]"
---

You are starting a **fresh session** on microbrowser with no memory of any previous one. Everything
you need is on disk. Your job is **exactly one task** from `docs/wpt-plan.md`, taken to a committed,
verified, clean state — and then you stop.

`$ARGUMENTS`, if non-empty, is the task id (`F6`, `O2`, `H9`). Otherwise pick as in step 2.

**This is not `/next-session`.** That command works `docs/roadmap-sessions.json`, which sequences
work by *which page it unblocks*. This one works `docs/wpt-tasks.json`, which ranks it by *how many
test files Firefox passes and we do not*. The two ledgers are different state and disagree on
purpose; `docs/wpt-plan.md` says so at the top and supersedes the roadmap wherever they collide.

---

## 1. Orient — read before you write anything

In this order:

1. `git log --oneline -20` — the subjects are declarative sentences about what landed.
2. `git status` — see step 6 before you touch anything you did not create.
3. `tail -80 docs/session-log.md` — what the last sessions found and warned about.
4. `docs/wpt-plan.md` §The target and §2 "The order" — **the unit is a test file Firefox passes and
   we do not, never a percentage.** A subtest rate has a denominator that shrinks when a test dies
   early, so it is not comparable across browsers or across areas. This is the single most
   important thing on this page; sessions were picked by the wrong column for weeks.
5. `docs/wpt-firefox-gap.md` — the ranked gap, per area, with a `blocked` column.
6. `docs/wpt-tasks.json` — the state.

## 2. Pick

Take the task with the **highest `firefox_gap.files`** whose `status` is `not_started`, subject to:

- **Gate 0 first.** If F2, F9 or B6 is unclaimed, take it before anything else. Until F9 lands,
  20,998 reftest files — 48% of the suite — are in no number this project quotes, and the ranking
  below them is a floor rather than a measurement.
- **Respect `depends`.** A task whose dependency is not `done` is not startable. H8 is the clearest
  case: 1,330 files, 1,083 of them blocked, and writing feature code for it today moves nothing.
- **Prefer a high `blocked` count when two tasks are close.** A blocked file is one whose harness
  never reported; that is plumbing, and plumbing is usually cheaper per file than a specification.
- **An `in_progress` task may be stale.** Read `docs/wpt-plan.md` §Claiming a task. A claim with no
  commit touching its area since, or more than three days old, may be re-taken — and the audit in
  each task's `stale_claim` field tells you which. Say what you re-took in the claiming commit.

Then claim it: set `status: "in_progress"`, `agent`, and `claimed` to today's date. **Commit that
alone and push before starting.**

Then read, in full, before writing code: the named section of `docs/wpt-plan.md`, every ADR in the
`adr` field, and the `MODULE.deps` of every module you expect to touch.

## 3. Rank the work inside the area before you choose a fix

The task names an area, not a fix — deliberately, because which three causes dominate an area is
not knowable until the tests are run.

```bash
python3 tools/wpt/firefox-gap.py --cache /tmp/firefox-wpt-summary.json --list-gap <area>
```

That prints every file Firefox passes and we fail, each tagged `blocked` or `feature`. Then rank
the *subtests* within them by failing count over `tests/wpt/expectations/<area>.txt`. C3 spent an
afternoon on the thing its title named and got its points from four types that ranking made
visible; **the ability a task names is rarely where its failures are.**

Before reading an area's failures as a specification gap, check whether the harness can reach the
feature at all: one `--verbose` run, a look at the server's log for 501s, and a grep of the output
for `not implemented` and `is not defined` *harness errors* rather than subtest failures. On
2026-08-14, three of the five largest causes in the baseline were capabilities this browser already
had that the tests could not reach.

If the area's data is a checked-in table, **build a runner that reads it directly first**.
`tools/urlconf` runs 3,900 pinned vectors against `src/url` in a second and names the field that
differed; `url/` went 37% → 98% in one session because of it.

## 4. Sanity-check the tree before you build on it

```bash
cmake --preset microbrowser-perf && cmake --build --preset microbrowser-perf -j$(nproc)
tools/run-checks.sh tests    # then read /tmp/microbrowser-tests.log
```

**Use the perf build for anything WPT.** The expectations were recorded there, and a page that has
not reported inside testharness.js's ten seconds is a `TIMEOUT` whatever the reason — the Debug
build is four to seven times slower on every page. If the tree is already broken, that is your
session: fix it, commit it, log it, stop.

## 5. Implement — one task, no more

The repo's rules are not advisory, and four of them bite here in particular:

- **A failing test is a question, not a task.** Read the test, decide what the specification says,
  then decide whether this browser is wrong. A deliberate refusal gets an expectation line **and** a
  comment naming its ADR. Making a refusal pass by implementing what it refuses is a change to an
  ADR, which is a separate commit and a separate argument.
- **Fix the cause, not the test.** Ten tests failing on one missing method is one fix. If a change
  makes exactly one test pass, suspect it.
- **A stub is worse than an absence** (ADR 0012).
- **`MODULE.deps` is a contract**, and widening `allow:` to reach across a security boundary is not
  a budget raise — ADR 0008 and ADR 0015 exist because of that temptation.

It is **unacceptable** to delete, skip or weaken a test for a green run. When a WPT subtest
disagrees with a test in `tests/`, read the local test first and look at whether its comment argues
from the specification or from what the code used to do — three local tests turned out to be
pinning an accident.

## 6. Verify — re-record, then measure. They are different runs

```bash
./build/microbrowser-perf/microbrowser/microbrowser_wpt --update-expectations <area>/
./build/microbrowser-perf/microbrowser/microbrowser_wpt <area>/     # the one that counts
```

**`--update-expectations` printing `0 unexpected` is not a measurement.** The counter is guarded by
`&& !options.update_expectations`, so a recording run cannot report anything else — nothing it sees
is unexpected, by construction. The real check is the second run with the flag omitted; that is the
mode `ctest` uses and its exit status is non-zero when the count is not zero.

**Compare the subtest count against the previous record before committing a re-record.** A
denominator that fell means subtests were silently deleted from the measurement, and that has had
two different causes already: a `--long-timeout` that *shortens*, and a loaded machine turning
`reflection-*.html` into TIMEOUTs. Re-record at low concurrency or not at all.

**Nothing that changes behaviour may land while a re-record is in flight**, because a re-record
measures the binary it started with.

Then re-rank, so the ledger and the document describe what you left:

```bash
python3 tools/wpt/firefox-gap.py --cache /tmp/firefox-wpt-summary.json --annotate-tasks
```

Sanitizers for anything touching a parser, a decoder or memory:
`tools/run-checks.sh asan && tools/run-checks.sh ubsan`.

## 7. Commit — by explicit path, never `git add -A`

**Other sessions run in this repository in parallel and leave uncommitted files behind.** Stage only
the paths you touched, by name. Commit in coherent chunks as you go. Match the subject style — a
declarative sentence about what is now true:

```
Raise css/css-syntax from 13.8% to 90.2% of testharness subtests.
A frame that runs script and never has its queues drained is worse than one that runs none
```

**The expectation diff is the deliverable.** A session whose diff is all deletions did the work; one
that adds lines had better say why in the commit message.

## 8. Record — the handoff is the deliverable

In `docs/wpt-tasks.json`: `status` becomes `done` **only if you ran the `check` and it passed**.
Nothing else may set it. If the code landed and the check did not pass, it stays `in_progress`,
`notes` says exactly how far it got and what the check actually printed, and `claimed` stays so the
next agent can tell whether your claim has gone stale.

Append to `docs/session-log.md` using the template at the top of that file. Record **what a diff
does not say**: what you tried and rejected, what the check actually printed, what turned out to be
wrong. If the work proved the plan wrong — the task was really three tasks, the ranking was ranking
the wrong thing, an area turned out to be mostly refusals — say so there and amend `docs/wpt-plan.md`
in the same commit.

Commit the ledger and the log with the work where possible, so the state and the code cannot
disagree.

## 9. Stop

Do not start the next task. Do not "quickly also fix" the thing you noticed — write it in the log,
where the next agent will find it. Finish with a short report:

- which task, and what its check printed
- the expectation diff: files and subtests, both directions
- what you left, and anything that contradicts the plan, the ledger or `CLAUDE.md`

Leave the tree clean and buildable. The next agent starts with nothing but what you wrote down.
