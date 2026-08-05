#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "css/ComputedStyle.h"
#include "css/StyleSheet.h"
#include "dom/FlatTree.h"
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

// What re-resolving the cascade after a change would cost.
//
// Three answers rather than a bool, because the two ADR 0016 §3 protects are
// different properties: `None` is "a hover no rule mentions costs nothing", and
// `Paint` is "a hover that changes only colour does not run layout". Collapsing
// them loses the first, which is the one that decays silently.
enum class StyleChangeEffect : std::uint8_t {
  None,
  Paint,
  Layout,
};

// Which dynamic states the rules in a cascade actually depend on, and which of
// those can move a box.
//
// The index of ADR 0016 §3, in the form the state row of its table needs: a
// rule is filed under every state its selector mentions, and under `layout_`
// as well when any of its declarations can change geometry. A state no rule
// mentions is then a change nothing has to be recomputed for -- which is what
// makes a mouse crossing a page with no `:hover` rules free rather than a full
// cascade.
//
// Deliberately a *union* over every sheet in the cascade rather than one index
// per sheet. The question a change asks is "could anything have changed", and
// answering it per sheet would mean asking it once per sheet.
//
// The other three rows of that table -- a class, an attribute, a DOM edit --
// are not here yet. They need a change signal finer than dom::Document's
// mutation version, which today says only that *something* moved; adding keys
// nothing can query would be an index that is always right and never consulted.
class StyleInvalidation {
 public:
  // Files one rule. Called per rule as a sheet is added, so the cost is
  // proportional to the rule count and paid once.
  void AddRule(const Selector& selector, const std::vector<Declaration>& declarations);

  // What a change to any of `changed` costs. `changed` is a set, because a
  // pointer moving off one element and onto another can flip several states at
  // once and the answer is the most expensive of them.
  StyleChangeEffect EffectOf(dom::ElementState changed) const;

  bool DependsOn(dom::ElementState state) const { return Any(depends_ & state); }

 private:
  dom::ElementState depends_ = dom::ElementState::None;
  dom::ElementState layout_ = dom::ElementState::None;
};

// Whether changing this property can move a box, as opposed to only changing
// what is drawn in one.
//
// **Unknown answers layout**, which is the whole discipline of the table: a
// property nobody classified makes a change slower than it needed to be, where
// the other default would make it *wrong* -- a box that moved and a screen that
// did not. The paint-only list is therefore short and every entry on it is a
// property this engine implements and has checked.
//
// The same table `transition` will need (ADR 0014 step 5), which is why it is
// built once here rather than inside the invalidation index.
bool PropertyAffectsLayout(std::string_view property);

// Resolves computed styles for a document.
//
// The cascade order, in full, because getting it partly right is the usual
// outcome: origin, then `!important` (which *reverses* the origin order), then
// specificity, then document order. Every one of those is tested.
class StyleResolver {
 public:
  StyleResolver();

  void AddStyleSheet(const StyleSheet& sheet, Origin origin);

  // What the rules in this cascade depend on. ADR 0016 §3 -- the caller asks
  // this *before* deciding whether a state change is worth recomputing
  // anything, which is the only order in which "costs nothing" can be true.
  const StyleInvalidation& Invalidation() const { return invalidation_; }

  // The style of one element, given its parent's already-computed style.
  // Passing the parent style rather than looking it up is what makes
  // inheritance a single pass down the tree instead of a walk up per property.
  ComputedStyle StyleFor(const dom::Element& element, const ComputedStyle& parent) const;

  // Computes styles for the whole document, in tree order, and hands each one
  // to `visit(element, style)`.
  template <typename Visitor>
  void ForEachStyledElement(const dom::Document& document, Visitor&& visit) const {
    const ComputedStyle root = InitialStyle();
    for (dom::Node* child : dom::FlatChildren(document)) {
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
    // The *flattened* tree, so that inheritance crosses a shadow boundary the way
    // the specification says it does -- a slotted node inherits from where it
    // renders, not from where it is written. ADR 0019 §2-3, and the reason the
    // traversal is shared with layout rather than reimplemented: two answers to
    // "what are this node's children for rendering" is the disagreement the ADR
    // refuses to allow.
    for (dom::Node* child : dom::FlatChildren(node)) {
      Walk(*child, style, visit);
    }
  }

  std::vector<Entry> rules_;
  StyleInvalidation invalidation_;
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
