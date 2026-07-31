#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::tests::architecture {

// A per-class growth budget. Raising one requires editing the manifest, which
// puts the growth in the diff where a reviewer sees it — the mechanism, not the
// number, is the point.
struct ClassBudget {
  std::string name;
  std::size_t header_lines = 0;
  std::size_t public_methods = 0;
  std::size_t members = 0;
};

// Parsed src/<module>/MODULE.deps.
//
// Borrowed from three places that each solved one part of this:
//   * Chromium's DEPS include_rules — `allow`, checked by a presubmit lint.
//   * Gecko's moz.build EXPORTS — `public`, the module's advertised surface;
//     anything unlisted is module-private and unreachable from outside.
//   * Ladybird's per-library CMake targets — mirrored by the link edges in
//     CMakeLists.txt, so the build graph tells the same story.
// The budgets are this project's own addition, and the direct answer to
// "how do classes not grow unbounded".
struct ModuleManifest {
  std::string name;              // directory name under src/
  std::string purpose;           // one line; required and non-empty
  std::vector<std::string> allow;    // module names this one may include from
  std::vector<std::string> publics;  // header filenames other modules may include
  std::vector<std::string> externs;  // third-party groups this module may include
  std::size_t max_tu_lines = 0;
  std::vector<ClassBudget> budgets;

  bool Allows(std::string_view module) const;
  bool Exports(std::string_view header) const;
  bool AllowsExtern(std::string_view group) const;
  const ClassBudget* FindBudget(std::string_view class_name) const;
};

struct ManifestParseResult {
  ModuleManifest manifest;
  std::vector<std::string> errors;
};

// Parses manifest text. Never throws: a malformed manifest yields errors, which
// the lint reports like any other violation.
ManifestParseResult ParseModuleManifest(std::string_view module_name, std::string_view text);

using ModuleManifests = std::map<std::string, ModuleManifest, std::less<>>;

}  // namespace microbrowser::tests::architecture
