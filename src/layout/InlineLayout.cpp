#include "layout/LayoutEngine.h"

#include "text/LineBreak.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace microbrowser::layout {

// Line layout: turning a block's inline children into lines.
//
// Split from LayoutEngine.cpp for the reason TableLayout.cpp is: block layout
// stacks boxes and line layout fills rows, and the two share nothing but the
// box tree. Keeping them in one file made the one function here the largest
// thing in the module by a wide margin, which is what the translation unit cap
// is there to catch.

float LayoutEngine::LayoutInlineChildren(Box& box, float content_left, float content_width,
                                         float start_y, FloatContext& floats) const {
  // One item on the current line: a slice of a text box, or a whole replaced
  // box. Both are rectangles hung from a baseline; that is the only thing line
  // layout needs to know about either.
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
  };

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
    float above = 0.0f;
    float below = 0.0f;
    for (const LineItem& item : line) {
      above = std::max(above, item.above);
      below = std::max(below, item.below);
    }
    const float height = above + below;
    const float baseline = y + above;

    // Alignment is a shift of the whole finished line, applied here because
    // this is the first moment the line's used width is known. `justify` is
    // treated as `left`: spreading the gaps needs per-space adjustment inside a
    // shaped run, and a wrong stretch reads worse than no stretch.
    float align_offset = 0.0f;
    const css::TextAlign align = box.Style().text_align;
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
        item.box->AddFragment(fragment);
        item.box->Geometry().content = item.box->Fragments().size() == 1
                                           ? fragment.rect
                                           : item.box->Geometry().content.United(fragment.rect);
      } else if (item.is_atomic) {
        // Laid out again, at the place the line gave it, rather than moved
        // there. Geometry is absolute, so moving the box alone would leave
        // every descendant at the coordinates the measuring pass produced --
        // the same reason PlaceFloat runs LayoutBlock twice instead of
        // translating the subtree it already has.
        //
        // The origin handed over is the *margin box* corner, because that is
        // what LayoutBlock adds its own margins to. `item.x` was advanced by
        // the margin box width for exactly this reason.
        float top = baseline - item.above;
        FloatContext inner;
        LayoutBlock(*item.box, item.x + align_offset, content_width, top, inner);
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
      const float width = item->Geometry().content.width;
      const float height = item->Geometry().content.height;
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
      LayoutBlock(*item, content_left, content_width, probe, detached);
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
      line.push_back(LineItem{.box = text_box,
                              .is_text = true,
                              .begin = static_cast<std::uint32_t>(offset),
                              .length = static_cast<std::uint32_t>(piece.size()),
                              .x = x,
                              .width = advance,
                              .above = ascent,
                              .below = descent});
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

// Places one float and lays out its contents where it landed.
//
// Two passes over the child, and the reason is circular: a float's position
// depends on how wide it is, and its width depends on its content. So it is
// laid out once against a detached context to learn its size, placed, then laid
// out again at the position it got. Re-laid out rather than translated,
// because a box tree of absolute coordinates has no translate operation and
// inventing one here would be a second way to position a subtree.

}  // namespace microbrowser::layout
