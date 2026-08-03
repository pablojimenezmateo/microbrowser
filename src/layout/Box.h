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
  };

  Box(Kind kind, css::ComputedStyle style) : kind_(kind), style_(std::move(style)) {}

  Kind GetKind() const { return kind_; }
  const css::ComputedStyle& Style() const { return style_; }
  BoxGeometry& Geometry() { return geometry_; }
  const BoxGeometry& Geometry() const { return geometry_; }

  const std::vector<std::unique_ptr<Box>>& Children() const { return children_; }
  Box& Append(std::unique_ptr<Box> child);

  // The element this box came from, or null for text and anonymous boxes.
  const dom::Element* Origin() const { return origin_; }
  void SetOrigin(const dom::Element* element) { origin_ = element; }

  const std::string& Text() const { return text_; }
  void SetText(std::string text) { text_ = std::move(text); }

  bool IsBlockLevel() const { return kind_ == Kind::Block || kind_ == Kind::AnonymousBlock; }

  // Taken out of the normal flow. Asked of the box rather than of its style at
  // each call site, because a float is out of flow whatever kind it is -- a
  // floated <img> is a replaced box that is *not* placed on a line, and every
  // place that forgets to check produces a picture stacked above the text
  // instead of beside it.
  bool IsFloating() const { return style_.IsFloating(); }

  // Placed on a line as one unbreakable rectangle: text and replaced content
  // that is still in flow.
  bool IsInlineLevel() const {
    return !IsFloating() && (kind_ == Kind::Text || kind_ == Kind::Replaced);
  }

  // Participates in the block layout pass: stacked, or placed as a float.
  bool IsOutOfLineFlow() const { return IsBlockLevel() || IsFloating(); }

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
  virtual float MeasureWidth(std::string_view text, const css::ComputedStyle& style) const = 0;
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

  float MeasureWidth(std::string_view text, const css::ComputedStyle& style) const override;
  float LineHeight(const css::ComputedStyle& style) const override;
  float Ascent(const css::ComputedStyle& style) const override;

 private:
  float ratio_;
};

}  // namespace microbrowser::layout
