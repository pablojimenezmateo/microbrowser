#include "css/SelectorMatchInternal.h"

#include <cstddef>
#include <memory>
#include <vector>

#include "css/StyleSheet.h"
#include "dom/Node.h"
#include "util/PerformanceCounters.h"

// `:has()`, the relational pseudo-class -- and the only place in this engine
// where matching walks *down* a tree.
//
// Its own translation unit because it is its own cost model. Every other
// selector is answered by walking up from the element, which the tree depth
// bounds at a couple of dozen steps. This one searches a subtree, which nothing
// bounds, and ADR 0016 §1 said it should land "behind a measurement, and if it
// is expensive it stays behind one". The measurement is in
// `bench/CssBenchmarks.cpp` and what it produced is `kMaxHasCandidates`, whose
// reasoning is written where the constant is.

namespace microbrowser::css {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Whether `candidate` stands in relation `combinator` to `anchor`. The other
// half of `:has()`: the search below enumerates a superset and this decides.
bool AnchoredBy(const dom::Element& candidate, Combinator combinator,
                const dom::Element& anchor) {
  switch (combinator) {
    case Combinator::None:
    case Combinator::Descendant:
      for (const dom::Node* at = candidate.Parent(); at != nullptr; at = at->Parent()) {
        if (at == &anchor) {
          return true;
        }
      }
      return false;
    case Combinator::Child:
      return candidate.Parent() == &anchor;
    case Combinator::NextSibling:
    case Combinator::LaterSibling: {
      const dom::Node* parent = candidate.Parent();
      if (parent == nullptr || parent != anchor.Parent()) {
        return false;
      }
      const dom::Element* previous = nullptr;
      for (const std::unique_ptr<dom::Node>& sibling : parent->Children()) {
        if (sibling.get() == &candidate) {
          break;
        }
        if (sibling->IsElement()) {
          const auto* element = static_cast<const dom::Element*>(sibling.get());
          if (combinator == Combinator::LaterSibling && element == &anchor) {
            return true;
          }
          previous = element;
        }
      }
      return combinator == Combinator::NextSibling && previous == &anchor;
    }
  }
  return false;
}

// `:has(rel)`.
//
// Every other selector is answered by walking *up* from the element, which the
// tree depth bounds. This one walks down, and the subtree does not bound
// anything -- so it is the one selector with a budget. `kMaxHasCandidates` and
// the measurement behind it are in `StyleSheet.h`; running out answers "no",
// and `css.has_bound_hit` counts it because a bound nobody can see from outside
// is a rendering difference nobody can explain.
bool HasMatches(const SelectorPart& part, const dom::Element& element, MatchContext context) {
  if (context.depth > kMaxSelectorNestingDepth) {
    return false;
  }
  // The anchor moves; the scope does *not*. `:has()` prepends an implied
  // `:scope` to its argument, but the `:scope` *pseudo-class* still names
  // whatever scoped the whole match -- so `scope1.querySelectorAll(':has(:scope)')`
  // asks which descendants of `scope1` contain `scope1`, and the answer is
  // none. Setting `inner.scope` here instead would have made that selector
  // match everything with a child, which is the opposite answer.
  MatchContext inner = context;
  inner.anchor = &element;
  inner.depth = context.depth + 1;

  std::size_t budget = kMaxHasCandidates;
  bool exhausted = false;

  // The search space, which the leftmost combinator picks: a sibling-relative
  // argument cannot be satisfied by anything inside this element, and a
  // descendant-relative one cannot be satisfied by anything outside it. Getting
  // this wrong is not slow, it is wrong -- `:has(+ .b)` looking in the subtree
  // would match an element whose *child* is `.b`.
  const auto search = [&](const dom::Node& root, const auto& self) -> bool {
    for (const std::unique_ptr<dom::Node>& child : root.Children()) {
      if (!child->IsElement()) {
        continue;
      }
      const auto& candidate = static_cast<const dom::Element&>(*child);
      if (budget == 0) {
        exhausted = true;
        return false;
      }
      --budget;
      for (const Selector& argument : part.arguments) {
        if (argument.compounds.empty()) {
          continue;
        }
        // The cheap rejection, and it is only sound for a one-compound
        // argument. In `:has(> .b .c)` the enumerated candidate is the `.c`,
        // which is a *grand*child; the thing the leading `>` speaks about is
        // the `.b` several steps up, and `MatchesFrom` is what reaches it.
        // Applying the test here regardless was a bug that made every relative
        // selector with more than one compound match nothing.
        if (argument.compounds.size() == 1 &&
            !AnchoredBy(candidate, argument.compounds.front().combinator, element)) {
          continue;
        }
        if (MatchesFrom(argument.compounds, argument.compounds.size() - 1, candidate, inner)) {
          return true;
        }
      }
      if (self(candidate, self)) {
        return true;
      }
    }
    return false;
  };

  bool wants_subtree = false;
  bool wants_siblings = false;
  for (const Selector& argument : part.arguments) {
    if (argument.compounds.empty()) {
      continue;
    }
    switch (argument.compounds.front().combinator) {
      case Combinator::NextSibling:
      case Combinator::LaterSibling:
        wants_siblings = true;
        break;
      default:
        wants_subtree = true;
        break;
    }
  }

  bool found = false;
  if (wants_subtree) {
    found = search(element, search);
  }
  if (!found && wants_siblings && element.Parent() != nullptr) {
    // A following sibling, and everything under it: `:has(~ .b .c)` is a `.c`
    // inside a later sibling. Preceding siblings are not reachable from any
    // relative combinator, so the walk starts after this element.
    bool seen = false;
    for (const std::unique_ptr<dom::Node>& sibling : element.Parent()->Children()) {
      if (sibling.get() == &element) {
        seen = true;
        continue;
      }
      if (!seen || !sibling->IsElement()) {
        continue;
      }
      const auto& candidate = static_cast<const dom::Element&>(*sibling);
      if (budget == 0) {
        exhausted = true;
        break;
      }
      --budget;
      for (const Selector& argument : part.arguments) {
        if (argument.compounds.empty() ||
            (argument.compounds.size() == 1 &&
             !AnchoredBy(candidate, argument.compounds.front().combinator, element))) {
          continue;
        }
        if (MatchesFrom(argument.compounds, argument.compounds.size() - 1, candidate, inner)) {
          found = true;
          break;
        }
      }
      if (found || search(candidate, search)) {
        found = true;
        break;
      }
    }
  }

  AddPerformanceCounter(PerfCounterId::CssHasCandidatesVisited, kMaxHasCandidates - budget);
  if (exhausted) {
    AddPerformanceCounter(PerfCounterId::CssHasBoundHit);
  }
  return found;
}


}  // namespace microbrowser::css
