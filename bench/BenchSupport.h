#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::bench {

// A benchmark is a name, a per-iteration body, and a unit count so the report
// can print cost per pixel or per cell rather than only per call.
struct Benchmark {
  std::string name;
  // Units of work one iteration performs (pixels, cells, spans). Zero means the
  // call itself is the unit.
  std::size_t units_per_iteration = 0;
  std::string unit_name;
  std::function<void()> run;
};

void AddBenchmark(std::vector<Benchmark>& benchmarks, std::string_view name,
                  std::size_t units_per_iteration, std::string_view unit_name,
                  std::function<void()> run);

struct Measurement {
  double milliseconds_per_iteration = 0.0;
  double nanoseconds_per_unit = 0.0;
  std::size_t iterations = 0;
};

// Times `benchmark` and reports the *fastest* run of several rounds.
//
// Minimum, not mean: on a shared machine every source of noise makes a run
// slower and none makes it faster, so the minimum is the closest thing to the
// cost of the work itself. A mean measures the machine's mood.
Measurement Measure(const Benchmark& benchmark, double target_milliseconds = 200.0);

// True when this translation unit was compiled with NDEBUG.
//
// The report refuses to print without it, and that refusal is the whole reason
// this function exists: the first measurements of the span blitter were taken
// against a build with no CMAKE_BUILD_TYPE and therefore no optimization flags,
// and were forty times too slow with the ratio off by half. Numbers from a
// debug build are not slow numbers, they are wrong ones.
bool IsOptimizedBuild();

}  // namespace microbrowser::bench
