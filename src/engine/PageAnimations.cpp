// The animation pass, and the resolver it hangs off.
//
// ADR 0014 §5, session 35. Its own translation unit because Page.cpp reached the module's line cap --
// and the cap means a missing translation unit rather than a bigger file. The seam is real: everything
// here is *where* animation meets the cascade, and everything that decides how a value moves over time
// is `engine::Animations` and `css::Animation`.
//
// The one thing worth reading before the code is `ResetResolver`. There are two places that need a
// fresh resolver -- a navigation, and a re-parse of the stylesheets after a resize -- and when it was
// two lines the second one silently dropped the animation pass. A pointer set in one place and
// clobbered by an assignment in another produces no error; the feature just stops working, which is
// what happened, and the diagnosis was that `AdjustStyle` was never called at all.

#include <algorithm>
#include <string>
#include <vector>

#include "css/StyleResolver.h"
#include "dom/Node.h"
#include "engine/Page.h"

namespace microbrowser::engine {

void Page::ResetResolver() {
  resolver_ = css::StyleResolver{};
  resolver_.SetAdjuster(this);
}

css::ComputedStyle Page::StyleOfForTesting(const dom::Element& element) const {
  // Walks up to build the parent chain, because a resolved style depends on its parent's -- and a test
  // that passed the initial style as the parent would be asserting about a different element.
  std::vector<const dom::Element*> chain;
  for (const dom::Node* node = &element; node != nullptr; node = node->Parent()) {
    if (node->IsElement()) {
      chain.push_back(static_cast<const dom::Element*>(node));
    }
  }
  css::ComputedStyle style = css::StyleResolver::InitialStyle();
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    style = resolver_.StyleFor(**it, style);
  }
  return style;
}

void Page::AdjustStyle(const dom::Element& element, css::ComputedStyle& style) const {

  // **Observe first, then apply**, and the order is the whole design. What arrives here is the
  // cascade's answer with no animation in it, which is exactly the value a transition needs to compare
  // against the previous one -- so starting transitions and applying them are one pass over the
  // document rather than two, and there is no way for the two to see different styles.
  animations_.ObserveStyle(element, style, animation_time_ms_);
  animations_.Apply(element, style, animation_time_ms_);
}

void Page::CollectKeyframes(const css::StyleSheet& sheet) {
  if (sheet.keyframes.empty()) {
    return;
  }
  std::vector<css::KeyframesRule> merged = keyframes_;
  for (const css::KeyframesRule& rule : sheet.keyframes) {
    const auto existing = std::find_if(merged.begin(), merged.end(),
                                       [&rule](const css::KeyframesRule& held) {
                                         return held.name == rule.name;
                                       });
    if (existing != merged.end()) {
      *existing = rule;
    } else {
      merged.push_back(rule);
    }
  }
  keyframes_ = merged;
  animations_.SetKeyframes(std::move(merged));
}

}  // namespace microbrowser::engine
