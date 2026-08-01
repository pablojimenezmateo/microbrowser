#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "css/ComputedStyle.h"
#include "dom/Node.h"
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

 private:
  float ratio_;
};

}  // namespace microbrowser::layout
