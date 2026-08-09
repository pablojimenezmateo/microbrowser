#include "engine/Animations.h"

#include <algorithm>
#include <cmath>

#include "css/StyleResolver.h"
#include "dom/Node.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// How many transitions and animations one document may have running at once.
//
// A bound, because a page controls both: `document.querySelectorAll('*').forEach(e => e.style...)` on
// a large document starts one per element, and each one holds two `ComputedStyle`s. Ten thousand is far
// past any real page -- reddit's front page has a few dozen -- and it is a *refusal to start a new one*
// rather than an eviction of a running one, because dropping an animation halfway leaves an element at
// a value nobody asked for.
constexpr std::size_t kMaxRunning = 10000;

// Every property a transition spec covers. `all` expands to the list, which is what makes
// `transition: all 0.2s` work -- and what makes it the expensive spelling, because it means comparing
// every animatable property on every restyle.
void PropertiesOf(const css::TransitionSpec& spec,
                  std::vector<css::AnimatableProperty>& out) {
  if (!spec.all_properties) {
    if (spec.property != css::AnimatableProperty::None) {
      out.push_back(spec.property);
    }
    return;
  }
  static constexpr css::AnimatableProperty kAll[] = {
      css::AnimatableProperty::Color,        css::AnimatableProperty::BackgroundColor,
      css::AnimatableProperty::BorderColor,  css::AnimatableProperty::Width,
      css::AnimatableProperty::Height,       css::AnimatableProperty::MarginTop,
      css::AnimatableProperty::MarginRight,  css::AnimatableProperty::MarginBottom,
      css::AnimatableProperty::MarginLeft,   css::AnimatableProperty::PaddingTop,
      css::AnimatableProperty::PaddingRight, css::AnimatableProperty::PaddingBottom,
      css::AnimatableProperty::PaddingLeft,  css::AnimatableProperty::Top,
      css::AnimatableProperty::Right,        css::AnimatableProperty::Bottom,
      css::AnimatableProperty::Left,         css::AnimatableProperty::FontSize,
      css::AnimatableProperty::LineHeight,   css::AnimatableProperty::BorderWidth,
      css::AnimatableProperty::Transform};
  for (const css::AnimatableProperty property : kAll) {
    out.push_back(property);
  }
}

// Whether one property differs between two styles.
//
// Done by *interpolating to both ends and comparing* rather than by a switch over every property's
// accessor. That is not cleverness for its own sake: a second switch would be a second list of which
// member each property maps to, and the two would drift -- exactly the failure `css::InheritInto` was
// created to stop. One list, in `InterpolateProperty`, read twice.
bool AnimatableAffectsLayout(css::AnimatableProperty property) {
  switch (property) {
    case css::AnimatableProperty::None:
    case css::AnimatableProperty::Color:
    case css::AnimatableProperty::BackgroundColor:
    case css::AnimatableProperty::BorderColor:
    case css::AnimatableProperty::Transform:
      return false;
    default:
      return true;
  }
}

bool Differs(css::AnimatableProperty property, const css::ComputedStyle& a,
             const css::ComputedStyle& b) {
  css::ComputedStyle at_a = a;
  css::ComputedStyle at_b = a;
  if (!css::InterpolateProperty(property, a, b, 0.0, at_a) ||
      !css::InterpolateProperty(property, a, b, 1.0, at_b)) {
    return false;
  }
  return !(at_a == at_b);
}

// The eased progress of one running transition, and whether it is finished.
double TransitionProgress(double elapsed_ms, double delay_ms, double duration_ms,
                          const css::TimingFunction& timing, bool& finished) {
  const double since_start = elapsed_ms - delay_ms;
  if (since_start <= 0.0) {
    finished = false;
    // Before the delay elapses the property holds its *starting* value, which is what makes a delayed
    // transition wait rather than creep.
    return 0.0;
  }
  if (duration_ms <= 0.0 || since_start >= duration_ms) {
    finished = true;
    return 1.0;
  }
  finished = false;
  return css::Progress(timing, since_start / duration_ms);
}

}  // namespace

void Animations::ObserveStyle(const dom::Element& element, const css::ComputedStyle& resolved,
                              std::int64_t now_ms) {
  const auto previous = previous_.find(&element);
  // The first time an element is seen there is nothing to transition *from*. Starting one would animate
  // every property from its initial value on the first frame the page appears, which is the classic bug
  // a page works around with a `no-transition` class -- and not something to reproduce.
  const bool first_seen = previous == previous_.end();

  // Animations first, because they are declarative: a name appearing in `animation-name` starts one and
  // a name disappearing stops it, with no comparison of values involved.
  for (const css::AnimationSpec& spec : resolved.animations) {
    const auto key = std::make_pair(&element, spec.name);
    const auto running = animations_.find(key);
    if (running != animations_.end()) {
      // Already running. The spec is *updated* rather than restarted, so that a page changing the
      // duration mid-animation does not jump back to the beginning -- which is what the specification
      // says and what a hover that shortens a spin depends on.
      running->second.spec = spec;
      continue;
    }
    if (spec.duration_ms <= 0.0 || spec.name.empty() || RunningCount() >= kMaxRunning) {
      continue;
    }
    RunningAnimation started;
    started.spec = spec;
    started.start_ms = now_ms;
    started.base = resolved;
    animations_.emplace(key, std::move(started));
    AddPerformanceCounter(PerfCounterId::AnimationsStarted);
  }
  // A name no longer in the list stops. Collected first, because erasing inside the walk invalidates.
  std::vector<std::pair<const dom::Element*, std::string>> gone;
  for (const auto& [key, running] : animations_) {
    if (key.first != &element) {
      continue;
    }
    const bool still_named =
        std::any_of(resolved.animations.begin(), resolved.animations.end(),
                    [&key](const css::AnimationSpec& spec) { return spec.name == key.second; });
    if (!still_named) {
      gone.push_back(key);
    }
  }
  for (const auto& key : gone) {
    animations_.erase(key);
  }

  if (!first_seen) {
    std::vector<css::AnimatableProperty> properties;
    for (const css::TransitionSpec& spec : resolved.transitions) {
      if (spec.duration_ms <= 0.0) {
        continue;  // a zero duration is how a page turns one off, so nothing starts
      }
      properties.clear();
      PropertiesOf(spec, properties);
      for (const css::AnimatableProperty property : properties) {
        if (!Differs(property, previous->second, resolved)) {
          continue;
        }
        const Key key{&element, property};
        const auto existing = transitions_.find(key);
        // **A transition that is already running is replaced from where it currently is**, not from the
        // old start. That is what makes a hover-in interrupted by a hover-out reverse smoothly instead
        // of jumping to the far end and coming back -- and it is why the `from` is the *interpolated*
        // style rather than the previous one.
        css::ComputedStyle from = previous->second;
        if (existing != transitions_.end()) {
          bool finished = false;
          const double elapsed = static_cast<double>(now_ms - existing->second.start_ms);
          const double t = TransitionProgress(elapsed, existing->second.delay_ms,
                                              existing->second.duration_ms,
                                              existing->second.timing, finished);
          from = existing->second.from;
          css::InterpolateProperty(property, existing->second.from, existing->second.to, t, from);
        }
        if (existing == transitions_.end() && RunningCount() >= kMaxRunning) {
          continue;
        }
        RunningTransition started;
        started.property = property;
        started.from = from;
        started.to = resolved;
        started.start_ms = now_ms;
        started.duration_ms = spec.duration_ms;
        started.delay_ms = spec.delay_ms;
        started.timing = spec.timing;
        transitions_[key] = std::move(started);
        AddPerformanceCounter(PerfCounterId::TransitionsStarted);
      }
    }
  }
  previous_[&element] = resolved;
}

void Animations::Apply(const dom::Element& element, css::ComputedStyle& style,
                       std::int64_t now_ms) const {
  // The common path: an element with nothing running. Two map lookups that miss, and no copy of a
  // `ComputedStyle` -- which is what keeps a page with one animation from paying for it everywhere.
  if (transitions_.empty() && animations_.empty()) {
    return;
  }
  for (const auto& [key, running] : animations_) {
    if (key.first != &element) {
      continue;
    }
    ApplyAnimation(running, style, now_ms);
  }
  // Transitions after animations, because a transition's endpoints came from the cascade and an
  // animation's from `@keyframes`: the specification's cascade order puts animations above transitions,
  // so the transition writes last and wins.
  for (const auto& [key, running] : transitions_) {
    if (key.first != &element) {
      continue;
    }
    bool finished = false;
    const double elapsed = static_cast<double>(now_ms - running.start_ms);
    const double t = TransitionProgress(elapsed, running.delay_ms, running.duration_ms,
                                        running.timing, finished);
    css::InterpolateProperty(running.property, running.from, running.to, t, style);
  }
}

void Animations::ApplyAnimation(const RunningAnimation& running, css::ComputedStyle& style,
                                std::int64_t now_ms) const {
  const css::AnimationSpec& spec = running.spec;
  const css::KeyframesRule* rule = Keyframes(spec.name);
  if (rule == nullptr || rule->frames.empty() || spec.duration_ms <= 0.0) {
    // A name with no `@keyframes` behind it animates nothing. Not an error -- a page may set
    // `animation-name` before the stylesheet defining it has arrived, and the animation starts when it
    // does.
    return;
  }
  const double elapsed = static_cast<double>(now_ms - running.start_ms) - spec.delay_ms;
  const double total = spec.duration_ms * spec.iterations;

  // The three cases the fill mode exists for, and they are genuinely three: before the delay is out,
  // during, and after the last iteration.
  double iteration_progress = 0.0;
  double iteration_index = 0.0;
  if (elapsed < 0.0) {
    if (spec.fill != css::AnimationSpec::Fill::Backwards &&
        spec.fill != css::AnimationSpec::Fill::Both) {
      return;  // no backwards fill: the element shows its cascade value until the delay is out
    }
    iteration_progress = 0.0;
  } else if (elapsed >= total) {
    if (spec.fill != css::AnimationSpec::Fill::Forwards &&
        spec.fill != css::AnimationSpec::Fill::Both) {
      return;  // no forwards fill: the element snaps back, which is the default and surprises everyone
    }
    // The final iteration's end, which is not always progress 1: with `alternate` and an even count the
    // animation finishes back at the start, and a `forwards` fill has to hold *that*.
    iteration_progress = 1.0;
    iteration_index = std::max(0.0, std::ceil(spec.iterations) - 1.0);
  } else {
    iteration_index = std::floor(elapsed / spec.duration_ms);
    iteration_progress = std::fmod(elapsed, spec.duration_ms) / spec.duration_ms;
  }

  // The direction, applied to the iteration's progress. `alternate` reverses the odd iterations and
  // `alternate-reverse` the even ones, which is the whole difference between them.
  const bool odd = std::fmod(iteration_index, 2.0) >= 1.0;
  bool reversed = false;
  switch (spec.direction) {
    case css::AnimationSpec::Direction::Normal:
      break;
    case css::AnimationSpec::Direction::Reverse:
      reversed = true;
      break;
    case css::AnimationSpec::Direction::Alternate:
      reversed = odd;
      break;
    case css::AnimationSpec::Direction::AlternateReverse:
      reversed = !odd;
      break;
  }
  if (reversed) {
    iteration_progress = 1.0 - iteration_progress;
  }
  // The timing function applies *within* an iteration, after the direction. That order matters: eased
  // and then reversed is a different curve from reversed and then eased, and the specification says
  // this one.
  const double eased = css::Progress(spec.timing, iteration_progress);

  // The bracketing keyframes. A list with no frame at 0 or at 1 is legal -- `50% { … }` on its own is a
  // valid animation -- and the missing ends are the element's own value, which is what `base` is for.
  const css::Keyframe* before = nullptr;
  const css::Keyframe* after = nullptr;
  for (const css::Keyframe& frame : rule->frames) {
    if (frame.offset <= eased && (before == nullptr || frame.offset >= before->offset)) {
      before = &frame;
    }
    if (frame.offset >= eased && after == nullptr) {
      after = &frame;
    }
  }
  const auto resolve = [&running](const css::Keyframe* frame) {
    css::ComputedStyle out = running.base;
    if (frame == nullptr) {
      return out;
    }
    for (const auto& [property, value] : frame->declarations) {
      // Resolved against the element's *own* style as the parent, which is right for the relative units
      // a keyframe uses in practice -- `em` in a keyframe means the element's font size -- and is
      // recorded as an approximation for the one case it is not: a keyframe that sets `font-size` in
      // `em` resolves against the element's own size rather than its parent's.
      (void)css::ApplyDeclaration(css::Declaration{property, value, false}, running.base, out);
    }
    return out;
  };
  const css::ComputedStyle from = resolve(before);
  const css::ComputedStyle to = resolve(after);
  const double span = after != nullptr && before != nullptr ? after->offset - before->offset : 0.0;
  const double local = span > 0.0 ? (eased - before->offset) / span : (after != nullptr ? 1.0 : 0.0);

  // Every property either keyframe names, interpolated between the two resolved styles. Walked as a set
  // rather than as the union of two lists, because a property in one frame and not the other still
  // animates -- from the element's own value, which `resolve` already put in the other style.
  for (const css::Keyframe* frame : {before, after}) {
    if (frame == nullptr) {
      continue;
    }
    for (const auto& [property, value] : frame->declarations) {
      (void)value;
      const css::AnimatableProperty which = css::AnimatablePropertyFromName(property);
      if (which == css::AnimatableProperty::None) {
        continue;
      }
      css::InterpolateProperty(which, from, to, local, style);
    }
  }
}

std::optional<std::uint32_t> Animations::NextDelayMs(std::int64_t now_ms) const {
  if (transitions_.empty() && animations_.empty()) {
    return std::nullopt;  // the answer a settled page gives, and the reason the loop can block forever
  }
  // A frame at the display's cadence while something is moving. Sixteen milliseconds rather than a
  // computed "when does the next value differ", because every value differs on every frame of a
  // continuous animation -- there is nothing to be gained by asking, and asking would cost a pass over
  // every running animation per frame.
  //
  // The *delay* before a transition starts is worth waiting out exactly, though: a `transition-delay`
  // of two seconds must not cost 125 frames of nothing.
  std::optional<std::uint32_t> soonest;
  const auto consider = [&soonest](double milliseconds) {
    const double clamped = std::clamp(milliseconds, 0.0, 60000.0);
    const auto value = static_cast<std::uint32_t>(clamped);
    soonest = soonest.has_value() ? std::min(*soonest, value) : value;
  };
  for (const auto& [key, running] : transitions_) {
    const double elapsed = static_cast<double>(now_ms - running.start_ms);
    if (elapsed < running.delay_ms) {
      consider(running.delay_ms - elapsed);
    } else {
      consider(16.0);
    }
  }
  for (const auto& [key, running] : animations_) {
    if (running.spec.paused) {
      continue;  // `animation-play-state: paused` asks for no frames at all, which is the whole point
    }
    const double elapsed = static_cast<double>(now_ms - running.start_ms);
    if (elapsed < running.spec.delay_ms) {
      consider(running.spec.delay_ms - elapsed);
    } else {
      consider(16.0);
    }
  }
  return soonest;
}

bool Animations::Advance(std::int64_t now_ms) {
  bool changed = false;
  // A transition is *removed* when it finishes rather than left at progress 1. One that stayed would
  // keep answering "yes, I need a frame" forever, which is the zero-idle-CPU invariant lost to a leak.
  for (auto it = transitions_.begin(); it != transitions_.end();) {
    const double elapsed = static_cast<double>(now_ms - it->second.start_ms);
    if (elapsed >= it->second.delay_ms + it->second.duration_ms) {
      // **The final value is not dropped with it.** It is already in the cascade -- a transition's `to`
      // *is* the resolved style -- so removing the transition leaves the element exactly where the
      // animation was heading. That is why a transition needs no fill mode and an animation does.
      it = transitions_.erase(it);
      changed = true;
      AddPerformanceCounter(PerfCounterId::TransitionsFinished);
    } else {
      ++it;
    }
  }
  for (auto it = animations_.begin(); it != animations_.end();) {
    const css::AnimationSpec& spec = it->second.spec;
    const double elapsed = static_cast<double>(now_ms - it->second.start_ms) - spec.delay_ms;
    const double total = spec.duration_ms * spec.iterations;
    if (spec.duration_ms > 0.0 && elapsed >= total &&
        spec.fill != css::AnimationSpec::Fill::Forwards &&
        spec.fill != css::AnimationSpec::Fill::Both) {
      // No forwards fill, so the element returns to its cascade value and the animation is done.
      it = animations_.erase(it);
      changed = true;
      AddPerformanceCounter(PerfCounterId::AnimationsFinished);
      continue;
    }
    ++it;
  }
  // A finished animation with a forwards fill still holds its final value, so it cannot be erased --
  // but it must stop asking for frames. That is what `Settled` answers, and it is checked here so that
  // `NextDelayMs` does not have to walk the map twice.
  return changed;
}

const css::KeyframesRule* Animations::Keyframes(const std::string& name) const {
  for (const css::KeyframesRule& rule : keyframes_) {
    if (rule.name == name) {
      return &rule;
    }
  }
  return nullptr;
}

bool Animations::TickNeedsLayout() const {
  for (const auto& [key, running] : transitions_) {
    (void)key;
    if (AnimatableAffectsLayout(running.property)) {
      return true;
    }
  }
  for (const auto& [key, running] : animations_) {
    (void)key;
    const css::KeyframesRule* rule = Keyframes(running.spec.name);
    if (rule == nullptr) {
      continue;
    }
    for (const css::Keyframe& frame : rule->frames) {
      for (const auto& [property, value] : frame.declarations) {
        (void)value;
        if (css::PropertyAffectsLayout(property)) {
          return true;
        }
      }
    }
  }
  return false;
}

void Animations::Clear() {
  transitions_.clear();
  animations_.clear();
  previous_.clear();
  // The keyframes stay: they belong to the stylesheets, and `SetKeyframes` replaces them when those
  // change. Clearing them here would drop them on every relayout.
}

}  // namespace microbrowser::engine
