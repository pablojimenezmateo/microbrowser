#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

#include "css/Animation.h"
#include "css/BoxEdges.h"
#include "css/BackgroundStyle.h"
#include "css/Length.h"
#include "css/SvgStyle.h"
#include "css/MediaQuery.h"
#include "css/Transform.h"
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
  Grid,
  InlineGrid,
  None,
};

enum class FlexDirection : std::uint8_t { Row, RowReverse, Column, ColumnReverse };

// One grid track's size.
//
// ADR 0014 §6, session 39. Four kinds and no more, because they are the four the specification's sizing
// algorithm treats differently -- a fixed length, a percentage of the container, a share of what is left
// (`fr`), and "as big as the content needs" (`auto`). `minmax()` is expressed as a track with both a
// minimum and a maximum rather than as a fifth kind, because that is what it *is*: every other kind is a
// minmax with the two ends equal, and collapsing them means one sizing pass rather than two.
struct GridTrack {
  enum class Kind : std::uint8_t { Fixed, Percent, Fraction, Auto };

  Kind kind = Kind::Auto;
  // Pixels for `Fixed`, a percentage for `Percent`, the flex factor for `Fraction`, unused for `Auto`.
  float value = 0.0f;
  // The floor from a `minmax()`'s first argument, in pixels. Zero for a track written without one --
  // which is right: a `1fr` track's minimum is its content's minimum, and that is computed rather than
  // declared.
  float minimum = 0.0f;

  static GridTrack Pixels(float pixels) { return GridTrack{Kind::Fixed, pixels, 0.0f}; }
  static GridTrack Fr(float factor) { return GridTrack{Kind::Fraction, factor, 0.0f}; }

  friend bool operator==(const GridTrack&, const GridTrack&) = default;
};

// Where an item sits, as `grid-column` / `grid-row`.
//
// Line numbers rather than cell indices, because that is what the property takes and the off-by-one
// between them is the classic grid bug: `grid-column: 1 / 3` is two columns, not three, because those
// are the *lines* either side of them.
struct GridPlacement {
  // 1-based, and *signed*: a negative line counts from the end, which is how `grid-column: 1 / -1`
  // spans the whole row. Zero means `auto`.
  int start_line = 0;
  int end_line = 0;
  // `span N`, which is a length rather than a position -- so it combines with either end.
  int span = 0;

  bool IsAuto() const { return start_line == 0 && end_line == 0 && span == 0; }

  friend bool operator==(const GridPlacement&, const GridPlacement&) = default;
};
enum class FlexWrap : std::uint8_t { NoWrap, Wrap, WrapReverse };
// One enum for `justify-content` and `align-content`, because the spec gives
// them the same value set and a second copy would be a second thing to keep in
// step.
// `start`/`end` are *not* synonyms for `flex-start`/`flex-end` and folding them
// together is a layout bug rather than a serialization one: `flex-start` names
// the flex-relative edge, which `row-reverse` and `wrap-reverse` invert, while
// `start` names the writing mode's edge, which nothing in flexbox moves. A
// `wrap-reverse` container with `align-content: start` packs its lines at the
// top and with `flex-start` packs them at the bottom.
enum class Distribution : std::uint8_t {
  FlexStart,
  FlexEnd,
  Start,
  End,
  Center,
  SpaceBetween,
  SpaceAround,
  SpaceEvenly,
  Stretch,
};
// `align-items` and `align-self` share this. `Auto` is only meaningful on
// `align-self`, where it means "whatever the container says" -- which is why
// the two are one enum with a value the other never takes.
enum class Alignment : std::uint8_t {
  Auto,
  Stretch,
  FlexStart,
  FlexEnd,
  // Writing-mode-relative, and so unmoved by `wrap-reverse`. See Distribution.
  Start,
  End,
  Center,
  Baseline,
};
enum class FontStyle : std::uint8_t { Normal, Italic };
// `start` is the initial value, and it is a *distinct* value rather than a synonym for left:
// which edge it means depends on `direction`, and collapsing it to Left at parse time is why an
// unstyled right-to-left paragraph would hug the wrong margin.
enum class TextAlign : std::uint8_t { Start, End, Left, Right, Center, Justify };

// `direction`, which feeds the bidi paragraph level (UAX #9 rule P2, ADR 0025 §3). Inherited, and
// the only CSS property whose value reverses the order text is *painted* in.
enum class Direction : std::uint8_t { Ltr, Rtl };

// `unicode-bidi`: what a box does to the bidi algorithm around its own text. Each value is one pair
// of explicit control characters that UAX #9 already defines, which is why the property costs almost
// nothing here -- the algorithm was written with these in it.
//
// **Inherited in this browser, and it is not inherited in CSS.** The deviation is deliberate and its
// cost is bounded: what the property really applies to is an inline *box*, and line layout here is
// flattened -- a line is a sequence of text slices, and the inline boxes they came from are not items
// on it. Inheriting it means each text slice knows its own (direction, unicode-bidi) pair, which
// produces the same answer in every case except one: an inline box containing two child boxes gets
// two adjacent controlled runs where the specification has one around both. For `embed` and
// `bidi-override` those are indistinguishable (same level, adjacent, so one level run either way);
// for `isolate` they differ, and that case is `<bdi>a<span>b</span></bdi>`.
enum class UnicodeBidi : std::uint8_t {
  Normal,
  Embed,
  Isolate,
  BidiOverride,
  IsolateOverride,
  Plaintext,
};

// `vertical-align` (CSS 2.1 §10.8.1). Two groups, and the split is what line layout needs rather
// than a tidy transcription: `sub`, `super`, a length and a percentage are all a *shift of this
// item's baseline* away from the line's, and can be folded into one number before the line box
// exists; `top`, `bottom`, `middle`, `text-top` and `text-bottom` are stated against an edge that
// is not known until every other item on the line has been placed.
enum class VerticalAlign : std::uint8_t {
  Baseline,
  Sub,
  Super,
  TextTop,
  TextBottom,
  Middle,
  Top,
  Bottom,
  // `<length>` or `<percentage>`, carried in `vertical_align_offset`. A percentage is of the used
  // `line-height`, which is why it cannot be absolutized in the cascade: `line-height: normal`
  // computes to zero here and means "ask the font", which only the measurer can do.
  Offset,
};
// CSS Text 4 makes `white-space` a *shorthand* over two orthogonal longhands, and the four values
// this engine used to store were one enum standing in for both. Keeping one enum could not express
// `preserve-breaks nowrap` -- a pair the shorthand has no keyword for and which
// `css/css-text/parsing/white-space-shorthand.html` asks for by name -- and, worse, it made
// `nowrap` a *different* collapsing mode from `normal` when the two collapse identically and differ
// only in wrapping. Two fields say what the specification says.
enum class WhiteSpaceCollapse : std::uint8_t {
  Collapse,
  Preserve,
  PreserveBreaks,
  PreserveSpaces,
  BreakSpaces,
};
enum class TextWrapMode : std::uint8_t { Wrap, NoWrap };

// `text-transform`. The case keyword and the two width keywords are independent -- `capitalize
// full-width` is one value with two parts -- so this is a small struct rather than an enum.
enum class TextCase : std::uint8_t { None, Capitalize, Uppercase, Lowercase };
struct TextTransform {
  TextCase letter_case = TextCase::None;
  bool full_width = false;
  bool full_size_kana = false;

  bool IsNone() const {
    return letter_case == TextCase::None && !full_width && !full_size_kana;
  }
  friend bool operator==(const TextTransform&, const TextTransform&) = default;
};

// Where a line may break *inside* a word. Two properties rather than one because they answer
// different questions: `word-break` says what the writing system allows, `overflow-wrap` says what
// to do when a word does not fit however it is broken. `break-word` is `word-break`'s legacy
// spelling of `overflow-wrap: break-word` and computes to itself.
enum class WordBreak : std::uint8_t { Normal, BreakAll, KeepAll, BreakWord };
enum class OverflowWrap : std::uint8_t { Normal, BreakWord, Anywhere };

// `text-indent`, and the two keywords that change what it indents. Only the length is used today:
// `hanging` and `each-line` are stored so the computed value round-trips, and neither changes a
// box until the first-line machinery grows past one line.
struct TextIndent {
  Length length = Length::Pixels(0.0f);
  bool hanging = false;
  bool each_line = false;

  friend bool operator==(const TextIndent&, const TextIndent&) = default;
};

// `tab-size`: a multiple of the space advance, or a length. Both are one number and which one it
// is changes what it means, so the flag travels with it.
struct TabSize {
  float value = 8.0f;
  bool is_length = false;

  friend bool operator==(const TabSize&, const TabSize&) = default;
};

// A segment break (a newline in the source) survives whitespace processing. Only `collapse` and
// `preserve-spaces` turn one into a space; every other mode makes it a forced line break, which is
// what `<pre>` has always meant and what this browser did not do -- `<pre>a\nb</pre>` rendered on
// one line, because nothing in inline layout looked at a newline at all.
constexpr bool PreservesNewlines(WhiteSpaceCollapse collapse) {
  return collapse != WhiteSpaceCollapse::Collapse && collapse != WhiteSpaceCollapse::PreserveSpaces;
}

// A run of spaces and tabs survives as written rather than collapsing to one space.
constexpr bool PreservesSpaces(WhiteSpaceCollapse collapse) {
  return collapse == WhiteSpaceCollapse::Preserve ||
         collapse == WhiteSpaceCollapse::PreserveSpaces ||
         collapse == WhiteSpaceCollapse::BreakSpaces;
}

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
// `Sticky` is in the flow like `Relative` and moves like neither: its offset is
// a function of the scroll position of its nearest scrolling ancestor, so it is
// resolved at paint time rather than at layout time. Until session 8 there was
// no scroll offset to resolve it against and it was cascaded as `Relative`,
// which looked right until the page moved.
enum class Position : std::uint8_t { Static, Relative, Absolute, Fixed, Sticky };

// What happens to content that does not fit its box.
//
// Only the first is different in kind: `Visible` lets content escape, and the
// other three all clip it. Whether the overflow can then be *scrolled* back
// into view is a separate question from whether it is clipped, and only the
// clipping half is layout's.
enum class Overflow : std::uint8_t { Visible, Hidden, Scroll, Auto };

// `box-sizing`: which box `width` / `height` / `min-*` / `max-*` describe.
// `border-box` is what iron-fit writes next to `max-height` on youtube's consent
// dialog; treating that max as a content-box limit left the border box ~32px
// taller than the viewport clamp and Accept below the fold.
enum class BoxSizing : std::uint8_t { ContentBox, BorderBox };

// Whether the box is painted and hit-tested. Inherited; a descendant may set
// `visible` under a `hidden` ancestor and become a target again. `Collapse` is
// accepted as `Hidden` for now (no table-row collapsing yet).
enum class Visibility : std::uint8_t { Visible, Hidden };

// Whether the box is a pointer-event target. Inherited. `None` skips the box
// for hit testing while still walking descendants that may set `auto` — what
// youtube's `yt-interaction` ink layers use so the thumbnail link underneath
// receives the click (ADR 0017 §5).
enum class PointerEvents : std::uint8_t { Auto, None };

// The font size `rem` is a multiple of. A constant rather than the root
// element's resolved size, because carrying the root style into every Resolve
// call would put a parameter on a function called once per edge per element per
// frame. Named so that the two places that fold a `rem` into pixels — Resolve,
// and the `calc()` evaluator — cannot disagree about it.
inline constexpr float kRootFontSize = 16.0f;

// The custom properties in scope on one element, by name (with the leading
// `--`), holding *unparsed* text.
//
// Unparsed is the whole point: a custom property's value is not a value until
// it is substituted somewhere, and where it lands decides what it means.
// `--x: 20px` is a length in `padding` and a piece of a shorthand in
// `margin: var(--x) 0`, and parsing it on the way in would have to guess which.
//
// **Copy-on-write, because inheriting is by far the operation that happens
// most.** Every element inherits its parent's whole set, and a modern
// stylesheet declares its palette once on `:root` -- so a page with fifty
// custom properties was copying fifty string pairs into each of its elements,
// 19,000 of them on en.wikipedia.org/wiki/CSS, on every cascade pass. Sharing
// makes inheriting a refcount bump, and the copy happens only where a
// declaration actually writes one, which is a handful of elements on any real
// page.
//
// A vector rather than a map because the counts are small per element and a
// linear scan over a handful of short names beats a tree; that part of the
// original reasoning was right, it was the copy that was not.
class CustomProperties {
 public:
  const std::string* Find(std::string_view name) const {
    if (entries_ == nullptr) {
      return nullptr;
    }
    for (const auto& entry : *entries_) {
      if (entry.first == name) {
        return &entry.second;
      }
    }
    return nullptr;
  }

  void Set(std::string_view name, std::string value) {
    Entries& own = Mutable();
    for (auto& entry : own) {
      if (entry.first == name) {
        entry.second = std::move(value);
        return;
      }
    }
    own.emplace_back(std::string(name), std::move(value));
  }

  bool Empty() const { return entries_ == nullptr || entries_->empty(); }
  std::size_t Size() const { return entries_ == nullptr ? 0 : entries_->size(); }

  // By content rather than by identity: two elements that inherited the same
  // set and two that were given equal sets are the same style, and a
  // pointer comparison would say otherwise. ComputedStyle's `operator==` is
  // defaulted and would otherwise silently mean something else here.
  friend bool operator==(const CustomProperties& a, const CustomProperties& b) {
    if (a.entries_ == b.entries_) {
      return true;  // shared, which is the common case and is free
    }
    if (a.Empty() || b.Empty()) {
      return a.Empty() && b.Empty();
    }
    return *a.entries_ == *b.entries_;
  }

 private:
  using Entries = std::vector<std::pair<std::string, std::string>>;

  // The set this style may write to: its own, copied out of the shared one if
  // anybody else is still holding it. `use_count` is a safe question here
  // because the cascade is single-threaded and a style is never shared across
  // one -- if that ever stops being true this needs a different mechanism, not
  // a lock.
  Entries& Mutable() {
    if (entries_ == nullptr) {
      entries_ = std::make_shared<Entries>();
    } else if (entries_.use_count() > 1) {
      entries_ = std::make_shared<Entries>(*entries_);
    }
    return *entries_;
  }

  // Null rather than an empty vector, so an element that inherits nothing costs
  // a null pointer rather than an allocation. Most elements on most pages.
  std::shared_ptr<Entries> entries_;
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
  // Inherited, and the same number for every element: the root's computed `font-size`, which is
  // what `rem` is a multiple of. Carried on the style rather than on the resolver because it is an
  // input to computing a value -- `rem` absolutizes during the cascade, exactly as `font-size`
  // does above -- and the cascade already has the parent's style in hand at every element.
  //
  // Deliberately not on `MediaContext`: a `rem` inside `@media` resolves against the *initial*
  // font size and not the root's (Media Queries §units), so the two would have to disagree.
  //
  // Kevlar sets `html { font-size: 10px }` and then writes every length on youtube.com in `rem`.
  // Folding those at a constant 16 made the whole application 1.6x its size -- which is how a
  // mini-guide label that reads "Subscriptions" in Chrome was clipped to "Subscrip" here.
  float root_font_size = 16.0f;
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

  TextAlign text_align = TextAlign::Start;
  // **Not inherited**, which is the specification's rule and the one that matters here: a text box
  // takes only the inherited properties (`TextStyleFrom`), so the value on a `<sup>` reaches its
  // own text through line layout's walk rather than through the cascade. Applying it to a block
  // box is a no-op -- CSS 2.1 §10.8 says it applies to inline-level and table-cell boxes.
  VerticalAlign vertical_align = VerticalAlign::Baseline;
  // Only read when `vertical_align` is `Offset`. A percentage here is of the used `line-height`.
  Length vertical_align_offset;
  Direction direction = Direction::Ltr;
  UnicodeBidi unicode_bidi = UnicodeBidi::Normal;
  // Set by `text-align: -microbrowser-center`, which is what <center> means and
  // what no standard value expresses -- every engine carries an equivalent
  // (`-moz-center`, `-webkit-center`). Not inherited, unlike text_align: see
  // LayoutEngine::LayoutBlock's `center_in_container`.
  bool centers_block_children = false;
  // The two halves of `white-space`. Inherited, both of them.
  WhiteSpaceCollapse white_space_collapse = WhiteSpaceCollapse::Collapse;
  TextWrapMode text_wrap_mode = TextWrapMode::Wrap;
  // The rest of CSS Text that changes what a line looks like. All inherited.
  TextTransform text_transform;
  WordBreak word_break = WordBreak::Normal;
  OverflowWrap overflow_wrap = OverflowWrap::Normal;
  TextIndent text_indent;
  // `letter-spacing` and `word-spacing`. `normal` is the initial value of both and is stored as a
  // zero length, which is what it computes to: neither property's `normal` means "the font's own
  // idea of it" in a way this engine could act on differently from zero -- word-spacing's normal is
  // the space glyph's own advance, which the shaper already produced.
  Length letter_spacing;
  Length word_spacing;
  TabSize tab_size;
  Float css_float = Float::None;
  Clear clear = Clear::None;

  Position position = Position::Static;

  // `transform`, and the origin it is applied about. Paint-only: a transformed box
  // occupies exactly the space it would have occupied untransformed, which is why
  // `PropertyAffectsLayout` says no for both and a hover that only transforms costs
  // a repaint rather than a relayout.
  //
  // The origin defaults to the centre of the border box -- 50% 50% -- which is not
  // the initial value of any other property in this struct and is the reason a
  // rotation looks right without the author saying anything.
  TransformList transform;
  // `z-index`. Nothing means `auto`, which is *not* the same as zero: a zero
  // establishes a stacking context and an auto does not, and the difference decides
  // whether a positioned descendant can paint above this box's siblings.
  std::optional<int> z_index;
  // `transition` and `animation` (ADR 0014 §5). **Not inherited**, which is the specification's rule
  // and the useful one: a transition set on a container must not make every descendant animate the
  // same property. They are vectors because both properties are comma-separated lists, and a page
  // routinely transitions three properties at three speeds.
  std::vector<TransitionSpec> transitions;
  std::vector<AnimationSpec> animations;

  // CSS Transforms 2's `translate`/`rotate`/`scale`, and the box a percentage
  // origin is a fraction of. Grouped in `Transform.h` beside the operation type
  // they are made of.
  IndividualTransforms individual_transform;
  TransformBox transform_box = TransformBox::ViewBox;
  Length transform_origin_x = Length{50.0f, Length::Unit::Percent};
  Length transform_origin_y = Length{50.0f, Length::Unit::Percent};
  // Per axis, because a page sets them separately as often as together --
  // `overflow-x: hidden` with `overflow-y: auto` is the ordinary way to write
  // a vertical scroller.
  Overflow overflow_x = Overflow::Visible;
  Overflow overflow_y = Overflow::Visible;
  // Inherited. youtube's closed `tp-yt-app-drawer` covers the viewport with
  // `visibility: hidden` so clicks pass through; without this the scrim steals
  // every hit (ADR 0017 §5).
  Visibility visibility = Visibility::Visible;
  PointerEvents pointer_events = PointerEvents::Auto;
  // Not inherited. Initial 1. Paint multiplies this into ink (and skips the
  // whole subtree at 0). `opacity < 1` also forms a stacking context. Without
  // it, youtube's `yt-interaction .fill { background: #000; opacity: 0 }` and
  // the consent backdrop paint as solid black rectangles.
  float opacity = 1.0f;
  // `top`/`right`/`bottom`/`left`. All four default to `auto`, which for a
  // relative box means "no offset" and for an absolute one means "wherever the
  // flow would have put it" -- two different meanings for the same value, and
  // the reason they cannot default to zero.
  Edges inset{Length::Auto(), Length::Auto(), Length::Auto(), Length::Auto()};

  Edges margin;
  Edges padding;
  // `medium`, on every side, which is `border-width`'s initial value and is 3px in every engine.
  // Zero was the old default and it was invisible while a border needed `has_border` to draw at
  // all; now that a style lights a side up on its own, `border-left-style: solid` with no width is
  // a three-pixel line rather than nothing -- and so is every declaration that names an invalid
  // width, because a dropped declaration leaves the initial value behind.
  Edges border_width{Length::Pixels(3.0f), Length::Pixels(3.0f), Length::Pixels(3.0f),
                     Length::Pixels(3.0f)};
  BorderSides border_style;
  BorderColors border_color;

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
  BoxSizing box_sizing = BoxSizing::ContentBox;

  // `aspect-ratio`, as width divided by height. Zero is `auto`, which is the
  // initial value and means the box has no preferred ratio at all.
  //
  // One number rather than the pair the property is written as, because nothing
  // downstream needs the two apart -- and because a ratio kept as two integers
  // has a second way to say `auto` (0/0) that every reader would have to
  // remember to check.
  float aspect_ratio = 0.0f;

  // `content`, for `::before` / `::after` only. `Normal`/`None` generate no box;
  // `Empty` is `content: ""`, which is what youtube's thumbnail aspect hack uses
  // -- a block with percentage padding and no text; `String` is `content: "x"`
  // and the text is in `content_text`.
  //
  // `Empty` stays a value of its own rather than becoming a `String` with an
  // empty string, because the two are the same box and telling them apart in the
  // one place that reads it costs nothing, while collapsing them would make the
  // aspect hack's behaviour depend on a string comparison in the paint path.
  //
  // Urls, `counter()`, `attr()` and the quote keywords are still absent rather
  // than stubbed (ADR 0012): a `content: counter(x)` that produced the literal
  // text would be a wrong render, where a refused declaration is a missing one.
  enum class Content : std::uint8_t { Normal, None, Empty, String };
  Content content = Content::Normal;
  // The text of a `String` content, already unescaped and unquoted. Empty for
  // every other value, so nothing has to check the enum before reading it.
  std::string content_text;

  // A used *content* size, clamped by its bounds. `padding_border` is the sum of
  // the padding and border on the axis being clamped; under `box-sizing:
  // border-box` the bounds describe the border box, so they are applied to
  // `used + padding_border` and the content size is recovered after.
  //
  // Maximum first, then minimum, in that order: the spec resolves the two that
  // way and it is observable when they contradict each other. A `min-width`
  // larger than a `max-width` wins, and a page that writes both means the
  // minimum.
  // The border widths layout must reserve space for: a side's declared width when its style paints
  // something, and zero otherwise.
  //
  // Asked here rather than at the eleven call sites that used to read `has_border ? border_width :
  // Edges{}`, because "does this side draw?" is now four questions rather than one and eleven
  // copies of a four-way test is eleven chances to get one wrong.
  Edges UsedBorderWidths() const {
    Edges used;
    for (std::size_t i = 0; i < 4; ++i) {
      const BorderStyle side = border_style[i];
      if (side != BorderStyle::None && side != BorderStyle::Hidden) {
        used[i] = border_width[i];
      }
    }
    return used;
  }

  // This side's used colour. `border-color`'s initial value is `currentColor`, which is not a
  // colour until the element has one -- so an unset side answers with the element's `color`.
  gfx::Color BorderColorFor(std::size_t side) const {
    const std::optional<gfx::Color>& declared = border_color[side];
    return declared.has_value() ? *declared : color;
  }

  float ClampWidth(float used, float container, float padding_border = 0.0f) const {
    return ClampContent(used, min_width, max_width, container, padding_border);
  }
  float ClampHeight(float used, float container, float padding_border = 0.0f) const {
    return ClampContent(used, min_height, max_height, container, padding_border);
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

  // `grid`. ADR 0014 §6, session 39: last of the layout features on the measurement and still real at
  // 78 uses.
  //
  // Grouped for the reason `FlexStyle` is -- the container reads the tracks and the gaps, the item reads
  // its placement, and loose on `ComputedStyle` they would say nothing about belonging together.
  struct GridStyle {
    // The explicit tracks, in order. Empty means "no explicit tracks", which is not the same as one
    // auto track: a grid with no `grid-template-columns` puts everything in a single implicit column.
    std::vector<GridTrack> columns;
    std::vector<GridTrack> rows;
    // The size for tracks the placement creates beyond the explicit ones -- `grid-auto-rows`. `auto` is
    // the initial value and means "as tall as the content", which is what makes a grid with only
    // columns declared work at all.
    GridTrack auto_rows;
    GridTrack auto_columns;
    float row_gap = 0.0f;
    float column_gap = 0.0f;
    // `justify-items` and `align-items` on the container, and their `*-self` overrides on the item.
    // Stretch is the initial value for both, which is why a grid item with no width fills its cell.
    Alignment justify_items = Alignment::Stretch;
    Alignment align_items = Alignment::Stretch;
    Alignment justify_self = Alignment::Auto;
    Alignment align_self = Alignment::Auto;
    // Read by the item: its placement, as line numbers. Zero means `auto` -- the placement algorithm
    // chooses -- and a negative number counts from the end, which is why this is signed.
    GridPlacement column_placement;
    GridPlacement row_placement;

    friend bool operator==(const GridStyle&, const GridStyle&) = default;
  };
  GridStyle grid;

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
      return length.Used(container, font_size);
    };
    if (!high.IsAuto()) {
      used = std::min(used, resolve(high));
    }
    if (!low.IsAuto()) {
      used = std::max(used, resolve(low));
    }
    return std::max(0.0f, used);
  }

  float ClampContent(float used_content, const Length& low, const Length& high, float container,
                     float padding_border) const {
    if (box_sizing == BoxSizing::BorderBox && padding_border > 0.0f) {
      const float border_box = ClampBy(used_content + padding_border, low, high, container);
      return std::max(0.0f, border_box - padding_border);
    }
    return ClampBy(used_content, low, high, container);
  }

 public:

  bool IsInlineLevel() const {
    // A float is block-level whatever it was declared as: `float: left` on a
    // span makes it a block, per CSS 2.1 s9.7. So is an absolutely positioned
    // box, by the same rule and the same sentence -- both are out of flow, and
    // "on a line" is not a thing an out-of-flow box can be. Answering that here
    // rather than at each call site is what keeps the rule from being applied
    // in three places and forgotten in a fourth.
    return !IsFloating() && !IsAbsolutelyPositioned() &&
           (display == Display::Inline || display == Display::InlineBlock ||
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

  // The SVG painting properties (ADR 0043 §2), in `SvgStyle.h` because this
  // file is at its module's line cap and a file over its cap means a missing
  // header rather than a bigger one. Grouped for the reason `FlexStyle` is.
  SvgStyle svg;

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
  CustomProperties custom_properties;

  const std::string* CustomProperty(std::string_view name) const {
    return custom_properties.Find(name);
  }
  void SetCustomProperty(std::string_view name, std::string value) {
    custom_properties.Set(name, std::move(value));
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

// The computed value of a custom property. Specified text when it contains no
// `var()` — comments stay — otherwise the token stream after substitution,
// serialized with `/**/` between tokens that would otherwise re-parse as one.
std::string ComputedCustomProperty(const ComputedStyle& style, std::string_view name);

// Parses a colour: named, `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`.
// Nullopt for anything unrecognized, which is how an invalid declaration is
// dropped rather than turning an element transparent.
std::optional<gfx::Color> ParseColor(std::string_view text);

// Parses a length. Nullopt when the text is not one.
// `root_font_size` is what `rem` is a multiple of: the root element's computed `font-size`, which
// the cascade carries on every ComputedStyle. It defaults to the initial 16px, which is the right
// answer for the two callers that have no element -- a media query and the `@supports` scratch
// style.

std::optional<Length> ParseLength(std::string_view text, const MediaContext& context = {},
                                  float root_font_size = kRootFontSize);

// The flex properties and the sizing bounds. True when the declaration was
// *applied* -- which means both that `property` is one of these and that its
// value was one this engine has. False covers both an unrecognized property and
// an unusable value, because CSS treats them the same and `@supports` has to
// answer for both: see ApplyDeclaration.
//
// Separate from ApplyDeclaration because Declarations.cpp is at its module's
// line cap, and the cap means a missing translation unit rather than a bigger
// file.
// `transform` and `transform-origin`. Its own entry point for the reason it is its
// own translation unit: eleven functions, per-function argument counts and units, and
// an all-or-nothing error rule that no other property here has.
bool ApplyTransformDeclaration(std::string_view property, std::string_view value,
                               const ComputedStyle& parent, ComputedStyle& style);

// The SVG presentation properties (ADR 0043 §2), in their own translation unit
// for the reason the box ones are: this module's files are at their line cap.

bool ApplySvgDeclaration(std::string_view property, std::string_view value,
                         const ComputedStyle& parent, ComputedStyle& style,
                         const MediaContext& context = {});

bool ApplyBoxDeclaration(std::string_view property, std::string_view value,
                         const ComputedStyle& parent, ComputedStyle& style,
                         const MediaContext& context = {});

// CSS Text: the `white-space` family. Its own entry point for the reason `transform`'s is --
// `white-space` is a shorthand over two orthogonal longhands whose two components may be written in
// either order, which is not the shape of the keyword switch the rest of ApplyDeclaration is.
bool ApplyTextDeclaration(std::string_view property, std::string_view value,
                          const ComputedStyle& parent, ComputedStyle& style,
                          const MediaContext& context = {});

// `transition-*` and `animation-*`. Their own entry points for the reason `transform`'s is: a
// comma-separated list of space-separated lists, where the order inside an item is mostly free and one
// position -- duration before delay -- is decided by which time came first. ADR 0014 §5.
bool ApplyTransitionDeclaration(std::string_view property, std::string_view value,
                                ComputedStyle& style);
bool ApplyAnimationDeclaration(std::string_view property, std::string_view value,
                               ComputedStyle& style);

// Copies exactly the properties that inherit, and nothing else.
//
// **One list, because two lists is how a property comes to be inherited by an element and not by its
// own text.** `src/layout` had its own copy of this -- `TextStyleFrom`, which built the style for an
// anonymous text box -- and it listed seven properties. When `direction` and `unicode-bidi` were
// added to the cascade they were added to the resolver's list and not to that one, so a
// right-to-left `<span>` was right-to-left and the text inside it was not. The bug was invisible in
// every rendering: the paragraph's direction came from the block, so only `unicode-bidi` -- read off
// the text box -- silently did nothing.
//
// `custom_properties` is skipped when `with_custom_properties` is false, which is the one case where
// the two callers legitimately differ: a text box never resolves a `var()`, and copying the table
// into every one of them is a vector copy per text node.
void InheritInto(const ComputedStyle& parent, ComputedStyle& child,
                 bool with_custom_properties = true);

}  // namespace microbrowser::css
