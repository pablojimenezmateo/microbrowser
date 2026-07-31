#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "gfx/Canvas.h"

namespace microbrowser::tests {

// Pixel reference testing: render, dump, compare against a checked-in golden.
//
// This is the single most valuable kind of test a browser can have, and it is
// only possible because the rasterizer is deterministic software — the same
// input produces the same bytes on every machine, so a golden file means
// something. It is the concrete payoff of not using a GPU or a JIT rasterizer.
//
// PPM (P6) is the format because it is eight lines of code to write, needs no
// library, and diffs meaningfully as binary. Goldens live under tests/ref/.

std::string EncodePpm(const gfx::Canvas& canvas);

struct ComparisonResult {
  bool matches = false;
  std::size_t differing_pixels = 0;
  // First differing pixel, for a message that says where rather than just that.
  int first_x = -1;
  int first_y = -1;
  std::string message;
};

ComparisonResult ComparePpm(const std::string& actual, const std::string& expected);

// Compares `canvas` against the golden at tests/ref/<name>.ppm.
//
// On mismatch, writes <name>.actual.ppm beside the golden so the failure can be
// inspected rather than only described, and returns a result whose message
// names both paths. On a missing golden it writes the actual and fails: a
// golden is never created silently, because a silently-created golden records
// whatever bug was present when the test was written.
ComparisonResult CompareAgainstGolden(const gfx::Canvas& canvas, const std::string& name);

std::filesystem::path ReferenceDirectory();

}  // namespace microbrowser::tests
