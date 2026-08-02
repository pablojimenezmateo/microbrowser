#include "layout/LayoutEngine.h"

#include <algorithm>
#include <memory>

namespace microbrowser::layout {

namespace {

bool IsTableRowGroup(css::Display display) {
  return display == css::Display::TableHeaderGroup || display == css::Display::TableRowGroup ||
         display == css::Display::TableFooterGroup;
}

bool IsTableRow(css::Display display) {
  return display == css::Display::TableRow;
}

bool IsTableCell(css::Display display) {
  return display == css::Display::TableCell;
}

bool IsTableColumnBox(css::Display display) {
  return display == css::Display::TableColumnGroup || display == css::Display::TableColumn;
}

std::size_t CellCount(const Box& row) {
  std::size_t count = 0;
  for (const std::unique_ptr<Box>& child : row.Children()) {
    if (IsTableCell(child->Style().display)) {
      ++count;
    }
  }
  return count;
}

std::size_t MaxTableColumns(const Box& box) {
  if (IsTableRow(box.Style().display)) {
    return CellCount(box);
  }
  std::size_t count = 0;
  for (const std::unique_ptr<Box>& child : box.Children()) {
    if (IsTableRowGroup(child->Style().display) || IsTableRow(child->Style().display)) {
      count = std::max(count, MaxTableColumns(*child));
    }
  }
  return count;
}

}  // namespace

float LayoutEngine::LayoutTableChildren(Box& box, float content_left, float content_width,
                                        float start_y) const {
  const std::size_t column_count = std::max<std::size_t>(1, MaxTableColumns(box));
  float y = start_y;
  for (const std::unique_ptr<Box>& child : box.Children()) {
    const css::Display display = child->Style().display;
    if (display == css::Display::TableCaption) {
      FloatContext caption_floats;
      LayoutBlock(*child, content_left, content_width, y, caption_floats);
      continue;
    }
    if (IsTableRowGroup(display)) {
      y += LayoutTableRowGroup(*child, content_left, content_width, y, column_count);
      continue;
    }
    if (IsTableRow(display)) {
      y += LayoutTableRow(*child, content_left, content_width, y, column_count);
      continue;
    }
    if (IsTableColumnBox(display)) {
      child->Geometry().content = gfx::FloatRect{content_left, y, 0.0f, 0.0f};
      continue;
    }
    FloatContext fallback_floats;
    LayoutBlock(*child, content_left, content_width, y, fallback_floats);
  }
  return y - start_y;
}

float LayoutEngine::LayoutTableRowGroup(Box& group, float content_left, float content_width,
                                        float start_y, std::size_t column_count) const {
  const css::ComputedStyle& style = group.Style();
  BoxGeometry& geometry = group.Geometry();
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

  const float group_left = content_left + margin_left + border_left + padding_left;
  const float group_top = start_y + style.margin.top.Resolve(style.font_size) +
                          geometry.border.top.Resolve(style.font_size) +
                          style.padding.top.Resolve(style.font_size);
  const float group_width = std::max(0.0f, content_width - horizontal);

  float y = group_top;
  for (const std::unique_ptr<Box>& child : group.Children()) {
    if (IsTableRow(child->Style().display)) {
      y += LayoutTableRow(*child, group_left, group_width, y, column_count);
      continue;
    }
    if (IsTableColumnBox(child->Style().display)) {
      child->Geometry().content = gfx::FloatRect{group_left, y, 0.0f, 0.0f};
    }
  }

  geometry.content = gfx::FloatRect{group_left, group_top, group_width, y - group_top};
  return y + style.padding.bottom.Resolve(style.font_size) +
         geometry.border.bottom.Resolve(style.font_size) +
         style.margin.bottom.Resolve(style.font_size) - start_y;
}

float LayoutEngine::LayoutTableRow(Box& row, float content_left, float content_width,
                                   float start_y, std::size_t column_count) const {
  const css::ComputedStyle& style = row.Style();
  BoxGeometry& geometry = row.Geometry();
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

  const float row_left = content_left + margin_left + border_left + padding_left;
  const float row_top = start_y + style.margin.top.Resolve(style.font_size) +
                        geometry.border.top.Resolve(style.font_size) +
                        style.padding.top.Resolve(style.font_size);
  const float row_width = std::max(0.0f, content_width - horizontal);
  const float cell_width = row_width / static_cast<float>(std::max<std::size_t>(1, column_count));

  float row_bottom = row_top;
  std::size_t column = 0;
  for (const std::unique_ptr<Box>& child : row.Children()) {
    if (!IsTableCell(child->Style().display)) {
      continue;
    }
    const float cell_left = row_left + cell_width * static_cast<float>(column);
    float cell_cursor = row_top;
    FloatContext cell_floats;
    LayoutBlock(*child, cell_left, cell_width, cell_cursor, cell_floats);
    row_bottom = std::max(row_bottom, cell_cursor);
    ++column;
  }

  geometry.content = gfx::FloatRect{row_left, row_top, row_width, row_bottom - row_top};
  return row_bottom + style.padding.bottom.Resolve(style.font_size) +
         geometry.border.bottom.Resolve(style.font_size) +
         style.margin.bottom.Resolve(style.font_size) - start_y;
}

}  // namespace microbrowser::layout
