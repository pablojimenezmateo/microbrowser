#!/usr/bin/env python3
"""Compare microbrowser's WPT baseline against Firefox's latest results from wpt.fyi.

Downloads Firefox's latest WPT run summary from the wpt.fyi API, aggregates
pass/total counts by the areas in directories.txt, and produces a comparison
table against docs/wpt-baseline.md.

Usage:
    tools/wpt/firefox-ref.py
    tools/wpt/firefox-ref.py --baseline docs/wpt-baseline.md --output docs/wpt-firefox-ceiling.md
    tools/wpt/firefox-ref.py --cache /tmp/firefox-wpt-summary.json  # reuse a previous download
    tools/wpt/firefox-ref.py --refusals docs/wpt-refusals.tsv       # load refusal mapping

The output is a regenerable markdown table committed beside the baseline.
"""

import argparse
import gzip
import json
import os
import re
import sys
import urllib.request

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", ".."))


def fetch_latest_firefox_run():
    """Fetch metadata for the latest Firefox experimental run from wpt.fyi."""
    url = "https://wpt.fyi/api/runs?label=master&label=experimental&product=firefox&max-count=1"
    with urllib.request.urlopen(url, timeout=30) as resp:
        runs = json.loads(resp.read())
    if not runs:
        print("error: no Firefox runs found on wpt.fyi", file=sys.stderr)
        sys.exit(1)
    return runs[0]


def fetch_summary(results_url, cache_path=None):
    """Download and decompress a summary_v2 JSON from wpt.fyi (or read from cache)."""
    if cache_path and os.path.exists(cache_path):
        print(f"  using cached summary: {cache_path}", file=sys.stderr)
        with open(cache_path) as f:
            return json.load(f)

    print(f"  downloading: {results_url}", file=sys.stderr)
    req = urllib.request.Request(results_url)
    with urllib.request.urlopen(req, timeout=120) as resp:
        raw = resp.read()

    # The summary is gzip-compressed
    try:
        decompressed = gzip.decompress(raw)
        data = json.loads(decompressed)
    except gzip.BadGzipFile:
        data = json.loads(raw)

    if cache_path:
        os.makedirs(os.path.dirname(cache_path) or ".", exist_ok=True)
        with open(cache_path, "w") as f:
            json.dump(data, f)
        print(f"  cached to: {cache_path}", file=sys.stderr)

    return data


def aggregate_by_area(summary, areas):
    """Aggregate per-test results into per-area pass/total counts.

    summary_v2 format: { "/path/to/test.html": {"s": "O", "c": [passes, total]}, ... }
    "s" is status: "O" = OK, "P" = PASS, "F" = FAIL, "E" = ERROR, "T" = TIMEOUT, etc.
    "c" is [subtest_passes, subtest_total].
    A test with status PASS/OK and no subtests contributes 1/1.
    """
    area_stats = {}
    for area in areas:
        area_stats[area] = {"passed": 0, "total": 0, "tests": 0}

    for test_path, result in summary.items():
        # Find which area this test belongs to
        matched_area = None
        best_len = 0
        for area in areas:
            prefix = "/" + area
            if test_path.startswith(prefix) and len(prefix) > best_len:
                matched_area = area
                best_len = len(prefix)

        if matched_area is None:
            continue

        stats = area_stats[matched_area]
        stats["tests"] += 1

        if isinstance(result, dict):
            # v2 format
            counts = result.get("c", [0, 0])
            if len(counts) >= 2:
                stats["passed"] += counts[0]
                stats["total"] += counts[1]
            elif result.get("s") in ("P", "O"):
                stats["passed"] += 1
                stats["total"] += 1
            else:
                stats["total"] += 1
        elif isinstance(result, list) and len(result) == 2:
            # v1 format fallback
            stats["passed"] += result[0]
            stats["total"] += result[1]

    return area_stats


def parse_baseline(path):
    """Parse docs/wpt-baseline.md to extract per-area pass rates."""
    areas = {}
    if not os.path.exists(path):
        print(f"warning: baseline not found at {path}", file=sys.stderr)
        return areas

    with open(path) as f:
        in_table = False
        for line in f:
            line = line.strip()
            # Header: | area | tests | ok | error | timeout | crash | subtests | passed | % |
            if line.startswith("| area"):
                in_table = True
                continue
            if in_table and line.startswith("|---"):
                continue
            if in_table and line.startswith("|"):
                cols = [c.strip() for c in line.split("|")]
                # cols[0] is empty (before first |), cols[1]=area, cols[2]=tests,
                # cols[3]=ok, cols[4]=error, cols[5]=timeout, cols[6]=crash,
                # cols[7]=subtests, cols[8]=passed, cols[9]=%
                if len(cols) >= 10:
                    area = cols[1].strip("`").rstrip("/")
                    try:
                        subtests = int(cols[7])
                        passed = int(cols[8])
                        pct = float(cols[9])
                    except (ValueError, IndexError):
                        continue
                    areas[area] = {
                        "passed": passed,
                        "total": subtests,
                        "pct": pct,
                    }
            elif in_table and not line.startswith("|"):
                in_table = False

    return areas


def load_refusals(path):
    """Load the refusal mapping: area -> ADR reason.

    Format: tab-separated, area<TAB>ADR-NNNN<TAB>reason
    Lines starting with # are comments.
    """
    refusals = {}
    if not path or not os.path.exists(path):
        return refusals
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) >= 2:
                refusals[parts[0]] = parts[1] if len(parts) >= 2 else "refused"
    return refusals


def load_areas(directories_path):
    """Extract area prefixes from directories.txt."""
    areas = []
    with open(directories_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            # Strip leading/trailing slashes
            area = line.strip("/")
            # Skip resource/support directories that aren't test areas
            if area in ("resources", "interfaces", "common", "fonts", "images",
                        "media", "css/support", "css/reference"):
                continue
            areas.append(area)
    return areas


def write_ceiling_table(our_areas, firefox_areas, refusals, run_info, output_path):
    """Write the comparison table."""
    # Collect all areas from both sources, sorted
    all_areas = sorted(set(list(our_areas.keys()) + list(firefox_areas.keys())))

    lines = []
    lines.append("# WPT Firefox Ceiling")
    lines.append("")
    lines.append("**Generated** by `tools/wpt/firefox-ref.py`. Do not edit by hand.")
    lines.append("")
    lines.append(f"Firefox version: {run_info.get('browser_version', 'unknown')}")
    lines.append(f"Firefox run date: {run_info.get('time_start', 'unknown')[:10]}")
    lines.append(f"WPT revision: `{run_info.get('full_revision_hash', 'unknown')[:12]}`")
    lines.append("")
    lines.append("The ceiling for each area is Firefox's pass rate on the same tests,")
    lines.append("or `refused` when an ADR says this browser will not implement the area.")
    lines.append("A gap between microbrowser and the ceiling is a bug; a test Firefox")
    lines.append("also fails is not our problem (yet); a test we refuse is a decision")
    lines.append("with a name.")
    lines.append("")
    lines.append("| area | us % | us pass/total | firefox % | firefox pass/total | ceiling | gap | note |")
    lines.append("|---|--:|---|--:|---|---|---|---|")

    total_us_passed = 0
    total_us_total = 0
    total_ff_passed = 0
    total_ff_total = 0

    for area in all_areas:
        ours = our_areas.get(area)
        ff = firefox_areas.get(area)
        refusal = refusals.get(area)

        us_pct = ""
        us_counts = ""
        ff_pct = ""
        ff_counts = ""
        ceiling = ""
        gap = ""
        note = ""

        if ours and ours["total"] > 0:
            us_pct_val = 100.0 * ours["passed"] / ours["total"]
            us_pct = f"{us_pct_val:.1f}"
            us_counts = f"{ours['passed']}/{ours['total']}"
            total_us_passed += ours["passed"]
            total_us_total += ours["total"]

        if ff and ff["total"] > 0:
            ff_pct_val = 100.0 * ff["passed"] / ff["total"]
            ff_pct = f"{ff_pct_val:.1f}"
            ff_counts = f"{ff['passed']}/{ff['total']}"
            total_ff_passed += ff["passed"]
            total_ff_total += ff["total"]

        if refusal:
            ceiling = "refused"
            gap = "-"
            note = refusal
        elif ff and ff["total"] > 0:
            ff_pct_val = 100.0 * ff["passed"] / ff["total"]
            ceiling = f"{ff_pct_val:.1f}"
            if ours and ours["total"] > 0:
                us_pct_val = 100.0 * ours["passed"] / ours["total"]
                gap_val = ff_pct_val - us_pct_val
                if gap_val <= 0.5:
                    gap = "done"
                else:
                    gap = f"{gap_val:.1f}"
        elif ours:
            ceiling = "no firefox data"
            gap = "-"

        lines.append(f"| `{area}` | {us_pct} | {us_counts} | {ff_pct} | {ff_counts} | {ceiling} | {gap} | {note} |")

    lines.append("")
    if total_us_total > 0 and total_ff_total > 0:
        lines.append(f"Aggregate (do not quote): us {total_us_passed}/{total_us_total} "
                      f"({100.0*total_us_passed/total_us_total:.1f}%), "
                      f"Firefox {total_ff_passed}/{total_ff_total} "
                      f"({100.0*total_ff_passed/total_ff_total:.1f}%)")
    lines.append("")

    text = "\n".join(lines) + "\n"

    if output_path == "-":
        sys.stdout.write(text)
    else:
        with open(output_path, "w") as f:
            f.write(text)
        print(f"wrote {output_path} ({len(all_areas)} areas)", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline", default=os.path.join(REPO_ROOT, "docs/wpt-baseline.md"),
                        help="Path to docs/wpt-baseline.md")
    parser.add_argument("--output", default=os.path.join(REPO_ROOT, "docs/wpt-firefox-ceiling.md"),
                        help="Output path (- for stdout)")
    parser.add_argument("--cache", default=None,
                        help="Cache the downloaded summary JSON to this path")
    parser.add_argument("--refusals", default=os.path.join(REPO_ROOT, "docs/wpt-refusals.tsv"),
                        help="Tab-separated refusal mapping: area<TAB>ADR reason")
    parser.add_argument("--directories", default=os.path.join(SCRIPT_DIR, "directories.txt"),
                        help="Path to directories.txt")
    args = parser.parse_args()

    print("fetching Firefox's latest WPT run from wpt.fyi...", file=sys.stderr)
    run_info = fetch_latest_firefox_run()
    print(f"  Firefox {run_info['browser_version']}, "
          f"run {run_info.get('time_start', '?')[:10]}", file=sys.stderr)

    summary = fetch_summary(run_info["results_url"], args.cache)
    print(f"  {len(summary)} test results in summary", file=sys.stderr)

    areas = load_areas(args.directories)
    print(f"  {len(areas)} areas from directories.txt", file=sys.stderr)

    firefox_areas = aggregate_by_area(summary, areas)
    our_areas = parse_baseline(args.baseline)

    # Our baseline uses deeper paths (e.g. "dom/nodes") while Firefox summary
    # has full test paths. Aggregate Firefox at the same granularity as our baseline.
    # First, find all area prefixes our baseline uses.
    baseline_prefixes = set(our_areas.keys())
    # Add the directories.txt areas too
    for a in areas:
        baseline_prefixes.add(a)

    # Re-aggregate Firefox at the baseline's granularity
    ff_fine = {}
    for test_path, result in summary.items():
        matched = None
        best_len = 0
        for prefix in sorted(baseline_prefixes, key=len, reverse=True):
            check = "/" + prefix + "/"
            if test_path.startswith(check) and len(check) > best_len:
                matched = prefix
                best_len = len(check)
        if matched is None:
            for prefix in sorted(baseline_prefixes, key=len, reverse=True):
                check = "/" + prefix
                if test_path.startswith(check) and len(check) > best_len:
                    matched = prefix
                    best_len = len(check)
        if matched is None:
            continue
        if matched not in ff_fine:
            ff_fine[matched] = {"passed": 0, "total": 0, "tests": 0}
        stats = ff_fine[matched]
        stats["tests"] += 1
        if isinstance(result, dict):
            counts = result.get("c", [0, 0])
            if len(counts) >= 2:
                stats["passed"] += counts[0]
                stats["total"] += counts[1]
            elif result.get("s") in ("P", "O"):
                stats["passed"] += 1
                stats["total"] += 1
            else:
                stats["total"] += 1
        elif isinstance(result, list) and len(result) == 2:
            stats["passed"] += result[0]
            stats["total"] += result[1]

    refusals = load_refusals(args.refusals)

    write_ceiling_table(our_areas, ff_fine, refusals, run_info, args.output)


if __name__ == "__main__":
    main()
