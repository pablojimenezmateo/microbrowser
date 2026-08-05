#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "gfx/SfntContainer.h"

// An sfnt table directory, from a server.
//
// ADR 0024 §3 puts this check in front of FreeType for every downloaded font, which
// makes it the first thing that reads a hostile font file -- so it is fuzzed for the
// same reason the WOFF2 container is.
//
// The property asserted is the one the check exists to provide, re-derived here a
// different way: **if it says the container is sane, then every table lies inside the
// file and no two tables share a byte.** The validator sorts extents and compares
// neighbours; this walks every pair. Agreement between two formulations is the point
// -- a bug in the fast one shows up as a disagreement rather than as a crash.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace microbrowser;
  const std::span<const std::byte> file(reinterpret_cast<const std::byte*>(data), size);
  if (!gfx::SfntContainerIsSane(file)) {
    return 0;
  }
  // Deciding twice must decide the same thing: the check reads nothing but its
  // argument, and a validator with state would be one a second font could influence.
  if (!gfx::SfntContainerIsSane(file)) {
    __builtin_trap();
  }
  if (size < 12) {
    __builtin_trap();  // nothing shorter than a header can be sane
  }

  const auto u16 = [data](std::size_t at) -> std::size_t {
    return (static_cast<std::size_t>(data[at]) << 8) | data[at + 1];
  };
  const auto u32 = [&u16](std::size_t at) -> std::size_t { return (u16(at) << 16) | u16(at + 2); };
  const std::size_t count = u16(4);
  if (count == 0 || 12u + count * 16u > size) {
    __builtin_trap();
  }
  std::vector<std::pair<std::size_t, std::size_t>> extents;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t entry = 12u + i * 16u;
    const std::size_t offset = u32(entry + 8);
    const std::size_t length = u32(entry + 12);
    if (length > size || offset > size - length) {
      __builtin_trap();  // a table outside the file
    }
    if (length > 0) {
      extents.emplace_back(offset, offset + length);
    }
  }
  for (std::size_t i = 0; i < extents.size(); ++i) {
    for (std::size_t j = i + 1; j < extents.size(); ++j) {
      if (extents[i].first < extents[j].second && extents[j].first < extents[i].second) {
        __builtin_trap();  // two tables over one byte
      }
    }
  }
  return 0;
}
