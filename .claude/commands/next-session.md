---
description: Pick the next unfinished roadmap session, implement it, verify it, commit it, record it, stop.
argument-hint: "[session number, to override the pick]"
---

You are starting a **fresh session** on microbrowser with no memory of any previous one. Everything
you need to know is on disk. Your job is **exactly one session** from
`docs/roadmap-to-any-page.md`, taken to a committed, verified, clean state — and then you stop.

`$ARGUMENTS`, if non-empty, is the session id to work on. Otherwise pick as described in step 2.

---

## 1. Orient — read before you write anything

Do these first, in this order:

1. `git log --oneline -20` — the commit subjects are declarative sentences about what landed. They
   are the most reliable record in the repo.
2. `git status` — see step 6 before you touch anything you did not create.
3. `tail -60 docs/session-log.md` — what the last few sessions did, and anything they warned you
   about.
4. `docs/roadmap-sessions.json` — the ledger. This is the state.
5. `CLAUDE.md` — you have it already, but re-read the "Where To Pick Up" section against the ledger.
   If the two disagree, the ledger and the git log win, and say so in your log entry.

## 2. Pick

Take the **lowest-id session whose `status` is not `done`**, preferring an `in_progress` one over a
`not_started` one at the same position. Do not skip ahead because a later session looks easier; the
order is the decision the roadmap made, and the reasoning is in the ADR it names.

If the session's `check` is `null` (the Phase E table rows), your **first** deliverable is to write a
check a person can run, into the ledger, before you write code. A session with no check cannot be
finished, only abandoned.

Then read, in full, before writing code:

- the named section of `docs/roadmap-to-any-page.md` (the `anchor` field finds it),
- every ADR in the `adr` field,
- the `MODULE.deps` of every module you expect to touch.

## 3. Sanity-check the tree before you build on it

Build and run the suite **before** you change anything:

```bash
cmake -S . -B build -G Ninja && cmake --build build -j$(nproc)
tools/run-checks.sh tests    # then read /tmp/microbrowser-tests.log
```

If it is already broken, that is your session: fix it, commit it, log it, stop. Do not build on a
red tree.

## 4. Implement — one session, no more

The repo's rules are not advisory. In particular:

- **`MODULE.deps` is a contract.** A new file, include, class member or public method may need a
  budget raised — that is fine, and the reason goes in the commit message. Widening `allow:` to
  reach across a security boundary is not fine; ADR 0008 and ADR 0015 exist because of exactly that
  temptation.
- **A parser, decoder or IPC message lands its fuzz target on the same commit.** Not the next one.
  Bounds-check every read, saturate every size computed from input.
- **A stub is worse than an absence** (ADR 0012). Feature detection sends a page down the native
  path into a wall where a missing name would have sent it to a working polyfill. Do not define a
  name you do not implement.
- **Zero idle CPU is an invariant.** A timer, poll or wakeup routes through
  `IdleWaitState::next_deadline_ms` and gets a test that says the loop sleeps when nothing is
  happening.
- **Every network request is user-caused and passes the privacy layer.**

It is **unacceptable** to delete, skip, weaken or `#if 0` a test in order to get a green run. If a
test now fails because it encoded an assumption the session deliberately changed, change the test
*and say so explicitly in the commit message and the log entry*. If a test fails and you do not
understand why, that is the session's finding — write it down rather than routing around it.

## 5. Verify — the check, not the suite

The suite passing is necessary and not sufficient. **Run the session's `check` and report what you
actually saw.**

```bash
tools/run-checks.sh tests           # -> /tmp/microbrowser-tests.log, read it, do not re-run
./build/microbrowser/microbrowser_snapshot <url> -o /tmp/out.ppm -v
```

`microbrowser_snapshot` needs no display. `-v` dumps the display list, `-click x,y` follows a link
first, and it always prints a script that threw. **Look at the rendered page.** Every layout and
paint bug in this repo's history was found by rendering a real page and looking at it; none of them
failed a test first. Convert the PPM and read it as an image if you can.

Then run the sanitizers for anything touching a parser, a decoder, or memory:

```bash
tools/run-checks.sh asan && tools/run-checks.sh ubsan
```

## 6. Commit — by explicit path, never `git add -A`

**Other sessions run in this repository in parallel and leave uncommitted files behind.** `git
status` at the start of your session may show work that is not yours. Do not stage it, do not
revert it, do not build on it. Stage only the paths you touched, by name.

Commit in coherent chunks as you go rather than once at the end. Match the existing subject style —
a declarative sentence about what is now true, not a `feat:` prefix and not a list of files:

```
Three percent-decoders become one, and the form serializer becomes correct
requestAnimationFrame, and no frame when nobody asked
```

If you raised a budget or widened a manifest, the commit message says why.

## 7. Record — the handoff is the deliverable

Update `docs/roadmap-sessions.json`:

- `status` becomes `done` **only if you ran the `check` and it passed.** If the code landed and the
  check did not pass, it is `in_progress` and `notes` says exactly how far it got and what the check
  actually printed. Nothing else may set `done`. A session marked done that is not done costs the
  next agent a whole session to discover.
- `notes` records what a fresh agent would otherwise have to rediscover.
- If the work proved the roadmap wrong — the session was really a phase, the measurement was
  measuring the wrong thing, a gate was reached and the site still does not work — say so in
  `notes` and in your log entry. The roadmap names those three failure modes on purpose.

Append to `docs/session-log.md`, newest last, using the template at the top of that file.

Commit the ledger and the log **in the same commit as the work** where possible, so the state and
the code cannot disagree.

## 8. Stop

Do not start the next session. Do not "quickly also fix" the thing you noticed — write it in the
log instead, where the next agent will find it. Finish with a short report:

- which session, and what its check printed
- what landed, what did not, and what you left for the next agent
- anything that contradicts the roadmap, the ledger or `CLAUDE.md`

Leave the tree clean and buildable. The next agent starts with nothing but what you wrote down.
