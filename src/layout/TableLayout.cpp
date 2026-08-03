#include "layout/LayoutEngine.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>

#include "util/Parse.h"
#include "util/PerformanceTrace.h"

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

std::size_t ColumnSpan(const Box& cell) {
  if (cell.Origin() == nullptr) {
    return 1;
  }
  const std::string* attribute = cell.Origin()->GetAttribute("colspan");
  if (attribute == nullptr) {
    return 1;
  }
  const std::optional<int> parsed = util::ParseInt(*attribute);
  if (!parsed.has_value() || *parsed <= 0) {
    return 1;
  }
  return static_cast<std::size_t>(std::min(*parsed, 1000));
}

std::size_t CellCount(const Box& row) {
  std::size_t count = 0;
  for (const std::unique_ptr<Box>& child : row.Children()) {
    if (IsTableCell(child->Style().display)) {
      count += ColumnSpan(*child);
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

// Calls `visit(row)` for every row of `box`, in document order, descending
// through row groups. Rows and row groups are the only children that carry
// cells; a caption or a <col> is skipped here and handled where it is laid out.
template <typename Visitor>
void ForEachRow(const Box& box, Visitor&& visit) {
  for (const std::unique_ptr<Box>& child : box.Children()) {
    const css::Display display = child->Style().display;
    if (IsTableRow(display)) {
      visit(*child);
    } else if (IsTableRowGroup(display)) {
      ForEachRow(*child, visit);
    }
  }
}

// The horizontal edges a cell adds to whatever its content needs.
float CellEdges(const Box& cell) {
  const css::ComputedStyle& style = cell.Style();
  return style.padding.left.Resolve(style.font_size) +
         style.padding.right.Resolve(style.font_size) +
         (style.has_border ? style.border_width.left.Resolve(style.font_size) +
                                 style.border_width.right.Resolve(style.font_size)
                           : 0.0f);
}

// A width the cell states outright, in pixels. Percentages are excluded on
// purpose: a percentage cell width resolves against the table's used width,
// which is the number this measurement is being taken to compute. Feeding it
// back in is circular, and the wrong ways to break the cycle all produce a
// table whose columns change size depending on which pass measured them.
std::optional<float> DefiniteCellWidth(const Box& cell) {
  const css::ComputedStyle& style = cell.Style();
  if (style.width.IsAuto() || style.width.IsPercent()) {
    return std::nullopt;
  }
  return std::max(0.0f, style.width.Resolve(style.font_size)) + CellEdges(cell);
}

}  // namespace

// The narrowest and widest each column can be.
//
// This is CSS 2.1's automatic table layout, which is what a table with no
// `table-layout: fixed` gets -- and what essentially every table written in
// HTML relies on. Dividing the width evenly instead (which is what this used
// to do) gives a rank column the same third of the page as the article titles
// beside it.
//
// Two passes over the cells. Single-column cells set their column's bounds
// directly; a cell spanning several columns can only say something about the
// group, so it is applied afterwards and only when the columns it spans are
// already too narrow for it. Doing both in one pass would let a wide spanning
// cell inflate a column that a later single-column cell proves can be narrow.
TableColumnWidths LayoutEngine::MeasureTableColumns(const Box& table) const {
  util::PerformanceTrace::Scope scope("layout::MeasureTableColumns");
  const std::size_t column_count = std::max<std::size_t>(1, MaxTableColumns(table));
  TableColumnWidths widths;
  widths.min.assign(column_count, 0.0f);
  widths.max.assign(column_count, 0.0f);

  struct Spanning {
    std::size_t column;
    std::size_t span;
    float min;
    float max;
  };
  std::vector<Spanning> spanning;

  ForEachRow(table, [&](const Box& row) {
    std::size_t column = 0;
    for (const std::unique_ptr<Box>& cell : row.Children()) {
      if (!IsTableCell(cell->Style().display) || column >= column_count) {
        continue;
      }
      const std::size_t span = std::min(ColumnSpan(*cell), column_count - column);
      const float edges = CellEdges(*cell);
      float cell_min = MinContentWidth(*cell) + edges;
      float cell_max = MaxContentWidth(*cell) + edges;
      if (const std::optional<float> definite = DefiniteCellWidth(*cell)) {
        // A stated width is a floor for both bounds, not a ceiling: content
        // wider than the declared width overflows in CSS, but a *column*
        // narrower than its content is unreadable, and every table on the web
        // that states a width expects to get at least it.
        cell_min = std::max(cell_min, *definite);
        cell_max = std::max(cell_max, *definite);
      }
      if (span == 1) {
        widths.min[column] = std::max(widths.min[column], cell_min);
        widths.max[column] = std::max(widths.max[column], cell_max);
      } else {
        spanning.push_back(Spanning{column, span, cell_min, cell_max});
      }
      column += span;
    }
  });

  for (const Spanning& cell : spanning) {
    const auto spread = [&](std::vector<float>& bounds, float wanted) {
      float current = 0.0f;
      for (std::size_t i = 0; i < cell.span; ++i) {
        current += bounds[cell.column + i];
      }
      if (current >= wanted) {
        return;
      }
      // Shared out evenly rather than proportionally. Proportional sharing
      // reads better but multiplies a zero-width column by anything and leaves
      // it at zero, so a row of empty cells under a wide spanning header stays
      // collapsed and the header overflows all of them.
      const float share = (wanted - current) / static_cast<float>(cell.span);
      for (std::size_t i = 0; i < cell.span; ++i) {
        bounds[cell.column + i] += share;
      }
    };
    spread(widths.min, cell.min);
    spread(widths.max, cell.max);
  }

  for (std::size_t i = 0; i < column_count; ++i) {
    widths.max[i] = std::max(widths.max[i], widths.min[i]);
  }
  return widths;
}

std::vector<float> LayoutEngine::DistributeTableColumns(const TableColumnWidths& bounds,
                                                        float table_width) {
  const std::size_t count = bounds.min.size();
  std::vector<float> used(count, 0.0f);
  if (count == 0) {
    return used;
  }

  float total_min = 0.0f;
  float total_max = 0.0f;
  for (std::size_t i = 0; i < count; ++i) {
    total_min += bounds.min[i];
    total_max += bounds.max[i];
  }

  if (table_width <= total_min) {
    // Narrower than the content can go. Every column gets its minimum and the
    // table overflows, which is what a browser does: shrinking below the
    // minimum does not make the text fit, it only makes it unreadable.
    used = bounds.min;
    return used;
  }
  if (table_width <= total_max) {
    // Between the two: each column gets its minimum plus a share of the slack,
    // in proportion to how much room it could still use.
    const float slack = table_width - total_min;
    const float range = total_max - total_min;
    for (std::size_t i = 0; i < count; ++i) {
      const float want = bounds.max[i] - bounds.min[i];
      used[i] = bounds.min[i] + (range > 0.0f ? slack * want / range : slack /
                                                                          static_cast<float>(count));
    }
    return used;
  }

  // Wider than anything wants. The excess is shared in proportion to what each
  // column already takes, so a wide column stays wide -- distributing it evenly
  // would make a one-character rank column as wide as a headline.
  const float excess = table_width - total_max;
  for (std::size_t i = 0; i < count; ++i) {
    used[i] = bounds.max[i] + (total_max > 0.0f
                                   ? excess * bounds.max[i] / total_max
                                   : excess / static_cast<float>(count));
  }
  return used;
}

float LayoutEngine::LayoutTableChildren(Box& box, float content_left, float content_width,
                                        float start_y,
                                        std::optional<TableColumnWidths>& measured) const {
  // The caller has already measured when the table's own width was
  // shrink-to-fit. Measuring again would walk every cell in the table a second
  // time for an answer that cannot have changed.
  if (!measured.has_value()) {
    measured = MeasureTableColumns(box);
  }
  const std::vector<float> columns = DistributeTableColumns(*measured, content_width);
  float y = start_y;
  for (const std::unique_ptr<Box>& child : box.Children()) {
    const css::Display display = child->Style().display;
    if (display == css::Display::TableCaption) {
      FloatContext caption_floats;
      LayoutBlock(*child, content_left, content_width, y, caption_floats);
      continue;
    }
    if (IsTableRowGroup(display)) {
      y += LayoutTableRowGroup(*child, content_left, content_width, y, columns);
      continue;
    }
    if (IsTableRow(display)) {
      y += LayoutTableRow(*child, content_left, content_width, y, columns);
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
                                        float start_y,
                                        const std::vector<float>& columns) const {
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
      y += LayoutTableRow(*child, group_left, group_width, y, columns);
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
                                   float start_y, const std::vector<float>& columns) const {
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

  float row_bottom = row_top;
  std::size_t column = 0;
  float cell_left = row_left;
  for (const std::unique_ptr<Box>& child : row.Children()) {
    if (!IsTableCell(child->Style().display)) {
      continue;
    }
    if (column >= columns.size()) {
      break;
    }
    const std::size_t span = std::min(ColumnSpan(*child), columns.size() - column);
    float spanned_width = 0.0f;
    for (std::size_t i = 0; i < span; ++i) {
      spanned_width += columns[column + i];
    }
    float cell_cursor = row_top;
    FloatContext cell_floats;
    LayoutBlock(*child, cell_left, spanned_width, cell_cursor, cell_floats);
    row_bottom = std::max(row_bottom, cell_cursor);
    cell_left += spanned_width;
    column += span;
  }

  // A stated height is a floor, not a ceiling: a row is at least as tall as it
  // says and at least as tall as its tallest cell, because a cell clipped to a
  // shorter row would lose text. This is what a spacer row -- `<tr
  // style="height:5px">` with no cells at all -- is for, and without it every
  // such row collapses to nothing and the rows it was separating run together.
  if (!style.height.IsAuto() && !style.height.IsPercent()) {
    row_bottom = std::max(row_bottom, row_top + style.height.Resolve(style.font_size));
  }

  geometry.content = gfx::FloatRect{row_left, row_top, row_width, row_bottom - row_top};
  return row_bottom + style.padding.bottom.Resolve(style.font_size) +
         geometry.border.bottom.Resolve(style.font_size) +
         style.margin.bottom.Resolve(style.font_size) - start_y;
}

}  // namespace microbrowser::layout
