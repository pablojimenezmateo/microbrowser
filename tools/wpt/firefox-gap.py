#!/usr/bin/env python3
"""Compare microbrowser against Firefox one WPT test *file* at a time.

This is the successor to `firefox-ref.py`, which compared two subtest
percentages computed over two different denominators. That comparison is not
sound: our denominator is the subtests we *reported*, Firefox's is the subtests
that *exist*, and a test that dies before `done()` contributes zero subtests to
ours and its full count to Firefox's. The rate therefore rises when we fail
worse, and the "gap" column subtracts two numbers that are not on the same
footing.

The unit here is a test file, and the question is the one that has an answer:

    for each test file in our scope, does Firefox pass every subtest,
    and do we?

That is the same measurement on both sides. It is stricter than a subtest rate
by construction -- a file counts only when every subtest in it passes -- and
that is the point: it cannot be inflated by a harness that stopped early.

Three buckets come out of it:

  * `firefox passes, we fail`  -- the work. Ranked, this is the plan.
  * `both fail`               -- not our problem (yet).
  * `we pass, firefox fails`  -- audit these. Some are real; some are a test we
                                 never ran, because an expectation file records
                                 only failures and absence means PASS.

Usage:
    tools/wpt/firefox-gap.py                        # fetch, measure, write the doc
    tools/wpt/firefox-gap.py --cache /tmp/ff.json   # reuse a previous download
    tools/wpt/firefox-gap.py --json /tmp/gap.json   # per-area numbers for the ledger
    tools/wpt/firefox-gap.py --list-gap css/selectors   # the actual file names
"""

import argparse
import collections
import gzip
import json
import os
import subprocess
import sys
import urllib.request

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", ".."))

RUNS_URL = ("https://wpt.fyi/api/runs?label=master&label=experimental"
            "&product=firefox&max-count=1")


# --------------------------------------------------------------------------
# Firefox's side


def fetch_latest_firefox_run():
    with urllib.request.urlopen(RUNS_URL, timeout=30) as resp:
        runs = json.loads(resp.read())
    if not runs:
        sys.exit("error: no Firefox runs found on wpt.fyi")
    return runs[0]


def fetch_summary(results_url, cache_path=None):
    """summary_v2: { "/path/to/test.html": {"s": status, "c": [passes, total]} }.

    The status letters are testharness's own: O=OK, P=PASS, F=FAIL, E=ERROR,
    T=TIMEOUT, S=SKIP. `c` counts subtests; a test with no subtests reports
    [0, 0] and carries its verdict in `s` alone.
    """
    if cache_path and os.path.exists(cache_path):
        print(f"  using cached summary: {cache_path}", file=sys.stderr)
        with open(cache_path) as f:
            return json.load(f)

    print(f"  downloading: {results_url}", file=sys.stderr)
    with urllib.request.urlopen(results_url, timeout=240) as resp:
        raw = resp.read()
    try:
        data = json.loads(gzip.decompress(raw))
    except gzip.BadGzipFile:
        # The bucket serves it with Content-Encoding: gzip, so urllib may have
        # already decompressed it.
        data = json.loads(raw)

    if cache_path:
        os.makedirs(os.path.dirname(cache_path) or ".", exist_ok=True)
        with open(cache_path, "w") as f:
            json.dump(data, f)
        print(f"  cached to: {cache_path}", file=sys.stderr)
    return data


def firefox_passes(result):
    """True when Firefox passed the whole file, False when it did not."""
    if not isinstance(result, dict):
        return None
    counts = result.get("c") or [0, 0]
    status = result.get("s")
    if status not in ("O", "P"):
        return False
    if len(counts) >= 2 and counts[1] > 0:
        return counts[0] == counts[1]
    return status == "P"


# --------------------------------------------------------------------------
# Our side


def load_scope(binary, list_file):
    """The in-scope test list: (kind, path) pairs from `microbrowser_wpt --list`."""
    if list_file:
        text = open(list_file).read()
    else:
        if not os.path.exists(binary):
            sys.exit(f"error: {binary} not found -- build it, or pass --list-file")
        text = subprocess.run([binary, "--list"], capture_output=True, text=True,
                              check=True).stdout
    scope = []
    for line in text.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[0] in ("testharness", "reftest"):
            scope.append((parts[0], parts[1].strip()))
    return scope


def load_expectations(directory):
    """test path -> the set of failure kinds recorded for it.

    Only failures are written down, so a test absent from these files is either
    passing or was never run. `--list-gap` prints both; the audit bucket in the
    document is where that ambiguity is meant to be noticed.
    """
    failures = {}
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".txt"):
            continue
        current = None
        for line in open(os.path.join(directory, name)):
            line = line.rstrip("\n")
            if line.startswith("[") and line.endswith("]"):
                current = line[1:-1]
                failures[current] = set()
            elif current and line.startswith("harness="):
                failures[current].add(line.split("=", 1)[1])
            elif current and line and not line.startswith("#"):
                failures[current].add("subtest")
    return failures


BLOCKED = {"TIMEOUT", "ERROR", "CRASH", "PRECONDITION_FAILED", "NOTRUN"}


def why(kinds):
    """Blocked (the harness never reported) or a real feature gap."""
    return "blocked" if kinds & BLOCKED else "feature"


# --------------------------------------------------------------------------


def load_refusals(path):
    """area -> "full|partial: ADR -- what". Both kinds are kept and labelled;
    only `full` means the area should not be worked at all."""
    refusals = {}
    if not path or not os.path.exists(path):
        return refusals
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split("\t")]
        if len(parts) < 2:
            continue
        kind = parts[2] if len(parts) >= 3 else "full"
        what = parts[3] if len(parts) >= 4 else ""
        refusals[parts[0].strip("/")] = f"**{kind}** {parts[1]} -- {what}".rstrip(" -")
    return refusals


def area_of(path, depth=2):
    parts = path.split("/")
    return "/".join(parts[:depth]) if len(parts) > depth else "/".join(parts[:-1])


def measure(scope, summary, failures):
    """Classify every in-scope test file. Returns per-test records."""
    records = []
    for kind, path in scope:
        result = summary.get("/" + path)
        ff = firefox_passes(result)
        recorded = failures.get(path)
        # Absence means PASS, for a reftest exactly as for a testharness test.
        #
        # That reading is only sound because the whole reftest suite was
        # recorded in one run -- 20,998 files in 105 seconds, not the six hours
        # docs/wpt-plan.md projected (task F9, 2026-08-17). Before that run this
        # function counted every reftest as a failure, which was the honest
        # thing to do while nothing had ever written one down: a format that
        # records only failures cannot tell "passed" from "never run", and for
        # the reftest half nothing had run.
        us = recorded is None
        records.append({
            "kind": kind,
            "path": path,
            "area": area_of(path),
            "us_pass": us,
            "ff_pass": ff,
            "why": why(recorded) if recorded else "",
        })
    return records


def write_document(records, refusals, run_info, output_path):
    harness = [r for r in records if r["kind"] == "testharness"]
    reftests = [r for r in records if r["kind"] == "reftest"]

    known = [r for r in harness if r["ff_pass"] is not None]
    gap = [r for r in known if r["ff_pass"] and not r["us_pass"]]
    both = [r for r in known if not r["ff_pass"] and not r["us_pass"]]
    ours = [r for r in known if r["ff_pass"] and r["us_pass"]]
    audit = [r for r in known if not r["ff_pass"] and r["us_pass"]]

    ref_known = [r for r in reftests if r["ff_pass"] is not None]
    ref_ff = [r for r in ref_known if r["ff_pass"]]
    ref_gap = [r for r in ref_known if r["ff_pass"] and not r["us_pass"]]
    ref_ours = [r for r in ref_known if r["ff_pass"] and r["us_pass"]]
    ref_both = [r for r in ref_known if not r["ff_pass"] and not r["us_pass"]]
    ref_audit = [r for r in ref_known if not r["ff_pass"] and r["us_pass"]]

    # One row per area, counting both kinds. `reftest` is a third column beside
    # `blocked` and `feature` rather than being folded into them: a reftest has
    # no subtests and no harness status of its own, so "the harness never
    # reported" is not a distinction that exists for one, and merging it into
    # `feature` would put 13,000 pixel comparisons into a column that has meant
    # "subtests ran and failed" since the document was written.
    per_area = collections.defaultdict(
        lambda: {"scope": 0, "ff": 0, "us": 0, "blocked": 0, "feature": 0,
                 "reftest": 0, "ref_scope": 0, "ref_ff": 0, "ref_us": 0})
    for r in known:
        a = per_area[r["area"]]
        a["scope"] += 1
        if r["ff_pass"]:
            a["ff"] += 1
            if r["us_pass"]:
                a["us"] += 1
            else:
                a[r["why"] or "feature"] += 1
    for r in ref_known:
        a = per_area[r["area"]]
        a["ref_scope"] += 1
        if r["ff_pass"]:
            a["ref_ff"] += 1
            if r["us_pass"]:
                a["ref_us"] += 1
            else:
                a["reftest"] += 1

    lines = []
    out = lines.append
    out("# The Firefox gap, one test file at a time")
    out("")
    out("**Generated** by `tools/wpt/firefox-gap.py`. Do not edit by hand.")
    out("")
    out(f"Firefox version: {run_info.get('browser_version', 'unknown')}")
    out(f"Firefox run date: {run_info.get('time_start', 'unknown')[:10]}")
    out(f"Firefox's WPT revision: `{run_info.get('full_revision_hash', 'unknown')[:12]}`")
    revision_path = os.path.join(SCRIPT_DIR, "REVISION")
    if os.path.exists(revision_path):
        out(f"Our pinned WPT revision: `{open(revision_path).read().strip()[:12]}`")
    out("")
    out("The unit is a **test file**, and a file counts as passed only when every")
    out("subtest in it passed. That is the one comparison that is the same")
    out("measurement on both sides: a subtest *rate* cannot be compared across")
    out("engines, because a test that dies before `done()` contributes zero")
    out("subtests to its own denominator and its full count to Firefox's. Read")
    out("`docs/wpt-firefox-ceiling.md` with that in mind -- and read the")
    out("`blocked` column here first, because a blocked file is plumbing rather")
    out("than a specification gap.")
    out("")
    out("## Where this browser is")
    out("")
    out("| | testharness files | reftest files | total |")
    out("|---|--:|--:|--:|")
    out(f"| in our scope | {len(harness)} | {len(reftests)} | {len(records)} |")
    out(f"| Firefox passes | {len(gap) + len(ours)} | {len(ref_ff)} | "
        f"{len(gap) + len(ours) + len(ref_ff)} |")
    out(f"| **we pass** | **{len(ours)}** | **{len(ref_ours)}** | "
        f"**{len(ours) + len(ref_ours)}** |")
    out(f"| **Firefox passes, we fail** | **{len(gap)}** | **{len(ref_gap)}** | "
        f"**{len(gap) + len(ref_gap)}** |")
    out(f"| both fail | {len(both)} | {len(ref_both)} | {len(both) + len(ref_both)} |")
    out(f"| we pass, Firefox fails (audit) | {len(audit)} | {len(ref_audit)} | "
        f"{len(audit) + len(ref_audit)} |")
    out("")
    denominator = len(gap) + len(ours) + len(ref_ff)
    if denominator:
        out(f"**{100.0 * (len(ours) + len(ref_ours)) / denominator:.1f}% of what "
            f"Firefox passes.** Both halves of the suite are in that number as of")
        out("task F9 (2026-08-17). Before it, every reftest counted as a failure --")
        out("which was the honest reading while nothing had ever recorded one, "
            "because")
        out("a format that writes down only failures cannot tell a pass from a test")
        out("nobody ran.")
    out("")
    out("### Why the testharness gap fails")
    out("")
    blocked = sum(1 for r in gap if r["why"] == "blocked")
    out("| cause | files |")
    out("|---|--:|")
    out(f"| the harness never reported (TIMEOUT/ERROR/CRASH) | {blocked} |")
    out(f"| subtests ran and failed | {len(gap) - blocked} |")
    out("")
    out("A blocked file is worth more than its count: none of its subtests are")
    out("in any denominator anywhere, so it is invisible in every rate.")
    out("")
    out("### How to read the reftest column")
    out("")
    out("A reftest renders two pages and compares the pixels, so it has no")
    out("subtests, no harness status and no `blocked`/`feature` distinction: it")
    out("agrees with its reference or it does not. Two things about the number:")
    out("")
    out("- **Two blank pages agree exactly.** A reference that failed to load")
    out("  therefore passes against any test at all, and the run counts those")
    out("  separately -- `microbrowser_wpt --reftests-only` closes with")
    out("  `N reftests: M passed, K of those with both pages blank`. K was 757 of")
    out("  7,394 when this was first measured. They are not deducted, because")
    out("  wptrunner compares screenshots without asking what is on them and a")
    out("  rule of our own would make the two sides incomparable; some of them")
    out("  are real, since a reftest whose point is that nothing is visible")
    out("  passes blank in every engine.")
    out("- **About eight of 20,998 are intermittent**, measured over two full")
    out("  runs of the same binary. Seven of the eight are `@font-face` tests")
    out("  whose font this browser never loads and one is an animation; the")
    out("  runner's `--retries` smooths a disagreement but not a recording run.")
    out("")
    out("## Ranked: test files Firefox passes and we do not")
    out("")
    out("`gap` is the whole of it. `blocked` is where the harness never reported")
    out("and `feature` is where it reported and subtests failed -- both of those")
    out("are testharness columns. `reftest` is the pixel half, which has neither")
    out("distinction. A refused area is a decision with a name")
    out("(`docs/wpt-refusals.tsv`) and its row is marked.")
    out("")
    out("| area | gap | blocked | feature | reftest | we pass | firefox passes | in scope | refusal |")
    out("|---|--:|--:|--:|--:|--:|--:|--:|---|")
    ranked = sorted(per_area.items(),
                    key=lambda kv: -(kv[1]["blocked"] + kv[1]["feature"] + kv[1]["reftest"]))
    for area, a in ranked:
        total_gap = a["blocked"] + a["feature"] + a["reftest"]
        if total_gap == 0:
            continue
        # Exact matches only. A refusal on `html` is about `document.write`;
        # hanging it on `html/canvas`'s 2,654-file row would read as a claim
        # that 2,654 files are refused, which is the opposite of the truth.
        # The area-wide rows are listed in full below the table instead.
        refusal = refusals.get(area, "")
        out(f"| `{area}` | {total_gap} | {a['blocked']} | {a['feature']} | "
            f"{a['reftest']} | {a['us'] + a['ref_us']} | {a['ff'] + a['ref_ff']} | "
            f"{a['scope'] + a['ref_scope']} | {refusal} |")
    out("")
    out("### Refusals that apply across these rows")
    out("")
    out("From `docs/wpt-refusals.tsv`. Every one is *partial*: it names failures")
    out("inside an area whose other tests are ordinary bugs, so none of these is")
    out("a reason to leave an area alone.")
    out("")
    out("| area | kind | what is refused |")
    out("|---|---|---|")
    for area in sorted(refusals):
        text = refusals[area]
        kind, _, what = text.partition(" ")
        out(f"| `{area}` | {kind} | {what} |")
    out("")
    out("## Areas with no gap left")
    out("")
    clean = [(area, a) for area, a in per_area.items()
             if a["blocked"] + a["feature"] + a["reftest"] == 0 and a["ff"] + a["ref_ff"] > 0]
    if clean:
        out("| area | we pass | firefox passes |")
        out("|---|--:|--:|")
        for area, a in sorted(clean):
            out(f"| `{area}` | {a['us'] + a['ref_us']} | {a['ff'] + a['ref_ff']} |")
    else:
        out("None.")
    out("")
    out("## Audit: files we record as passing that Firefox fails")
    out("")
    out("An expectation file records only failures, so absence means PASS -- and")
    out("a test that was never run is absent too. Every file here is either a")
    out("real divergence worth a comment or a test nobody has run; there is no")
    out("third possibility, and telling them apart needs one run.")
    out("")
    audit_by_area = collections.Counter(r["area"] for r in audit + ref_audit)
    if audit_by_area:
        out("| area | files |")
        out("|---|--:|")
        for area, count in audit_by_area.most_common(25):
            out(f"| `{area}` | {count} |")
    out("")

    text = "\n".join(lines) + "\n"
    if output_path == "-":
        sys.stdout.write(text)
    else:
        with open(output_path, "w") as f:
            f.write(text)
        print(f"wrote {output_path}", file=sys.stderr)

    return per_area


def annotate_milestones(records, ledger):
    """`firefox_gap` and `order` on each milestone that owns an area.

    Hand-maintained until task F9, and that is why it had to be recomputed by
    hand every time an area moved -- which is one of the two places
    docs/wpt-plan.md §2's ordering came from. A milestone's areas are its tasks'
    areas with the redundant ones dropped: `fetch/api/` inside `fetch/` and
    `html/browsers/history/` inside `html/browsers/` are the same files twice,
    and counting them twice is how M-H would outrank layout.

    Milestones with no area at all (the instrument, the baseline, the
    performance and acceptance milestones) keep whatever `order` they were
    given: they are not ranked by files and never were.
    """
    areas = collections.defaultdict(set)
    for task in ledger.get("tasks", []):
        area = (task.get("area") or "").strip("/")
        if area:
            areas[task["milestone"]].add(area + "/")
    ranked = []
    for milestone, prefixes in areas.items():
        outer = {a for a in prefixes
                 if not any(a != b and a.startswith(b) for b in prefixes)}
        gap = blocked = feature = reftest = 0
        for r in records:
            if not any(r["path"].startswith(a) for a in outer):
                continue
            if not (r["ff_pass"] and not r["us_pass"]):
                continue
            gap += 1
            if r["kind"] == "reftest":
                reftest += 1
            elif r["why"] == "blocked":
                blocked += 1
            else:
                feature += 1
        ranked.append((gap, milestone, blocked, feature, reftest))
    ranked.sort(reverse=True)

    by_id = {m["id"]: m for m in ledger.get("milestones", [])}
    for rank, (gap, milestone, blocked, feature, reftest) in enumerate(ranked, start=1):
        entry = by_id.get(milestone)
        if entry is None:
            continue
        entry["order"] = rank
        entry["firefox_gap"] = {
            "files": gap,
            "blocked": blocked,
            "feature": feature,
            "reftest": reftest,
        }


def annotate_tasks(records, path, run_info):
    """Write `firefox_gap` into every task in the ledger that names an area.

    A task's area is its own prefix (`html/browsers/history/`), not the
    two-deep bucket the document ranks by, so the count is recomputed per task
    rather than looked up.
    """
    with open(path) as f:
        ledger = json.load(f)

    harness = [r for r in records if r["kind"] == "testharness"]
    reftests = [r for r in records if r["kind"] == "reftest"]

    annotated = 0
    for task in ledger.get("tasks", []):
        area = (task.get("area") or "").strip("/")
        if not area:
            task.pop("firefox_gap", None)
            continue
        prefix = area + "/"
        gap = [r for r in harness
               if r["path"].startswith(prefix) and r["ff_pass"] and not r["us_pass"]]
        ref_gap = [r for r in reftests
                   if r["path"].startswith(prefix) and r["ff_pass"] and not r["us_pass"]]
        ours = sum(1 for r in records
                   if r["path"].startswith(prefix) and r["ff_pass"] and r["us_pass"])
        ref = sum(1 for r in reftests if r["path"].startswith(prefix) and r["ff_pass"])
        # **`files` is both halves as of task F9 (2026-08-17), and it was the
        # testharness half alone before.** The plan ranks by this number, and
        # ranking the whole tree by a column that could not see 48% of the suite
        # is what gate 0 exists to end -- docs/wpt-plan.md §2 says so and says
        # the ordering it produced was a floor rather than a measurement. The
        # breakdown is kept beside it so a session can still ask which half its
        # area is.
        task["firefox_gap"] = {
            "files": len(gap) + len(ref_gap),
            "harness_files": len(gap),
            "blocked": sum(1 for r in gap if r["why"] == "blocked"),
            "feature": sum(1 for r in gap if r["why"] != "blocked"),
            "reftest_files": len(ref_gap),
            "we_pass": ours,
            "reftests_firefox_passes": ref,
        }
        annotated += 1

    annotate_milestones(records, ledger)

    ledger["gap_measured"] = run_info.get("time_start", "")[:10]
    ledger["gap_source"] = (f"firefox {run_info.get('browser_version', '?')} via "
                            f"tools/wpt/firefox-gap.py")
    with open(path, "w") as f:
        # ensure_ascii matches how the file is already written; turning it off
        # would rewrite every `§` in every note into `§` and bury the
        # annotation in a diff nobody reads.
        json.dump(ledger, f, indent=2)
        f.write("\n")
    print(f"annotated {annotated} tasks in {path}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary",
                        default=os.path.join(REPO_ROOT,
                                             "build/microbrowser-perf/microbrowser/microbrowser_wpt"))
    parser.add_argument("--list-file", default=None,
                        help="A saved `microbrowser_wpt --list` instead of running one")
    parser.add_argument("--expectations",
                        default=os.path.join(REPO_ROOT, "tests/wpt/expectations"))
    parser.add_argument("--refusals",
                        default=os.path.join(REPO_ROOT, "docs/wpt-refusals.tsv"))
    parser.add_argument("--output",
                        default=os.path.join(REPO_ROOT, "docs/wpt-firefox-gap.md"),
                        help="- for stdout")
    parser.add_argument("--cache", default=None)
    parser.add_argument("--json", default=None,
                        help="Write per-area counts as JSON (for docs/wpt-tasks.json)")
    parser.add_argument("--list-gap", default=None, metavar="AREA",
                        help="Print the test files Firefox passes and we fail, and stop")
    parser.add_argument("--annotate-tasks", nargs="?", const=os.path.join(
                            REPO_ROOT, "docs/wpt-tasks.json"), default=None,
                        help="Write a firefox_gap field into each task with an area")
    args = parser.parse_args()

    print("fetching Firefox's latest WPT run from wpt.fyi...", file=sys.stderr)
    run_info = fetch_latest_firefox_run()
    print(f"  Firefox {run_info['browser_version']}, "
          f"run {run_info.get('time_start', '?')[:10]}", file=sys.stderr)
    summary = fetch_summary(run_info["results_url"], args.cache)
    print(f"  {len(summary)} test results", file=sys.stderr)

    scope = load_scope(args.binary, args.list_file)
    print(f"  {len(scope)} tests in our scope", file=sys.stderr)
    failures = load_expectations(args.expectations)
    records = measure(scope, summary, failures)

    if args.list_gap:
        prefix = args.list_gap.strip("/")
        for r in records:
            if r["ff_pass"] and not r["us_pass"] and r["path"].startswith(prefix):
                # A reftest has no blocked/feature distinction, so it is tagged
                # by kind. Listing them at all is task F9: before it, half the
                # files an area owes were absent from the list a session picks
                # its work from.
                print(f"{r['why'] or r['kind']:<12}{r['path']}")
        return

    if args.annotate_tasks:
        annotate_tasks(records, args.annotate_tasks, run_info)

    per_area = write_document(records, load_refusals(args.refusals), run_info,
                              args.output)

    if args.json:
        with open(args.json, "w") as f:
            json.dump({a: dict(v) for a, v in per_area.items()}, f, indent=1,
                      sort_keys=True)
        print(f"wrote {args.json}", file=sys.stderr)


if __name__ == "__main__":
    main()
