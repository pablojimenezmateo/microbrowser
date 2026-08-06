#pragma once

#include <cstdint>
#include <string_view>

namespace microbrowser::css {

// An easing function: the curve between "started" and "finished".
//
// ADR 0014 §5, session 35. This is the smallest piece of the animation system and the one worth
// having on its own, because it is a **pure function of one number** — no element, no clock, no style
// — and therefore the only part that can be checked against the specification's own arithmetic rather
// than against a rendering.
//
// `linear` is a straight line and nothing else is. `ease`, `ease-in`, `ease-out` and `ease-in-out` are
// defined by the specification *as* particular cubic Béziers, so they are stored as their control
// points rather than as cases in a switch: a page that writes `cubic-bezier(0.25, 0.1, 0.25, 1)` and a
// page that writes `ease` must animate identically, and they will not if one is a curve and the other
// is a special case.
struct TimingFunction {
  enum class Kind : std::uint8_t {
    CubicBezier,
    Steps,
  };

  // How a step function distributes its jumps. The two the specification names `start` and `end`, plus
  // the two `jump-*` keywords that generalise them -- kept because `step-start` is `steps(1, start)`
  // and expressing one in terms of the other is what stops them disagreeing.
  enum class StepPosition : std::uint8_t { JumpStart, JumpEnd, JumpNone, JumpBoth };

  Kind kind = Kind::CubicBezier;
  // The two control points, with the first and last fixed at (0,0) and (1,1) by definition.
  float x1 = 0.0f;
  float y1 = 0.0f;
  float x2 = 1.0f;
  float y2 = 1.0f;
  std::uint32_t steps = 1;
  StepPosition position = StepPosition::JumpEnd;

  friend bool operator==(const TimingFunction&, const TimingFunction&) = default;
};

// The four keyword curves, as the control points the specification gives them.
TimingFunction LinearTiming();
TimingFunction EaseTiming();
TimingFunction EaseInTiming();
TimingFunction EaseOutTiming();
TimingFunction EaseInOutTiming();

// `t` in [0,1] to eased progress.
//
// **The output is deliberately not clamped to [0,1].** `cubic-bezier(0, 1.5, 1, 1)` overshoots, which
// is what a page asking for a bounce is asking for, and clamping it would silently flatten every
// spring animation on the web. Callers that need a clamped value -- an opacity, a colour channel --
// clamp at the point they apply it, where the range is actually known.
double Progress(const TimingFunction& timing, double t);

// One `<easing-function>`, or nothing when the text is not one. Nothing rather than a default,
// because an unparseable timing function makes the whole declaration invalid and the *previous* value
// must survive -- a silent fallback to `ease` would animate at a speed nobody asked for.
bool ParseTimingFunction(std::string_view text, TimingFunction& out);

}  // namespace microbrowser::css
