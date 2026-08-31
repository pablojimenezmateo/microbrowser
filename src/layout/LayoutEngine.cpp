#include "layout/LayoutEngine.h"

#include "layout/ReplacedBoxes.h"

#include "dom/FlatTree.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

#include "html/FormControl.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::layout {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

}  // namespace

gfx::FloatRect BoxGeometry::PaddingBox() const {
  const float left = padding.left.Resolve(0.0f);
  const float top = padding.top.Resolve(0.0f);
  return gfx::FloatRect{content.x - left, content.y - top,
                        content.width + left + padding.right.Resolve(0.0f),
                        content.height + top + padding.bottom.Resolve(0.0f)};
}

gfx::FloatRect BoxGeometry::BorderBox() const {
  const gfx::FloatRect inner = PaddingBox();
  const float left = border.left.Resolve(0.0f);
  const float top = border.top.Resolve(0.0f);
  return gfx::FloatRect{inner.x - left, inner.y - top,
                        inner.width + left + border.right.Resolve(0.0f),
                        inner.height + top + border.bottom.Resolve(0.0f)};
}

gfx::FloatRect BoxGeometry::MarginBox() const {
  const gfx::FloatRect inner = BorderBox();
  const float left = margin.left.Resolve(0.0f);
  const float top = margin.top.Resolve(0.0f);
  return gfx::FloatRect{inner.x - left, inner.y - top,
                        inner.width + left + margin.right.Resolve(0.0f),
                        inner.height + top + margin.bottom.Resolve(0.0f)};
}

// A request rather than a Font: the display list must be describable without a
// live face -- see gfx/DisplayList.h.
gfx::FontRequest FontRequestFor(const css::ComputedStyle& style) {
  gfx::FontRequest request;
  request.families = style.font_family;
  request.size = style.font_size;
  request.weight = static_cast<int>(style.font_weight);
  request.italic = style.font_style == css::FontStyle::Italic;
  // Resolved here rather than in the cascade because both are lengths in `em`, and `em` on these
  // two is this element's own font size -- which is the number two lines above. Resolving at the
  // one point a font is asked for is also what keeps measurement and paint from ever seeing
  // different values.
  // A percentage on either is a fraction of this element's own font size (css-text-4), so the
  // basis and the em base are the same number -- which is why this is `Used` with `font_size`
  // twice rather than a `Resolve` that would answer zero for a percentage.
  request.letter_spacing = style.letter_spacing.Used(style.font_size, style.font_size);
  request.word_spacing = style.word_spacing.Used(style.font_size, style.font_size);
  return request;
}

css::ComputedStyle TextStyleFrom(const css::ComputedStyle& parent) {
  // The inherited properties and nothing else -- through the *cascade's* one list rather than a
  // second one here. This function used to name them itself, and it had drifted: `direction` and
  // `unicode-bidi` inherit and were missing, so a right-to-left `<span>` was right-to-left and its
  // own text was not.
  //
  // Custom properties are deliberately not copied. A text box has no declarations, so it never
  // resolves a `var()`, and copying the table into every text node is a vector copy per text node.
  css::ComputedStyle text_style;
  css::InheritInto(parent, text_style, /*with_custom_properties=*/false);
  return text_style;
}

Box& Box::Append(std::unique_ptr<Box> child) {
  children_.push_back(std::move(child));
  AddPerformanceCounter(PerfCounterId::LayoutBoxesCreated);
  return *children_.back();
}

float FixedTextMeasurer::MeasureWidth(std::string_view text, const css::ComputedStyle& style,
                                      bool right_to_left) const {
  // A fixed advance per byte does not depend on direction, and saying so here is better than a
  // signature that pretends it might.
  (void)right_to_left;
  return static_cast<float>(text.size()) * style.font_size * ratio_;
}

float FixedTextMeasurer::LineHeight(const css::ComputedStyle& style) const {
  return style.line_height > 0.0f ? style.line_height : style.font_size * 1.2f;
}

float FixedTextMeasurer::Ascent(const css::ComputedStyle& style) const {
  // 0.8 em, which is close enough to a typical face that a test asserting a
  // baseline position states a number rather than a font's opinion.
  return style.font_size * 0.8f;
}

// Lays out the inline children of `box` into line boxes. Returns the height used.
//
// Two passes per line, and the reason is baselines. A line's items do not sit at
// its top: they sit on a shared baseline, and where that baseline falls depends
// on the tallest thing above it -- which is not known until every item on the
// line has been measured. Placing items as they are encountered puts short text
// next to a tall image at the top of the line instead of along its bottom,
// which is exactly what it looks like: text floating beside a picture.
//
// Line breaking is at spaces only. That is deliberate rather than a stub: the
// correct rule is UAX #14, which needs the line-breaking property of every code
// point, and a wrong break inside a word is more visibly wrong than a missing
// opportunity. Breaking only where the text says it may is the conservative
// direction.
void LayoutEngine::PlaceFloat(Box& child, float content_left, float content_width, float cursor_y,
                              FloatContext& floats) const {
  const css::ComputedStyle& style = child.Style();

  if (child.GetKind() == Box::Kind::Replaced) {
    // A replaced float already knows its size; there is no content to lay out.
    child.Geometry().margin = style.margin;
    const float margin_left = style.margin.left.Resolve(style.font_size);
    const float margin_right = style.margin.right.Resolve(style.font_size);
    const float margin_top = style.margin.top.Resolve(style.font_size);
    const float margin_bottom = style.margin.bottom.Resolve(style.font_size);
    // Percentages need the float's containing block (the content box being
    // laid out), same as inline replaced. ReplacedWidth alone skips them.
    const ReplacedUsedSize used = ResolveReplacedSize(child, content_width, std::nullopt);
    const float width = used.width;
    const float height = used.height;
    const gfx::FloatRect placed =
        floats.Place(style.css_float, width + margin_left + margin_right,
                     height + margin_top + margin_bottom, cursor_y, content_left,
                     content_left + content_width);
    child.Geometry().content =
        gfx::FloatRect{placed.x + margin_left, placed.y + margin_top, width, height};
    return;
  }

  float probe = cursor_y;
  FloatContext detached;
  LayoutBlock(child, content_left, content_width, probe, detached);
  const gfx::FloatRect margin_box = child.Geometry().MarginBox();
  const gfx::FloatRect placed =
      floats.Place(style.css_float, margin_box.width, margin_box.height, cursor_y, content_left,
                   content_left + content_width);

  // Same available width as the probe; only the origin changed. Translating
  // the measured subtree is TD-0001's float half -- a second LayoutBlock was
  // walking every descendant just to move them.
  OffsetLaidOutSubtree(child, placed.x - margin_box.x, placed.y - margin_box.y);
  AddPerformanceCounter(PerfCounterId::LayoutMeasureCacheHits);
}

// `floats` is the formatting context this box participates in. A box that
// establishes its own -- the root, and every float -- passes a fresh one to
// its children, which is what keeps a float inside a sidebar from shortening
// the lines of the article next to it.
//
// `center_in_container` is the <center> rule: the containing block centres its
// block-level children outright, whatever their margins say. It centres this
// box's lines too, but that part is ordinary `text-align: center` and inherits
// with it; *this* half is a property of the container, which is why it is a
// parameter and why css::ComputedStyle::centers_block_children is deliberately
// not inherited. Inheriting it would make every block inside a <center>,
// however deep, re-centre itself against a container it already fits exactly
// -- and a nested block that fits exactly must not move.
void LayoutEngine::LayoutBlock(Box& box, float container_left, float available_width,
                               float& cursor_y, FloatContext& floats,
                               bool center_in_container, const ForcedSize* forced) const {
  AddPerformanceCounter(PerfCounterId::LayoutBlockPasses);
  const css::ComputedStyle& style = box.Style();
  BoxGeometry& geometry = box.Geometry();
  geometry.margin = style.margin;
  geometry.border = style.UsedBorderWidths();

  // Not const: an auto margin is resolved below, once the used width is known.
  float margin_left = style.margin.left.Resolve(style.font_size);
  const float margin_right = style.margin.right.Resolve(style.font_size);
  // Padding percentages are of the containing-block *width*, including
  // padding-top/bottom -- CSS 2.1 §8.4. That is what makes
  // `::before { content:""; display:block; padding-top:56% }` reserve an
  // aspect-ratio box. Resolving against font size left every such box at 0.
  const float padding_left = style.padding.left.Used(available_width, style.font_size);
  const float padding_right = style.padding.right.Used(available_width, style.font_size);
  const float padding_top = style.padding.top.Used(available_width, style.font_size);
  const float padding_bottom = style.padding.bottom.Used(available_width, style.font_size);
  // Store used pixels so PaddingBox()/paint do not re-Resolve percentages to 0.
  geometry.padding = css::Edges{css::Length::Pixels(padding_top), css::Length::Pixels(padding_right),
                                css::Length::Pixels(padding_bottom), css::Length::Pixels(padding_left)};
  const float border_left = geometry.border.left.Resolve(style.font_size);
  const float border_right = geometry.border.right.Resolve(style.font_size);

  const float horizontal = margin_left + margin_right + padding_left + padding_right +
                           border_left + border_right;
  float content_width = available_width - horizontal;
  // Measured here when the table's own width depends on it, and handed to
  // LayoutTableChildren so it is measured once rather than once per question
  // asked about it.
  std::optional<TableColumnWidths> table_columns;
  if (box.GetKind() == Box::Kind::Replaced) {
    // CSS 2.1 s10.3.4: a block-level replaced box is as wide as its *content*,
    // not as wide as its containing block -- a `display: block` image does not
    // stretch. ReplacedWidth has already folded in a declared width and the
    // presentational attribute; a percentage is the one case it cannot answer,
    // because only the container knows what it is a percentage of.
    //
    // Recomputed rather than read back out of the geometry the box tree put
    // there, because LayoutBlock runs twice on the same box in more than one
    // path (a float probes, then places) and the second run would read the
    // first run's *used* width as though it were the intrinsic one.
    content_width = style.width.IsPercent() ? style.width.Used(available_width, style.font_size)
                                            : ReplacedWidth(box);
  } else if (!style.width.IsAuto()) {
    // A percentage width resolves against the containing block, which is the
    // one place a percentage *can* be resolved — this is why the cascade
    // carried it instead of guessing.
    content_width = style.width.Used(available_width, style.font_size);
  } else if (style.display == css::Display::Table) {
    // Shrink-to-fit, which is what a table with no stated width gets: as wide
    // as its columns want, but never wider than what is available and never
    // narrower than what they need. A table that filled its containing block
    // like an ordinary block would stretch a two-word column across the page,
    // which is the single most visible way a table can be laid out wrong.
    table_columns = MeasureTableColumns(box);
    float total_min = 0.0f;
    float total_max = 0.0f;
    for (std::size_t i = 0; i < table_columns->min.size(); ++i) {
      total_min += table_columns->min[i];
      total_max += table_columns->max[i];
    }
    content_width = std::min(std::max(total_min, content_width), total_max);
  }
  if (forced != nullptr && forced->content_width.has_value()) {
    // The flex algorithm already decided this, against the other items. The
    // box's own `width` was an input to that decision and must not be applied
    // a second time here.
    content_width = *forced->content_width;
  }
  // Shrink-to-fit: as wide as its content wants, but never wider than what is
  // left. A float that filled its containing block would leave nothing to flow
  // beside it, which is the one thing a float is for; an inline-block that
  // filled it would push everything after it onto the next line, which is the
  // one thing an inline-block is for. Same rule, same reason, so one condition.
  if ((style.IsFloating() || box.IsAtomicInline()) && style.width.IsAuto()) {
    content_width = std::clamp(MaxContentWidth(box) - horizontal, 0.0f, content_width);
  }
  // The bounds apply to whatever decided the width above -- a declared one,
  // shrink-to-fit, or the flex algorithm. One place, so a `max-width` cannot
  // be honoured on a block and forgotten on a float. Under `border-box` the
  // bound describes padding+border+content (iron-fit's `max-height` on youtube).
  const float width_padding_border = padding_left + padding_right + border_left + border_right;
  content_width =
      style.ClampWidth(std::max(0.0f, content_width), available_width, width_padding_border);

  // Auto margins absorb whatever the box does not use, which is how
  // `margin: 0 auto` centres a block and how <center> centres a table. A float
  // is placed by the float context and never gets any of this.
  //
  // The leftover can be negative -- a percentage width is a percentage of the
  // containing block and margins are added *outside* it, so `width: 100%` with
  // a margin legitimately overflows. Only a positive leftover is shared.
  // Neither a float nor an atomic inline gets any of this: both are placed by
  // something other than their containing block's margin arithmetic -- the
  // float context and the line, respectively -- and an `auto` margin on either
  // computes to zero rather than absorbing the leftover.
  if (!style.IsFloating() && !box.IsAtomicInline()) {
    const float leftover = available_width - horizontal - content_width;
    if (leftover > 0.0f) {
      const bool auto_left = style.margin.left.IsAuto();
      const bool auto_right = style.margin.right.IsAuto();
      if (auto_left && auto_right) {
        margin_left += leftover * 0.5f;
      } else if (auto_left) {
        margin_left += leftover;
      } else if (center_in_container) {
        // The <center> case: the containing block centres its block children
        // outright, whatever their margins say.
        margin_left += leftover * 0.5f;
      }
      // A lone `margin-right: auto` needs no adjustment: the box is already at
      // its container's left edge, which is where the leftover on the right
      // puts it.
    }
  }

  // Relative to the containing block, not to the viewport. Geometry is stored
  // in absolute coordinates -- the painter walks the box tree without an
  // ancestor stack and BuildDisplayList takes no offset per box -- so the
  // container's own left edge has to be carried in. Vertical position was
  // already threaded through `cursor_y`; horizontal was not, and every nested
  // block painted at its parent's left margin instead of past it.
  const float content_left = container_left + margin_left + border_left + padding_left;
  const float content_top = cursor_y + style.margin.top.Resolve(style.font_size) +
                            geometry.border.top.Resolve(style.font_size) + padding_top;

  // A float establishes a formatting context of its own, so a float inside a
  // sidebar does not shorten the lines of the article beside it. An atomic
  // inline does the same, and for the same reason: it is a rectangle on
  // somebody else's line, and a float inside it must not reach out and shorten
  // that line.
  FloatContext own_floats;
  FloatContext& child_floats =
      (style.IsFloating() || box.IsAtomicInline()) ? own_floats : floats;

  // Inline children are laid out as lines; block children stack.
  bool has_block_child = false;
  for (const std::unique_ptr<Box>& child : box.Children()) {
    has_block_child = has_block_child || child->IsOutOfLineFlow();
  }

  float content_height = 0.0f;
  // Definite height known before children when it does not depend on them
  // (stated length, or ForcedSize from abspos stretch / flex). Needed for
  // percentage heights on both block children and inline replaced boxes.
  std::optional<float> definite_content_height;
  if (forced != nullptr && forced->content_height.has_value()) {
    definite_content_height = *forced->content_height;
  } else if (!style.height.IsAuto() && !style.height.IsPercent()) {
    definite_content_height = style.height.Resolve(style.font_size);
  }

  if (style.IsFlexContainer()) {
    content_height = LayoutFlexContainer(box, content_left, content_width, content_top,
                                         padding_top, padding_bottom, forced);
  } else if (style.display == css::Display::Table) {
    content_height =
        LayoutTableChildren(box, content_left, content_width, content_top, table_columns);
  } else if (!has_block_child && !box.Children().empty()) {
    content_height = LayoutInlineChildren(box, content_left, content_width, content_top,
                                          child_floats, definite_content_height);
  } else {
    // Block children. Percentage heights resolve against `definite_content_height`
    // computed above (CSS 2.1 §10.5) — same value inline replaced boxes use.

    float child_cursor = content_top;
    for (const std::unique_ptr<Box>& child : box.Children()) {
      if (!child->IsOutOfLineFlow()) {
        continue;
      }
      if (child->IsAbsolutelyPositioned()) {
        // Out of the flow entirely: it takes no space from its siblings and
        // is placed later, once this box has a size to place it against.
        continue;
      }
      const css::ComputedStyle& child_style = child->Style();
      // `clear` first: it moves the box down before anything else decides where
      // it goes, including before a float on it is placed.
      child_cursor = child_floats.ClearanceBelow(child_style.clear, child_cursor);

      if (child->IsFloating()) {
        PlaceFloat(*child, content_left, content_width, child_cursor, child_floats);
        continue;
      }

      ForcedSize percent_height;
      const ForcedSize* child_forced = nullptr;
      if (definite_content_height.has_value() && child_style.height.IsPercent()) {
        percent_height.content_height =
            child_style.height.Used(*definite_content_height, child_style.font_size);
        child_forced = &percent_height;
      }
      LayoutBlock(*child, content_left, content_width, child_cursor, child_floats,
                  style.centers_block_children, child_forced);
    }
    content_height = child_cursor - content_top;
  }

  // What the children actually took, before a stated height or a bound replaces it. The only
  // reader is the button centring below, and it has to be taken here: every branch after this
  // one overwrites `content_height` with a number the children had no part in.
  const float content_height_from_children = content_height;

  if (style.IsFloating()) {
    // A float contains its own floats: it establishes a formatting context, and
    // a context that did not contain them would let them escape a box that has
    // no other relationship to the page.
    content_height = std::max(content_height, own_floats.LowestBottom() - content_top);
  }
  if (!style.height.IsAuto() && !style.height.IsPercent()) {
    content_height = style.height.Resolve(style.font_size, content_height);
  } else if (style.aspect_ratio > 0.0f) {
    // `aspect-ratio` with an automatic height and a width that is known: the
    // height comes from the ratio rather than from the content. This is where
    // a media box reserves its own space before anything is in it, which is
    // what the property is for -- and why it is checked only in the `auto`
    // branch, since a stated height is still the stated height.
    content_height = content_width / style.aspect_ratio;
  } else if (box.GetKind() == Box::Kind::Replaced) {
    // A replaced box has no child boxes to give it a height, so an automatic
    // one comes from the content: the image's own size, or the row arithmetic
    // a form control is measured by. Without this a `display: block` image was
    // laid out at its intrinsic width and zero pixels tall.
    content_height = ReplacedHeight(box);
  }
  if (forced != nullptr && forced->content_height.has_value()) {
    content_height = *forced->content_height;  // same reasoning as the width above
  }
  // A percentage min/max-height resolves against the containing block's
  // height, which a block in normal flow does not have -- so the container
  // passed here is the content height itself, which makes a percentage bound a
  // no-op rather than a wrong number. `border-box` subtracts padding+border
  // from the bound so `max-height: 896px; box-sizing: border-box` actually
  // yields a 896px border box (youtube consent).
  const float border_top = geometry.border.top.Resolve(style.font_size);
  const float border_bottom = geometry.border.bottom.Resolve(style.font_size);
  const float height_padding_border = padding_top + padding_bottom + border_top + border_bottom;
  content_height =
      style.ClampHeight(content_height, content_height, height_padding_border);

  // A `<button>` centres its content in its content box (CentersContentVertically). Applied here,
  // after every source of a height has had its say and before anything is placed against this
  // box, so the label of a `<button style="height:40px">` sits in the middle rather than on the
  // first line. A flex or table container has its own alignment and is left alone.
  if (content_height > content_height_from_children && box.Origin() != nullptr &&
      CentersContentVertically(*box.Origin()) && !style.IsFlexContainer() &&
      style.display != css::Display::Table) {
    OffsetBoxContents(box, 0.0f, (content_height - content_height_from_children) * 0.5f);
  }

  geometry.content = gfx::FloatRect{content_left, content_top, content_width, content_height};
  cursor_y = content_top + content_height + padding_bottom +
             geometry.border.bottom.Resolve(style.font_size) +
             style.margin.bottom.Resolve(style.font_size);

  // Now that this box has a size, it can place the absolutely positioned boxes
  // that named it as their containing block. Only a positioned box does: a
  // static one is transparent to the search, which is what makes `position:
  // relative` with no offsets the idiomatic way to anchor a child.
  if (style.IsPositioned()) {
    LayoutAbsoluteDescendants(box, geometry.PaddingBox());
  }
  // Last, so the offset moves a subtree that is already complete -- including
  // anything absolutely positioned against this box.
  ApplyRelativeOffset(box);
}

float LayoutEngine::Layout(Box& root, float width, float viewport_height) const {
  AddPerformanceCounter(PerfCounterId::LayoutRuns);
  // Intrinsic widths are cached within a pass and not across them: a replaced
  // box's used width can change between two layouts of the same tree.
  root.ClearIntrinsicWidths();
  float cursor = 0.0f;
  // The root establishes the initial block formatting context.
  FloatContext floats;
  LayoutBlock(root, 0.0f, width, cursor, floats);
  // The initial containing block is the viewport (CSS 2.1 §10.1), not the
  // root element's padding box. When every in-flow child is abspos the root
  // collapses to height 0 — using that as the CB made youtube's
  // `ytd-app { position:absolute; min-height:100% }` resolve 100% against 0
  // and leave the page a 128px strip of masthead over a white viewport.
  const gfx::FloatRect icb =
      viewport_height > 0.0f
          ? gfx::FloatRect{0.0f, 0.0f, width, viewport_height}
          : root.Geometry().PaddingBox();
  LayoutAbsoluteDescendants(root, icb);
  // Document height is the scrollable overflow of the root after abspos
  // placement, not only in-flow cursor. Youtube's `ytd-app` is
  // `position:absolute; min-height:100%` against the ICB — flow collapses and
  // abspos carries the page; returning cursor alone left scrollHeight at the
  // viewport and scrollIntoView / wheel unable to reach result thumbs.
  const float overflow_height = MeasureScrollableOverflow(root).height;
  return std::max(std::max(cursor, floats.LowestBottom()), overflow_height);
}

// The width a box states outright, plus its horizontal edges, or nullopt.
//
// A stated width is what both intrinsic measurements are: an element that says
// it is ten pixels wide wants ten pixels whether or not anything wraps, and
// what is inside it does not enter into it. Without this an empty box with a
// width -- an icon drawn entirely by `background-image`, which is how most of
// the web draws small icons -- measures as its margins, and the table column
// holding it collapses onto its neighbour.
//
// A percentage is excluded: it resolves against a containing block, and an
// intrinsic measurement is taken precisely when that is not yet known.
std::optional<float> DeclaredContentWidth(const Box& box) {
  const css::ComputedStyle& style = box.Style();
  if (style.width.IsAuto() || style.width.IsPercent()) {
    return std::nullopt;
  }
  const float edges = style.margin.left.Resolve(style.font_size) +
                      style.margin.right.Resolve(style.font_size) +
                      style.padding.left.Resolve(style.font_size) +
                      style.padding.right.Resolve(style.font_size) +
                      style.UsedBorderWidths().left.Resolve(style.font_size) +
                      style.UsedBorderWidths().right.Resolve(style.font_size);
  return std::max(0.0f, style.width.Resolve(style.font_size)) + edges;
}

// The width this box wants if nothing ever wrapped.
//
// Text contributes its whole run, a replaced box its used width, a block the
// widest of its children, and an inline sequence the sum of them -- which is
// the definition of max-content, applied to the box kinds that exist.
float LayoutEngine::MaxContentWidth(const Box& box) const {
  if (box.Intrinsic().max >= 0.0f) {
    return box.Intrinsic().max;
  }
  const float measured = MeasureMaxContentWidth(box);
  box.Intrinsic().max = measured;
  return measured;
}

namespace {

// A preserved newline ends a line whatever the width, so the intrinsic width of preserved text is
// its widest segment rather than its whole length. Before this, one `<pre>` with a long last line
// made its table column as wide as the entire block of code.
std::string_view NextSegment(std::string_view text, std::size_t& at) {
  const std::size_t end = std::min(text.find('\n', at), text.size());
  const std::string_view piece = text.substr(at, end - at);
  at = end + 1;
  return piece;
}

}  // namespace

float LayoutEngine::WidestSegment(std::string_view text, const css::ComputedStyle& style) const {
  float widest = 0.0f;
  std::size_t at = 0;
  while (at <= text.size()) {
    widest = std::max(widest, measurer_->MeasureWidth(NextSegment(text, at), style));
  }
  return widest;
}

float LayoutEngine::MeasureMaxContentWidth(const Box& box) const {
  const css::ComputedStyle& style = box.Style();
  if (const std::optional<float> declared = DeclaredContentWidth(box)) {
    return *declared;
  }
  if (box.GetKind() == Box::Kind::Text) {
    return WidestSegment(box.Text(), style);
  }
  if (box.GetKind() == Box::Kind::Replaced) {
    // The element's own width, not the geometry layout last gave it. They are
    // the same before the first pass, and the intrinsic measurement has to stay
    // a pure function of the tree and its styles -- it is cached, and a cache
    // over something layout writes to is a value from the previous viewport.
    return ReplacedWidth(box);
  }

  float widest = 0.0f;
  float inline_run = 0.0f;
  for (const std::unique_ptr<Box>& child : box.Children()) {
    const float child_width = MaxContentWidth(*child);
    if (child->IsBlockLevel()) {
      widest = std::max(widest, child_width);
      inline_run = 0.0f;
    } else {
      inline_run += child_width;
      widest = std::max(widest, inline_run);
    }
  }

  const float edges = style.margin.left.Resolve(style.font_size) +
                      style.margin.right.Resolve(style.font_size) +
                      style.padding.left.Resolve(style.font_size) +
                      style.padding.right.Resolve(style.font_size) +
                      style.UsedBorderWidths().left.Resolve(style.font_size) +
                      style.UsedBorderWidths().right.Resolve(style.font_size);
  return widest + edges;
}

// The narrowest this box can be before its content spills out.
//
// For text that is the widest single word, because a word is where wrapping is
// allowed to happen and nowhere else -- which is why this measures the pieces
// rather than scaling the whole run down. `white-space: pre` has no wrapping
// opportunities at all, so its minimum is its maximum.
//
// For a box, the widest of its children rather than their sum: an inline
// sequence can wrap between its items, so a paragraph's minimum is its longest
// word and not the width of all of them.
float LayoutEngine::MinContentWidth(const Box& box) const {
  if (box.Intrinsic().min >= 0.0f) {
    return box.Intrinsic().min;
  }
  const float measured = MeasureMinContentWidth(box);
  box.Intrinsic().min = measured;
  return measured;
}

float LayoutEngine::MeasureMinContentWidth(const Box& box) const {
  const css::ComputedStyle& style = box.Style();
  if (const std::optional<float> declared = DeclaredContentWidth(box)) {
    return *declared;
  }
  if (box.GetKind() == Box::Kind::Text) {
    if (style.text_wrap_mode == css::TextWrapMode::NoWrap) {
      // Text that may not wrap is as wide as its widest *segment*: a preserved newline is still a
      // break, so `<pre>` is not one long line even though nothing may wrap it.
      return WidestSegment(box.Text(), style);
    }
    float widest = 0.0f;
    const std::string_view text = box.Text();
    std::size_t at = 0;
    while (at < text.size()) {
      while (at < text.size() && IsSpace(text[at])) {
        ++at;
      }
      const std::size_t begin = at;
      while (at < text.size() && !IsSpace(text[at])) {
        ++at;
      }
      if (at > begin) {
        widest = std::max(widest, measurer_->MeasureWidth(text.substr(begin, at - begin), style));
      }
    }
    return widest;
  }
  if (box.GetKind() == Box::Kind::Replaced) {
    // A replaced box does not wrap, so its minimum is its own width -- which is
    // also why an image in a table column stops that column shrinking. Its own,
    // not the geometry layout last wrote: see the note in the maximum.
    return ReplacedWidth(box);
  }

  float widest = 0.0f;
  for (const std::unique_ptr<Box>& child : box.Children()) {
    widest = std::max(widest, MinContentWidth(*child));
  }

  const float edges = style.margin.left.Resolve(style.font_size) +
                      style.margin.right.Resolve(style.font_size) +
                      style.padding.left.Resolve(style.font_size) +
                      style.padding.right.Resolve(style.font_size) +
                      style.UsedBorderWidths().left.Resolve(style.font_size) +
                      style.UsedBorderWidths().right.Resolve(style.font_size);
  return widest + edges;
}

}  // namespace microbrowser::layout
