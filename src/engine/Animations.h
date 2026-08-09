#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
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
// Programmatic effects from `Element.animate` (TD-0021 / Web Animations) live here too: same
// Apply path, same NextDelayMs nullopt-when-idle rule. Writing `el.style` every frame is the
// polyfill; this is the native answer that stops that.
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
  void ObserveStyle(const dom::Element& element, const css::ComputedStyle& resolved,
                    std::int64_t now_ms);

  void Apply(const dom::Element& element, css::ComputedStyle& style, std::int64_t now_ms) const;

  std::optional<std::uint32_t> NextDelayMs(std::int64_t now_ms) const;

  bool Advance(std::int64_t now_ms);

  bool Running() const {
    return !transitions_.empty() || !animations_.empty() || !programmatic_.empty();
  }

  bool TickNeedsLayout() const;

  void SetKeyframes(std::vector<css::KeyframesRule> keyframes) {
    keyframes_ = std::move(keyframes);
  }
  const css::KeyframesRule* Keyframes(const std::string& name) const;

  // `Element.animate`: keyframes are already CSS names and offsets. Zero when empty or at
  // `kMaxRunning`. `base` is the cascade style at start (missing keyframe props fall back to it).
  std::uint64_t StartProgrammatic(const dom::Element& element, css::KeyframesRule frames,
                                  css::AnimationSpec spec, css::ComputedStyle base,
                                  std::int64_t now_ms);
  void PauseProgrammatic(std::uint64_t id, std::int64_t now_ms);
  void PlayProgrammatic(std::uint64_t id, std::int64_t now_ms);
  void CancelProgrammatic(std::uint64_t id);
  bool ProgrammaticPaused(std::uint64_t id) const;
  bool ProgrammaticFinished(std::uint64_t id) const;
  bool ProgrammaticExists(std::uint64_t id) const;
  std::optional<double> ProgrammaticCurrentTimeMs(std::uint64_t id, std::int64_t now_ms) const;
  void SetProgrammaticCurrentTimeMs(std::uint64_t id, double local_ms, std::int64_t now_ms);

  // Ids that finished or were cancelled since the last take (for `finished` promises).
  struct FinishedNotice {
    std::uint64_t id = 0;
    bool cancelled = false;
  };
  std::vector<FinishedNotice> TakeFinishedProgrammatic();

  void Clear();

  std::size_t RunningCount() const {
    return transitions_.size() + animations_.size() + programmatic_.size();
  }

 private:
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
    css::ComputedStyle base;
  };

  struct RunningProgrammatic {
    std::uint64_t id = 0;
    const dom::Element* element = nullptr;
    css::KeyframesRule frames;
    css::AnimationSpec spec;
    css::ComputedStyle base;
    std::int64_t start_ms = 0;
    // Wall time when pause began; subtracted from elapsed so pause does not advance.
    std::int64_t pause_started_ms = 0;
    std::int64_t paused_total_ms = 0;
    bool hold_finished = false;
  };

  void ApplyKeyframeEffect(const css::AnimationSpec& spec, const css::KeyframesRule& rule,
                           const css::ComputedStyle& base, std::int64_t start_ms,
                           std::int64_t now_ms, css::ComputedStyle& style) const;
  void ApplyAnimation(const RunningAnimation& running, css::ComputedStyle& style,
                      std::int64_t now_ms) const;

  using Key = std::pair<const dom::Element*, css::AnimatableProperty>;
  std::map<Key, RunningTransition> transitions_;
  std::map<std::pair<const dom::Element*, std::string>, RunningAnimation> animations_;
  std::map<std::uint64_t, RunningProgrammatic> programmatic_;
  std::map<const dom::Element*, css::ComputedStyle> previous_;
  std::vector<css::KeyframesRule> keyframes_;
  std::vector<FinishedNotice> finished_programmatic_;
  std::uint64_t next_programmatic_id_ = 1;
};

}  // namespace microbrowser::engine
