#pragma once

#include <cstddef>
#include <vector>

#include "css/StyleSheet.h"
#include "dom/Node.h"

// The seam between the two halves of selector matching.
//
// `SelectorMatch.cpp` walks *up* from an element, which the tree depth bounds.
// `SelectorRelational.cpp` is `:has()`, which walks *down*, which nothing
// bounds -- and that difference is the whole of ADR 0016 §1's warning about the
// relational pseudo-class. They are two translation units because they are two
// different cost models over one shared recursion, and this header is exactly
// the three things that recursion needs.
//
// Private to the module. Nothing outside `src/css` may see a `MatchContext`:
// `Selector::Matches(element)` is still the only public entry, and it is still
// a pure function of (element, selector).

namespace microbrowser::css {

// Everything one match needs to know that is *not* (element, selector).
//
// Both pointers exist only for `:has()` and `:scope`, the two selectors defined
// relative to a third thing. Passed by value down the recursion rather than
// held as state, so there is nothing to reset and nothing to leak between two
// unrelated matches.
struct MatchContext {
  // What `:scope` names. Null means the document element, which is what a
  // scopeless match is defined to fall back to.
  const dom::Element* scope = nullptr;
  // The element a *relative* selector hangs off, set only while evaluating a
  // `:has()` argument. Non-null makes the leftmost compound's combinator load
  // bearing -- it is the only place in this model that reads it.
  const dom::Element* anchor = nullptr;
  // Bounds recursion through nested selector lists. The parser already refuses
  // to build one deeper than `kMaxSelectorNestingDepth`; this bounds a selector
  // assembled in code, which has no parser between it and here.
  int depth = 0;
};

// Matches right to left over `compounds`, ending at `index`.
bool MatchesFrom(const std::vector<CompoundSelector>& compounds, std::size_t index,
                 const dom::Element& element, MatchContext context);

// Whether `candidate` stands in relation `combinator` to `anchor` -- the half
// of a relative selector that the compound matcher cannot answer.
bool AnchoredBy(const dom::Element& candidate, Combinator combinator,
                const dom::Element& anchor);

// `:has(rel)`, and the only selector in this engine with a budget on it.
bool HasMatches(const SelectorPart& part, const dom::Element& element, MatchContext context);

}  // namespace microbrowser::css
