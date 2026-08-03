#include "layout/LayoutEngine.h"

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

// Collapses runs of whitespace to a single space, which is what `white-space:
// normal` means. Done at box-building time rather than during line breaking,
// because the collapsed text is what every later step measures.
//
// Leading and trailing spaces survive. They carry real information across an
// element boundary: in `<b>bold</b> and <i>italic</i>`, the space between the
// two inlines is the leading space of the middle text node, and dropping it
// here renders "boldand italic". Dropping a space that turns out to be at the
// start of a line is line breaking's job, because only line breaking knows
// where a line starts.
std::string CollapseWhitespace(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool in_space = false;
  for (const char c : text) {
    if (IsSpace(c)) {
      in_space = true;
      continue;
    }
    if (in_space) {
      out.push_back(' ');
    }
    in_space = false;
    out.push_back(c);
  }
  if (in_space) {
    out.push_back(' ');
  }
  return out;
}

bool IsAllWhitespace(std::string_view text) {
  return std::all_of(text.begin(), text.end(), IsSpace);
}

// The used size of a replaced element.
//
// CSS width/height win, then the element's width/height attributes, then the
// image's own size. An <img> with no image and no declared size is 0x0 rather
// than a placeholder box: a browser that reserved space for something it may
// never receive would jump when it learned better.
float ReplacedIntrinsic(const Box& box, bool horizontal) {
  const css::ComputedStyle& style = box.Style();
  const css::Length& declared = horizontal ? style.width : style.height;
  if (!declared.IsAuto() && !declared.IsPercent()) {
    return std::max(0.0f, declared.Resolve(style.font_size, 0.0f));
  }
  if (box.Origin() != nullptr) {
    // The presentational attribute, which is where most of the web still puts
    // an image's size and which the cascade does not see.
    const std::string* attribute = box.Origin()->GetAttribute(horizontal ? "width" : "height");
    if (attribute != nullptr) {
      if (const std::optional<double> value = util::ParseDouble(*attribute)) {
        if (*value >= 0.0 && *value < 1e6) {
          return static_cast<float>(*value);
        }
      }
    }
  }
  if (box.Image() != nullptr && box.Image()->IsValid()) {
    return static_cast<float>(horizontal ? box.Image()->Width() : box.Image()->Height());
  }
  if (box.Origin() != nullptr && (box.Origin()->TagName() == "input" ||
                                  box.Origin()->TagName() == "button" ||
                                  box.Origin()->TagName() == "textarea" ||
                                  box.Origin()->TagName() == "select")) {
    if (!horizontal) {
      if (box.Origin()->TagName() == "textarea") {
        const std::string* rows = box.Origin()->GetAttribute("rows");
        if (rows != nullptr) {
          if (const std::optional<double> parsed = util::ParseDouble(*rows)) {
            if (*parsed > 0.0 && *parsed < 1000.0) {
              return static_cast<float>(*parsed) * style.font_size * 1.2f + 6.0f;
            }
          }
        }
      }
      return style.font_size * 1.2f + 6.0f;
    }
    const std::string* size =
        box.Origin()->GetAttribute(box.Origin()->TagName() == "textarea" ? "cols" : "size");
    if (size != nullptr) {
      if (const std::optional<double> parsed = util::ParseDouble(*size)) {
        if (*parsed > 0.0 && *parsed < 1000.0) {
          return static_cast<float>(*parsed) * style.font_size * 0.6f + 12.0f;
        }
      }
    }
    if (!box.Text().empty()) {
      return static_cast<float>(box.Text().size()) * style.font_size * 0.6f + 18.0f;
    }
    return style.font_size * 20.0f * 0.6f + 12.0f;
  }
  return 0.0f;
}

float ReplacedWidth(const Box& box) { return ReplacedIntrinsic(box, true); }
float ReplacedHeight(const Box& box) { return ReplacedIntrinsic(box, false); }

// A text box that is nothing but collapsible whitespace.
bool IsCollapsibleSpace(const Box& box) {
  return box.GetKind() == Box::Kind::Text &&
         box.Style().white_space == css::WhiteSpace::Normal && IsAllWhitespace(box.Text());
}

bool IsReplacedElement(const dom::Element& element) {
  return element.TagName() == "img" || element.TagName() == "input" ||
         element.TagName() == "button" || element.TagName() == "textarea" ||
         element.TagName() == "select";
}

std::string FormControlText(const dom::Element& element) {
  if (const std::optional<std::string> selected = html::SelectedOptionText(element)) {
    return *selected;
  }
  if (element.TagName() == "button") {
    return CollapseWhitespace(element.TextContent());
  }
  if (html::IsTextareaElement(element)) {
    const std::string* value = element.GetAttribute("value");
    const std::string current = value != nullptr ? *value : element.TextContent();
    if (!current.empty()) {
      return current;
    }
    if (const std::string* placeholder = element.GetAttribute("placeholder")) {
      return *placeholder;
    }
    return {};
  }
  if (html::IsCheckboxInput(element) || html::IsRadioInput(element)) {
    return {};
  }
  if (const std::string* value = element.GetAttribute("value"); value != nullptr && !value->empty()) {
    if (html::IsPasswordInput(element)) {
      return {};
    }
    return *value;
  }
  if (html::IsTextInputType(element)) {
    if (const std::string* placeholder = element.GetAttribute("placeholder")) {
      return *placeholder;
    }
    return {};
  }
  if (html::IsSubmitControl(element)) {
    return "Submit";
  }
  if (html::IsResetControl(element)) {
    return "Reset";
  }
  return {};
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
  return request;
}

Box& Box::Append(std::unique_ptr<Box> child) {
  children_.push_back(std::move(child));
  AddPerformanceCounter(PerfCounterId::LayoutBoxesCreated);
  return *children_.back();
}

float FixedTextMeasurer::MeasureWidth(std::string_view text,
                                      const css::ComputedStyle& style) const {
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

std::unique_ptr<Box> LayoutEngine::BuildFor(const dom::Node& node,
                                            const css::ComputedStyle& parent_style,
                                            bool& produced_inline) const {
  if (node.IsText()) {
    const auto& text_node = static_cast<const dom::Text&>(node);
    std::string text = parent_style.white_space == css::WhiteSpace::Pre ||
                               parent_style.white_space == css::WhiteSpace::PreWrap
                           ? text_node.Data()
                           : CollapseWhitespace(text_node.Data());
    if (text.empty()) {
      return nullptr;
    }
    // A text box carries only the *inherited* properties of its parent. Copying
    // the whole computed style gives every text run its parent's background,
    // border, margin and width, and the painter then draws all of them a second
    // time inside the box that already has them.
    css::ComputedStyle text_style;
    text_style.color = parent_style.color;
    text_style.font_size = parent_style.font_size;
    text_style.font_weight = parent_style.font_weight;
    text_style.font_style = parent_style.font_style;
    text_style.font_family = parent_style.font_family;
    text_style.line_height = parent_style.line_height;
    text_style.text_align = parent_style.text_align;
    text_style.white_space = parent_style.white_space;

    auto box = std::make_unique<Box>(Box::Kind::Text, std::move(text_style));
    box->SetText(std::move(text));
    produced_inline = true;
    return box;
  }

  if (!node.IsElement()) {
    return nullptr;  // comments and doctypes generate no boxes
  }

  const auto& element = static_cast<const dom::Element&>(node);
  const css::ComputedStyle style = resolver_->StyleFor(element, parent_style);
  if (!style.GeneratesBox()) {
    return nullptr;
  }

  if (html::IsHiddenInput(element)) {
    return nullptr;
  }

  if (element.TagName() == "br") {
    auto box = std::make_unique<Box>(Box::Kind::LineBreak, style);
    box->SetOrigin(&element);
    produced_inline = true;
    return box;
  }

  // A replaced element's children generate no boxes: whatever is inside an
  // <img> is fallback content the element replaces, and form controls have
  // their own control surface rather than ordinary DOM child boxes.
  // Every box may carry one, replaced or not, so it is resolved before the
  // kinds diverge rather than in each branch.
  const auto attach_background = [this, &style](Box& box) {
    if (images_ != nullptr && !style.background.image.empty()) {
      box.SetBackgroundImage(images_->ImageFor(style.background.image));
    }
  };

  if (IsReplacedElement(element)) {
    auto box = std::make_unique<Box>(Box::Kind::Replaced, style);
    box->SetOrigin(&element);
    attach_background(*box);
    if (element.TagName() == "img" && images_ != nullptr) {
      if (const std::string* src = element.GetAttribute("src"); src != nullptr) {
        box->SetImage(images_->ImageFor(*src));
      }
    } else if (element.TagName() == "input" || element.TagName() == "button" ||
               element.TagName() == "textarea" || element.TagName() == "select") {
      box->SetText(FormControlText(element));
    }
    box->Geometry().content = gfx::FloatRect{0.0f, 0.0f, ReplacedWidth(*box), ReplacedHeight(*box)};
    produced_inline = true;
    return box;
  }

  // Children are gathered before this box is created, because two things about
  // it are not knowable until they exist: whether it mixes inline and block
  // content, and -- for a declared inline -- whether it contains a block at all.
  const bool declared_inline = style.IsInlineLevel();
  std::vector<std::unique_ptr<Box>> children;
  bool any_inline = false;
  bool any_block = false;
  for (const std::unique_ptr<dom::Node>& child : node.Children()) {
    bool child_inline = false;
    std::unique_ptr<Box> child_box = BuildFor(*child, style, child_inline);
    if (child_box == nullptr) {
      continue;
    }
    any_inline = any_inline || child_inline;
    any_block = any_block || child_box->IsBlockLevel();
    children.push_back(std::move(child_box));
  }

  // Whitespace between two blocks generates no box -- keeping it would put a
  // blank line between every pair of paragraphs -- but whitespace between two
  // *inlines* is the space between two words, and dropping it renders
  // "boldand italic". The difference is what the neighbours are, which is only
  // knowable here, after they have all been built.
  if (style.white_space == css::WhiteSpace::Normal) {
    std::vector<std::unique_ptr<Box>> kept;
    kept.reserve(children.size());
    for (std::size_t i = 0; i < children.size(); ++i) {
      if (!IsCollapsibleSpace(*children[i])) {
        kept.push_back(std::move(children[i]));
        continue;
      }
      // Dropped at the edges of the block too: a line never begins or ends
      // with a collapsible space.
      // The previous sibling is read from `kept`, not from `children`: the ones
      // already kept were moved out and left null behind them.
      const bool inline_before = !kept.empty() && !kept.back()->IsOutOfLineFlow();
      const bool inline_after =
          i + 1 < children.size() && !children[i + 1]->IsOutOfLineFlow();
      if (inline_before && inline_after) {
        kept.push_back(std::move(children[i]));
      }
    }
    children = std::move(kept);
    any_inline = false;
    any_block = false;
    for (const std::unique_ptr<Box>& child : children) {
      any_inline = any_inline || !child->IsOutOfLineFlow();
      any_block = any_block || child->IsOutOfLineFlow();
    }
  }

  // An inline box containing a block is not an inline box. CSS 2.1 s9.2.1.1
  // splits the inline around the block and wraps each part in an anonymous
  // block; this promotes the whole element instead, which produces the same
  // boxes for the case the web actually writes -- `<a><div>...</div></a>`,
  // which is how Hacker News draws its vote arrows and how most of the web
  // makes a card clickable.
  //
  // Left inline, the block child is never laid out at all: line layout walks
  // *through* a non-inline child collecting the inline content inside it, so a
  // block with no inline content simply vanishes, taking its background and its
  // borders with it. That is a wrong render with no error, which is the worst
  // kind.
  const bool inline_level = declared_inline && !any_block;
  auto box = std::make_unique<Box>(inline_level ? Box::Kind::Inline : Box::Kind::Block, style);
  box->SetOrigin(&element);
  attach_background(*box);
  produced_inline = inline_level;

  // Every child of a flex container is an item, so a run of inline children
  // has to become one box rather than a line inside the container. That is the
  // same anonymous-block wrapping mixed content already needs, applied
  // unconditionally.
  const bool wrap_inline_runs = style.IsFlexContainer() ? any_inline : (any_inline && any_block);
  if (!inline_level && wrap_inline_runs) {
    // Mixed content. Consecutive inline children are wrapped in anonymous
    // blocks, which is the only way the two kinds can be siblings — a block
    // formatting context contains blocks, and inline content needs one of its
    // own.
    std::unique_ptr<Box> pending;
    for (std::unique_ptr<Box>& child : children) {
      if (child->IsOutOfLineFlow()) {
        if (pending != nullptr) {
          box->Append(std::move(pending));
        }
        box->Append(std::move(child));
        continue;
      }
      if (pending == nullptr) {
        pending = std::make_unique<Box>(Box::Kind::AnonymousBlock, style);
      }
      pending->Append(std::move(child));
    }
    if (pending != nullptr) {
      box->Append(std::move(pending));
    }
  } else {
    for (std::unique_ptr<Box>& child : children) {
      box->Append(std::move(child));
    }
  }
  return box;
}

std::unique_ptr<Box> LayoutEngine::BuildBoxTree(const dom::Document& document) const {
  AddPerformanceCounter(PerfCounterId::LayoutTreeBuilds);
  css::ComputedStyle root_style = css::StyleResolver::InitialStyle();
  root_style.display = css::Display::Block;

  auto root = std::make_unique<Box>(Box::Kind::Block, root_style);
  for (const std::unique_ptr<dom::Node>& child : document.Children()) {
    bool produced_inline = false;
    if (std::unique_ptr<Box> box = BuildFor(*child, root_style, produced_inline)) {
      root->Append(std::move(box));
    }
  }
  return root;
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
    const gfx::FloatRect content = child.Geometry().content;
    const gfx::FloatRect placed = floats.Place(
        style.css_float, content.width + margin_left + margin_right,
        content.height + margin_top + margin_bottom, cursor_y, content_left,
        content_left + content_width);
    child.Geometry().content = gfx::FloatRect{placed.x + margin_left, placed.y + margin_top,
                                              content.width, content.height};
    return;
  }

  float probe = cursor_y;
  FloatContext detached;
  LayoutBlock(child, content_left, content_width, probe, detached);
  const gfx::FloatRect margin_box = child.Geometry().MarginBox();
  const gfx::FloatRect placed =
      floats.Place(style.css_float, margin_box.width, margin_box.height, cursor_y, content_left,
                   content_left + content_width);

  float final_cursor = placed.y;
  FloatContext inner;
  LayoutBlock(child, placed.x, content_width, final_cursor, inner);
}

void LayoutEngine::LayoutBlock(Box& box, float container_left, float available_width,
                               float& cursor_y, FloatContext& floats,
                               bool center_in_container, const ForcedSize* forced) const {
  const css::ComputedStyle& style = box.Style();
  BoxGeometry& geometry = box.Geometry();
  geometry.margin = style.margin;
  geometry.padding = style.padding;
  geometry.border = style.has_border ? style.border_width : css::Edges{};

  // Not const: an auto margin is resolved below, once the used width is known.
  float margin_left = style.margin.left.Resolve(style.font_size);
  const float margin_right = style.margin.right.Resolve(style.font_size);
  const float padding_left = style.padding.left.Resolve(style.font_size);
  const float padding_right = style.padding.right.Resolve(style.font_size);
  const float border_left = geometry.border.left.Resolve(style.font_size);
  const float border_right = geometry.border.right.Resolve(style.font_size);

  const float horizontal = margin_left + margin_right + padding_left + padding_right +
                           border_left + border_right;
  float content_width = available_width - horizontal;
  // Measured here when the table's own width depends on it, and handed to
  // LayoutTableChildren so it is measured once rather than once per question
  // asked about it.
  std::optional<TableColumnWidths> table_columns;
  if (!style.width.IsAuto()) {
    // A percentage width resolves against the containing block, which is the
    // one place a percentage *can* be resolved — this is why the cascade
    // carried it instead of guessing.
    content_width = style.width.IsPercent()
                        ? available_width * style.width.value / 100.0f
                        : style.width.Resolve(style.font_size, content_width);
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
  if (style.IsFloating() && style.width.IsAuto()) {
    // Shrink-to-fit: as wide as its content wants, but never wider than what is
    // left. A float that filled its containing block would leave nothing to
    // flow beside it, which is the one thing a float is for.
    content_width = std::clamp(MaxContentWidth(box) - horizontal, 0.0f, content_width);
  }
  content_width = std::max(0.0f, content_width);

  // Auto margins absorb whatever the box does not use, which is how
  // `margin: 0 auto` centres a block and how <center> centres a table. A float
  // is placed by the float context and never gets any of this.
  //
  // The leftover can be negative -- a percentage width is a percentage of the
  // containing block and margins are added *outside* it, so `width: 100%` with
  // a margin legitimately overflows. Only a positive leftover is shared.
  if (!style.IsFloating()) {
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
  const float content_top =
      cursor_y + style.margin.top.Resolve(style.font_size) +
      geometry.border.top.Resolve(style.font_size) + style.padding.top.Resolve(style.font_size);

  // A float establishes a formatting context of its own, so a float inside a
  // sidebar does not shorten the lines of the article beside it.
  FloatContext own_floats;
  FloatContext& child_floats = style.IsFloating() ? own_floats : floats;

  // Inline children are laid out as lines; block children stack.
  bool has_block_child = false;
  for (const std::unique_ptr<Box>& child : box.Children()) {
    has_block_child = has_block_child || child->IsOutOfLineFlow();
  }

  float content_height = 0.0f;
  if (style.IsFlexContainer()) {
    content_height = LayoutFlexChildren(box, content_left, content_width, content_top);
  } else if (style.display == css::Display::Table) {
    content_height =
        LayoutTableChildren(box, content_left, content_width, content_top, table_columns);
  } else if (!has_block_child && !box.Children().empty()) {
    content_height =
        LayoutInlineChildren(box, content_left, content_width, content_top, child_floats);
  } else {
    float child_cursor = content_top;
    for (const std::unique_ptr<Box>& child : box.Children()) {
      if (!child->IsOutOfLineFlow()) {
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

      LayoutBlock(*child, content_left, content_width, child_cursor, child_floats,
                  style.centers_block_children);
    }
    content_height = child_cursor - content_top;
  }

  if (style.IsFloating()) {
    // A float contains its own floats: it establishes a formatting context, and
    // a context that did not contain them would let them escape a box that has
    // no other relationship to the page.
    content_height = std::max(content_height, own_floats.LowestBottom() - content_top);
  }
  if (!style.height.IsAuto() && !style.height.IsPercent()) {
    content_height = style.height.Resolve(style.font_size, content_height);
  }
  if (forced != nullptr && forced->content_height.has_value()) {
    content_height = *forced->content_height;  // same reasoning as the width above
  }

  geometry.content = gfx::FloatRect{content_left, content_top, content_width, content_height};
  cursor_y = content_top + content_height + style.padding.bottom.Resolve(style.font_size) +
             geometry.border.bottom.Resolve(style.font_size) +
             style.margin.bottom.Resolve(style.font_size);
}

float LayoutEngine::Layout(Box& root, float width) const {
  AddPerformanceCounter(PerfCounterId::LayoutRuns);
  // Intrinsic widths are cached within a pass and not across them: a replaced
  // box's used width can change between two layouts of the same tree.
  root.ClearIntrinsicWidths();
  float cursor = 0.0f;
  // The root establishes the initial block formatting context.
  FloatContext floats;
  LayoutBlock(root, 0.0f, width, cursor, floats);
  // The document is as tall as the lower of its flow content and its floats: a
  // page that is nothing but a tall float still scrolls.
  return std::max(cursor, floats.LowestBottom());
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
                      (style.has_border ? style.border_width.left.Resolve(style.font_size) +
                                              style.border_width.right.Resolve(style.font_size)
                                        : 0.0f);
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

float LayoutEngine::MeasureMaxContentWidth(const Box& box) const {
  const css::ComputedStyle& style = box.Style();
  if (const std::optional<float> declared = DeclaredContentWidth(box)) {
    return *declared;
  }
  if (box.GetKind() == Box::Kind::Text) {
    return measurer_->MeasureWidth(box.Text(), style);
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
                      (style.has_border ? style.border_width.left.Resolve(style.font_size) +
                                              style.border_width.right.Resolve(style.font_size)
                                        : 0.0f);
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
    if (style.white_space == css::WhiteSpace::Pre) {
      return measurer_->MeasureWidth(box.Text(), style);
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
                      (style.has_border ? style.border_width.left.Resolve(style.font_size) +
                                              style.border_width.right.Resolve(style.font_size)
                                        : 0.0f);
  return widest + edges;
}

}  // namespace microbrowser::layout
