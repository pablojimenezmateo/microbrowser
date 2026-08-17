# web-platform-tests expectations

One file per top-level WPT directory. **Only failures are written down** — the
default for every test and every subtest is PASS.

That direction is the whole design. A file that lists only failures shrinks as
the browser improves, so the diff of a session that fixed something is a
*deletion*, and a file that grows is a regression somebody had to choose. The
alternative — every subtest with its status — is a 40,000-line file whose diffs
nobody reads, which is the same as having no expectations at all.

## The format

```
[dom/nodes/Node-appendChild.html]
harness=ERROR
FAIL=Appending a child to itself
TIMEOUT=Adopting an orphan
```

- The bracketed line is the test's URL path, including any variant query.
- `harness=` is the harness status when it is not `OK`: `ERROR`, `TIMEOUT`,
  `CRASH`, `PRECONDITION_FAILED`, or — for a reftest — `FAIL`.
- Every other line is `STATUS=subtest name`. The **status is the key** because a
  subtest name may contain `=` and a status never can.
- `disabled=reason` excludes the test from the run entirely. The reason is
  required: a test worth excluding is worth excluding loudly.
- **A harness status that is not `OK` subsumes the subtests**, so a test with
  `harness=TIMEOUT` lists none. A test that times out half way through reports
  every remaining subtest as `NOTRUN`, and those are the consequence of one
  failure rather than a thousand independent ones — the first run of this
  harness wrote 188,172 `NOTRUN` lines into `encoding.txt` before this rule
  existed. Fixing the timeout is the task; what is behind it is not yet a fact.
- `#` starts a comment. Use one whenever a line records a *deliberate* deviation
  rather than a bug, and name the ADR:

```
# ADR 0033: this browser trims the referrer by default and the test asserts the
# platform default. Deliberate; do not "fix".
[referrer-policy/gen/top.http-rp/unsafe-url/fetch.http.html]
FAIL=Referrer Policy: expects full referrer
```

## Reftests

A reftest renders two pages and compares the pixels, so it has no subtests: its
whole result is one `harness=FAIL` line, or nothing at all when it agreed with
its reference. **13,604 of them were recorded on 2026-08-17** (plan task F9);
before that day nothing had ever written a reftest result down, so a reftest that
passed and a reftest nobody ran were the same silence and every number this
project quoted covered 52% of its own suite.

Three things follow from having them here:

- **Absence means PASS for a reftest too**, and that reading is only sound
  because the *whole* reftest suite was recorded in one run — 20,998 files in
  105 seconds. Re-record all of them, not a directory, if that ever stops being
  true.
- **`ctest` does not compare them yet** (task F10). Eight of the 20,998 are
  intermittent across two runs of the same binary — seven `@font-face` tests
  whose font this browser never loads, and one animation — and a gate that is a
  coin toss is worse than one that does not run. `microbrowser_wpt
  --reftests-only` is the check, and it takes two minutes.
- **`--reftest-artifacts DIR` is how to read one.** A `harness=FAIL` line says
  how many pixels differed, which cannot tell an antialiasing tolerance from a
  missing feature; the three PPMs it writes can, by eye, in one second.

## Working with them

```bash
./build/microbrowser/microbrowser_wpt dom/                     # check
./build/microbrowser/microbrowser_wpt --update-expectations dom/   # record
./build/microbrowser/microbrowser_wpt --reftests-only css/CSS2/    # the pixel half
git diff tests/wpt/expectations/                               # the session
```

A newly *passing* subtest is an unexpected result and fails the run, exactly
like a newly failing one. That is intentional: silently accepting improvement is
how an expectation file stops describing anything.

See `docs/adr/0040-web-platform-tests.md` for why any of this exists, and
`docs/wpt-plan.md` for what to work on.
