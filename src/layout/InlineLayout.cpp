#include "layout/LayoutEngine.h"

#include "text/Bidi.h"
#include "text/LineBreak.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "layout/ReplacedBoxes.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::layout {

// Line layout: turning a block's inline children into lines.
//
// Split from LayoutEngine.cpp for the reason TableLayout.cpp is: block layout
// stacks boxes and line layout fills rows, and the two share nothing but the
// box tree. Keeping them in one file made the one function here the largest
// thing in the module by a wide margin, which is what the translation unit cap
// is there to catch.

namespace {

// One item on the current line: a slice of a text box, or a whole replaced
// box. Both are rectangles hung from a baseline; that is the only thing line
// layout needs to know about either.
//
// At file scope rather than inside the function because the bidi reorder below
// takes a whole line, and a type local to one function cannot appear in another
// one's signature.
struct LineItem {
  Box* box = nullptr;
  bool is_text = false;
  // An atomic inline: laid out inside like a block, so its final position is
  // not something to *write* into its geometry but something to lay it out
  // *at*. Writing the rectangle would move the box and leave every descendant
  // where the measuring pass put it -- geometry here is absolute, so a box
  // that moves takes its whole subtree with it or it takes none of it.
  bool is_atomic = false;
  std::uint32_t begin = 0;
  std::uint32_t length = 0;
  float x = 0.0f;
  float width = 0.0f;
  float above = 0.0f;  // from the baseline up
  float below = 0.0f;  // from the baseline down
  // `vertical-align`, as the one number line placement can use: how far *up* from the line's
  // baseline this item's own baseline sits. Every value of the property that can be resolved before
  // the line box exists is folded into this at push time (CSS 2.1 §10.8.1); `top` and `bottom`
  // cannot be, so they arrive as `edge` and become a shift in `finish_line` once the line's height
  // is known. Zero and `Baseline` is what every item on an ordinary line carries, and the placement
  // code below is written so that pair reproduces exactly what it did before the property existed.
  float shift = 0.0f;
  css::VerticalAlign edge = css::VerticalAlign::Baseline;
  // Which inline box asked for that edge. `top` and `bottom` align the box that carries them, not
  // each thing inside it -- so every item descended from one `<span style="vertical-align: top">`
  // is *one* thing to place, and `line`'s items are the pieces it was flattened into. Aligning them
  // individually moves a short word up to the top of a line its own tall sibling defines, which is
  // css/CSS2/linebox/anonymous-inline-inherit-001.html exactly.
  const css::ComputedStyle* group = nullptr;
  // Set by the bidi reorder, and false for every item on a line that never needed it.
  bool right_to_left = false;
};

// One entry of the flattened inline run: the box, plus the `vertical-align` that applies to it.
//
// The second half is not on the box and cannot be. Line layout here walks *through* inline boxes to
// reach the text inside them, so a `<sup>`'s own box is never an item on the line -- and a text box
// carries only the inherited properties (`TextStyleFrom`), which `vertical-align` is not. So the
// walk that flattens the run is the one place that still knows which inline box a text slice came
// out of, and it records the answer here rather than losing it.
struct RunItem {
  Box* box = nullptr;
  // The style whose `vertical-align` applies: the item's own, or -- for a text slice -- the inline
  // box it came out of. Null when the item hangs directly off the block container, whose own
  // `vertical-align` does not apply to it (CSS 2.1 §10.8: the property applies to inline-level and
  // table-cell boxes).
  const css::ComputedStyle* align = nullptr;
  // The box this item sits inside. `text-top`, `text-bottom` and `middle` are stated against the
  // *parent's* content area and x-height, so they need a second style, and it is never the item's
  // own -- `vertical-align: text-top` on a big word inside a small paragraph aligns it to the small
  // font's ascender, which is the whole visible effect of the value.
  const css::ComputedStyle* parent = nullptr;
  // How far up the *parent* box's baseline sits from the line's, from the inline boxes this item is
  // nested in. `align`'s own contribution is added when the item is placed, not here.
  float ancestor_shift = 0.0f;
  // The nearest inline box on the chain that asked to be aligned to a line-box edge, and which
  // edge. Inherited by everything inside it, because that box is what gets placed.
  const css::ComputedStyle* group = nullptr;
  css::VerticalAlign edge = css::VerticalAlign::Baseline;
};

// The part of `vertical-align` that is a shift of this box's baseline away from its parent's, up
// positive -- and zero for the four values that are stated against an edge instead, which cannot be
// answered without the box's own height.
//
// `sub` and `super` are fractions of the font size. CSS says only "the baseline of the parent's
// subscript/superscript position", which is a font metric no engine reads the same way; a third of
// the em up and a fifth down is what the shipping engines land within a pixel of at ordinary sizes,
// and -- because a reftest's reference is rendered by this same code -- a self-consistent
// approximation is worth more here than a differently-wrong exact one.
float BaselineShiftOf(const css::ComputedStyle& style, const TextMeasurer& measurer) {
  switch (style.vertical_align) {
    case css::VerticalAlign::Sub:
      return -style.font_size / 5.0f;
    case css::VerticalAlign::Super:
      return style.font_size / 3.0f;
    case css::VerticalAlign::Offset:
      // A percentage is of the used `line-height`, which is why the measurer is needed: `normal`
      // computes to zero in the cascade and means "ask the font".
      return style.vertical_align_offset.Used(measurer.LineHeight(style), style.font_size);
    default:
      return 0.0f;
  }
}

// Has an earlier item on this line already been placed as part of `group`?
//
// The group loop below walks the line once and does the work at the *first* item of each group;
// this is what makes the second and later items of one group skip it. A map would be a heap
// allocation per line for a case almost no line has, and a line is short.
bool SeenGroupBefore(const std::vector<LineItem>& line, std::size_t at,
                     const css::ComputedStyle* group) {
  for (std::size_t i = 0; i < at; ++i) {
    if (line[i].group == group) {
      return true;
    }
  }
  return false;
}

// Rewrites `line` into visual order, and re-assigns every `x` from `left`.
//
// UAX #9 L1 and L2, at the one point in layout where they apply -- see the call
// site. The line's own text is assembled from its items, the algorithm runs over
// it once, and the runs it returns are turned back into line items: a run that
// spans two boxes becomes one item per box, and an item the algorithm split at a
// direction boundary becomes two.
//
// **Nothing happens at all to a line with no right-to-left character in a
// left-to-right paragraph**, which is the case every English page is, and the
// test for it is a byte comparison before any decoding. That is deliberate:
// bidi is one of the few features that could plausibly cost every page in the
// world something, and this is where it is stopped from doing so.
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

}  // namespace

float LayoutEngine::LayoutInlineChildren(Box& box, float content_left, float content_width,
                                         float start_y, FloatContext& floats,
                                         std::optional<float> definite_content_height) const {

  std::vector<LineItem> line;
  float y = start_y;
  // The band a line may use, narrowed by any float it runs alongside. Computed
  // per line rather than once, because a float ends partway down a paragraph
  // and the lines below it get their full width back.
  float line_left = content_left;
  float line_right = content_left + content_width;
  float x = line_left;

  // Height guess for the band query. A line's real height is not known until it
  // is finished, and the band depends on the height; using the largest text
  // height in the box over-narrows nothing in the common case where every line
  // is the same height, and errs toward *more* clearance when it is wrong.
  float probe_height = 0.0f;
  {
    const auto measure = [&](const Box& node, auto& self) -> void {
      if (node.GetKind() == Box::Kind::Text) {
        probe_height = std::max(probe_height, measurer_->LineHeight(node.Style()));
      } else if (node.GetKind() == Box::Kind::Replaced) {
        probe_height = std::max(probe_height, node.Geometry().content.height);
      } else if (node.IsAtomicInline()) {
        // A declared height if there is one, and otherwise the ordinary line
        // height: the box has not been measured yet at this point, and probing
        // is allowed to be wrong in the direction of more clearance.
        probe_height = std::max(probe_height, node.Style().height.IsAuto()
                                                  ? measurer_->LineHeight(node.Style())
                                                  : node.Style().height.Resolve(
                                                        node.Style().font_size));
      }
      for (const std::unique_ptr<Box>& child : node.Children()) {
        self(*child, self);
      }
    };
    measure(box, measure);
  }

  const auto refresh_band = [&] {
    const FloatContext::Band band =
        floats.BandAt(y, probe_height, content_left, content_left + content_width);
    line_left = band.left;
    line_right = band.right;
    x = line_left;
  };
  refresh_band();

  const auto finish_line = [&] {
    if (line.empty()) {
      refresh_band();
      return;
    }
    // --- `vertical-align`, CSS 2.1 §10.8.1 ---------------------------------
    //
    // Two passes, because the property has two kinds of value and only one of them can be answered
    // before the line box exists. Every baseline-relative value has already been folded into
    // `item.shift`; what is left is `top` and `bottom`, which are stated against edges that are a
    // function of everything *else* on the line. So: size the line from the baseline-relative items
    // first, let an edge-aligned item that does not fit grow it, and then turn each edge alignment
    // into the shift that puts it where it asked to be. After that one loop places every item and
    // `shift == 0` reproduces exactly what this code did before the property existed.
    float above = 0.0f;
    float below = 0.0f;
    for (const LineItem& item : line) {
      if (item.group != nullptr) {
        continue;
      }
      above = std::max(above, item.above + item.shift);
      below = std::max(below, item.below - item.shift);
    }
    // Each edge-aligned group is measured as one box -- the extent of everything flattened out of
    // it -- and then either fits in the line the rest of the items made or grows it away from the
    // edge it is pinned to.
    for (std::size_t i = 0; i < line.size(); ++i) {
      const css::ComputedStyle* group = line[i].group;
      if (group == nullptr || (i > 0 && SeenGroupBefore(line, i, group))) {
        continue;
      }
      float group_above = 0.0f;
      float group_below = 0.0f;
      for (const LineItem& item : line) {
        if (item.group != group) {
          continue;
        }
        group_above = std::max(group_above, item.above + item.shift);
        group_below = std::max(group_below, item.below - item.shift);
      }
      const bool to_top = line[i].edge == css::VerticalAlign::Top;
      if (group_above + group_below > above + below) {
        (to_top ? below : above) = group_above + group_below - (to_top ? above : below);
      }
      // Where the group's own baseline ends up, once the line's is known: its top flush with the
      // line's top, or its bottom flush with the line's bottom.
      const float delta = to_top ? above - group_above : group_below - below;
      for (LineItem& item : line) {
        if (item.group == group) {
          item.shift += delta;
        }
      }
    }
    const float height = above + below;
    const float baseline = y + above;

    // --- Bidi: the line reordered into visual order (UAX #9 L2, ADR 0025 §3) --
    //
    // **Here, and not earlier or later.** L1 and L2 reorder per *line*, so this
    // cannot run before line breaking; and a shaped run has to be uniform in
    // direction, so it cannot run after shaping. `finish_line` is the one place
    // in this file where a line exists and nothing has been measured for paint
    // yet, which is why the whole of bidi is these forty lines and a call.
    //
    // The reorder is across the *line*, not per box: `<span>שלום</span> world`
    // is one bidi paragraph, and reordering each span separately would leave the
    // spans in logical order with their insides reversed -- which is a different
    // wrong answer from no bidi at all.
    ReorderLineForBidi(line, box.Style().direction, *measurer_, line_left);

    // Alignment is a shift of the whole finished line, applied here because
    // this is the first moment the line's used width is known. `justify` is
    // treated as `left`: spreading the gaps needs per-space adjustment inside a
    // shaped run, and a wrong stretch reads worse than no stretch.
    //
    // `start` and `end` resolve against `direction` at this point rather than in
    // the cascade, because that is what they mean: the initial value of
    // `text-align` is `start`, and in a right-to-left paragraph that is the
    // right edge. Resolving it at parse time is how an unstyled Arabic page ends
    // up hugging the left margin with correctly-reordered text on it.
    float align_offset = 0.0f;
    css::TextAlign align = box.Style().text_align;
    const bool rtl = box.Style().direction == css::Direction::Rtl;
    if (align == css::TextAlign::Start) {
      align = rtl ? css::TextAlign::Right : css::TextAlign::Left;
    } else if (align == css::TextAlign::End) {
      align = rtl ? css::TextAlign::Left : css::TextAlign::Right;
    }
    if (align == css::TextAlign::Center || align == css::TextAlign::Right) {
      const LineItem& last = line.back();
      const float slack = line_right - (last.x + last.width);
      if (slack > 0.0f) {
        align_offset = align == css::TextAlign::Center ? slack * 0.5f : slack;
      }
    }

    for (const LineItem& item : line) {
      if (item.is_text) {
        TextFragment fragment;
        fragment.begin = item.begin;
        fragment.length = item.length;
        // The whole line's height, translated by this item's shift. Not the item's own height:
        // that is what this code has always described, and a fragment rectangle is what an inline
        // box's background and `getBoundingClientRect` are answered from. `shift` is zero for every
        // item on a line nobody aligned, so this is the same rectangle it always was.
        fragment.rect = gfx::FloatRect{item.x + align_offset, y - item.shift, item.width, height};
        fragment.baseline = baseline - item.shift;
        fragment.right_to_left = item.right_to_left;
        item.box->AddFragment(fragment);
        item.box->Geometry().content = item.box->Fragments().size() == 1
                                           ? fragment.rect
                                           : item.box->Geometry().content.United(fragment.rect);
      } else if (item.is_atomic) {
        // Measured earlier against the containing block; the line only chose
        // an origin. Geometry is absolute, so translate the measured subtree
        // rather than LayoutBlock again (TD-0001 / same reason PlaceFloat
        // stopped probing twice).
        //
        // The origin handed over is the *margin box* corner, because that is
        // what LayoutBlock adds its own margins to. `item.x` was advanced by
        // the margin box width for exactly this reason.
        const gfx::FloatRect margin_box = item.box->Geometry().MarginBox();
        const float top = baseline - item.above - item.shift;
        OffsetLaidOutSubtree(*item.box, item.x + align_offset - margin_box.x,
                             top - margin_box.y);
        util::AddPerformanceCounter(util::PerfCounterId::LayoutMeasureCacheHits);
      } else {
        // A replaced element's baseline is its bottom edge, per CSS 2.1
        // §10.8.1. That is why an image on a line of text sits *on* the text
        // rather than beside it.
        item.box->Geometry().content =
            gfx::FloatRect{item.x + align_offset, baseline - item.above - item.shift, item.width,
                           item.above + item.below};
      }
    }

    y += height;
    line.clear();
    refresh_band();
  };

  // Flattened: an inline box's own children participate in the same line
  // sequence as its siblings, which is what makes `a <b>bold</b> c` one line.
  //
  // The walk carries the `vertical-align` down with it. An item's own value applies to it, except
  // for a text slice -- text has no `vertical-align` of its own (the property is not inherited and
  // a text box carries only the inherited ones), so what applies to it is the inline box it came
  // out of. Values nest by adding, which is what CSS 2.1 §10.8.1 means by "relative to the parent
  // box's baseline"; `top` and `bottom` do not nest, and the innermost one wins.
  std::vector<RunItem> run;
  // `node_align` is the style whose `vertical-align` governs `node`'s direct content, and
  // `ancestor_shift` is everything accumulated *above* it. Splitting them that way is what keeps
  // the nesting from double-counting: the item resolution below adds `node_align`'s own shift, so
  // this walk must not have added it already.
  const auto is_edge = [](const css::ComputedStyle& style) {
    return style.vertical_align == css::VerticalAlign::Top ||
           style.vertical_align == css::VerticalAlign::Bottom;
  };
  const auto collect = [&](Box& node, const css::ComputedStyle* node_align, float ancestor_shift,
                           const css::ComputedStyle* group, css::VerticalAlign edge,
                           auto& self) -> void {
    const float here = node_align == nullptr ? 0.0f : BaselineShiftOf(*node_align, *measurer_);
    for (const std::unique_ptr<Box>& child : node.Children()) {
      if (child->IsFloating()) {
        continue;  // out of flow; placed by the block pass
      }
      if (child->IsInlineLevel()) {
        // A text slice and a `<br>` have no `vertical-align` of their own -- the property is not
        // inherited, so a text box does not carry it -- and take the inline box they came out of.
        const bool takes_own =
            child->GetKind() != Box::Kind::Text && child->GetKind() != Box::Kind::LineBreak;
        run.push_back(RunItem{.box = child.get(),
                              .align = takes_own ? &child->Style() : node_align,
                              .parent = &node.Style(),
                              .ancestor_shift = takes_own ? ancestor_shift + here : ancestor_shift,
                              .group = group,
                              .edge = edge});
      } else if (is_edge(child->Style())) {
        // An edge-aligned inline box starts a group, and the shift accumulated above it stops
        // applying: what is inside it is positioned against *its* baseline, and where that baseline
        // ends up is decided against the finished line box rather than against anything here.
        self(*child, &child->Style(), 0.0f, &child->Style(), child->Style().vertical_align, self);
      } else {
        self(*child, &child->Style(), ancestor_shift + here, group, edge, self);
      }
    }
  };
  collect(box, nullptr, 0.0f, nullptr, css::VerticalAlign::Baseline, collect);

  // Resolves `vertical-align` for one item, now that its extent above and below its own baseline is
  // known -- which is what the three content-area values need, and is why this cannot happen in the
  // walk above. Leaves `shift` at the ancestors' contribution and `edge` at `Baseline` when nothing
  // on the chain asked for anything, which is every item on an ordinary page.
  const auto align_line_item = [&](const RunItem& entry, LineItem& item) {
    item.shift = entry.ancestor_shift;
    item.group = entry.group;
    item.edge = entry.edge;
    if (entry.align == nullptr) {
      return;
    }
    const css::ComputedStyle& style = *entry.align;
    const css::ComputedStyle& parent = *entry.parent;
    switch (style.vertical_align) {
      case css::VerticalAlign::Top:
      case css::VerticalAlign::Bottom:
        // Its own edge alignment beats an ancestor's, and starts a group of one.
        item.group = &style;
        item.edge = style.vertical_align;
        item.shift = 0.0f;
        break;
      case css::VerticalAlign::TextTop:
        item.shift += measurer_->Ascent(parent) - item.above;
        break;
      case css::VerticalAlign::TextBottom:
        item.shift +=
            item.below - std::max(0.0f, measurer_->LineHeight(parent) - measurer_->Ascent(parent));
        break;
      case css::VerticalAlign::Middle:
        // The item's midpoint, at half the parent's x-height above the parent's baseline. Nothing
        // in the measurer reports an x-height, so it is half an em: within a pixel of every text
        // font at ordinary sizes, and -- since a reftest's reference is drawn by this same code --
        // self-consistent, which is worth more here than a differently-wrong exact figure.
        item.shift += (item.below - item.above) * 0.5f + parent.font_size * 0.25f;
        break;
      default:
        item.shift += BaselineShiftOf(style, *measurer_);
        break;
    }
  };

  for (const RunItem& entry : run) {
    Box* item = entry.box;
    if (item->GetKind() == Box::Kind::LineBreak) {
      // A zero-width item first, so the line has this element's height even
      // when nothing else is on it -- which is what makes two `<br>`s in a row
      // produce a blank line rather than collapsing into one break.
      const css::ComputedStyle& break_style = item->Style();
      const float ascent = measurer_->Ascent(break_style);
      const float descent = std::max(0.0f, measurer_->LineHeight(break_style) - ascent);
      item->Geometry().content = gfx::FloatRect{x, y, 0.0f, ascent + descent};
      LineItem line_item{.box = item, .x = x, .above = ascent, .below = descent};
      align_line_item(entry, line_item);
      line.push_back(line_item);
      finish_line();
      continue;
    }
    if (item->GetKind() == Box::Kind::Replaced) {
      // An atomic inline: one unbreakable rectangle. It wraps to the next line
      // if it does not fit and the line already has something on it, and
      // otherwise overflows -- which is what a too-wide image does.
      //
      // Percentage width/height need the containing block (CSS 2.1 §10.3.2 /
      // §10.6.2). The box tree left them unresolved; without this, youtube's
      // `.ytCoreImageFillParentWidth/Height` stayed 0×0 (or jumped to intrinsic
      // after decode) inside a definite abspos thumbnail.
      const css::ComputedStyle& replaced_style = item->Style();
      float width = item->Geometry().content.width;
      float height = item->Geometry().content.height;
      if (replaced_style.width.IsPercent() || replaced_style.height.IsPercent()) {
        const ReplacedUsedSize used =
            ResolveReplacedSize(*item, content_width, definite_content_height);
        width = used.width;
        height = used.height;
        item->Geometry().content.width = width;
        item->Geometry().content.height = height;
      }
      if (!line.empty() && x + width > line_right) {
        finish_line();
      }
      LineItem line_item{.box = item, .x = x, .width = width, .above = height};
      align_line_item(entry, line_item);
      line.push_back(line_item);
      x += width;
      continue;
    }
    if (item->IsAtomicInline()) {
      // Measured before it can be placed, and its size depends on nothing the
      // line knows -- an inline-block is shrink-to-fit against the containing
      // block, not against what is left of the current line. Measuring against
      // the remaining space would make the same box a different width depending
      // on how much text preceded it, and then a different width again after it
      // wrapped.
      float probe = y;
      FloatContext detached;
      // A percentage height resolves against the containing block, exactly as it does for a block
      // child (CSS 2.1 §10.5) and for a replaced atomic inline just above. Without this an
      // inline-block was the only box on the page whose `height: 100%` was silently dropped --
      // which is what `<button style="height:100%">` is, now that a button lays out its own
      // children.
      ForcedSize percent_height;
      const ForcedSize* item_forced = nullptr;
      if (definite_content_height.has_value() && item->Style().height.IsPercent()) {
        percent_height.content_height =
            item->Style().height.Used(*definite_content_height, item->Style().font_size);
        item_forced = &percent_height;
      }
      LayoutBlock(*item, content_left, content_width, probe, detached, false, item_forced);
      const gfx::FloatRect margin_box = item->Geometry().MarginBox();
      const float width = margin_box.width;

      // CSS 2.1 s10.8.1: an inline-block sits on the baseline of its own last
      // line box -- which is what puts a bordered `<span>` of text level with
      // the text beside it rather than a border-width too high. Two cases fall
      // back to the bottom margin edge: no in-flow line boxes to have a
      // baseline, and `overflow` other than `visible`, because a scroller's
      // last line is not a fixed thing to align to.
      float above = margin_box.height;
      if (!item->ClipsOverflow()) {
        float last_baseline = 0.0f;
        bool any = false;
        const auto find = [&](const Box& node, auto& self) -> void {
          for (const TextFragment& fragment : node.Fragments()) {
            last_baseline = any ? std::max(last_baseline, fragment.baseline) : fragment.baseline;
            any = true;
          }
          for (const std::unique_ptr<Box>& child : node.Children()) {
            self(*child, self);
          }
        };
        find(*item, find);
        if (any) {
          above = std::max(0.0f, last_baseline - margin_box.y);
        }
      }
      const float below = std::max(0.0f, margin_box.height - above);

      if (!line.empty() && x + width > line_right) {
        finish_line();
      }
      LineItem line_item{
          .box = item, .is_atomic = true, .x = x, .width = width, .above = above, .below = below};
      align_line_item(entry, line_item);
      line.push_back(line_item);
      x += width;
      continue;
    }

    Box* text_box = item;
    const css::ComputedStyle& style = text_box->Style();
    const float ascent = measurer_->Ascent(style);
    const float descent = std::max(0.0f, measurer_->LineHeight(style) - ascent);
    // Relayout must not append to the last one's fragments. A box laid out at
    // one width and then another would otherwise paint both.
    text_box->ClearFragments();

    const std::string& text = text_box->Text();
    std::size_t offset = 0;
    while (offset < text.size()) {
      // A line never begins with a collapsible space. This is the other half of
      // CollapseWhitespace keeping leading spaces: they matter between two
      // inlines, and only here is it known whether this one landed at the start
      // of a line.
      while (offset < text.size() && text[offset] == ' ' && line.empty()) {
        ++offset;
      }
      if (offset >= text.size()) {
        break;
      }
      const std::string_view remaining(text.data() + offset, text.size() - offset);
      const float available = line_right - x;
      const float full_width = measurer_->MeasureWidth(remaining, style);

      if (full_width > available && !line.empty()) {
        // Does not fit and the line already has something on it: wrap and retry
        // against a full-width line.
        finish_line();
        continue;
      }

      std::string_view piece = remaining;
      if (full_width > available) {
        // Break at the last **break opportunity** that fits, rather than at the last space.
        //
        // ADR 0025 §4: a space is not the only place a line may break, and for CJK it is not a place
        // at all -- Japanese and Chinese have no spaces, so a space-only search found nothing and the
        // whole paragraph went on one line as wide as the text was long. `text::FindBreakOpportunities`
        // is UAX #14, and it offers a break between almost every pair of ideographs.
        //
        // When nothing fits, the whole remainder goes on this line anyway: the line is empty, and a
        // piece that never shrinks is how a line-breaking loop spins forever.
        std::size_t best = std::string_view::npos;
        for (const text::BreakOpportunity& opportunity : text::FindBreakOpportunities(remaining)) {
          if (opportunity.offset == 0 || opportunity.offset >= remaining.size()) {
            continue;
          }
          // Measured *without* the trailing space, which is what the space-only version did by
          // construction and what has to be explicit now: a break after a space puts the space at the
          // end of the line, where it takes no visible width.
          std::string_view candidate = remaining.substr(0, opportunity.offset);
          while (!candidate.empty() && candidate.back() == ' ') {
            candidate.remove_suffix(1);
          }
          if (measurer_->MeasureWidth(candidate, style) <= available) {
            best = opportunity.offset;
          } else {
            break;
          }
        }
        if (best != std::string_view::npos && best > 0) {
          piece = remaining.substr(0, best);
        }
      }

      const float advance = measurer_->MeasureWidth(piece, style);
      LineItem line_item{.box = text_box,
                         .is_text = true,
                         .begin = static_cast<std::uint32_t>(offset),
                         .length = static_cast<std::uint32_t>(piece.size()),
                         .x = x,
                         .width = advance,
                         .above = ascent,
                         .below = descent};
      align_line_item(entry, line_item);
      line.push_back(line_item);
      x += advance;

      offset += piece.size();
      while (offset < text.size() && text[offset] == ' ') {
        ++offset;
      }
      if (offset < text.size()) {
        finish_line();
      }
    }
  }

  finish_line();
  return y - start_y;
}

}  // namespace microbrowser::layout
