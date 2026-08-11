#include <vector>

#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"
#include "util/PerformanceCounters.h"

// The invalidation index of ADR 0016 §3: which dynamic states the rules in a
// cascade actually depend on, and which of those can move a box.
//
// Its own translation unit rather than a corner of `StyleResolver.cpp` because
// it answers a different question. The resolver's job is "what is this
// element's style"; this is "could anything have changed", asked *before*
// deciding whether to ask the first question at all -- and it is what makes a
// pointer crossing a page with no `:hover` rules cost a bitmask test.

namespace microbrowser::css {

using util::AddPerformanceCounter;
using util::PerfCounterId;

void StyleInvalidation::AddRule(const Selector& selector,
                                const std::vector<Declaration>& declarations) {
  // `DynamicStates()` walks the whole selector, nested lists included -- so a
  // `:has(:hover)` rule is filed under Hover exactly like `:hover` is.
  //
  // **`:has()` inverts the direction of invalidation and this index survives
  // it**, which is worth stating because ADR 0016 §3 predicted it would not.
  // Every other selector's truth depends on the element and its ancestors, so a
  // change to an element can only restyle it and its descendants; `:has()`
  // makes a change to a *descendant* restyle an *ancestor*. That would break an
  // index keyed by element. This one is keyed by **state**, and the answer it
  // gives is document-wide ("recompute everything" or "recompute nothing"), so
  // the direction of the dependency never enters into it.
  //
  // The cost of that is the same as before: a `:has(:hover)` anywhere in the
  // cascade makes every pointer move a full restyle. The cost of getting it
  // *wrong* would be a rule that stops applying, so the coarse answer is the
  // right one until there is a per-element index to be careful with -- and when
  // there is, `:has()` is the case to design it against rather than the case to
  // discover afterwards.
  const dom::ElementState states = selector.DynamicStates();
  if (!Any(states)) {
    return;
  }
  AddPerformanceCounter(PerfCounterId::CssDynamicRulesIndexed);
  depends_ |= states;
  for (const Declaration& declaration : declarations) {
    if (PropertyAffectsLayout(declaration.property)) {
      layout_ |= states;
      return;
    }
  }
}

StyleChangeEffect StyleInvalidation::EffectOf(dom::ElementState changed) const {
  if (Any(changed & layout_)) {
    return StyleChangeEffect::Layout;
  }
  if (Any(changed & depends_)) {
    return StyleChangeEffect::Paint;
  }
  return StyleChangeEffect::None;
}

}  // namespace microbrowser::css
