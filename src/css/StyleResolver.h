#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "css/ComputedStyle.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"

namespace microbrowser::css {

// Where a declaration came from. The cascade compares origin *before*
// specificity, which is why this is an ordered enum rather than a label: a
// user-agent rule with a thousand ids still loses to an author rule.
enum class Origin : std::uint8_t {
  UserAgent = 0,
  Author = 1,
  // A `style=""` attribute, which outranks every selector-based author rule
  // regardless of specificity.
  Inline = 2,
};

// Resolves computed styles for a document.
//
// The cascade order, in full, because getting it partly right is the usual
// outcome: origin, then `!important` (which *reverses* the origin order), then
// specificity, then document order. Every one of those is tested.
class StyleResolver {
 public:
  StyleResolver();

  void AddStyleSheet(const StyleSheet& sheet, Origin origin);

  // The style of one element, given its parent's already-computed style.
  // Passing the parent style rather than looking it up is what makes
  // inheritance a single pass down the tree instead of a walk up per property.
  ComputedStyle StyleFor(const dom::Element& element, const ComputedStyle& parent) const;

  // Computes styles for the whole document, in tree order, and hands each one
  // to `visit(element, style)`.
  template <typename Visitor>
  void ForEachStyledElement(const dom::Document& document, Visitor&& visit) const {
    const ComputedStyle root = InitialStyle();
    for (const std::unique_ptr<dom::Node>& child : document.Children()) {
      Walk(*child, root, visit);
    }
  }

  // The style an element inherits when it has no parent element: the initial
  // values, which are what the root inherits from.
  static ComputedStyle InitialStyle() { return ComputedStyle{}; }

  std::size_t RuleCount() const { return rules_.size(); }

 private:
  struct Entry {
    Selector selector;
    std::vector<Declaration> declarations;
    Origin origin = Origin::Author;
    Specificity specificity;
    // Position in the sheet, which is the last tiebreak. Two rules that are
    // equal in every other respect are decided by which came later.
    std::size_t order = 0;
  };

  template <typename Visitor>
  void Walk(const dom::Node& node, const ComputedStyle& parent, Visitor& visit) const {
    ComputedStyle style = parent;
    if (node.IsElement()) {
      const auto& element = static_cast<const dom::Element&>(node);
      style = StyleFor(element, parent);
      visit(element, style);
    }
    for (const std::unique_ptr<dom::Node>& child : node.Children()) {
      Walk(*child, style, visit);
    }
  }

  std::vector<Entry> rules_;
  std::size_t next_order_ = 0;
};

// The built-in stylesheet. Every browser has one, and without it `<div>` is
// inline and every document is one long line of text.
//
// Compiled in rather than loaded from disk: it is not user content, and a
// stylesheet the browser reads at startup is a file somebody can replace.
std::string_view UserAgentStyleSheet();

// Applies one declaration to a style. Exposed because it is the single place a
// property name becomes a value, and a property that parses but is not applied
// is invisible without a direct test.
//
// True when the declaration was applied: the property is one this engine
// implements *and* the value is one it understands. False is every other case,
// and the two are deliberately one answer, because CSS makes no distinction --
// an unknown property and a bad value are both dropped, and `@supports` reports
// no for both. This return value is what makes SupportsDeclaration honest: it
// cannot drift from the property table because it *is* the property table.
bool ApplyDeclaration(const Declaration& declaration, const ComputedStyle& parent,
                      ComputedStyle& style);

// Whether this engine supports `property: value` -- the question `@supports`
// asks, answered by trying it.
//
// A custom property is always supported; it has no grammar to fail. Anything
// else is applied to a scratch style, and the answer is whether it took. A
// table of supported names maintained beside the implementation would be the
// obvious alternative and is the trap ADR 0014 §3 names: it starts correct and
// then a property is added without it, and the page is told no about something
// that works -- or, worse, yes about something that does not.
bool SupportsDeclaration(std::string_view property, std::string_view value);

}  // namespace microbrowser::css
