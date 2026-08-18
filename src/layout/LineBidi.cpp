#include "layout/LineItem.h"

#include "layout/LayoutEngine.h"

#include "text/Bidi.h"

#include "util/StringUtil.h"

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "util/PerformanceCounters.h"

namespace microbrowser::layout {

void ReorderLineForBidi(std::vector<LineItem>& line, css::Direction direction,
                        const TextMeasurer& measurer, float left) {
  const bool rtl_paragraph = direction == css::Direction::Rtl;
  bool interesting = rtl_paragraph;
  for (const LineItem& item : line) {
    if (interesting || !item.is_text || item.box == nullptr) {
      continue;
    }
    // **A box that asks for a `unicode-bidi` other than `normal` is interesting whatever its text
    // says.** Its controls are synthetic -- this function inserts them -- so no byte in the document
    // announces them, and the byte scan below cannot see them. `<bdo dir=rtl>abcdef</bdo>` is exactly
    // that case: all-ASCII text that must come out reversed, and it did not until this test existed.
    // Same for a box whose own `direction` disagrees with the paragraph's.
    const css::ComputedStyle& style = item.box->Style();
    if (style.unicode_bidi != css::UnicodeBidi::Normal ||
        (style.direction == css::Direction::Rtl) != rtl_paragraph) {
      interesting = true;
      continue;
    }
    interesting =
        text::NeedsBidi(std::string_view(item.box->Text()).substr(item.begin, item.length));
  }
  if (!interesting || line.size() > 4096) {
    // The bound is a bound on *work*, not on correctness: a line of ten thousand
    // items is a pathological document, and reordering it is quadratic in the
    // grouping step below. Left in logical order, which is what it would have
    // been without this function at all.
    return;
  }

  // The line as code points, and where each one came from. An atomic inline or a
  // replaced box contributes U+FFFC -- OBJECT REPLACEMENT CHARACTER -- which is
  // what the specification says an inline object is for bidi purposes, and it
  // matters: an image between two Hebrew words must not break the run.
  struct Slot {
    std::size_t item = 0;
    std::uint32_t begin = 0;
    std::uint32_t length = 0;
  };
  // A control character has no bytes behind it, so its slot names no item. `kVirtual` is how the
  // grouping pass below knows to skip it: an explicit control is *input* to the algorithm and never
  // output, because there is nothing on the page to paint for it.
  constexpr std::size_t kVirtual = static_cast<std::size_t>(-1);
  std::vector<std::uint32_t> code_points;
  std::vector<Slot> slots;
  // `unicode-bidi`, as the pair of explicit controls it is defined to be. UAX #9 already implements
  // every one of them, so the property costs this function two pushes and nothing else.
  const auto controls_for = [](const css::ComputedStyle& style) {
    struct Pair {
      std::uint32_t open = 0;
      std::uint32_t open2 = 0;
      std::uint32_t close = 0;
      std::uint32_t close2 = 0;
    };
    const bool rtl = style.direction == css::Direction::Rtl;
    switch (style.unicode_bidi) {
      case css::UnicodeBidi::Embed:
        return Pair{rtl ? 0x202Bu : 0x202Au, 0, 0x202Cu, 0};
      case css::UnicodeBidi::BidiOverride:
        return Pair{rtl ? 0x202Eu : 0x202Du, 0, 0x202Cu, 0};
      case css::UnicodeBidi::Isolate:
        return Pair{rtl ? 0x2067u : 0x2066u, 0, 0x2069u, 0};
      case css::UnicodeBidi::IsolateOverride:
        // Both, in that order, and closed in the reverse order -- an isolate around an override.
        return Pair{rtl ? 0x2067u : 0x2066u, rtl ? 0x202Eu : 0x202Du, 0x202Cu, 0x2069u};
      case css::UnicodeBidi::Plaintext:
        // A first-strong isolate is exactly "work the direction out from the contents", which is what
        // `plaintext` means -- so it is one control rather than a second copy of P2.
        return Pair{0x2068u, 0, 0x2069u, 0};
      case css::UnicodeBidi::Normal:
        break;
    }
    return Pair{};
  };
  for (std::size_t i = 0; i < line.size(); ++i) {
    const LineItem& item = line[i];
    if (!item.is_text || item.box == nullptr) {
      code_points.push_back(0xFFFC);
      slots.push_back({i, item.begin, item.length});
      continue;
    }
    const auto controls = controls_for(item.box->Style());
    for (const std::uint32_t control : {controls.open, controls.open2}) {
      if (control != 0) {
        code_points.push_back(control);
        slots.push_back({kVirtual, 0, 0});
      }
    }
    const std::string_view text =
        std::string_view(item.box->Text()).substr(item.begin, item.length);
    std::size_t at = 0;
    while (at < text.size()) {
      const std::size_t start = at;
      std::uint32_t code = 0;
      if (!util::DecodeUtf8(text, at, code)) {
        break;
      }
      code_points.push_back(code);
      slots.push_back({i, static_cast<std::uint32_t>(item.begin + start),
                       static_cast<std::uint32_t>(at - start)});
    }
    for (const std::uint32_t control : {controls.close, controls.close2}) {
      if (control != 0) {
        code_points.push_back(control);
        slots.push_back({kVirtual, 0, 0});
      }
    }
  }
  if (code_points.empty()) {
    return;
  }

  const std::vector<text::BidiRun> runs =
      text::ResolveVisualRuns(code_points, rtl_paragraph ? 1 : 0);
  std::vector<LineItem> reordered;
  reordered.reserve(line.size());
  for (const text::BidiRun& run : runs) {
    // Within a run, consecutive code points from the same item become one item.
    // The run is a slice of *logical* text either way -- that is what a shaper
    // needs -- so the only thing its direction changes is the order the groups
    // are laid down in.
    std::vector<LineItem> groups;
    for (std::size_t k = 0; k < run.length; ++k) {
      const Slot& slot = slots[run.start + k];
      if (slot.item == kVirtual) {
        continue;  // a control this function inserted: input to the algorithm, never output
      }
      if (!groups.empty() && groups.back().box == line[slot.item].box &&
          line[slot.item].is_text &&
          groups.back().begin + groups.back().length == slot.begin) {
        groups.back().length += slot.length;
        continue;
      }
      LineItem group = line[slot.item];
      group.begin = slot.begin;
      group.length = slot.length;
      group.right_to_left = run.right_to_left;
      groups.push_back(group);
    }
    if (run.right_to_left) {
      std::reverse(groups.begin(), groups.end());
    }
    for (LineItem& group : groups) {
      // Re-measured, because a slice of a run is not a fixed fraction of its
      // width: shaping "fi" and shaping "f" then "i" give different answers, and
      // a width carried over from the unsplit item would leave a gap or an
      // overlap exactly at the direction boundary.
      if (group.is_text && group.box != nullptr) {
        const std::string_view slice =
            std::string_view(group.box->Text()).substr(group.begin, group.length);
        // Measured *mirrored* when the run is right-to-left, for the same reason paint mirrors it:
        // measuring one string and drawing another is how a line ends up a pixel short. In practice
        // a mirrored pair has the same advance, which is exactly why a mismatch here would go
        // unnoticed until some font where it does not.
        const std::string mirrored =
            group.right_to_left ? text::MirrorForRightToLeft(slice) : std::string();
        group.width = measurer.MeasureWidth(
            group.right_to_left ? std::string_view(mirrored) : slice, group.box->Style(),
            group.right_to_left);
      }
      reordered.push_back(group);
    }
    util::AddPerformanceCounter(util::PerfCounterId::TextBidiRuns);
  }
  float x = left;
  for (LineItem& item : reordered) {
    item.x = x;
    x += item.width;
  }
  line = std::move(reordered);
}

}  // namespace microbrowser::layout
