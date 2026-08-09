#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "css/Timing.h"

namespace microbrowser::css {

struct ComputedStyle;

// `transition` and `animation`, as what the cascade stores about them.
//
// ADR 0014 §5, session 35. Two features that share almost everything: a duration, a delay, an easing
// function, and a way of getting a value at a fraction of the way through. They differ in where the
// endpoints come from -- a transition's are "what it was" and "what it is", an animation's are
// `@keyframes` -- and in almost nothing else, which is why the timing model is one type.
//
// **What is deliberately not here is time.** No start instant, no current progress, no element. The
// cascade produces these; `src/engine` owns the running state, because time is the engine's and a
// resolver that knew what time it was could not be a pure function of (element, stylesheets).

// Which properties can be interpolated at all, as this browser's list.
//
// It is short on purpose. A property that is *not* on it does not animate -- it changes at the end of
// the transition's duration, which is what the specification calls a discrete animation and what every
// browser does with `display`. **Answering "animatable" for a property that then interpolated wrongly
// would be worse**: a `font-family` interpolated halfway is a font nobody named.
enum class AnimatableProperty : std::uint8_t {
  None,
  Color,
  BackgroundColor,
  BorderColor,
  // **`opacity` is deliberately not here yet.** The paint property exists (cascade +
  // display-list alpha multiply / skip-at-zero). Animating it still wants an
  // interpolator on this enum; until then a transition on `opacity` flips at the end.
  Width,
  Height,
  MarginTop,
  MarginRight,
  MarginBottom,
  MarginLeft,
  PaddingTop,
  PaddingRight,
  PaddingBottom,
  PaddingLeft,
  Top,
  Right,
  Bottom,
  Left,
  FontSize,
  LineHeight,
  BorderWidth,
  Transform,
};

// The property a name refers to, or `None` when this browser will not interpolate it. Also answers for
// the two names that mean "everything": `all` and the empty string.
AnimatableProperty AnimatablePropertyFromName(std::string_view name);

// One `transition-*` set, per property.
struct TransitionSpec {
  // The property this applies to, or `None` with `all_properties` set.
  AnimatableProperty property = AnimatableProperty::None;
  bool all_properties = false;
  // Milliseconds. Zero duration is legal and means "no transition", which matters: it is how a page
  // turns one off for one property while leaving the shorthand alone.
  double duration_ms = 0.0;
  // Negative is legal and means "start partway in", which is how a staggered list is written.
  double delay_ms = 0.0;
  TimingFunction timing;

  friend bool operator==(const TransitionSpec&, const TransitionSpec&) = default;
};

// One `animation-*` set, per name.
struct AnimationSpec {
  std::string name;
  double duration_ms = 0.0;
  double delay_ms = 0.0;
  TimingFunction timing;
  // `infinite` is stored as a very large count rather than as a flag, because every place that reads
  // it wants "how many" and a flag would make each of them branch. The number is finite so that the
  // arithmetic cannot produce an infinity a caller would have to check for.
  double iterations = 1.0;
  enum class Direction : std::uint8_t { Normal, Reverse, Alternate, AlternateReverse };
  Direction direction = Direction::Normal;
  // `animation-fill-mode`: whether the first keyframe applies before the animation starts and the last
  // one after it ends. Both matter and neither is cosmetic -- a `forwards` fill is what makes an
  // animation that moves something *leave* it there.
  enum class Fill : std::uint8_t { None, Forwards, Backwards, Both };
  Fill fill = Fill::None;
  bool paused = false;

  friend bool operator==(const AnimationSpec&, const AnimationSpec&) = default;
};

// One `@keyframes` block: a name and the styles at each offset.
//
// The declarations are kept as text rather than as a resolved `ComputedStyle`, and that is a real
// decision: a keyframe's values have to be resolved against **the element the animation runs on** --
// `width: 50%` means different pixels for different parents, and `em` means different pixels for
// different font sizes. Resolving them at parse time would freeze them against the wrong element.
struct Keyframe {
  double offset = 0.0;  // 0 for `from`, 1 for `to`, and the percentage otherwise
  std::vector<std::pair<std::string, std::string>> declarations;
};

struct KeyframesRule {
  std::string name;
  // Sorted by offset, and with duplicates kept: two blocks at the same offset are legal and the later
  // one wins per property, which is the cascade rule applied inside an animation.
  std::vector<Keyframe> frames;
};

// Interpolates one property between two resolved styles and writes the result into `out`.
//
// `t` is *eased* progress -- the caller has already applied the timing function -- and it may be
// outside [0,1] because a `cubic-bezier` with a y outside the range overshoots. Each case clamps where
// its own range is known: a colour channel clamps, a length does not, because a margin genuinely can
// go negative and clamping one would turn a bounce into a stop.
//
// False when the property is not one this browser interpolates, which tells the caller to switch the
// value discretely at the end rather than to animate it.
bool InterpolateProperty(AnimatableProperty property, const ComputedStyle& from,
                         const ComputedStyle& to, double t, ComputedStyle& out);

}  // namespace microbrowser::css
