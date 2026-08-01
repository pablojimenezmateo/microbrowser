#include "util/PerformanceCounters.h"

#include <algorithm>
#include <array>
#include <atomic>

#include "util/Env.h"

namespace microbrowser::util {
namespace {

using CounterArray = std::array<std::atomic<std::uint64_t>, kPerfCounterCount>;

CounterArray& Counters() {
  static CounterArray counters;
  return counters;
}

// Positionally aligned with PerfCounterId by construction: both are expanded
// from the same MICROBROWSER_PERF_COUNTERS list, so an id cannot exist without its
// name or drift out of position.
constexpr std::array<std::string_view, kPerfCounterCount> kCounterNames = {
#define MICROBROWSER_PERF_COUNTER_NAME(id, name) std::string_view(name),
    MICROBROWSER_PERF_COUNTERS(MICROBROWSER_PERF_COUNTER_NAME)
#undef MICROBROWSER_PERF_COUNTER_NAME
};

std::size_t ToIndex(PerfCounterId id) {
  return std::min(static_cast<std::size_t>(id), kPerfCounterCount - 1);
}

bool& DumpedFlag() {
  static bool dumped = false;
  return dumped;
}

}  // namespace

void ResetPerformanceCounters() {
  for (std::atomic<std::uint64_t>& counter : Counters()) {
    counter.store(0, std::memory_order_relaxed);
  }
}

void AddPerformanceCounter(PerfCounterId id, std::uint64_t delta) {
  Counters()[ToIndex(id)].fetch_add(delta, std::memory_order_relaxed);
}

std::uint64_t ReadPerformanceCounter(PerfCounterId id) {
  return Counters()[ToIndex(id)].load(std::memory_order_relaxed);
}

PerfCounterSnapshot CapturePerformanceCounters() {
  PerfCounterSnapshot snapshot{};
  for (std::size_t i = 0; i < kPerfCounterCount; ++i) {
    snapshot[i] = Counters()[i].load(std::memory_order_relaxed);
  }
  return snapshot;
}

std::vector<std::pair<std::string_view, std::uint64_t>> NonZeroCounterDelta(
    const PerfCounterSnapshot& before,
    const PerfCounterSnapshot& after) {
  std::vector<std::pair<std::string_view, std::uint64_t>> deltas;
  deltas.reserve(kPerfCounterCount);
  for (std::size_t i = 0; i < kPerfCounterCount; ++i) {
    const std::uint64_t prior = before[i];
    const std::uint64_t next = after[i];
    if (next <= prior) {
      continue;
    }
    const std::uint64_t delta = next - prior;
    if (delta == 0) {
      continue;
    }
    deltas.emplace_back(kCounterNames[i], delta);
  }
  return deltas;
}

std::string_view PerformanceCounterName(PerfCounterId id) {
  return kCounterNames[ToIndex(id)];
}

void WritePerformanceCounters(std::FILE* out) {
  if (out == nullptr) {
    return;
  }
  const PerfCounterSnapshot snapshot = CapturePerformanceCounters();
  std::vector<std::pair<std::string_view, std::uint64_t>> rows;
  rows.reserve(kPerfCounterCount);
  for (std::size_t i = 0; i < kPerfCounterCount; ++i) {
    if (snapshot[i] != 0) {
      rows.emplace_back(kCounterNames[i], snapshot[i]);
    }
  }
  std::sort(rows.begin(), rows.end());

  if (rows.empty()) {
    std::fprintf(out, "[counters] no non-zero counters\n");
    std::fflush(out);
    return;
  }
  std::fprintf(out, "[counters] %zu of %zu counters non-zero\n", rows.size(), kPerfCounterCount);
  for (const auto& [name, value] : rows) {
    std::fprintf(out, "[counters] %18llu  %.*s\n", static_cast<unsigned long long>(value),
                 static_cast<int>(name.size()), name.data());
  }
  std::fflush(out);
}

bool PerformanceCounterDumpRequested() {
  // Cached: this is checked on every dump path, and the env cannot change under
  // a running process in any way we are willing to observe.
  static const bool requested = EnvFlagEnabled("MICROBROWSER_PERF_COUNTERS");
  return requested;
}

void DumpPerformanceCountersOnce() {
  if (!PerformanceCounterDumpRequested() || DumpedFlag()) {
    return;
  }
  DumpedFlag() = true;
  WritePerformanceCounters(stderr);
}

}  // namespace microbrowser::util
