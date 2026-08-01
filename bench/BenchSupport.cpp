#include "BenchSupport.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace microbrowser::bench {

namespace {

using Clock = std::chrono::steady_clock;

double ElapsedMilliseconds(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

}  // namespace

void AddBenchmark(std::vector<Benchmark>& benchmarks, std::string_view name,
                  std::size_t units_per_iteration, std::string_view unit_name,
                  std::function<void()> run) {
  benchmarks.push_back(Benchmark{std::string(name), units_per_iteration, std::string(unit_name),
                                 std::move(run)});
}

Measurement Measure(const Benchmark& benchmark, double target_milliseconds) {
  // One untimed iteration first: the first call faults in the arenas the
  // rasterizer is designed to reuse, and timing it would measure the allocator.
  benchmark.run();

  // Calibrate the iteration count from a short pilot so a fast benchmark is not
  // dominated by clock overhead and a slow one does not run for a minute.
  const Clock::time_point pilot_start = Clock::now();
  benchmark.run();
  const double pilot = std::max(ElapsedMilliseconds(pilot_start, Clock::now()), 1e-6);

  const std::size_t rounds = 5;
  const auto iterations = static_cast<std::size_t>(
      std::clamp(target_milliseconds / (pilot * static_cast<double>(rounds)), 1.0, 1e7));

  double best = 0.0;
  for (std::size_t round = 0; round < rounds; ++round) {
    const Clock::time_point start = Clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
      benchmark.run();
    }
    const double per_iteration =
        ElapsedMilliseconds(start, Clock::now()) / static_cast<double>(iterations);
    if (round == 0 || per_iteration < best) {
      best = per_iteration;
    }
  }

  Measurement measurement;
  measurement.iterations = iterations;
  measurement.milliseconds_per_iteration = best;
  if (benchmark.units_per_iteration > 0) {
    measurement.nanoseconds_per_unit =
        best * 1e6 / static_cast<double>(benchmark.units_per_iteration);
  }
  return measurement;
}

bool IsOptimizedBuild() {
#if defined(NDEBUG)
  return true;
#else
  return false;
#endif
}

}  // namespace microbrowser::bench
