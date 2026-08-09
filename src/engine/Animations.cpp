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
  // The common path: an element with nothing running. Map lookups that miss keep a page with
  // one animation from paying for it everywhere.
  if (transitions_.empty() && animations_.empty() && programmatic_.empty()) {
    return;
  }
  for (const auto& [key, running] : animations_) {
    if (key.first != &element) {
      continue;
    }
    ApplyAnimation(running, style, now_ms);
  }
  for (const auto& [id, running] : programmatic_) {
    (void)id;
    if (running.element != &element) {
      continue;
    }
    const std::int64_t effective_now =
        running.spec.paused && running.pause_started_ms > 0
            ? running.pause_started_ms
            : now_ms - running.paused_total_ms;
    ApplyKeyframeEffect(running.spec, running.frames, running.base, running.start_ms,
                        effective_now, style);
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
  const css::KeyframesRule* rule = Keyframes(running.spec.name);
  if (rule == nullptr) {
    return;
  }
  ApplyKeyframeEffect(running.spec, *rule, running.base, running.start_ms, now_ms, style);
}

void Animations::ApplyKeyframeEffect(const css::AnimationSpec& spec, const css::KeyframesRule& rule,
                                     const css::ComputedStyle& base, std::int64_t start_ms,
                                     std::int64_t now_ms, css::ComputedStyle& style) const {
  if (rule.frames.empty() || spec.duration_ms <= 0.0) {
    return;
  }
  const double elapsed = static_cast<double>(now_ms - start_ms) - spec.delay_ms;
  const double total = spec.duration_ms * spec.iterations;

  double iteration_progress = 0.0;
  double iteration_index = 0.0;
  if (elapsed < 0.0) {
    if (spec.fill != css::AnimationSpec::Fill::Backwards &&
        spec.fill != css::AnimationSpec::Fill::Both) {
      return;
    }
    iteration_progress = 0.0;
  } else if (elapsed >= total) {
    if (spec.fill != css::AnimationSpec::Fill::Forwards &&
        spec.fill != css::AnimationSpec::Fill::Both) {
      return;
    }
    iteration_progress = 1.0;
    iteration_index = std::max(0.0, std::ceil(spec.iterations) - 1.0);
  } else {
    iteration_index = std::floor(elapsed / spec.duration_ms);
    iteration_progress = std::fmod(elapsed, spec.duration_ms) / spec.duration_ms;
  }

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
  const double eased = css::Progress(spec.timing, iteration_progress);

  const css::Keyframe* before = nullptr;
  const css::Keyframe* after = nullptr;
  for (const css::Keyframe& frame : rule.frames) {
    if (frame.offset <= eased && (before == nullptr || frame.offset >= before->offset)) {
      before = &frame;
    }
    if (frame.offset >= eased && after == nullptr) {
      after = &frame;
    }
  }
  const auto resolve = [&base](const css::Keyframe* frame) {
    css::ComputedStyle out = base;
    if (frame == nullptr) {
      return out;
    }
    for (const auto& [property, value] : frame->declarations) {
      (void)css::ApplyDeclaration(css::Declaration{property, value, false}, base, out);
    }
    return out;
  };
  const css::ComputedStyle from = resolve(before);
  const css::ComputedStyle to = resolve(after);
  const double span = after != nullptr && before != nullptr ? after->offset - before->offset : 0.0;
  const double local = span > 0.0 ? (eased - before->offset) / span : (after != nullptr ? 1.0 : 0.0);

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
  if (transitions_.empty() && animations_.empty() && programmatic_.empty()) {
    return std::nullopt;
  }
  std::optional<std::uint32_t> soonest;
  const auto consider = [&soonest](double milliseconds) {
    const double clamped = std::clamp(milliseconds, 0.0, 60000.0);
    const auto value = static_cast<std::uint32_t>(clamped);
    soonest = soonest.has_value() ? std::min(*soonest, value) : value;
  };
  for (const auto& [key, running] : transitions_) {
    (void)key;
    const double elapsed = static_cast<double>(now_ms - running.start_ms);
    if (elapsed < running.delay_ms) {
      consider(running.delay_ms - elapsed);
    } else {
      consider(16.0);
    }
  }
  for (const auto& [key, running] : animations_) {
    (void)key;
    if (running.spec.paused) {
      continue;
    }
    const double elapsed = static_cast<double>(now_ms - running.start_ms);
    if (elapsed < running.spec.delay_ms) {
      consider(running.spec.delay_ms - elapsed);
    } else {
      consider(16.0);
    }
  }
  for (const auto& [id, running] : programmatic_) {
    (void)id;
    if (running.spec.paused || running.hold_finished) {
      continue;
    }
    const double elapsed =
        static_cast<double>(now_ms - running.start_ms - running.paused_total_ms);
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
  for (auto it = transitions_.begin(); it != transitions_.end();) {
    const double elapsed = static_cast<double>(now_ms - it->second.start_ms);
    if (elapsed >= it->second.delay_ms + it->second.duration_ms) {
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
      it = animations_.erase(it);
      changed = true;
      AddPerformanceCounter(PerfCounterId::AnimationsFinished);
      continue;
    }
    ++it;
  }
  for (auto it = programmatic_.begin(); it != programmatic_.end();) {
    RunningProgrammatic& running = it->second;
    if (running.hold_finished || running.spec.paused) {
      ++it;
      continue;
    }
    const double elapsed =
        static_cast<double>(now_ms - running.start_ms - running.paused_total_ms) -
        running.spec.delay_ms;
    const double total = running.spec.duration_ms * running.spec.iterations;
    if (running.spec.duration_ms > 0.0 && elapsed >= total) {
      finished_programmatic_.push_back(FinishedNotice{running.id, false});
      AddPerformanceCounter(PerfCounterId::WaapiAnimationsFinished);
      if (running.spec.fill == css::AnimationSpec::Fill::Forwards ||
          running.spec.fill == css::AnimationSpec::Fill::Both) {
        running.hold_finished = true;
        changed = true;
        ++it;
        continue;
      }
      it = programmatic_.erase(it);
      changed = true;
      continue;
    }
    ++it;
  }
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
  for (const auto& [id, running] : programmatic_) {
    (void)id;
    for (const css::Keyframe& frame : running.frames.frames) {
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

std::uint64_t Animations::StartProgrammatic(const dom::Element& element, css::KeyframesRule frames,
                                            css::AnimationSpec spec, css::ComputedStyle base,
                                            std::int64_t now_ms) {
  if (frames.frames.empty() || spec.duration_ms < 0.0 || RunningCount() >= kMaxRunning) {
    return 0;
  }
  // Sort offsets so ApplyKeyframeEffect's before/after walk is monotonic.
  std::sort(frames.frames.begin(), frames.frames.end(),
            [](const css::Keyframe& a, const css::Keyframe& b) { return a.offset < b.offset; });
  const std::uint64_t id = next_programmatic_id_++;
  RunningProgrammatic started;
  started.id = id;
  started.element = &element;
  started.frames = std::move(frames);
  started.spec = std::move(spec);
  started.base = std::move(base);
  started.start_ms = now_ms;
  programmatic_.emplace(id, std::move(started));
  AddPerformanceCounter(PerfCounterId::WaapiAnimationsStarted);
  return id;
}

void Animations::PauseProgrammatic(std::uint64_t id, std::int64_t now_ms) {
  const auto found = programmatic_.find(id);
  if (found == programmatic_.end() || found->second.spec.paused || found->second.hold_finished) {
    return;
  }
  found->second.spec.paused = true;
  found->second.pause_started_ms = now_ms;
}

void Animations::PlayProgrammatic(std::uint64_t id, std::int64_t now_ms) {
  const auto found = programmatic_.find(id);
  if (found == programmatic_.end() || !found->second.spec.paused) {
    return;
  }
  if (found->second.pause_started_ms > 0) {
    found->second.paused_total_ms += now_ms - found->second.pause_started_ms;
    found->second.pause_started_ms = 0;
  }
  found->second.spec.paused = false;
}

void Animations::CancelProgrammatic(std::uint64_t id) {
  const auto found = programmatic_.find(id);
  if (found == programmatic_.end()) {
    return;
  }
  finished_programmatic_.push_back(FinishedNotice{id, true});
  programmatic_.erase(found);
  AddPerformanceCounter(PerfCounterId::WaapiAnimationsCancelled);
}

bool Animations::ProgrammaticPaused(std::uint64_t id) const {
  const auto found = programmatic_.find(id);
  return found != programmatic_.end() && found->second.spec.paused;
}

bool Animations::ProgrammaticFinished(std::uint64_t id) const {
  const auto found = programmatic_.find(id);
  return found != programmatic_.end() && found->second.hold_finished;
}

bool Animations::ProgrammaticExists(std::uint64_t id) const {
  return programmatic_.find(id) != programmatic_.end();
}

std::optional<double> Animations::ProgrammaticCurrentTimeMs(std::uint64_t id,
                                                            std::int64_t now_ms) const {
  const auto found = programmatic_.find(id);
  if (found == programmatic_.end()) {
    return std::nullopt;
  }
  const RunningProgrammatic& running = found->second;
  const std::int64_t effective =
      running.spec.paused && running.pause_started_ms > 0
          ? running.pause_started_ms
          : now_ms - running.paused_total_ms;
  return static_cast<double>(effective - running.start_ms);
}

void Animations::SetProgrammaticCurrentTimeMs(std::uint64_t id, double local_ms,
                                              std::int64_t now_ms) {
  const auto found = programmatic_.find(id);
  if (found == programmatic_.end()) {
    return;
  }
  RunningProgrammatic& running = found->second;
  running.paused_total_ms = 0;
  running.pause_started_ms = running.spec.paused ? now_ms : 0;
  running.start_ms = now_ms - static_cast<std::int64_t>(local_ms);
  running.hold_finished = false;
}

std::vector<Animations::FinishedNotice> Animations::TakeFinishedProgrammatic() {
  std::vector<FinishedNotice> out = std::move(finished_programmatic_);
  finished_programmatic_.clear();
  return out;
}

void Animations::Clear() {
  for (const auto& [id, running] : programmatic_) {
    (void)running;
    finished_programmatic_.push_back(FinishedNotice{id, true});
  }
  transitions_.clear();
  animations_.clear();
  programmatic_.clear();
  previous_.clear();
}

}  // namespace microbrowser::engine
