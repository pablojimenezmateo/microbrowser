#!/usr/bin/env bash
#
# Build + test wrapper that tees everything to a deterministic log under /tmp,
# so a result can be read back without rerunning the (slow) sanitizer builds.
#
#   tools/run-checks.sh tests   -> /tmp/microbrowser-tests.log
#   tools/run-checks.sh asan    -> /tmp/microbrowser-asan.log
#   tools/run-checks.sh ubsan   -> /tmp/microbrowser-ubsan.log
#   tools/run-checks.sh tsan    -> /tmp/microbrowser-tsan.log
#   tools/run-checks.sh all     -> all of the above, in sequence
#   tools/run-checks.sh wpt     -> /tmp/microbrowser-wpt.log
#
# After a run, READ the log instead of rebuilding.
#
# `wpt` is web-platform-tests against the committed expectations (ADR 0040). It
# needs the checkout that tools/wpt/fetch.sh makes and says so rather than
# failing when it is absent.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# ccache cannot cache a compile whose command line embeds __TIME__ or a PCH
# without being told those are acceptable to ignore. Exporting this here means
# callers do not each have to remember it.
export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-pch_defines,time_macros}"

JOBS="${MICROBROWSER_JOBS:-$(nproc)}"
CTEST_JOBS="${MICROBROWSER_CTEST_JOBS:-$JOBS}"
# The sanitizers multiply peak RSS by several times; running every core in
# parallel is how a 24-core machine starts swapping.
CTEST_SAN_JOBS="${MICROBROWSER_CTEST_SAN_JOBS:-6}"

# ThreadSanitizer maps its shadow memory at fixed addresses and aborts with
# "unexpected memory mapping" when the kernel's ASLR entropy is higher than it
# expects — the default on recent Ubuntu. `setarch -R` clears ASLR for this
# process tree only, so no sudo and no machine-wide sysctl change is needed.
# (The machine-wide fallback, if personality() is ever blocked, is
# `sudo sysctl vm.mmap_rnd_bits=28`.)
ctest_launcher() {
  if [[ "$1" == "tsan" ]] && command -v setarch >/dev/null 2>&1; then
    echo "setarch -R"
  fi
}

run_target() {
  local name="$1" preset="$2" ctest_jobs="$3"
  local log="/tmp/microbrowser-${name}.log"
  local launcher
  read -r -a launcher <<< "$(ctest_launcher "$name")"

  {
    echo "=== microbrowser: ${name} ==="
    echo "=== preset: ${preset} ==="
    echo "=== started: $(date -Is) ==="
    echo

    cmake --preset "$preset" \
      && cmake --build --preset "$preset" -j "$JOBS" \
      && "${launcher[@]}" ctest --preset "$preset" -j "$ctest_jobs"
    local status=$?

    echo
    echo "=== finished: $(date -Is) ==="
    echo "=== exit status: ${status} ==="
    exit $status
  } 2>&1 | tee "$log"

  local status="${PIPESTATUS[0]}"
  if [[ "$status" -eq 0 ]]; then
    echo "OK   ${name}  (log: ${log})"
  else
    echo "FAIL ${name}  (log: ${log})"
  fi
  return "$status"
}

# web-platform-tests. Its own function rather than a preset, because it is not a
# build configuration: it is the debug build plus a checkout that may not exist.
run_wpt() {
  local log="/tmp/microbrowser-wpt.log"
  shift || true
  {
    echo "=== microbrowser: wpt ==="
    echo "=== started: $(date -Is) ==="
    echo
    if [[ ! -d third_party/wpt ]]; then
      echo "third_party/wpt is absent. Run tools/wpt/fetch.sh to check it out."
      echo "=== exit status: 0 (skipped) ==="
      exit 0
    fi
    cmake -S . -B build -G Ninja \
      && cmake --build build --target microbrowser_wpt -j "$JOBS" \
      && ./build/microbrowser/microbrowser_wpt --jobs "$JOBS" "$@"
    local status=$?
    echo
    echo "=== finished: $(date -Is) ==="
    echo "=== exit status: ${status} ==="
    exit $status
  } 2>&1 | tee "$log"
  local status="${PIPESTATUS[0]}"
  if [[ "$status" -eq 0 ]]; then
    echo "OK   wpt  (log: ${log})"
  else
    echo "FAIL wpt  (log: ${log})"
  fi
  return "$status"
}

target="${1:-tests}"
overall=0

case "$target" in
  tests) run_target tests microbrowser-debug "$CTEST_JOBS" || overall=1 ;;
  asan)  run_target asan  microbrowser-asan  "$CTEST_SAN_JOBS" || overall=1 ;;
  ubsan) run_target ubsan microbrowser-ubsan "$CTEST_SAN_JOBS" || overall=1 ;;
  tsan)  run_target tsan  microbrowser-tsan  "$CTEST_SAN_JOBS" || overall=1 ;;
  wpt)   run_wpt "$@" || overall=1 ;;
  all)
    run_target tests microbrowser-debug "$CTEST_JOBS"     || overall=1
    run_target asan  microbrowser-asan  "$CTEST_SAN_JOBS" || overall=1
    run_target ubsan microbrowser-ubsan "$CTEST_SAN_JOBS" || overall=1
    run_target tsan  microbrowser-tsan  "$CTEST_SAN_JOBS" || overall=1
    ;;
  *)
    echo "usage: tools/run-checks.sh [tests|asan|ubsan|tsan|wpt|all]" >&2
    exit 2
    ;;
esac

exit "$overall"
