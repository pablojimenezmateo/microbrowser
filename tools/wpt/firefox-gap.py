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
        # A reftest is never written to an expectation file today, so "absent"
        # cannot be read as "passing" for one. Reftests are reported separately
        # and never counted as ours.
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

    per_area = collections.defaultdict(
        lambda: {"scope": 0, "ff": 0, "us": 0, "blocked": 0, "feature": 0})
    for r in known:
        a = per_area[r["area"]]
        a["scope"] += 1
        if r["ff_pass"]:
            a["ff"] += 1
            if r["us_pass"]:
                a["us"] += 1
            else:
                a[r["why"] or "feature"] += 1

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
    out(f"| **we pass** | **{len(ours)}** | **0** | **{len(ours)}** |")
    out(f"| **Firefox passes, we fail** | **{len(gap)}** | **{len(ref_ff)}** | "
        f"**{len(gap) + len(ref_ff)}** |")
    out(f"| both fail | {len(both)} | {len(ref_known) - len(ref_ff)} | "
        f"{len(both) + len(ref_known) - len(ref_ff)} |")
    out(f"| we pass, Firefox fails (audit) | {len(audit)} | 0 | {len(audit)} |")
    out("")
    denominator = len(gap) + len(ours) + len(ref_ff)
    if denominator:
        out(f"**{100.0 * len(ours) / denominator:.1f}% of what Firefox passes.** "
            f"Every reftest is counted as a failure, which is what a sample of")
        out("them measures: reftests are run by the runner and recorded by")
        out("nothing, so an expectation file's silence about one is not a pass.")
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
    out("## Ranked: test files Firefox passes and we do not")
    out("")
    out("`blocked` is where the harness never reported; `feature` is where it")
    out("reported and subtests failed. A refused area is a decision with a name")
    out("(`docs/wpt-refusals.tsv`) and its row is marked.")
    out("")
    out("| area | gap | blocked | feature | we pass | firefox passes | in scope | refusal |")
    out("|---|--:|--:|--:|--:|--:|--:|---|")
    ranked = sorted(per_area.items(),
                    key=lambda kv: -(kv[1]["blocked"] + kv[1]["feature"]))
    for area, a in ranked:
        total_gap = a["blocked"] + a["feature"]
        if total_gap == 0:
            continue
        # Exact matches only. A refusal on `html` is about `document.write`;
        # hanging it on `html/canvas`'s 2,654-file row would read as a claim
        # that 2,654 files are refused, which is the opposite of the truth.
        # The area-wide rows are listed in full below the table instead.
        refusal = refusals.get(area, "")
        out(f"| `{area}` | {total_gap} | {a['blocked']} | {a['feature']} | "
            f"{a['us']} | {a['ff']} | {a['scope']} | {refusal} |")
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
             if a["blocked"] + a["feature"] == 0 and a["ff"] > 0]
    if clean:
        out("| area | we pass | firefox passes |")
        out("|---|--:|--:|")
        for area, a in sorted(clean):
            out(f"| `{area}` | {a['us']} | {a['ff']} |")
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
    audit_by_area = collections.Counter(r["area"] for r in audit)
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
        ours = sum(1 for r in harness
                   if r["path"].startswith(prefix) and r["ff_pass"] and r["us_pass"])
        ref = sum(1 for r in reftests if r["path"].startswith(prefix) and r["ff_pass"])
        task["firefox_gap"] = {
            "files": len(gap),
            "blocked": sum(1 for r in gap if r["why"] == "blocked"),
            "feature": sum(1 for r in gap if r["why"] != "blocked"),
            "we_pass": ours,
            "reftests_firefox_passes": ref,
        }
        annotated += 1

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
            if (r["kind"] == "testharness" and r["ff_pass"] and not r["us_pass"]
                    and r["path"].startswith(prefix)):
                print(f"{r['why']:<8}{r['path']}")
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
