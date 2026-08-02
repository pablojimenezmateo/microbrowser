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
  if (box.Origin() != nullptr && box.Origin()->TagName() == "input") {
    if (!horizontal) {
      return style.font_size * 1.2f + 6.0f;
    }
    const std::string* size = box.Origin()->GetAttribute("size");
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
  return element.TagName() == "img" || element.TagName() == "input";
}

std::string InputControlText(const dom::Element& element) {
  if (html::IsPasswordInput(element)) {
    return {};
  }
  if (html::IsCheckboxInput(element) || html::IsRadioInput(element)) {
    return {};
  }
  if (const std::string* value = element.GetAttribute("value")) {
    return *value;
  }
  if (html::IsSubmitInput(element)) {
    return "Submit";
  }
  if (html::IsInputType(element, "reset")) {
    return "Reset";
  }
  return {};
}

std::optional<float> TableAttributeWidth(const Box& box, float available_width) {
  if (box.Origin() == nullptr || box.Origin()->TagName() != "table") {
    return std::nullopt;
  }
  const std::string* attribute = box.Origin()->GetAttribute("width");
  if (attribute == nullptr || attribute->empty()) {
    return std::nullopt;
  }
  if (attribute->back() == '%') {
    const std::string_view number(attribute->data(), attribute->size() - 1);
    if (const std::optional<double> percent = util::ParseDouble(number)) {
      return std::max(0.0f, available_width * static_cast<float>(*percent) / 100.0f);
    }
    return std::nullopt;
  }
  if (const std::optional<double> pixels = util::ParseDouble(*attribute)) {
    if (*pixels >= 0.0 && *pixels < 1e6) {
      return static_cast<float>(*pixels);
    }
  }
  return std::nullopt;
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
  request.family = style.font_family;
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

  // A replaced element's children generate no boxes: whatever is inside an
  // <img> is fallback content the element replaces, and an <input> has its own
  // control surface rather than DOM children.
  if (IsReplacedElement(element)) {
    auto box = std::make_unique<Box>(Box::Kind::Replaced, style);
    box->SetOrigin(&element);
    if (element.TagName() == "img" && images_ != nullptr) {
      if (const std::string* src = element.GetAttribute("src"); src != nullptr) {
        box->SetImage(images_->ImageFor(*src));
      }
    } else if (element.TagName() == "input") {
      box->SetText(InputControlText(element));
    }
    box->Geometry().content = gfx::FloatRect{0.0f, 0.0f, ReplacedWidth(*box), ReplacedHeight(*box)};
    produced_inline = true;
    return box;
  }

  const bool inline_level = style.IsInlineLevel();
  auto box = std::make_unique<Box>(inline_level ? Box::Kind::Inline : Box::Kind::Block, style);
  box->SetOrigin(&element);
  if (inline_level) {
    produced_inline = true;
  }

  // Children are gathered first so that "does this block mix inline and block
  // children" is answerable before any of them are attached.
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

  if (!inline_level && any_inline && any_block) {
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
float LayoutEngine::LayoutInlineChildren(Box& box, float content_left, float content_width,
                                         float start_y, FloatContext& floats) const {
  // One item on the current line: a slice of a text box, or a whole replaced
  // box. Both are rectangles hung from a baseline; that is the only thing line
  // layout needs to know about either.
  struct LineItem {
    Box* box = nullptr;
    bool is_text = false;
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

    for (const LineItem& item : line) {
      if (item.is_text) {
        TextFragment fragment;
        fragment.begin = item.begin;
        fragment.length = item.length;
        fragment.rect = gfx::FloatRect{item.x, y, item.width, height};
        fragment.baseline = baseline;
        item.box->AddFragment(fragment);
        item.box->Geometry().content = item.box->Fragments().size() == 1
                                           ? fragment.rect
                                           : item.box->Geometry().content.United(fragment.rect);
      } else {
        // A replaced element's baseline is its bottom edge, per CSS 2.1
        // §10.8.1. That is why an image on a line of text sits *on* the text
        // rather than beside it.
        item.box->Geometry().content =
            gfx::FloatRect{item.x, baseline - item.above, item.width, item.above + item.below};
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
    if (item->GetKind() == Box::Kind::Replaced) {
      // An atomic inline: one unbreakable rectangle. It wraps to the next line
      // if it does not fit and the line already has something on it, and
      // otherwise overflows -- which is what a too-wide image does.
      const float width = item->Geometry().content.width;
      const float height = item->Geometry().content.height;
      if (!line.empty() && x + width > line_right) {
        finish_line();
      }
      line.push_back(LineItem{item, false, 0, 0, x, width, height, 0.0f});
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
        // Break at the last space that fits. When nothing fits, the whole
        // remainder goes on this line anyway -- the line is empty, and a piece
        // that never shrinks is how a line-breaking loop spins forever.
        std::size_t best = std::string_view::npos;
        for (std::size_t at = 0; at < remaining.size(); ++at) {
          if (remaining[at] != ' ') {
            continue;
          }
          if (measurer_->MeasureWidth(remaining.substr(0, at), style) <= available) {
            best = at;
          } else {
            break;
          }
        }
        if (best != std::string_view::npos && best > 0) {
          piece = remaining.substr(0, best);
        }
      }

      const float advance = measurer_->MeasureWidth(piece, style);
      line.push_back(LineItem{text_box, true, static_cast<std::uint32_t>(offset),
                              static_cast<std::uint32_t>(piece.size()), x, advance, ascent,
                              descent});
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
                               float& cursor_y, FloatContext& floats) const {
  const css::ComputedStyle& style = box.Style();
  BoxGeometry& geometry = box.Geometry();
  geometry.margin = style.margin;
  geometry.padding = style.padding;
  geometry.border = style.has_border ? style.border_width : css::Edges{};

  const float margin_left = style.margin.left.Resolve(style.font_size);
  const float margin_right = style.margin.right.Resolve(style.font_size);
  const float padding_left = style.padding.left.Resolve(style.font_size);
  const float padding_right = style.padding.right.Resolve(style.font_size);
  const float border_left = geometry.border.left.Resolve(style.font_size);
  const float border_right = geometry.border.right.Resolve(style.font_size);

  const float horizontal = margin_left + margin_right + padding_left + padding_right +
                           border_left + border_right;
  float content_width = available_width - horizontal;
  if (!style.width.IsAuto()) {
    // A percentage width resolves against the containing block, which is the
    // one place a percentage *can* be resolved — this is why the cascade
    // carried it instead of guessing.
    content_width = style.width.IsPercent()
                        ? available_width * style.width.value / 100.0f
                        : style.width.Resolve(style.font_size, content_width);
  } else if (const std::optional<float> attribute_width =
                 TableAttributeWidth(box, available_width)) {
    content_width = *attribute_width;
  }
  if (style.IsFloating() && style.width.IsAuto()) {
    // Shrink-to-fit: as wide as its content wants, but never wider than what is
    // left. A float that filled its containing block would leave nothing to
    // flow beside it, which is the one thing a float is for.
    content_width = std::clamp(MaxContentWidth(box) - horizontal, 0.0f, content_width);
  }
  content_width = std::max(0.0f, content_width);

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
  if (style.display == css::Display::Table) {
    content_height = LayoutTableChildren(box, content_left, content_width, content_top);
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

      LayoutBlock(*child, content_left, content_width, child_cursor, child_floats);
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

  geometry.content = gfx::FloatRect{content_left, content_top, content_width, content_height};
  cursor_y = content_top + content_height + style.padding.bottom.Resolve(style.font_size) +
             geometry.border.bottom.Resolve(style.font_size) +
             style.margin.bottom.Resolve(style.font_size);
}

float LayoutEngine::Layout(Box& root, float width) const {
  AddPerformanceCounter(PerfCounterId::LayoutRuns);
  float cursor = 0.0f;
  // The root establishes the initial block formatting context.
  FloatContext floats;
  LayoutBlock(root, 0.0f, width, cursor, floats);
  // The document is as tall as the lower of its flow content and its floats: a
  // page that is nothing but a tall float still scrolls.
  return std::max(cursor, floats.LowestBottom());
}

// The width this box wants if nothing ever wrapped.
//
// Text contributes its whole run, a replaced box its used width, a block the
// widest of its children, and an inline sequence the sum of them -- which is
// the definition of max-content, applied to the box kinds that exist.
float LayoutEngine::MaxContentWidth(const Box& box) const {
  const css::ComputedStyle& style = box.Style();
  if (box.GetKind() == Box::Kind::Text) {
    return measurer_->MeasureWidth(box.Text(), style);
  }
  if (box.GetKind() == Box::Kind::Replaced) {
    return box.Geometry().content.width;
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

}  // namespace microbrowser::layout
