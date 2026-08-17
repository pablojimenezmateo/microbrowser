#!/usr/bin/env bash
#
# One full testharness baseline, sharded, resumable, into the committed state
# file at tests/wpt/summary-state.tsv (docs/wpt-plan.md task B6).
#
#   tools/wpt/baseline.sh                      # run every shard that is not done
#   tools/wpt/baseline.sh --fresh              # start the state and the ledger over
#   tools/wpt/baseline.sh --state /tmp/s.tsv   # somewhere else
#
# **Why a script and not one invocation.** `microbrowser_wpt` writes its
# `--summary-state` when the run *finishes*, so a single command over all 23,146
# testharness files is a five-hour bet that nothing interrupts it -- and the
# whole reason task B6 sat open for a week is that nobody wanted to take that
# bet. Sharded, each shard's counts are durable the moment it finishes and the
# next invocation skips it: the run is interruptible, and picking it up costs
# whatever the shard that was in flight cost.
#
# **The shards partition the suite exactly**, and the script refuses to run
# unless they do. An overlap would measure an area twice (harmless but slow); a
# gap would leave the state file quietly incomplete, which is the failure this
# task exists to end. A top-level directory is one shard unless it is large
# *and* has no test files loose at its top level, in which case it splits into
# its immediate children -- `html/` and `css/` are 13,473 of the 23,146 files
# between them and neither has a loose one.
#
# **Cost is timeouts, not CPU.** 6,934 of the files are expected to TIMEOUT and
# 2,807 carry a `timeout=long` meta, which is a 65-second budget each; the
# measured figure on a 24-core machine is ~2.75 seconds of wall clock per
# expected timeout at the default `--jobs`. That is where the hours go, and it
# is why `--jobs` stays at the default here: raising it is measured to delete
# subtests from CPU-bound areas (see the comment on `jobs` in tools/wpt/main.cpp).
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BIN="./build/microbrowser-perf/microbrowser/microbrowser_wpt"
STATE="tests/wpt/summary-state.tsv"
WORK="/tmp/microbrowser-wpt-baseline"
FRESH=0
# A directory bigger than this splits into its children, so that no single shard
# is a long bet on nothing going wrong.
SPLIT_ABOVE=600

while [[ $# -gt 0 ]]; do
  case "$1" in
    --state) STATE="$2"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    --fresh) FRESH=1; shift ;;
    -h|--help)
      sed -n '3,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ ! -x "$BIN" ]]; then
  echo "$BIN is not built. cmake --build --preset microbrowser-perf --target microbrowser_wpt" >&2
  exit 2
fi
if [[ ! -d third_party/wpt ]]; then
  echo "third_party/wpt is absent. Run tools/wpt/fetch.sh." >&2
  exit 2
fi

mkdir -p "$WORK/shards"
TESTS="$WORK/testharness.txt"
SHARDS="$WORK/shards.txt"
DONE="$WORK/done.txt"
LOG="$WORK/baseline.log"

if [[ "$FRESH" == 1 ]]; then
  rm -f "$STATE" "$DONE" "$LOG"
fi
touch "$DONE" "$LOG"

"$BIN" --list 2>/dev/null | awk '$1 == "testharness" { print $2 }' > "$TESTS"
if [[ ! -s "$TESTS" ]]; then
  echo "--list produced no testharness tests" >&2
  exit 2
fi

# The shard list. A top-level directory splits only when it is large and has no
# loose files at its top level, because a loose file cannot be named by a
# two-segment prefix and would fall through the split.
awk -F/ -v split_above="$SPLIT_ABOVE" '
  { total[$1]++; if (NF == 2) loose[$1]++ }
  { if (NF >= 3) child[$1 "/" $2] = 1 }
  END {
    for (dir in total) {
      if (total[dir] > split_above && !(dir in loose)) {
        for (c in child) {
          split(c, parts, "/")
          if (parts[1] == dir) print c "/"
        }
      } else {
        print dir "/"
      }
    }
  }
' "$TESTS" | sort > "$SHARDS"

# Refuse to run on anything but an exact partition. A gap here is a state file
# that looks complete and is not, which is the whole failure mode B6 closes.
awk '
  NR == FNR { shard[FNR] = $0; shards = FNR; next }
  {
    matched = 0
    for (i = 1; i <= shards; i++) {
      if (index($0, shard[i]) == 1) { matched++ }
    }
    if (matched != 1) { printf "%s matched %d shards\n", $0, matched; bad++ }
  }
  END { if (bad) { printf "%d tests are not covered by exactly one shard\n", bad; exit 1 } }
' "$SHARDS" "$TESTS" || exit 2

total_shards=$(grep -c . "$SHARDS")
echo "$total_shards shards over $(grep -c . "$TESTS") testharness tests; state: $STATE"

n=0
while read -r shard; do
  [[ -z "$shard" ]] && continue
  n=$((n + 1))
  if grep -qxF "$shard" "$DONE"; then
    continue
  fi
  slug="${shard//\//_}"
  start=$(date +%s)
  # Each shard's whole output goes to its own file. stdout is block-buffered
  # when it is not a terminal, so a shard with failures flushes its failure
  # report *after* the stderr trailer -- reading the tail of a merged stream
  # loses the counts for exactly the shards that had something to say.
  "$BIN" --testharness-only --summary-state "$STATE" "$shard" \
      > "$WORK/shards/$slug.log" 2>&1
  status=$?
  finish=$(date +%s)
  {
    printf '=== [%d/%d] %s (%ds, exit %d)\n' \
        "$n" "$total_shards" "$shard" "$((finish - start))" "$status"
    grep -aE '^[0-9]+ tests in |unexpected results' "$WORK/shards/$slug.log" | tail -2
  } | tee -a "$LOG"
  if [[ "$status" -gt 1 ]]; then
    # Exit 1 is "unexpected results", which is the normal state of this suite.
    # Anything above it is the runner failing, and continuing would write a
    # state file with a hole in it.
    echo "shard $shard failed with exit $status; stopping. Log: $WORK/shards/$slug.log" >&2
    exit "$status"
  fi
  echo "$shard" >> "$DONE"
done < "$SHARDS"

echo "=== every shard done. Areas in $STATE: $(grep -c '^A' "$STATE")" | tee -a "$LOG"
echo "Now write the document:"
echo "  $BIN --testharness-only --summary docs/wpt-baseline.md --summary-state $STATE console/"
