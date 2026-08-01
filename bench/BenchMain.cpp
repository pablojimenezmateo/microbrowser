#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "BenchSupport.h"
#include "gfx/Blitter.h"

namespace microbrowser::bench {
void RegisterGfxBenchmarks(std::vector<Benchmark>& benchmarks);
}  // namespace microbrowser::bench

namespace {

using microbrowser::bench::Benchmark;
using microbrowser::bench::Measure;
using microbrowser::bench::Measurement;

std::vector<Benchmark> CollectBenchmarks() {
  std::vector<Benchmark> benchmarks;
  microbrowser::bench::RegisterGfxBenchmarks(benchmarks);
  return benchmarks;
}

bool Matches(const std::string& name, const std::vector<std::string>& filters) {
  if (filters.empty()) {
    return true;
  }
  for (const std::string& filter : filters) {
    if (name.find(filter) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> filters;
  bool allow_debug = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--allow-debug-build") {
      allow_debug = true;
    } else if (argument.starts_with("--")) {
      std::fprintf(stderr, "usage: microbrowser_bench [--allow-debug-build] [name filter...]\n");
      return 2;
    } else {
      filters.emplace_back(argument);
    }
  }

  // A refusal rather than a warning. Numbers from a build with no optimization
  // flags are not slow numbers, they are wrong ones: the first measurements of
  // the span blitter came from such a build and were forty times too slow with
  // the ratio between the two implementations off by half. A warning printed
  // above a table gets pasted into a document without it.
  if (!microbrowser::bench::IsOptimizedBuild() && !allow_debug) {
    std::fprintf(stderr,
                 "refusing to report timings from a build without NDEBUG.\n"
                 "  cmake --preset microbrowser-perf && cmake --build --preset microbrowser-perf\n"
                 "  ./build/microbrowser-perf/microbrowser/microbrowser_bench\n"
                 "Pass --allow-debug-build if you genuinely want the wrong numbers.\n");
    return 2;
  }

  std::printf("span blitter: %s\n\n",
              microbrowser::gfx::BlendSpanIsVectorized() ? "vectorized" : "scalar");
  std::printf("%-45s %12s %14s %10s\n", "benchmark", "ms/iter", "per unit", "iters");
  std::printf("%s\n", std::string(84, '-').c_str());

  for (const Benchmark& benchmark : CollectBenchmarks()) {
    if (!Matches(benchmark.name, filters)) {
      continue;
    }
    const Measurement measurement = Measure(benchmark);
    char per_unit[32] = "";
    if (benchmark.units_per_iteration > 0) {
      std::snprintf(per_unit, sizeof(per_unit), "%.3f ns/%s", measurement.nanoseconds_per_unit,
                    benchmark.unit_name.c_str());
    }
    std::printf("%-45s %12.4f %14s %10zu\n", benchmark.name.c_str(),
                measurement.milliseconds_per_iteration, per_unit, measurement.iterations);
  }
  return 0;
}
