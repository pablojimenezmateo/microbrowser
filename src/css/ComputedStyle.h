#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

#include "gfx/Color.h"

namespace microbrowser::css {

enum class Display : std::uint8_t {
  Inline,
  Block,
  InlineBlock,
  ListItem,
  Table,
  TableCaption,
  TableColumnGroup,
  TableColumn,
  TableHeaderGroup,
  TableFooterGroup,
  TableRowGroup,
  TableRow,
  TableCell,
  Flex,
  InlineFlex,
  None,
};

enum class FlexDirection : std::uint8_t { Row, RowReverse, Column, ColumnReverse };
enum class FlexWrap : std::uint8_t { NoWrap, Wrap, WrapReverse };
// One enum for `justify-content` and `align-content`, because the spec gives
// them the same value set and a second copy would be a second thing to keep in
// step.
enum class Distribution : std::uint8_t {
  FlexStart,
  FlexEnd,
  Center,
  SpaceBetween,
  SpaceAround,
  SpaceEvenly,
  Stretch,
};
// `align-items` and `align-self` share this. `Auto` is only meaningful on
// `align-self`, where it means "whatever the container says" -- which is why
// the two are one enum with a value the other never takes.
enum class Alignment : std::uint8_t { Auto, Stretch, FlexStart, FlexEnd, Center, Baseline };
enum class FontStyle : std::uint8_t { Normal, Italic };
enum class TextAlign : std::uint8_t { Left, Right, Center, Justify };

enum class BackgroundRepeat : std::uint8_t { Repeat, RepeatX, RepeatY, NoRepeat };
enum class WhiteSpace : std::uint8_t { Normal, Pre, NoWrap, PreWrap };

// Taken out of the normal flow and shifted to one side, with the following
// line boxes shortened around it. Not a display value: a float is a
// block-level box wherever it came from, which is why `float: left` on a span
// makes it a block.
enum class Float : std::uint8_t { None, Left, Right };

// Moves a box below the floats on the named side. `Both` is not the union of
// two decisions -- it is one decision about the lowest of them -- which is why
// it is a value here rather than two booleans.
enum class Clear : std::uint8_t { None, Left, Right, Both };

// Where a box is placed relative to where the flow would have put it.
//
// The split that matters is not four ways but two: `Static` and `Relative`
// stay in the flow and take up space, `Absolute` and `Fixed` do not. Which is
// why the question layout asks is `IsOutOfFlow`, and the enum is only ever
// read to answer it and to pick a containing block.
enum class Position : std::uint8_t { Static, Relative, Absolute, Fixed };

// What happens to content that does not fit its box.
//
// Only the first is different in kind: `Visible` lets content escape, and the
// other three all clip it. Whether the overflow can then be *scrolled* back
// into view is a separate question from whether it is clipped, and only the
// clipping half is layout's.
enum class Overflow : std::uint8_t { Visible, Hidden, Scroll, Auto };

// A CSS length, resolved as far as it can be without a layout context.
//
// Percentages cannot be resolved here — they need a containing block, which
// does not exist until layout runs — so they are carried rather than
// collapsed. A style system that resolved them early would have to guess at the
// containing block, and the guess is wrong for every element inside a float.
struct Length {
  enum class Unit : std::uint8_t { Pixels, Percent, Em, Rem, Auto };

  float value = 0.0f;
  Unit unit = Unit::Pixels;

  static Length Auto() { return Length{0.0f, Unit::Auto}; }
  static Length Pixels(float value) { return Length{value, Unit::Pixels}; }

  bool IsAuto() const { return unit == Unit::Auto; }
  bool IsPercent() const { return unit == Unit::Percent; }

  // Absolute pixels, given the font size this length is relative to. Percentage
  // and auto have no answer without a containing block, so they return the
  // fallback the caller supplies rather than silently becoming zero.
  float Resolve(float font_size, float fallback = 0.0f) const;

  friend bool operator==(const Length&, const Length&) = default;
};

struct Edges {
  Length top;
  Length right;
  Length bottom;
  Length left;

  friend bool operator==(const Edges&, const Edges&) = default;
};

// Everything `background-image` needs beyond the pixels.
//
// One value rather than five fields on ComputedStyle because they are one
// concept: nothing here means anything without `image`, and a shorthand that
// sets the image resets all of them together. Keeping them apart made the
// style struct read as though a page could have a background position with no
// background.
//
// A single layer. CSS allows a list, and a page that writes one gets its first
// image -- see where the shorthand is parsed for why the rest are dropped
// rather than approximated.
struct BackgroundLayer {
  // The `url()`, exactly as the stylesheet wrote it, or empty. Resolving it
  // against the document is the loader's job: the cascade does not know what a
  // base URL is, and doing it in two places is how the two disagree.
  std::string image;
  BackgroundRepeat repeat = BackgroundRepeat::Repeat;
  // `auto` on an axis means the image's own size there, which is what keeps an
  // icon's proportions when a stylesheet gives only a width.
  Length size_x = Length::Auto();
  Length size_y = Length::Auto();
  // A percentage is a fraction of the space the image does *not* fill, which is
  // what makes `50%` centre rather than offset by half the box.
  Length position_x;
  Length position_y;

  friend bool operator==(const BackgroundLayer&, const BackgroundLayer&) = default;
};

// The style of one element, after the cascade.
//
// Every property is resolved to a value — there is no "unset" state to check at
// use time. That is what makes layout able to read a style without asking
// whether each field was ever set, and it is why inheritance happens here
// rather than being a lookup that walks the tree on every read.
struct ComputedStyle {
  Display display = Display::Inline;
  gfx::Color color = gfx::Color::Rgb(0, 0, 0);
  gfx::Color background_color = gfx::Color::Transparent();
  BackgroundLayer background;

  // Inherited. Absolute pixels: font-size is the one length that must be
  // resolved during the cascade, because `em` on every other property is
  // relative to it.
  float font_size = 16.0f;
  float font_weight = 400.0f;
  FontStyle font_style = FontStyle::Normal;
  // The families the stylesheet named, best first. A list rather than a name
  // because that is what CSS says: `font-family: Verdana, Geneva, sans-serif`
  // asks for three fonts and settles for the first that exists. Which of them
  // exists is a property of the machine, so the choice cannot be made here --
  // the whole list travels to the font provider, which is the only thing that
  // knows. Bounded at gfx::kMaxFontFamilies where it is parsed.
  std::vector<std::string> font_family{"sans-serif"};
  // Zero means "normal", which is a multiple of the font size rather than a
  // length, and is resolved by layout.
  float line_height = 0.0f;

  TextAlign text_align = TextAlign::Left;
  // Set by `text-align: -microbrowser-center`, which is what <center> means and
  // what no standard value expresses -- every engine carries an equivalent
  // (`-moz-center`, `-webkit-center`). Not inherited, unlike text_align: see
  // LayoutEngine::LayoutBlock's `center_in_container`.
  bool centers_block_children = false;
  WhiteSpace white_space = WhiteSpace::Normal;
  Float css_float = Float::None;
  Clear clear = Clear::None;

  Position position = Position::Static;
  // Per axis, because a page sets them separately as often as together --
  // `overflow-x: hidden` with `overflow-y: auto` is the ordinary way to write
  // a vertical scroller.
  Overflow overflow_x = Overflow::Visible;
  Overflow overflow_y = Overflow::Visible;
  // `top`/`right`/`bottom`/`left`. All four default to `auto`, which for a
  // relative box means "no offset" and for an absolute one means "wherever the
  // flow would have put it" -- two different meanings for the same value, and
  // the reason they cannot default to zero.
  Edges inset{Length::Auto(), Length::Auto(), Length::Auto(), Length::Auto()};

  Edges margin;
  Edges padding;
  Edges border_width;
  gfx::Color border_color = gfx::Color::Rgb(0, 0, 0);
  bool has_border = false;

  Length width = Length::Auto();
  Length height = Length::Auto();
  // The bounds on the two above. A minimum of zero and a maximum of `auto`
  // are the initial values, and `auto` here reads as "none" -- the property
  // spells it that way and Length has no separate word for it, so the one
  // meaning "unbounded" is reused rather than a fifth unit added for it.
  Length min_width = Length::Pixels(0.0f);
  Length max_width = Length::Auto();
  Length min_height = Length::Pixels(0.0f);
  Length max_height = Length::Auto();

  // A used size, clamped by its bounds. Applied after a size is decided by
  // whatever decided it -- the block model, shrink-to-fit, or the flex
  // algorithm -- which is what lets one rule serve all three.
  //
  // Maximum first, then minimum, in that order: the spec resolves the two that
  // way and it is observable when they contradict each other. A `min-width`
  // larger than a `max-width` wins, and a page that writes both means the
  // minimum.
  float ClampWidth(float used, float container) const {
    return ClampBy(used, min_width, max_width, container);
  }
  float ClampHeight(float used, float container) const {
    return ClampBy(used, min_height, max_height, container);
  }

  // The flex properties, grouped.
  //
  // Twelve fields for one feature, and they are only ever read together --
  // loose on ComputedStyle they would be more than half its members and would
  // say nothing about belonging to each other. The container reads the first
  // five and the item reads the rest, which is the only split that matters and
  // is written here rather than inferred.
  struct FlexStyle {
    // Read by the container.
    FlexDirection direction = FlexDirection::Row;
    FlexWrap wrap = FlexWrap::NoWrap;
    Distribution justify_content = Distribution::FlexStart;
    Alignment align_items = Alignment::Stretch;
    Distribution align_content = Distribution::Stretch;
    float row_gap = 0.0f;
    float column_gap = 0.0f;

    // Read by the item, from its own style.
    Alignment align_self = Alignment::Auto;
    float grow = 0.0f;
    // One, not zero: an item shrinks by default and grows only when asked,
    // which is the asymmetry that makes `flex: 1` mean something different
    // from the initial value.
    float shrink = 1.0f;
    Length basis = Length::Auto();
    int order = 0;

    friend bool operator==(const FlexStyle&, const FlexStyle&) = default;
  };
  FlexStyle flex;

  bool IsFloating() const { return css_float != Float::None; }

  // Out of the normal flow: it neither takes space from its siblings nor gets
  // any from them. A float is out of flow too, but differently -- it shortens
  // the lines beside it, which an absolutely positioned box does not.
  bool IsAbsolutelyPositioned() const {
    return position == Position::Absolute || position == Position::Fixed;
  }
  // Establishes a containing block for the absolutely positioned boxes inside
  // it. `static` does not, which is what makes `position: relative` with no
  // offsets the idiomatic way to anchor a child.
  bool IsPositioned() const { return position != Position::Static; }

  // Content that does not fit is cut off at the box's edge. True for every
  // value but `visible`, including the scrolling ones -- a scroller clips
  // what is outside it and offers the rest back, which is two behaviours and
  // only one of them belongs to paint.
  //
  // This is the declaration, not the used behaviour: `overflow` does not apply
  // to a non-replaced inline box at all, and only the *box* knows whether it is
  // one. Ask `layout::Box::ClipsOverflow()`, which asks this and then checks.
  // Getting that backwards cost every story title on old.reddit.com, whose
  // stylesheet puts `overflow: hidden` on an `<a>`.
  bool ClipsOverflow() const {
    return overflow_x != Overflow::Visible || overflow_y != Overflow::Visible;
  }

 private:
  float ClampBy(float used, const Length& low, const Length& high, float container) const {
    const auto resolve = [this, container](const Length& length) {
      return length.IsPercent() ? container * length.value / 100.0f
                                : length.Resolve(font_size, container);
    };
    if (!high.IsAuto()) {
      used = std::min(used, resolve(high));
    }
    if (!low.IsAuto()) {
      used = std::max(used, resolve(low));
    }
    return std::max(0.0f, used);
  }

 public:

  bool IsInlineLevel() const {
    // A float is block-level whatever it was declared as: `float: left` on a
    // span makes it a block, per CSS 2.1 s9.7. Answering that here rather than
    // at each call site is what keeps the rule from being applied in three
    // places and forgotten in a fourth.
    return !IsFloating() && (display == Display::Inline || display == Display::InlineBlock ||
                             display == Display::InlineFlex);
  }
  // Laid out inside like a block, placed outside like a replaced element.
  //
  // The float and absolute exclusions are the same blockification rule
  // IsInlineLevel states: an out-of-flow box is not on a line at all, so
  // "atomic inline" is not a thing it can be.
  bool IsAtomicInline() const {
    return !IsFloating() && !IsAbsolutelyPositioned() &&
           (display == Display::InlineBlock || display == Display::InlineFlex);
  }
  // A flex container lays its children out itself, so the box tree has to make
  // every one of them an item -- which is a different question from how the
  // container itself sits in its own parent.
  bool IsFlexContainer() const {
    return display == Display::Flex || display == Display::InlineFlex;
  }
  bool GeneratesBox() const { return display != Display::None; }

  // The custom properties in scope on this element, by name (with the leading
  // `--`), holding *unparsed* text.
  //
  // Unparsed is the whole point and the reason this is a member rather than
  // something the resolver could handle in passing: a custom property's value
  // is not a value until it is substituted somewhere, and where it lands
  // decides what it means. `--x: 20px` is a length in `padding` and a piece of
  // a shorthand in `margin: var(--x) 0`, and parsing it on the way in would
  // have to guess which.
  //
  // Inherited, which is what makes `--fg` set on `:root` reachable from every
  // element under it -- the way essentially every modern stylesheet is built.
  // A vector rather than a map because the counts are small per element and
  // the copy on inherit is the operation that happens most.
  std::vector<std::pair<std::string, std::string>> custom_properties;

  const std::string* CustomProperty(std::string_view name) const {
    for (const auto& entry : custom_properties) {
      if (entry.first == name) {
        return &entry.second;
      }
    }
    return nullptr;
  }
  void SetCustomProperty(std::string_view name, std::string value) {
    for (auto& entry : custom_properties) {
      if (entry.first == name) {
        entry.second = std::move(value);
        return;
      }
    }
    custom_properties.emplace_back(std::string(name), std::move(value));
  }

  friend bool operator==(const ComputedStyle&, const ComputedStyle&) = default;
};

// Replaces every `var(--name[, fallback])` in `value` with what `style` has
// for it, or with the fallback when it has none.
//
// False when a reference resolves to nothing and has no fallback. That is not
// "leave it alone": the declaration is then **invalid at computed-value time**,
// which is a defined outcome and not the same as an unrecognized one -- see
// ADR 0014 and the note in StyleResolver.cpp where it is acted on.
bool SubstituteVars(std::string_view value, const ComputedStyle& style, std::string& out);

// Parses a colour: named, `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`.
// Nullopt for anything unrecognized, which is how an invalid declaration is
// dropped rather than turning an element transparent.
std::optional<gfx::Color> ParseColor(std::string_view text);

// Parses a length. Nullopt when the text is not one.
std::optional<Length> ParseLength(std::string_view text);

// The flex properties and the sizing bounds. True when `property` was one of
// them, whether or not its value was usable -- an unusable value is a dropped
// declaration, not an unrecognized property.
//
// Separate from ApplyDeclaration because Declarations.cpp is at its module's
// line cap, and the cap means a missing translation unit rather than a bigger
// file.
bool ApplyBoxDeclaration(const std::string& property, const std::string& value,
                         const ComputedStyle& parent, ComputedStyle& style);

}  // namespace microbrowser::css
