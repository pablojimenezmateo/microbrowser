#pragma once

#include <cstdint>
#include <utility>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "css/Animation.h"
#include "css/ComputedStyle.h"
#include "css/StyleSheet.h"  // for css::KeyframesRule

namespace microbrowser::dom {
class Element;
}

namespace microbrowser::engine {

// Every transition and animation currently running, and the clock they run against.
//
// ADR 0014 §5, session 35. The cascade produces the *specs*; this owns the state that makes them
// happen -- when each started, how far through it is, and what value that means right now.
//
// **The zero-idle-CPU invariant is the point of this class.** ADR 0014 §5 says it in as many words:
// "an animation system that keeps a 60Hz loop alive on a static page is the most likely way this
// project loses its central property". So the interface is arranged so that the loop *cannot* stay
// awake by accident:
//
//   * `NextDelayMs` returns **nothing** when nothing is running. That is the answer that lets the
//     platform wait block forever, and it is the only reason a page with a `:hover` transition costs
//     nothing while the pointer is elsewhere.
//   * A transition is *removed* when it finishes rather than left at progress 1. A finished animation
//     that stayed in the map would keep answering "yes, I need a frame" forever.
//   * `Advance` returns whether anything changed. A frame is produced because a value moved, not
//     because time passed.
//
// The other decision worth reading: **the animated value is applied to the resolved style rather than
// stored beside it.** `Apply` takes the style the cascade produced and overwrites the animated
// properties in place, so layout and paint see one style and there is no second source of truth about
// what an element's width is. It also means an animation costs nothing for an element that has none:
// one map lookup that misses.
class Animations {
 public:
  // What the cascade says about an element, recorded so that the *next* resolve can tell what changed.
  // A transition starts from a difference between two styles, so something has to remember the first
  // one -- and it has to be the engine, because the cascade is a pure function and would produce the
  // same answer twice.
  //
  // `now_ms` is passed in rather than read from a clock, for the reason the timers and the animation
  // frames take one: two decisions inside a single turn must not disagree about what time it is, and a
  // test must be able to say what time it is.
  void ObserveStyle(const dom::Element& element, const css::ComputedStyle& resolved,
                    std::int64_t now_ms);

  // Overwrites the properties any running transition or animation controls. Called after the cascade
  // and before layout reads the style.
  void Apply(const dom::Element& element, css::ComputedStyle& style, std::int64_t now_ms) const;

  // Milliseconds until the next frame is worth producing, or nothing when nothing is running.
  //
  // **Nothing is the important answer.** It is what a settled page returns, and what lets the loop
  // block on the platform wait indefinitely.
  std::optional<std::uint32_t> NextDelayMs(std::int64_t now_ms) const;

  // Drops everything that has finished. True when something was dropped, which means the next frame is
  // the one that shows the final value and the one after it is not needed.
  bool Advance(std::int64_t now_ms);

  // Whether anything at all is running, which is what a caller asks before deciding to lay out again.
  bool Running() const { return !transitions_.empty() || !animations_.empty(); }

  // The `@keyframes` this document defines. Replaced wholesale when the stylesheets change, because a
  // named animation is looked up by name every frame and a stale definition would animate to a value
  // that is no longer written anywhere.
  void SetKeyframes(std::vector<css::KeyframesRule> keyframes) {
    keyframes_ = std::move(keyframes);
  }
  const css::KeyframesRule* Keyframes(const std::string& name) const;

  // A navigation. Everything goes: an animation that outlived its document would keep asking for
  // frames on behalf of a page that is gone, which is the zero-idle-CPU invariant broken by a leak
  // rather than by a design decision.
  void Clear();

  std::size_t RunningCount() const { return transitions_.size() + animations_.size(); }

 private:
  // One running transition: an element, a property, and the two endpoints.
  //
  // The endpoints are whole `ComputedStyle`s rather than single values, and that is deliberate: the
  // interpolation functions take styles, so keeping styles means there is one interpolator rather than
  // one per property type. A `ComputedStyle` is not small, but a page has a handful of transitions
  // running at once and the alternative is a variant per animatable property.
  struct RunningTransition {
    css::AnimatableProperty property = css::AnimatableProperty::None;
    css::ComputedStyle from;
    css::ComputedStyle to;
    std::int64_t start_ms = 0;
    double duration_ms = 0.0;
    double delay_ms = 0.0;
    css::TimingFunction timing;
  };

  struct RunningAnimation {
    css::AnimationSpec spec;
    std::int64_t start_ms = 0;
    // The style the element had when the animation started, which every keyframe's missing properties
    // fall back to -- a `@keyframes` block that only names `transform` must leave everything else at
    // the cascade's value.
    css::ComputedStyle base;
  };

  // One running animation's contribution, written into `style`. Declared after the struct it takes,
  // which is the only ordering that works -- and it is split out because it is the only part of this
  // class that touches `@keyframes`, and because it is where the direction, the iteration count and the
  // fill mode all meet: three small rules that are easy to get right separately and easy to get wrong
  // together.
  void ApplyAnimation(const RunningAnimation& running, css::ComputedStyle& style,
                      std::int64_t now_ms) const;

  // Keyed by element *and* property, because two transitions on one element are independent and a
  // second one on the same property replaces the first rather than joining it.
  using Key = std::pair<const dom::Element*, css::AnimatableProperty>;
  std::map<Key, RunningTransition> transitions_;
  std::map<std::pair<const dom::Element*, std::string>, RunningAnimation> animations_;
  // The style each element had at the last resolve, which is what a transition's "from" comes from.
  std::map<const dom::Element*, css::ComputedStyle> previous_;
  std::vector<css::KeyframesRule> keyframes_;
};

}  // namespace microbrowser::engine
