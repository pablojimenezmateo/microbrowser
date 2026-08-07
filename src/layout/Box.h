#pragma once

#include <cstdint>
#include <string_view>
#include <memory>
#include <string>
#include <vector>

#include "css/ComputedStyle.h"
#include "dom/Node.h"
#include "gfx/Font.h"
#include "gfx/Image.h"
#include "gfx/Geometry.h"

namespace microbrowser::layout {

// The CSS box model, as four nested rectangles.
//
// Kept as edges rather than as one rect plus deltas because every consumer
// wants a different one of the four: backgrounds paint the padding box, borders
// the border box, hit testing the border box, and layout positions the margin
// box. Deriving three of them at every use is where off-by-one errors live.
struct BoxGeometry {
  gfx::FloatRect content;
  css::Edges padding;
  css::Edges border;
  css::Edges margin;

  gfx::FloatRect PaddingBox() const;
  gfx::FloatRect BorderBox() const;
  gfx::FloatRect MarginBox() const;
};

// The font a computed style asks for.
//
// One implementation, used by both the measurer and the painter. Two would let
// text be measured with one face and drawn with another, which shows up as
// lines that overflow their boxes by a few pixels and is very hard to trace
// back to a duplicated four-line function.
gfx::FontRequest FontRequestFor(const css::ComputedStyle& style);

// The style a text box carries: only the *inherited* properties of the element
// around it.
//
// One implementation, for the reason FontRequestFor is one. Copying the whole
// computed style gives every text run its parent's background, border, margin
// and width, and the painter then draws all of them a second time inside the
// box that already has them -- so a second copy of this list that forgot a
// property would be a box painted twice with two different opinions about where
// its edges are. Two callers: building a text box, and re-resolving the cascade
// over a box tree that is already laid out (ADR 0016 §3).
css::ComputedStyle TextStyleFrom(const css::ComputedStyle& parent);

// One piece of a text box, on one line.
//
// A text box that wraps occupies several rectangles, and the box cannot hold
// just one: the last one would win and the earlier lines would paint at the
// wrong place, or not at all. Fragments are also the only structure that makes
// selection and hit testing expressible later — a caret lands in a fragment at
// a byte offset, and both numbers are here.
struct TextFragment {
  // Byte range within the box's text.
  std::uint32_t begin = 0;
  std::uint32_t length = 0;
  // The line box this fragment occupies. Its width is the run's advance.
  gfx::FloatRect rect;
  // Where the glyphs actually sit. Distinct from rect.y by the ascent, which
  // only the font knows: mixing the two is the classic reason text renders one
  // line too low.
  float baseline = 0.0f;
  // Whether bidi resolved this fragment to a right-to-left level. Paint needs it for rule L4 -- a
  // `(` in right-to-left text is painted as `)` -- and it is a property of the *fragment* rather than
  // of the box, because one text box can contribute a left-to-right and a right-to-left fragment to
  // the same line.
  bool right_to_left = false;

  friend bool operator==(const TextFragment&, const TextFragment&) = default;
};

// One box in the layout tree.
//
// A layout box is not a DOM node: one element can generate several boxes (a
// block containing inline content generates anonymous blocks), and some
// elements generate none. Keeping them separate types is what makes that
// expressible rather than a special case.
class Box {
 public:
  enum class Kind : std::uint8_t {
    Block,
    Inline,
    Text,
    // Generated to hold inline content inside a block that also has block
    // children. Without it, "block and inline siblings" has no representation
    // and the tree cannot be laid out.
    AnonymousBlock,
    // An element whose content comes from outside CSS -- an image, and later a
    // video or a form control. It sits on a line as a single unbreakable
    // rectangle rather than as text, which is why it is a kind rather than an
    // Inline box with a picture in it.
    Replaced,
    // A forced line break: `<br>`. A kind rather than a zero-width text box
    // with a newline in it, because line breaking would then have to decide
    // whether a newline in text means a break -- which depends on
    // `white-space` -- and answer differently for the same character.
    LineBreak,
    // `display: inline-block` and `display: inline-flex`: a box that is laid
    // out inside like a block and placed outside like one unbreakable
    // rectangle.
    //
    // A kind rather than a flag on Inline, because the two are opposites in
    // every pass that matters. Line layout walks *through* an Inline box to
    // reach the text inside it, so an Inline box never gets geometry of its
    // own; an InlineBlock is a leaf as far as the line is concerned, and its
    // geometry is the whole point. Treating one as the other is what made
    // `display: inline-block` a no-op: every such box on old.reddit.com --
    // its post flair, its domain links, the whole `.buttons` row -- was laid
    // out as plain inline, so its width, padding and background went nowhere
    // and an `overflow: hidden` on it clipped to a rectangle that was never
    // filled in.
    InlineBlock,
  };

  Box(Kind kind, css::ComputedStyle style) : kind_(kind), style_(std::move(style)) {}

  Kind GetKind() const { return kind_; }
  const css::ComputedStyle& Style() const { return style_; }
  // Replaces the style without rebuilding the box. The one caller is the
  // paint-only restyle of ADR 0016 §3, which re-resolves the cascade over a box
  // tree whose geometry is still correct -- and it is correct only because the
  // invalidation index has already established that every rule keyed on what
  // changed affects paint alone. Calling it with a style that changes a length
  // leaves the box's geometry describing the old one.
  void SetStyle(css::ComputedStyle style) { style_ = std::move(style); }
  BoxGeometry& Geometry() { return geometry_; }
  const BoxGeometry& Geometry() const { return geometry_; }

  const std::vector<std::unique_ptr<Box>>& Children() const { return children_; }
  Box& Append(std::unique_ptr<Box> child);

  // The element this box came from, or null for text and anonymous boxes.
  const dom::Element* Origin() const { return origin_; }
  void SetOrigin(const dom::Element* element) { origin_ = element; }

  const std::string& Text() const { return text_; }
  void SetText(std::string text) { text_ = std::move(text); }

  // A replaced element takes its level from its own `display`, like everything
  // else: `img { display: block }` is how a picture gets a line of its own and
  // how `margin: 0 auto` centres one. Answering "inline" unconditionally kept
  // it beside its siblings, and kept an absolutely positioned one in a flow it
  // had left -- so it was laid out twice, once here and once as an absolute.
  bool IsBlockLevelReplaced() const {
    return kind_ == Kind::Replaced && !style_.IsInlineLevel() && !style_.IsFloating();
  }
  bool IsBlockLevel() const {
    return kind_ == Kind::Block || kind_ == Kind::AnonymousBlock || IsBlockLevelReplaced();
  }

  // Taken out of the normal flow. Asked of the box rather than of its style at
  // each call site, because a float is out of flow whatever kind it is -- a
  // floated <img> is a replaced box that is *not* placed on a line, and every
  // place that forgets to check produces a picture stacked above the text
  // instead of beside it.
  bool IsFloating() const { return style_.IsFloating(); }

  // Placed on a line as one unbreakable rectangle: text and replaced content
  // that is still in flow.
  bool IsInlineLevel() const {
    return !IsFloating() && !IsBlockLevelReplaced() &&
           (kind_ == Kind::Text || kind_ == Kind::Replaced || kind_ == Kind::LineBreak ||
            kind_ == Kind::InlineBlock);
  }

  // Sized and laid out inside like a block, placed outside like a replaced
  // element. Its contents are its own business: it establishes a block
  // formatting context, so a float inside it does not shorten the lines of the
  // paragraph it sits in.
  bool IsAtomicInline() const { return kind_ == Kind::InlineBlock; }

  // Does this box cut its content off at its padding box?
  //
  // Asked of the box rather than of its style because `overflow` **does not
  // apply to a non-replaced inline box** (CSS 2.1 s11.1.1), and only the box
  // knows whether it is one -- an element with `display: inline` that contains
  // a block is promoted to a block box here, and then it does clip.
  //
  // Found on old.reddit.com, whose stylesheet says `.thing .title { overflow:
  // hidden }` and whose titles are `<a>` elements. Clipping them to an inline
  // box's geometry -- which is empty, because an inline box's content lives in
  // its container's line boxes -- deleted every story title on the front page.
  // They were all in the display list, in the right place, in the right
  // colour; the clip around them was 0x0.
  bool ClipsOverflow() const { return kind_ != Kind::Inline && style_.ClipsOverflow(); }

  // A box that clips its overflow is a **scroll container**: what it cut off is
  // still there, and an offset decides which part of it shows. Every value but
  // `visible` makes one, including `hidden` -- which a user cannot scroll but a
  // script can, and which is how a carousel is built.
  bool IsScrollContainer() const { return ClipsOverflow(); }

  // ...and a *user* may only scroll the two that say so. `overflow: hidden` on
  // a wrapper is a page saying "this does not scroll", and a wheel that moved
  // it anyway would scroll things no other browser scrolls.
  bool AllowsUserScroll() const {
    return IsScrollContainer() && (style_.overflow_x == css::Overflow::Scroll ||
                                   style_.overflow_x == css::Overflow::Auto ||
                                   style_.overflow_y == css::Overflow::Scroll ||
                                   style_.overflow_y == css::Overflow::Auto);
  }

  // Where this box's content is displaced to, and how big that content is.
  //
  // **Not derived from style, which is what makes it a different kind of thing
  // from everything else on this class.** Layout computes the overflow size and
  // clamps the offset into it; the offset itself is state a wheel or a script
  // wrote, and it survives a relayout because the engine keeps it per element
  // and puts it back. See ADR 0018 §1: a scroll is a paint, not a layout, and
  // the offset is the one input to paint that layout does not own.
  gfx::FloatPoint ScrollOffset() const { return scroll_offset_; }
  void SetScrollOffset(gfx::FloatPoint offset) { scroll_offset_ = offset; }
  // The scrollable overflow size: what `scrollWidth`/`scrollHeight` report, and
  // never smaller than the padding box, because a box that fits its content
  // still reports its own size rather than zero.
  gfx::FloatSize ScrollableOverflow() const { return scrollable_overflow_; }
  void SetScrollableOverflow(gfx::FloatSize size) { scrollable_overflow_ = size; }

  // Participates in the block layout pass: stacked, or placed as a float.
  bool IsOutOfLineFlow() const { return IsBlockLevel() || IsFloating(); }

  // Absolutely positioned: laid out after the flow, against a containing block
  // rather than after its siblings. Asked of the box for the reason
  // IsFloating is -- it is true whatever kind the box is, and a call site that
  // forgets to check leaves the box in the flow taking up space it should not.
  bool IsAbsolutelyPositioned() const { return style_.IsAbsolutelyPositioned(); }

  // Mutable children, for the passes that move a box after it was placed:
  // relative offsets and absolute positioning both run over a subtree that
  // already has geometry.
  std::vector<std::unique_ptr<Box>>& MutableChildren() { return children_; }
  std::vector<TextFragment>& MutableFragments() { return fragments_; }

  // The pixels a replaced box shows. Null until the resource loads, and a
  // replaced box with no image still occupies its intrinsic size -- otherwise
  // the page reflows when the image arrives, which is the layout shift every
  // user has learned to hate.
  const std::shared_ptr<const gfx::Image>& Image() const { return image_; }
  void SetImage(std::shared_ptr<const gfx::Image> image) { image_ = std::move(image); }

  // `background-image`. A second slot rather than a reuse of the first: a
  // replaced element can have both, and they paint at different times and in
  // different places -- the background under the border, the content over it.
  const std::shared_ptr<const gfx::Image>& BackgroundImage() const { return background_image_; }
  void SetBackgroundImage(std::shared_ptr<const gfx::Image> image) {
    background_image_ = std::move(image);
  }

  // The intrinsic widths this box was last measured at, cached because the
  // answer depends only on the subtree and its styles -- both fixed for the
  // duration of a layout pass. Without it every ancestor table re-walks every
  // descendant's whole subtree to size its columns, which is O(nodes x nesting
  // depth) and was 70% of the time spent laying out a Hacker News comment page.
  //
  // Negative means unmeasured. Cleared by LayoutEngine::Layout at the start of
  // each pass rather than trusted across passes: a replaced box's used width
  // can change between them, and a stale intrinsic width is a column that is
  // the right size for the previous viewport.
  struct IntrinsicWidths {
    float min = -1.0f;
    float max = -1.0f;
  };
  IntrinsicWidths& Intrinsic() const { return intrinsic_; }
  void ClearIntrinsicWidths() const {
    intrinsic_ = IntrinsicWidths{};
    for (const std::unique_ptr<Box>& child : children_) {
      child->ClearIntrinsicWidths();
    }
  }

  const std::vector<TextFragment>& Fragments() const { return fragments_; }
  void AddFragment(const TextFragment& fragment) { fragments_.push_back(fragment); }
  void ClearFragments() { fragments_.clear(); }

  template <typename Visitor>
  void ForEachDescendant(Visitor&& visit) const {
    for (const std::unique_ptr<Box>& child : children_) {
      visit(*child);
      child->ForEachDescendant(visit);
    }
  }

 private:
  Kind kind_;
  css::ComputedStyle style_;
  BoxGeometry geometry_;
  std::vector<std::unique_ptr<Box>> children_;
  const dom::Element* origin_ = nullptr;
  std::string text_;
  std::vector<TextFragment> fragments_;
  std::shared_ptr<const gfx::Image> image_;
  std::shared_ptr<const gfx::Image> background_image_;
  gfx::FloatPoint scroll_offset_;
  gfx::FloatSize scrollable_overflow_;
  // Mutable: measuring a box does not change it, and the measurement is a pure
  // function of a tree that layout treats as const while it reads it.
  mutable IntrinsicWidths intrinsic_;
};

// Supplies the pixels for a replaced element.
//
// An interface for the same reason TextMeasurer is one: layout must be
// testable without a network and without a decoder, and what an `src` resolves
// to is the engine's problem rather than layout's.
class ImageProvider {
 public:
  virtual ~ImageProvider() = default;
  // Null when the resource has not loaded, failed, or is not an image. All
  // three render the same way -- a box of the element's declared size with
  // nothing in it -- which is why they are one return value.
  virtual std::shared_ptr<const gfx::Image> ImageFor(std::string_view src) const = 0;

  // The pixels for an element that is an image, which is not the same question
  // as the one above: `srcset` and `<picture>` mean the URL an <img> loads is
  // chosen from the markup and the viewport together, and layout may not make
  // that choice -- it would need a media query evaluator and a device pixel
  // ratio, neither of which is layout's. So the element is passed and the
  // provider answers.
  //
  // The default is the URL the element wrote, which is what every caller had
  // before selection existed and is right for a provider that does not select.
  // `css_width`/`css_height` are the used content size in CSS pixels; inline
  // `<svg>` needs them because its intrinsic size may be absent and DecodeSvg
  // refuses a zero surface.
  virtual std::shared_ptr<const gfx::Image> ImageForElement(const dom::Element& element,
                                                           int css_width = 0,
                                                           int css_height = 0) const {
    (void)css_width;
    (void)css_height;
    const std::string* src = element.GetAttribute("src");
    return src == nullptr ? nullptr : ImageFor(*src);
  }

  // Queues a background URL during box-tree build; default is a no-op.
  virtual void WantImage(std::string_view /*src*/) const {}

 protected:
  ImageProvider() = default;
};

// Measures text. An interface because layout must be testable without a font:
// a real font makes every expected width depend on which version of which
// typeface is installed, and then the tests assert nothing about layout.
//
// Production supplies a FreeType-backed one; tests supply a fixed-advance one
// whose numbers are exact, which is what lets a line-breaking test say the line
// broke in the right place rather than that it broke somewhere.
class TextMeasurer {
 public:
  virtual ~TextMeasurer() = default;
  // `right_to_left` is bidi's answer, not a guess from the text -- and it is here rather than
  // defaulted because a measurement taken in one direction and a paint done in the other is a line
  // that ends up a fraction of a pixel short at every direction boundary. Shaping is
  // direction-dependent even when the advances happen to match, which they usually do; "usually" is
  // not a property to build a line box on.
  virtual float MeasureWidth(std::string_view text, const css::ComputedStyle& style,
                             bool right_to_left = false) const = 0;
  virtual float LineHeight(const css::ComputedStyle& style) const = 0;
  // Distance from the top of the line box to the baseline. Positive.
  virtual float Ascent(const css::ComputedStyle& style) const = 0;

 protected:
  TextMeasurer() = default;
};

// A measurer with a fixed advance per byte. Used by tests, and by the engine
// before a font is loaded, where a wrong-but-consistent measurement is better
// than no layout at all.
class FixedTextMeasurer : public TextMeasurer {
 public:
  explicit FixedTextMeasurer(float advance_ratio = 0.5f) : ratio_(advance_ratio) {}

  float MeasureWidth(std::string_view text, const css::ComputedStyle& style,
                     bool right_to_left = false) const override;
  float LineHeight(const css::ComputedStyle& style) const override;
  float Ascent(const css::ComputedStyle& style) const override;

 private:
  float ratio_;
};

}  // namespace microbrowser::layout
