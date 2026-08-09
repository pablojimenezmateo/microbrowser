#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "css/Animation.h"
#include "css/Timing.h"
#include "dom/Node.h"

namespace microbrowser::bindings {

// One keyframe of an `Element.animate` call: offset in [0, 1] and property/value
// pairs already converted to CSS names (`transform`, not `webkitTransform`).
struct WaapiKeyframe {
  double offset = 0.0;
  std::vector<std::pair<std::string, std::string>> declarations;
};

// Timing for one programmatic animation. Mirrors `css::AnimationSpec` fields the
// engine already understands, so Apply reuses the same progress/fill/direction
// math as `@keyframes`.
struct WaapiTiming {
  double duration_ms = 0.0;
  double delay_ms = 0.0;
  double iterations = 1.0;
  css::TimingFunction easing;
  css::AnimationSpec::Direction direction = css::AnimationSpec::Direction::Normal;
  css::AnimationSpec::Fill fill = css::AnimationSpec::Fill::None;
};

enum class WaapiPlayState : std::uint8_t {
  Idle,
  Running,
  Paused,
  Finished,
};

// Where `Element.animate` goes. Declared here and implemented by `src/engine`,
// the same inversion GeometrySource uses: bindings may not include engine
// (ADR 0008), and the animated value must land through AdjustStyle/Apply rather
// than `el.style` writes (TD-0021 — that path is the polyfill).
class AnimationSource {
 public:
  virtual ~AnimationSource() = default;

  // Starts a programmatic effect. Zero when the request is empty or the engine
  // is at its running-animation bound. `base` is the element's cascade style at
  // start, used for missing keyframe properties.
  virtual std::uint64_t StartAnimation(dom::Element& element,
                                       std::vector<WaapiKeyframe> keyframes,
                                       WaapiTiming timing) = 0;
  virtual void PauseAnimation(std::uint64_t id) = 0;
  virtual void PlayAnimation(std::uint64_t id) = 0;
  virtual void CancelAnimation(std::uint64_t id) = 0;
  virtual WaapiPlayState AnimationPlayState(std::uint64_t id) const = 0;
  virtual std::optional<double> AnimationCurrentTimeMs(std::uint64_t id) const = 0;
  virtual void SetAnimationCurrentTimeMs(std::uint64_t id, double local_ms) = 0;

  // Ids that finished (or were cancelled) since the last take — settle
  // `finished` promises. `cancelled` is true for `cancel()` / navigation.
  struct FinishedAnimation {
    std::uint64_t id = 0;
    bool cancelled = false;
  };
  virtual std::vector<FinishedAnimation> TakeFinishedAnimations() = 0;
};

}  // namespace microbrowser::bindings
