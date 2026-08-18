#include "layout/LayoutEngine.h"

#include "text/Bidi.h"
#include "text/LineBreak.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "layout/ReplacedBoxes.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::layout {

namespace {

// The last UTF-8 character boundary in `text` whose prefix still fits in `available`.
//
// This is what `word-break: break-all` and `overflow-wrap: break-word`/`anywhere` need and what
// UAX #14 deliberately does not offer: a break between two letters of one word. Zero means "not
// even one character fits", and the caller must then place at least one anyway -- a piece that
// never shrinks is how a line-breaking loop spins forever.
std::size_t LastCharacterBoundaryThatFits(std::string_view text, const TextMeasurer& measurer,
                                          const css::ComputedStyle& style, float available) {
  std::size_t fits = 0;
  std::size_t at = 0;
  while (at < text.size()) {
    ++at;
    while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
      ++at;
    }
    if (measurer.MeasureWidth(text.substr(0, at), style) > available) {
      break;
    }
    fits = at;
  }
  return fits;
}

}  // namespace


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
  // Set by the bidi reorder, and false for every item on a line that never needed it.
  bool right_to_left = false;
};

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

  // `text-indent`: the first line starts this far in -- or, with `hanging`, every line *but* the
  // first does, which is what the keyword means. A percentage is of the containing block's width,
  // which is `content_width` here, the one place it is known.
  const float declared_indent =
      box.Style().text_indent.length.Used(content_width, box.Style().font_size);
  const bool hanging_indent = box.Style().text_indent.hanging;
  bool first_line = true;

  const auto refresh_band = [&] {
    const FloatContext::Band band =
        floats.BandAt(y, probe_height, content_left, content_left + content_width);
    line_left = band.left;
    line_right = band.right;
    x = line_left + (first_line != hanging_indent ? declared_indent : 0.0f);
  };
  refresh_band();

  const auto finish_line = [&] {
    if (line.empty()) {
      refresh_band();
      return;
    }
    float above = 0.0f;
    float below = 0.0f;
    for (const LineItem& item : line) {
      above = std::max(above, item.above);
      below = std::max(below, item.below);
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
        fragment.rect = gfx::FloatRect{item.x + align_offset, y, item.width, height};
        fragment.baseline = baseline;
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
        const float top = baseline - item.above;
        OffsetLaidOutSubtree(*item.box, item.x + align_offset - margin_box.x,
                             top - margin_box.y);
        util::AddPerformanceCounter(util::PerfCounterId::LayoutMeasureCacheHits);
      } else {
        // A replaced element's baseline is its bottom edge, per CSS 2.1
        // §10.8.1. That is why an image on a line of text sits *on* the text
        // rather than beside it.
        item.box->Geometry().content = gfx::FloatRect{item.x + align_offset, baseline - item.above,
                                                      item.width, item.above + item.below};
      }
    }

    y += height;
    line.clear();
    first_line = false;
    refresh_band();
  };

  // Flattened: an inline box's own children participate in the same line
  // sequence as its siblings, which is what makes `a <b>bold</b> c` one line.
  std::vector<Box*> run;
  const auto collect = [&run](Box& node, auto& self) -> void {
    for (const std::unique_ptr<Box>& child : node.Children()) {
      if (child->IsFloating()) {
        continue;  // out of flow; placed by the block pass
      }
      if (child->IsInlineLevel()) {
        run.push_back(child.get());
      } else {
        self(*child, self);
      }
    }
  };
  collect(box, collect);

  for (Box* item : run) {
    if (item->GetKind() == Box::Kind::LineBreak) {
      // A zero-width item first, so the line has this element's height even
      // when nothing else is on it -- which is what makes two `<br>`s in a row
      // produce a blank line rather than collapsing into one break.
      const css::ComputedStyle& break_style = item->Style();
      const float ascent = measurer_->Ascent(break_style);
      const float descent = std::max(0.0f, measurer_->LineHeight(break_style) - ascent);
      item->Geometry().content = gfx::FloatRect{x, y, 0.0f, ascent + descent};
      line.push_back(LineItem{.box = item, .x = x, .above = ascent, .below = descent});
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
      line.push_back(LineItem{.box = item, .x = x, .width = width, .above = height});
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
      line.push_back(LineItem{
          .box = item, .is_atomic = true, .x = x, .width = width, .above = above, .below = below});
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

    // What whitespace processing left in the string, and what layout is still allowed to do with
    // it. A newline in the text is a *segment break* the collapsing pass decided to keep, and it is
    // a forced line break here -- nothing in this loop used to look at one, so `<pre>a\nb</pre>`
    // put both lines on one. `nowrap` is the other half: a line that may not wrap must never take
    // the break opportunity below, however far it overflows.
    const bool preserve_spaces = css::PreservesSpaces(style.white_space_collapse);
    const bool preserve_breaks = css::PreservesNewlines(style.white_space_collapse);
    const bool may_wrap = style.text_wrap_mode == css::TextWrapMode::Wrap;

    const std::string& text = text_box->Text();
    std::size_t offset = 0;
    while (offset < text.size()) {
      // A line never begins with a collapsible space. This is the other half of
      // CollapseWhitespace keeping leading spaces: they matter between two
      // inlines, and only here is it known whether this one landed at the start
      // of a line. A *preserved* space is not collapsible and stays.
      while (!preserve_spaces && offset < text.size() && text[offset] == ' ' && line.empty()) {
        ++offset;
      }
      if (offset >= text.size()) {
        break;
      }
      // The segment this line may hold at most: everything up to the next preserved break.
      const std::size_t segment_end =
          preserve_breaks ? text.find('\n', offset) : std::string::npos;
      if (segment_end == offset) {
        // An empty line. It still has this box's height, which is what makes a blank line in a
        // `<pre>` occupy one.
        const float ascent_here = measurer_->Ascent(style);
        const float descent_here = std::max(0.0f, measurer_->LineHeight(style) - ascent_here);
        line.push_back(LineItem{
            .box = text_box, .x = x, .above = ascent_here, .below = descent_here});
        finish_line();
        offset = segment_end + 1;
        continue;
      }
      // A preserved tab advances to the next tab stop rather than drawing a glyph, so it is its
      // own fragment with a width the font never had an opinion about. `tab-size` is the distance
      // between stops -- a length, or a multiple of the space advance -- measured from the line's
      // start edge, which is the only place a stop can be counted from.
      const std::size_t tab_at = preserve_spaces ? text.find('\t', offset) : std::string::npos;
      if (tab_at == offset) {
        const css::TabSize tab = style.tab_size;
        const float stride = tab.is_length ? tab.value
                                           : tab.value * measurer_->MeasureWidth(" ", style);
        float advance_to = x;
        if (stride > 0.0f) {
          const float from_start = x - line_left;
          advance_to = line_left + (std::floor(from_start / stride) + 1.0f) * stride;
        }
        const float tab_width = std::max(0.0f, advance_to - x);
        if (may_wrap && advance_to > line_right && !line.empty()) {
          finish_line();
          continue;
        }
        const float tab_ascent = measurer_->Ascent(style);
        const float tab_descent = std::max(0.0f, measurer_->LineHeight(style) - tab_ascent);
        line.push_back(LineItem{.box = text_box,
                                .is_text = true,
                                .begin = static_cast<std::uint32_t>(offset),
                                .length = 1,
                                .x = x,
                                .width = tab_width,
                                .above = tab_ascent,
                                .below = tab_descent});
        x += tab_width;
        ++offset;
        continue;
      }
      // The run this line may take at once ends at the next preserved break or the next tab,
      // whichever comes first.
      const std::size_t chunk_end = std::min(segment_end, tab_at);
      const std::string_view remaining(
          text.data() + offset,
          (chunk_end == std::string::npos ? text.size() : chunk_end) - offset);
      const float available = line_right - x;
      const float full_width = measurer_->MeasureWidth(remaining, style);

      // `word-break: break-all` and `overflow-wrap: anywhere` may break between any two
      // characters, so a word that does not fit is *cut* on this line rather than moved to the
      // next. `overflow-wrap: break-word` is the other rule and the difference matters: it moves
      // the word first and only cuts it if it still does not fit, which is why it is not here.
      const bool cuts_on_this_line = style.word_break == css::WordBreak::BreakAll ||
                                     style.overflow_wrap == css::OverflowWrap::Anywhere;
      if (may_wrap && full_width > available && !line.empty() && !cuts_on_this_line) {
        // Does not fit and the line already has something on it: wrap and retry
        // against a full-width line.
        finish_line();
        continue;
      }

      std::string_view piece = remaining;
      if (may_wrap && full_width > available) {
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
          // `word-break: keep-all` forbids the implicit opportunities between typographic letter
          // units -- which for CJK is nearly all of them -- and leaves the ones a space provides.
          if (style.word_break == css::WordBreak::KeepAll &&
              remaining[opportunity.offset - 1] != ' ') {
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
        const std::size_t from_opportunity =
            best != std::string_view::npos && best > 0 ? best : 0;
        // `word-break: break-all` may break between any two characters, and `overflow-wrap` may
        // when nothing else worked -- which is exactly the case UAX #14 leaves overflowing. The
        // difference between the two is *when*: break-all competes with the opportunity search,
        // overflow-wrap only rescues it.
        const bool break_between_characters =
            cuts_on_this_line || (style.overflow_wrap == css::OverflowWrap::BreakWord &&
                                  from_opportunity == 0);
        std::size_t cut = from_opportunity;
        if (break_between_characters) {
          const std::size_t chars =
              LastCharacterBoundaryThatFits(remaining, *measurer_, style, available);
          if (chars == 0 && !line.empty()) {
            // Not one character fits beside what is already on the line. The line is finished and
            // the same text is retried against a full-width one, where at least one will.
            finish_line();
            continue;
          }
          cut = std::max(cut, chars);
        }
        if (cut > 0 && cut < remaining.size()) {
          piece = remaining.substr(0, cut);
        }
      }

      const float advance = measurer_->MeasureWidth(piece, style);
      line.push_back(LineItem{.box = text_box,
                              .is_text = true,
                              .begin = static_cast<std::uint32_t>(offset),
                              .length = static_cast<std::uint32_t>(piece.size()),
                              .x = x,
                              .width = advance,
                              .above = ascent,
                              .below = descent});
      x += advance;

      const bool cut_for_width = piece.size() < remaining.size();
      offset += piece.size();
      // A space at a break stays on the line it ended, where it takes no width. A preserved one is
      // content and is not skipped.
      while (!preserve_spaces && offset < text.size() && text[offset] == ' ') {
        ++offset;
      }
      if (offset == segment_end) {
        finish_line();
        offset = segment_end + 1;
        continue;
      }
      // Only a run that was cut *because it did not fit* ends the line. A run that ended at a tab
      // continues on the same one, which is the whole point of a tab.
      if (cut_for_width && offset < text.size()) {
        finish_line();
      }
    }
  }

  finish_line();
  return y - start_y;
}

}  // namespace microbrowser::layout
