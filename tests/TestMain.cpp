#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "util/Parse.h"

// Each suite exposes one registration function. Explicit registration rather
// than static-initializer self-registration: the order is deterministic, the
// link cannot silently drop a suite whose only reference is a global
// constructor, and finding every test is a grep for this list.
namespace microbrowser::tests {
void RegisterAffineTransformTests(std::vector<TestCase>& tests);
void RegisterAppDirectoriesTests(std::vector<TestCase>& tests);
void RegisterArchitectureInvariantsTests(std::vector<TestCase>& tests);
void RegisterBlitterTests(std::vector<TestCase>& tests);
void RegisterCanvasTests(std::vector<TestCase>& tests);
void RegisterCssTests(std::vector<TestCase>& tests);
void RegisterDirtyRegionPolicyTests(std::vector<TestCase>& tests);
void RegisterDirtyRegionTests(std::vector<TestCase>& tests);
void RegisterDisplayListTests(std::vector<TestCase>& tests);
void RegisterEnvTests(std::vector<TestCase>& tests);
void RegisterFetchTests(std::vector<TestCase>& tests);
void RegisterBrowserChromeTests(std::vector<TestCase>& tests);
void RegisterDisplayListDiffTests(std::vector<TestCase>& tests);
void RegisterEngineTests(std::vector<TestCase>& tests);
void RegisterFloatTests(std::vector<TestCase>& tests);
void RegisterFontCatalogTests(std::vector<TestCase>& tests);
void RegisterFontTests(std::vector<TestCase>& tests);
void RegisterGeometryTests(std::vector<TestCase>& tests);
void RegisterGlyphCacheTests(std::vector<TestCase>& tests);
void RegisterIdleWaitStrategyTests(std::vector<TestCase>& tests);
void RegisterInflateTests(std::vector<TestCase>& tests);
void RegisterIpcMessageTests(std::vector<TestCase>& tests);
void RegisterLayoutTests(std::vector<TestCase>& tests);
void RegisterNetTests(std::vector<TestCase>& tests);
void RegisterPaintPipelineTests(std::vector<TestCase>& tests);
void RegisterPainterTests(std::vector<TestCase>& tests);
void RegisterPngDecoderTests(std::vector<TestCase>& tests);
void RegisterPrivacyTests(std::vector<TestCase>& tests);
void RegisterPathTests(std::vector<TestCase>& tests);
void RegisterRasterizerTests(std::vector<TestCase>& tests);
void RegisterStrokerTests(std::vector<TestCase>& tests);
void RegisterStyleResolverTests(std::vector<TestCase>& tests);
void RegisterTextShaperTests(std::vector<TestCase>& tests);
void RegisterTokenizerTests(std::vector<TestCase>& tests);
void RegisterTreeBuilderTests(std::vector<TestCase>& tests);
void RegisterUrlTests(std::vector<TestCase>& tests);
void RegisterReferenceImageTests(std::vector<TestCase>& tests);
}  // namespace microbrowser::tests

namespace {

using microbrowser::tests::TestCase;

struct RunnerOptions {
  std::size_t shard_index = 0;
  std::size_t shard_count = 1;
  std::vector<std::string> filters;
  bool list_only = false;
  bool invalid = false;
  std::string error;
};

bool TakeValue(std::string_view argument, std::string_view flag, std::string_view& out_value) {
  if (argument.size() <= flag.size() || argument.compare(0, flag.size(), flag) != 0) {
    return false;
  }
  out_value = argument.substr(flag.size());
  return true;
}

RunnerOptions ParseOptions(int argc, char** argv) {
  RunnerOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    std::string_view value;

    if (TakeValue(argument, "--shard-index=", value)) {
      const auto parsed = microbrowser::util::ParseSize(value);
      if (!parsed.has_value()) {
        options.invalid = true;
        options.error = "invalid --shard-index";
        return options;
      }
      options.shard_index = *parsed;
    } else if (TakeValue(argument, "--shard-count=", value)) {
      const auto parsed = microbrowser::util::ParseSize(value);
      if (!parsed.has_value() || *parsed == 0) {
        options.invalid = true;
        options.error = "invalid --shard-count";
        return options;
      }
      options.shard_count = *parsed;
    } else if (argument == "--list") {
      options.list_only = true;
    } else if (argument.starts_with("--")) {
      options.invalid = true;
      options.error = "unknown option: " + std::string(argument);
      return options;
    } else {
      options.filters.emplace_back(argument);
    }
  }

  if (options.shard_index >= options.shard_count) {
    options.invalid = true;
    options.error = "--shard-index must be less than --shard-count";
  }
  return options;
}

bool MatchesAnyFilter(const std::string& name, const std::vector<std::string>& filters) {
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

std::vector<TestCase> CollectTests() {
  std::vector<TestCase> tests;
  microbrowser::tests::RegisterAffineTransformTests(tests);
  microbrowser::tests::RegisterAppDirectoriesTests(tests);
  microbrowser::tests::RegisterArchitectureInvariantsTests(tests);
  microbrowser::tests::RegisterBlitterTests(tests);
  microbrowser::tests::RegisterCanvasTests(tests);
  microbrowser::tests::RegisterCssTests(tests);
  microbrowser::tests::RegisterDirtyRegionPolicyTests(tests);
  microbrowser::tests::RegisterDirtyRegionTests(tests);
  microbrowser::tests::RegisterDisplayListTests(tests);
  microbrowser::tests::RegisterEnvTests(tests);
  microbrowser::tests::RegisterFetchTests(tests);
  microbrowser::tests::RegisterBrowserChromeTests(tests);
  microbrowser::tests::RegisterDisplayListDiffTests(tests);
  microbrowser::tests::RegisterEngineTests(tests);
  microbrowser::tests::RegisterFloatTests(tests);
  microbrowser::tests::RegisterFontCatalogTests(tests);
  microbrowser::tests::RegisterFontTests(tests);
  microbrowser::tests::RegisterGeometryTests(tests);
  microbrowser::tests::RegisterGlyphCacheTests(tests);
  microbrowser::tests::RegisterIdleWaitStrategyTests(tests);
  microbrowser::tests::RegisterInflateTests(tests);
  microbrowser::tests::RegisterIpcMessageTests(tests);
  microbrowser::tests::RegisterLayoutTests(tests);
  microbrowser::tests::RegisterNetTests(tests);
  microbrowser::tests::RegisterPaintPipelineTests(tests);
  microbrowser::tests::RegisterPainterTests(tests);
  microbrowser::tests::RegisterPngDecoderTests(tests);
  microbrowser::tests::RegisterPrivacyTests(tests);
  microbrowser::tests::RegisterPathTests(tests);
  microbrowser::tests::RegisterRasterizerTests(tests);
  microbrowser::tests::RegisterStrokerTests(tests);
  microbrowser::tests::RegisterStyleResolverTests(tests);
  microbrowser::tests::RegisterTextShaperTests(tests);
  microbrowser::tests::RegisterTokenizerTests(tests);
  microbrowser::tests::RegisterTreeBuilderTests(tests);
  microbrowser::tests::RegisterUrlTests(tests);
  microbrowser::tests::RegisterReferenceImageTests(tests);
  return tests;
}

}  // namespace

int main(int argc, char** argv) {
  const RunnerOptions options = ParseOptions(argc, argv);
  if (options.invalid) {
    std::fprintf(stderr, "%s\n", options.error.c_str());
    return 2;
  }

  const std::vector<TestCase> tests = CollectTests();

  int failures = 0;
  int ran = 0;
  // Round-robin, not contiguous blocks: shards stay balanced even when a suite
  // registered late is far slower than the rest.
  std::size_t position = 0;
  for (const TestCase& test : tests) {
    if (!MatchesAnyFilter(test.name, options.filters)) {
      continue;
    }
    const bool mine = (position % options.shard_count) == options.shard_index;
    ++position;
    if (!mine) {
      continue;
    }

    if (options.list_only) {
      std::printf("%s\n", test.name.c_str());
      continue;
    }

    ++ran;
    try {
      test.run();
    } catch (const std::exception& error) {
      ++failures;
      std::fprintf(stderr, "FAIL %s: %s\n", test.name.c_str(), error.what());
    } catch (...) {
      ++failures;
      std::fprintf(stderr, "FAIL %s: unknown exception\n", test.name.c_str());
    }
  }

  if (options.list_only) {
    return 0;
  }

  // A shard that matched nothing is almost always a typo in a filter or a
  // registration that was never added to CollectTests. Failing loudly beats
  // reporting success for zero work.
  if (ran == 0 && !options.filters.empty()) {
    std::fprintf(stderr, "no tests matched the given filters\n");
    return 3;
  }

  std::printf("%d test(s) run, %d failed\n", ran, failures);
  return failures == 0 ? 0 : 1;
}
