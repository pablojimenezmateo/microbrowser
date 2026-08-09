#include <algorithm>
#include <cstddef>
#include <vector>

#include "layout/LayoutEngine.h"
#include "util/PerformanceCounters.h"

// Flex layout.
//
// The one place in this engine where children are sized against *each other*
// rather than each against the container. Everything else -- blocks, lines,
// even table columns -- can be laid out in one pass over the children in
// order. A flex line cannot: how wide the first item ends up depends on how
// much the last one wanted, so the items have to be measured, then resolved
// together, and only then placed.
//
// There is one implementation and not two. `row` and `column` differ only in
// which physical direction is "main", so every size below is a main or cross
// size and the mapping to x/y happens in exactly two places: where an item is
// measured and where it is laid out. Writing the algorithm once per direction
// is how the two drift apart, and the second one is always the one that is
// wrong.
//
// What is deliberately not here: baseline alignment (`align-items: baseline`
// falls back to flex-start, because a box's baseline is a property of its first
// line and the line boxes are not exposed), and percentage gaps. Main-axis
// `margin: auto` *is* here — youtube's masthead depends on it.
//
// Placement reuses the measured subtree when the final forced sizes match the
// measuring ones (TD-0001): geometry is absolute, so a change of origin is an
// OffsetLaidOutSubtree rather than a second LayoutBlock. Stretch still
// re-lays out, because it changes a cross-axis content size.

namespace microbrowser::layout {

namespace {

// One flex item, through the algorithm.
struct Item {
  Box* box = nullptr;
  // Everything outside the content box on each axis: margin, border, padding.
  // Carried because every size below is an *outer* size -- the algorithm
  // distributes space between margin boxes, and converting at each step is
  // where a sign error lives.
  float main_extra = 0.0f;
  float cross_extra = 0.0f;
  // Auto margins on the main axis (0–2). Free space after flexing goes to
  // these equally; without that, youtube's masthead end-cluster (`margin-left:
  // auto`) sits after a max-content search box and overflows the viewport.
  int auto_main_margins = 0;
  // The size the item would take if nothing flexed, and the size it was given.
  float base_main = 0.0f;
  float outer_main = 0.0f;
  float outer_cross = 0.0f;
  float main_position = 0.0f;
  float cross_position = 0.0f;
  // Where the measuring LayoutBlock placed the margin box. Placement is a
  // translation from here when the forced sizes did not change.
  float measure_left = 0.0f;
  float measure_top = 0.0f;
  bool measured = false;
  // Resolved cross-axis content size when the item says a definite or
  // percentage height/width against a definite container cross size.
  std::optional<float> forced_cross;
};

// A run of items that fit on one line, and how tall that line is.
struct Line {
  std::size_t begin = 0;
  std::size_t end = 0;
  float cross_size = 0.0f;
  float cross_position = 0.0f;
};

bool IsRow(css::FlexDirection direction) {
  return direction == css::FlexDirection::Row || direction == css::FlexDirection::RowReverse;
}

bool IsReversed(css::FlexDirection direction) {
  return direction == css::FlexDirection::RowReverse ||
         direction == css::FlexDirection::ColumnReverse;
}

// The space before the first item and between each pair, for one distribution.
//
// Returned together because every mode is exactly these two numbers, and
// computing them apart is how `space-around` ends up with the wrong first gap.
struct Spacing {
  float leading = 0.0f;
  float between = 0.0f;
};

Spacing Distribute(css::Distribution mode, float free_space, std::size_t count) {
  Spacing spacing;
  if (count == 0) {
    return spacing;
  }
  // Nothing left over means every mode agrees, and a negative leftover means
  // the items overflow -- which they do from the start edge, whatever the
  // distribution says.
  if (free_space <= 0.0f) {
    return spacing;
  }
  const auto items = static_cast<float>(count);
  switch (mode) {
    case css::Distribution::FlexStart:
    case css::Distribution::Stretch:
      break;
    case css::Distribution::FlexEnd:
      spacing.leading = free_space;
      break;
    case css::Distribution::Center:
      spacing.leading = free_space / 2.0f;
      break;
    case css::Distribution::SpaceBetween:
      // A single item gets no gap and stays at the start, which is the case
      // that makes this different from space-around.
      spacing.between = count > 1 ? free_space / (items - 1.0f) : 0.0f;
      break;
    case css::Distribution::SpaceAround:
      // Half a gap at each end, so the outer spaces are half the inner ones.
      spacing.between = free_space / items;
      spacing.leading = spacing.between / 2.0f;
      break;
    case css::Distribution::SpaceEvenly:
      spacing.between = free_space / (items + 1.0f);
      spacing.leading = spacing.between;
      break;
  }
  return spacing;
}

// Where an item sits within its line's cross size.
float AlignOffset(css::Alignment alignment, float line_cross, float item_cross) {
  const float leftover = line_cross - item_cross;
  switch (alignment) {
    case css::Alignment::FlexEnd:
      return std::max(0.0f, leftover);
    case css::Alignment::Center:
      return std::max(0.0f, leftover / 2.0f);
    case css::Alignment::Auto:
    case css::Alignment::Stretch:
    case css::Alignment::FlexStart:
    // A box's baseline is a property of its first line box, and line boxes are
    // not exposed outside inline layout. Until they are, this is flex-start --
    // which is what `baseline` looks like for the single-line-of-text items it
    // is nearly always used on.
    case css::Alignment::Baseline:
      break;
  }
  return 0.0f;
}

}  // namespace

float LayoutEngine::LayoutFlexChildren(Box& box, float content_left, float content_width,
                                       float start_y,
                                       std::optional<float> definite_main_height,
                                       std::optional<float> definite_cross_size) const {
  const css::ComputedStyle& style = box.Style();
  const css::ComputedStyle::FlexStyle& flex = style.flex;
  const bool row = IsRow(flex.direction);
  const float font_size = style.font_size;

  // The container's cross size, when it has one. A row container with an auto
  // height has none until its lines are measured, which is what makes
  // `align-content` and a stretched line different questions from
  // `align-items`. A forced or stated height on a row *is* definite (TD-0028).
  //
  // A column's main size is the definite content height when the caller has
  // one. Zero means indefinite: measure by content, and do not grow or shrink
  // (there is nothing to flex against).
  const float main_size = row ? content_width : definite_main_height.value_or(0.0f);
  const float cross_size =
      row ? definite_cross_size.value_or(0.0f) : content_width;
  const float main_gap = row ? flex.column_gap : flex.row_gap;
  const float cross_gap = row ? flex.row_gap : flex.column_gap;

  // --- Collect the items ----------------------------------------------------

  std::vector<Item> items;
  for (const std::unique_ptr<Box>& child : box.Children()) {
    if (!child->Style().GeneratesBox()) {
      continue;
    }
    // Absolutely positioned children are not flex items (CSS Flexbox §4).
    // youtube's masthead keeps `#search-container` absolute inside the flex
    // row; treating it as an item left `#masthead-skeleton-icons { flex: 1 }`
    // with no free space and packed the end chips past the viewport.
    if (child->IsAbsolutelyPositioned()) {
      continue;
    }
    Item item;
    item.box = child.get();
    items.push_back(item);
  }
  if (items.empty()) {
    return 0.0f;
  }

  // `order` reorders the items and nothing else -- it changes where they are
  // drawn, not what they are. Stable, so equal orders keep document order,
  // which is the whole reason the default value is a number rather than a
  // flag.
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    return a.box->Style().flex.order < b.box->Style().flex.order;
  });

  // --- Measure each item ----------------------------------------------------

  FloatContext floats;  // a flex container establishes its own formatting context
  for (Item& item : items) {
    const css::ComputedStyle& item_style = item.box->Style();
    const css::Edges& border = item_style.has_border ? item_style.border_width : css::Edges{};
    const auto extra = [&](bool horizontal, int* auto_margins) {
      const css::Edges& margin = item_style.margin;
      const css::Edges& padding = item_style.padding;
      // An auto margin contributes zero to the base outer size; free space is
      // assigned to it after flexing (see the place loop below).
      const auto resolve = [font_size](const css::Length& length) {
        return length.IsAuto() ? 0.0f : length.Resolve(font_size);
      };
      if (horizontal) {
        if (auto_margins != nullptr) {
          *auto_margins = (margin.left.IsAuto() ? 1 : 0) + (margin.right.IsAuto() ? 1 : 0);
        }
        return resolve(margin.left) + resolve(margin.right) + resolve(padding.left) +
               resolve(padding.right) + resolve(border.left) + resolve(border.right);
      }
      if (auto_margins != nullptr) {
        *auto_margins = (margin.top.IsAuto() ? 1 : 0) + (margin.bottom.IsAuto() ? 1 : 0);
      }
      return resolve(margin.top) + resolve(margin.bottom) + resolve(padding.top) +
             resolve(padding.bottom) + resolve(border.top) + resolve(border.bottom);
    };
    item.main_extra = extra(row, &item.auto_main_margins);
    item.cross_extra = extra(!row, nullptr);

    // The flex base size: `flex-basis` if it says something, then the size
    // property on the main axis, then the content. The order is the spec's and
    // it matters -- `flex-basis: 0` with a declared width is zero, which is
    // what makes `flex: 1` distribute space evenly regardless of content.
    //
    // A *percentage* flex-basis against an indefinite main size is treated as
    // `auto` (CSS Flexbox §7.1 / CSS Sizing). `flex: 1` is `flex-basis: 0%`;
    // resolving that to 0 when the container's height is auto collapsed
    // youtube's feed nudge (`min-height: 0`) to a 0px box while its title still
    // laid out at ~127px inside (TD-0028).
    const css::Length& basis = item_style.flex.basis;
    const css::Length& main_length = row ? item_style.width : item_style.height;
    const css::Length& cross_length = row ? item_style.height : item_style.width;
    const bool basis_as_auto =
        basis.IsAuto() || (basis.IsPercent() && main_size <= 0.0f);
    // Percentage cross sizes resolve against a definite container cross size
    // (CSS Flexbox §4.5 / CSS 2.1 §10.5). Without this, `height:100%` on a row
    // item under a definite-height flex left the item content-sized and disabled
    // stretch (height not auto) — youtube `ytd-browse { height:100% }` stayed ~16px.
    std::optional<float> forced_cross;
    if (cross_size > 0.0f && cross_length.IsPercent()) {
      forced_cross = cross_length.Used(cross_size, font_size);
    } else if (!cross_length.IsAuto() && !cross_length.IsPercent()) {
      forced_cross = cross_length.Resolve(font_size);
    }
    item.forced_cross = forced_cross;
    if (!basis_as_auto) {
      item.base_main = basis.Used(main_size, font_size);
      item.base_main += item.main_extra;
    } else if (!main_length.IsAuto() &&
               !(main_length.IsPercent() && main_size <= 0.0f)) {
      // Same indefinite-% rule as flex-basis: `height:100%` (or width in a row)
      // against an auto-sized flex container is treated as `auto` and measured
      // from content. Resolving against 0 collapsed youtube wrappers that set
      // both `flex:1` *and* `height:100%` (TD-0028).
      item.base_main = main_length.Used(main_size, font_size);
      item.base_main += item.main_extra;
    } else if (row) {
      item.base_main = MaxContentWidth(*item.box);
    } else {
      // A column's base size is a height, and the only way to know a box's
      // height is to lay it out. Measured against the container's cross size,
      // which is the width it will actually have. Kept as the measuring pass
      // when flexing does not change the height.
      float probe = 0.0f;
      item.measure_left = content_left;
      item.measure_top = probe;
      ForcedSize probe_forced;
      if (forced_cross.has_value()) {
        probe_forced.content_width = *forced_cross;
      }
      LayoutBlock(*item.box, content_left, cross_size, probe, floats, false,
                  forced_cross.has_value() ? &probe_forced : nullptr);
      item.base_main = probe;
      item.measured = true;
    }
    // Clamped as an *outer* size, which is what the rest of the algorithm
    // distributes: the bound is on the content box, so the extras go back on
    // afterwards. Clamped again after flexing, in the loop that resolves the
    // lengths -- this is the bound on the *base* size, which is what an item
    // starts from.
    const float bounded = row ? item_style.ClampWidth(item.base_main - item.main_extra, main_size)
                              : item_style.ClampHeight(item.base_main - item.main_extra,
                                                       item.base_main - item.main_extra);
    item.base_main = std::max(0.0f, bounded + item.main_extra);
    item.outer_main = item.base_main;
    if (!row && item.measured && item.outer_main != item.box->Geometry().MarginBox().height) {
      // A min/max-height changed the outer size: the probe's geometry is for
      // the unclamped height and must be redone under the forced size.
      item.measured = false;
    }
  }

  // --- Break into lines -----------------------------------------------------

  std::vector<Line> lines;
  const bool wraps = flex.wrap != css::FlexWrap::NoWrap;
  if (!wraps || main_size <= 0.0f) {
    lines.push_back(Line{0, items.size(), 0.0f, 0.0f});
  } else {
    std::size_t begin = 0;
    float used = 0.0f;
    for (std::size_t i = 0; i < items.size(); ++i) {
      const float with_gap = (i == begin ? 0.0f : main_gap) + items[i].outer_main;
      // An item that does not fit starts a new line -- unless it is the first
      // on this one, in which case it overflows rather than leaving a line
      // empty.
      if (i != begin && used + with_gap > main_size) {
        lines.push_back(Line{begin, i, 0.0f, 0.0f});
        begin = i;
        used = items[i].outer_main;
        continue;
      }
      used += with_gap;
    }
    lines.push_back(Line{begin, items.size(), 0.0f, 0.0f});
  }

  // --- Resolve the flexible lengths -----------------------------------------

  for (const Line& line : lines) {
    const auto count = static_cast<float>(line.end - line.begin);
    const float gaps = std::max(0.0f, count - 1.0f) * main_gap;
    // No definite main size means nothing grows or shrinks — there is no free
    // space to speak of. That covers an auto-height column and a zero-width
    // row; a column that arrived with a definite height (or a max-height that
    // bound the content) flexes here like a row does against its width.
    if (main_size <= 0.0f) {
      continue;
    }

    // Freeze and redistribute, which is the part of the spec that is easy to
    // leave out and visible when it is. An item that hits a bound stops
    // flexing, and the space it did not take is shared out again among the
    // ones still able to move. Without the loop, a `flex: 1` item beside a
    // capped sibling stops halfway and leaves a gap -- which is exactly what
    // a broken flex row looks like.
    //
    // Bounded by the item count: every round freezes at least one item, or it
    // is the last round.
    std::vector<bool> frozen(items.size(), false);
    for (std::size_t round = 0; round <= line.end - line.begin; ++round) {
      float taken = gaps;
      float total_grow = 0.0f;
      float total_weighted_shrink = 0.0f;
      for (std::size_t i = line.begin; i < line.end; ++i) {
        taken += items[i].outer_main;
        if (frozen[i]) {
          continue;
        }
        total_grow += items[i].box->Style().flex.grow;
        total_weighted_shrink += items[i].box->Style().flex.shrink * items[i].base_main;
      }
      const float free_space = main_size - taken;
      const bool growing = free_space > 0.0f;
      const float total = growing ? total_grow : total_weighted_shrink;
      if (free_space == 0.0f || total <= 0.0f) {
        break;
      }

      bool froze_any = false;
      for (std::size_t i = line.begin; i < line.end; ++i) {
        if (frozen[i]) {
          continue;
        }
        Item& item = items[i];
        const css::ComputedStyle& item_style = item.box->Style();
        const float share = growing ? item_style.flex.grow / total
                                    : item_style.flex.shrink * item.base_main / total;
        const float wanted = std::max(item.main_extra, item.outer_main + free_space * share);
        const float allowed =
            row ? item_style.ClampWidth(wanted - item.main_extra, main_size) + item.main_extra
                : item_style.ClampHeight(wanted - item.main_extra, wanted - item.main_extra) +
                      item.main_extra;
        item.outer_main = allowed;
        // A bound that bit is what freezes the item: it cannot take any more
        // of the space, so the next round shares what is left without it.
        if (allowed != wanted) {
          frozen[i] = true;
          froze_any = true;
        }
      }
      if (!froze_any) {
        break;  // everything moved freely, so the space is fully distributed
      }
    }
  }

  // --- Lay each item out at its resolved main size --------------------------

  for (Line& line : lines) {
    for (std::size_t i = line.begin; i < line.end; ++i) {
      Item& item = items[i];
      if (!row && item.measured &&
          item.outer_main == item.box->Geometry().MarginBox().height) {
        // Column probe above already laid the item out at the final main size
        // (indefinite column, or flexing left this item unchanged). Reuse it.
        item.outer_cross = item.box->Geometry().MarginBox().width;
        line.cross_size = std::max(line.cross_size, item.outer_cross);
        continue;
      }
      ForcedSize forced;
      float cursor = 0.0f;
      item.measure_left = content_left;
      item.measure_top = cursor;
      if (row) {
        forced.content_width = std::max(0.0f, item.outer_main - item.main_extra);
        if (item.forced_cross.has_value()) {
          forced.content_height = *item.forced_cross;
        }
        LayoutBlock(*item.box, content_left, item.outer_main, cursor, floats, false, &forced);
      } else {
        forced.content_height = std::max(0.0f, item.outer_main - item.main_extra);
        if (item.forced_cross.has_value()) {
          forced.content_width = *item.forced_cross;
        }
        LayoutBlock(*item.box, content_left, cross_size, cursor, floats, false, &forced);
      }
      item.measured = true;
      // What the item actually occupies across the axis it was not sized on.
      item.outer_cross =
          row ? (item.forced_cross.has_value() ? *item.forced_cross + item.cross_extra : cursor)
              : item.box->Geometry().MarginBox().width;
      line.cross_size = std::max(line.cross_size, item.outer_cross);
    }
  }

  // A single-line row with a definite container height: the line's cross size
  // is the container's (CSS Flexbox §9.4). Content-sized lines alone left
  // ForcedSize height as a border-box lie that stretch never saw.
  if (row && definite_cross_size.has_value() && lines.size() == 1) {
    lines[0].cross_size = std::max(lines[0].cross_size, *definite_cross_size);
  }

  // --- Place the lines, then the items on them ------------------------------

  float lines_total = std::max(0.0f, static_cast<float>(lines.size()) - 1.0f) * cross_gap;
  for (const Line& line : lines) {
    lines_total += line.cross_size;
  }
  // The container's cross size is what its lines need. A definite one would
  // let `align-content` distribute the difference; there is none to distribute
  // when the size is derived from the content, so the lines simply stack.
  const Spacing line_spacing = Distribute(flex.align_content, 0.0f, lines.size());
  float cross_cursor = line_spacing.leading;
  for (Line& line : lines) {
    line.cross_position = cross_cursor;
    cross_cursor += line.cross_size + cross_gap + line_spacing.between;
  }
  if (flex.wrap == css::FlexWrap::WrapReverse) {
    for (Line& line : lines) {
      line.cross_position = lines_total - line.cross_position - line.cross_size;
    }
  }

  for (const Line& line : lines) {
    const std::size_t count = line.end - line.begin;
    float used = std::max(0.0f, static_cast<float>(count) - 1.0f) * main_gap;
    int auto_margins = 0;
    for (std::size_t i = line.begin; i < line.end; ++i) {
      used += items[i].outer_main;
      auto_margins += items[i].auto_main_margins;
    }
    // Auto margins absorb free space before justify-content. Spec: if any
    // main-axis margin is auto, justify-content does not distribute that space.
    const float free_space = main_size > 0.0f ? main_size - used : 0.0f;
    const float auto_share =
        (auto_margins > 0 && free_space > 0.0f) ? free_space / static_cast<float>(auto_margins)
                                                : 0.0f;
    const float justify_free = auto_share > 0.0f ? 0.0f : free_space;
    const Spacing spacing = Distribute(flex.justify_content, justify_free, count);

    // Positions first, then layout. A reversed direction is a mirror of the
    // whole line rather than a backwards walk over it: `row-reverse` moves the
    // main-*start* edge to the right, so the leftover space that
    // justify-content did not claim ends up on the left. Placing the items
    // backwards from the left edge instead would reverse their order and put
    // the free space on the wrong side, which looks right until there is any.
    float main_cursor = spacing.leading;
    for (std::size_t i = line.begin; i < line.end; ++i) {
      const css::Edges& margin = items[i].box->Style().margin;
      if (auto_share > 0.0f) {
        if (row) {
          if ((IsReversed(flex.direction) ? margin.right : margin.left).IsAuto()) {
            main_cursor += auto_share;
          }
        } else if ((IsReversed(flex.direction) ? margin.bottom : margin.top).IsAuto()) {
          main_cursor += auto_share;
        }
      }
      items[i].main_position = main_cursor;
      main_cursor += items[i].outer_main + main_gap + spacing.between;
      if (auto_share > 0.0f) {
        if (row) {
          if ((IsReversed(flex.direction) ? margin.left : margin.right).IsAuto()) {
            main_cursor += auto_share;
          }
        } else if ((IsReversed(flex.direction) ? margin.top : margin.bottom).IsAuto()) {
          main_cursor += auto_share;
        }
      }
    }
    if (IsReversed(flex.direction)) {
      // Mirrored against the line's own extent for a column, whose main size
      // is not known until the items are placed, and against the container's
      // for a row, where it is. A definite column height is known too.
      const float extent = main_size > 0.0f
                               ? main_size
                               : std::max(0.0f, main_cursor - main_gap - spacing.between);
      for (std::size_t i = line.begin; i < line.end; ++i) {
        items[i].main_position = extent - items[i].main_position - items[i].outer_main;
      }
    }

    for (std::size_t i = line.begin; i < line.end; ++i) {
      Item& item = items[i];
      const css::Alignment self = item.box->Style().flex.align_self;
      const css::Alignment alignment = self == css::Alignment::Auto ? flex.align_items : self;
      item.cross_position =
          line.cross_position + AlignOffset(alignment, line.cross_size, item.outer_cross);

      // Stretch is the default, and it is the reason an item's cross size is
      // set here rather than left as it was measured: a row of boxes with
      // different amounts of text ends up the same height, which is what a
      // page expects and what `align-items: flex-start` turns off.
      const bool stretches = alignment == css::Alignment::Stretch &&
                             (row ? item.box->Style().height.IsAuto()
                                  : item.box->Style().width.IsAuto());
      const float used_cross = stretches ? line.cross_size : item.outer_cross;

      ForcedSize forced;
      float cursor = row ? start_y + item.cross_position : start_y + item.main_position;
      const float place_left =
          row ? content_left + item.main_position : content_left + item.cross_position;
      // Stretch that does not change the measured cross size is still a
      // translation: the tallest item on a line, every item on a one-item line,
      // and any row whose children already share a height. Only a short item
      // being forced taller (or a narrow column item being forced wider) needs
      // another LayoutBlock.
      const bool size_changed = stretches && used_cross != item.outer_cross;
      if (item.measured && !size_changed) {
        OffsetLaidOutSubtree(*item.box, place_left - item.measure_left, cursor - item.measure_top);
        util::AddPerformanceCounter(util::PerfCounterId::LayoutMeasureCacheHits);
      } else {
        if (row) {
          forced.content_width = std::max(0.0f, item.outer_main - item.main_extra);
          if (stretches) {
            forced.content_height = std::max(0.0f, used_cross - item.cross_extra);
          }
        } else {
          forced.content_height = std::max(0.0f, item.outer_main - item.main_extra);
        }
        LayoutBlock(*item.box, place_left, row ? item.outer_main : used_cross, cursor, floats,
                    false, &forced);
        util::AddPerformanceCounter(util::PerfCounterId::LayoutMeasureCacheMisses);
      }
      item.outer_cross = used_cross;
    }
  }

  // The height the container's content occupies: across the lines for a row,
  // along the main axis for a column. A definite column height is the used
  // content height even when items overflow it (flex-shrink: 0) — clamping the
  // container after the fact without this would disagree with the size flexing
  // resolved against. A definite row cross size is the same for the other axis.
  if (row) {
    if (definite_cross_size.has_value()) {
      return *definite_cross_size;
    }
    return lines_total;
  }
  if (main_size > 0.0f) {
    return main_size;
  }
  float tallest = 0.0f;
  for (const Item& item : items) {
    tallest = std::max(tallest, item.main_position + item.outer_main);
  }
  return tallest;
}

}  // namespace microbrowser::layout
