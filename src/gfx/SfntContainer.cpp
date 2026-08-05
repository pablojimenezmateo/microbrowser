#include "gfx/SfntContainer.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "gfx/Woff2Internal.h"
#include "util/PerformanceCounters.h"
#include "util/SaturatingMath.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// The same bound the WOFF2 directory uses, and for the same reason: the field is 16
// bits, a real font has fewer than thirty tables, and every entry costs a read.
constexpr std::uint16_t kMaxTables = 512;

struct Extent {
  std::size_t offset = 0;
  std::size_t end = 0;
};

}  // namespace

bool SfntContainerIsSane(std::span<const std::byte> input) {
  woff2::Reader reader(input);
  const std::uint32_t version = reader.U32();
  const std::uint16_t table_count = reader.U16();
  reader.U16();  // searchRange
  reader.U16();  // entrySelector
  reader.U16();  // rangeShift
  if (!reader.Ok()) {
    AddPerformanceCounter(PerfCounterId::GfxSfntRefusals);
    return false;
  }
  // 0x00010000 is TrueType, `true` is the same thing as Apple spells it, `OTTO` is
  // CFF outlines. `ttcf` -- a collection -- is deliberately absent: see the header.
  if (version != 0x00010000u && version != 0x74727565u && version != 0x4F54544Fu) {
    AddPerformanceCounter(PerfCounterId::GfxSfntRefusals);
    return false;
  }
  if (table_count == 0 || table_count > kMaxTables) {
    AddPerformanceCounter(PerfCounterId::GfxSfntRefusals);
    return false;
  }
  const std::size_t directory_end =
      util::SaturatingAdd(std::size_t{12}, static_cast<std::size_t>(table_count) * 16u);
  if (directory_end > input.size()) {
    AddPerformanceCounter(PerfCounterId::GfxSfntRefusals);
    return false;
  }

  std::vector<Extent> extents;
  extents.reserve(table_count);
  for (std::uint16_t i = 0; i < table_count; ++i) {
    reader.U32();  // tag
    reader.U32();  // checksum, which nothing here verifies
    const std::size_t offset = reader.U32();
    const std::size_t length = reader.U32();
    if (!reader.Ok()) {
      AddPerformanceCounter(PerfCounterId::GfxSfntRefusals);
      return false;
    }
    // Saturating, because both numbers are the file's. A table whose end wraps would
    // otherwise compare as being inside a file it starts past the end of.
    const std::size_t end = util::SaturatingAdd(offset, length);
    if (offset < directory_end || end > input.size()) {
      AddPerformanceCounter(PerfCounterId::GfxSfntRefusals);
      return false;
    }
    // A zero-length table is legal and cannot overlap anything, so it is checked for
    // bounds and then left out of the overlap test -- where it would otherwise
    // compare equal to whatever table starts at the same offset.
    if (length > 0) {
      extents.push_back(Extent{offset, end});
    }
  }

  std::sort(extents.begin(), extents.end(),
            [](const Extent& a, const Extent& b) { return a.offset < b.offset; });
  for (std::size_t i = 1; i < extents.size(); ++i) {
    if (extents[i].offset < extents[i - 1].end) {
      AddPerformanceCounter(PerfCounterId::GfxSfntOverlapRefusals);
      return false;
    }
  }
  return true;
}

}  // namespace microbrowser::gfx
