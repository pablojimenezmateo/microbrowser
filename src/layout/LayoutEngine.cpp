#include "layout/LayoutEngine.h"

#include <algorithm>
#include <cmath>

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
std::string CollapseWhitespace(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool in_space = false;
  for (const char c : text) {
    if (IsSpace(c)) {
      in_space = true;
      continue;
    }
    if (in_space && !out.empty()) {
      out.push_back(' ');
    }
    in_space = false;
    out.push_back(c);
  }
  // A trailing space is kept only if there was text before it, so that
  // `<span>a </span><span>b</span>` renders "a b" rather than "ab".
  if (in_space && !out.empty()) {
    out.push_back(' ');
  }
  return out;
}

bool IsAllWhitespace(std::string_view text) {
  return std::all_of(text.begin(), text.end(), IsSpace);
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

std::unique_ptr<Box> LayoutEngine::BuildFor(const dom::Node& node,
                                            const css::ComputedStyle& parent_style,
                                            bool& produced_inline) const {
  if (node.IsText()) {
    const auto& text_node = static_cast<const dom::Text&>(node);
    std::string text = parent_style.white_space == css::WhiteSpace::Pre ||
                               parent_style.white_space == css::WhiteSpace::PreWrap
                           ? text_node.Data()
                           : CollapseWhitespace(text_node.Data());
    if (text.empty() ||
        (IsAllWhitespace(text) && parent_style.white_space == css::WhiteSpace::Normal)) {
      // Whitespace between blocks generates no box. Keeping it would put a
      // blank line between every pair of paragraphs.
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

  if (!inline_level && any_inline && any_block) {
    // Mixed content. Consecutive inline children are wrapped in anonymous
    // blocks, which is the only way the two kinds can be siblings — a block
    // formatting context contains blocks, and inline content needs one of its
    // own.
    std::unique_ptr<Box> pending;
    for (std::unique_ptr<Box>& child : children) {
      if (child->IsBlockLevel()) {
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

// Lays out the inline children of `box` into lines. Returns the height used.
//
// Line breaking is at spaces only. That is deliberate rather than a stub: the
// correct rule is UAX #14, which needs the line-breaking property of every code
// point, and a wrong break inside a word is more visibly wrong than a missing
// opportunity. Breaking only where the text says it may is the conservative
// direction.
float LayoutEngine::LayoutInlineChildren(Box& box, float content_left, float content_width,
                                         float start_y) const {
  float x = content_left;
  float y = start_y;
  float line_height = 0.0f;
  bool line_has_content = false;

  const auto finish_line = [&] {
    if (line_has_content) {
      y += line_height;
    }
    x = content_left;
    line_height = 0.0f;
    line_has_content = false;
  };

  // Flattened: an inline box's own children participate in the same line
  // sequence as its siblings, which is what makes `a <b>bold</b> c` one line.
  std::vector<Box*> run;
  const auto collect = [&run](Box& node, auto& self) -> void {
    for (const std::unique_ptr<Box>& child : node.Children()) {
      if (child->GetKind() == Box::Kind::Text) {
        run.push_back(child.get());
      } else {
        self(*child, self);
      }
    }
  };
  collect(box, collect);

  for (Box* text_box : run) {
    const css::ComputedStyle& style = text_box->Style();
    const float height = measurer_->LineHeight(style);
    std::string_view remaining = text_box->Text();
    bool placed_any = false;

    while (!remaining.empty()) {
      const float available = content_left + content_width - x;
      const float full_width = measurer_->MeasureWidth(remaining, style);

      if (full_width <= available || (!line_has_content && x == content_left)) {
        // Fits, or the line is empty and something must go on it regardless —
        // otherwise a single unbreakable word narrower than nothing loops
        // forever.
        std::string_view piece = remaining;
        if (full_width > available && line_has_content) {
          piece = remaining;
        }
        if (full_width > available) {
          // Break at the last space that fits.
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
        text_box->Geometry().content =
            gfx::FloatRect{x, y, measurer_->MeasureWidth(piece, style), height};
        x += measurer_->MeasureWidth(piece, style);
        line_height = std::max(line_height, height);
        line_has_content = true;
        placed_any = true;

        remaining.remove_prefix(piece.size());
        while (!remaining.empty() && remaining.front() == ' ') {
          remaining.remove_prefix(1);
        }
        if (!remaining.empty()) {
          finish_line();
        }
        continue;
      }

      // Does not fit and the line already has content: wrap and retry.
      finish_line();
    }
    (void)placed_any;
  }

  if (line_has_content) {
    y += line_height;
  }
  return y - start_y;
}

void LayoutEngine::LayoutBlock(Box& box, float available_width, float& cursor_y) const {
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
  }
  content_width = std::max(0.0f, content_width);

  const float content_left = margin_left + border_left + padding_left;
  const float content_top =
      cursor_y + style.margin.top.Resolve(style.font_size) +
      geometry.border.top.Resolve(style.font_size) + style.padding.top.Resolve(style.font_size);

  // Inline children are laid out as lines; block children stack.
  bool has_block_child = false;
  for (const std::unique_ptr<Box>& child : box.Children()) {
    has_block_child = has_block_child || child->IsBlockLevel();
  }

  float content_height = 0.0f;
  if (!has_block_child && !box.Children().empty()) {
    content_height = LayoutInlineChildren(box, content_left, content_width, content_top);
  } else {
    float child_cursor = content_top;
    for (const std::unique_ptr<Box>& child : box.Children()) {
      if (child->IsBlockLevel()) {
        LayoutBlock(*child, content_width, child_cursor);
      }
    }
    content_height = child_cursor - content_top;
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
  LayoutBlock(root, width, cursor);
  return cursor;
}

void BuildDisplayList(const Box& root, gfx::DisplayList& out) {
  // Backgrounds and borders paint before content, which is what makes a child
  // draw on top of its parent's background rather than under it.
  const auto paint = [&out](const Box& box, auto& self) -> void {
    const css::ComputedStyle& style = box.Style();
    // A text box has no background and no border by construction, but the
    // painter says so too: this is the kind of invariant that is cheap to
    // assert here and expensive to rediscover from a screenshot.
    if (box.GetKind() == Box::Kind::Text) {
      return;
    }
    const gfx::FloatRect border_box = box.Geometry().BorderBox();

    if (!style.background_color.IsFullyTransparent() && !border_box.IsEmpty()) {
      gfx::Path background;
      background.AddRect(border_box);
      out.FillPath(background, style.background_color);
    }
    if (style.has_border && !border_box.IsEmpty()) {
      const float width = style.border_width.top.Resolve(style.font_size);
      if (width > 0.0f) {
        gfx::Path outline;
        // Inset by half the stroke width so the border lands inside the border
        // box rather than straddling its edge.
        outline.AddRect(gfx::FloatRect{border_box.x + width * 0.5f, border_box.y + width * 0.5f,
                                       std::max(0.0f, border_box.width - width),
                                       std::max(0.0f, border_box.height - width)});
        gfx::StrokeStyle stroke;
        stroke.width = width;
        out.StrokePath(outline, stroke, style.border_color);
      }
    }

    for (const std::unique_ptr<Box>& child : box.Children()) {
      self(*child, self);
    }
  };
  paint(root, paint);
  AddPerformanceCounter(PerfCounterId::LayoutDisplayListsBuilt);
}

}  // namespace microbrowser::layout
