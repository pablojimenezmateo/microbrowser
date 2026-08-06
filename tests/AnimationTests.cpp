#include <cmath>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "css/Animation.h"
#include "css/ComputedStyle.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "css/Timing.h"

namespace microbrowser::tests {

using css::AnimatableProperty;
using css::AnimationSpec;
using css::ComputedStyle;
using css::Progress;
using css::TimingFunction;
using css::TransitionSpec;

namespace {

// A style with one declaration applied, which is how every endpoint here is built: going through the
// real declaration parser rather than assigning members means a test cannot pass because it and the
// parser agree on something the CSS does not say.
ComputedStyle StyleWith(const std::string& declarations) {
  ComputedStyle style;
  for (const css::Declaration& declaration : css::ParseDeclarationList(declarations)) {
    css::ApplyDeclaration(declaration, ComputedStyle{}, style);
  }
  return style;
}

std::string Round(double value) {
  // Three decimals, which is finer than a pixel and coarser than the noise a Bézier solver leaves.
  const double scaled = std::round(value * 1000.0) / 1000.0;
  std::string out = std::to_string(scaled);
  while (out.size() > 1 && out.back() == '0') {
    out.pop_back();
  }
  if (!out.empty() && out.back() == '.') {
    out.pop_back();
  }
  return out;
}

}  // namespace

void RegisterAnimationTests(std::vector<TestCase>& tests) {
  // --- Easing ----------------------------------------------------------------------------------

  AddTest(tests, "Animation/LinearIsAStraightLineAndTheKeywordsAreCurves", [] {
    ExpectEqString(Round(Progress(css::LinearTiming(), 0.0)), "0", "linear at zero");
    ExpectEqString(Round(Progress(css::LinearTiming(), 0.25)), "0.25", "and a quarter through");
    ExpectEqString(Round(Progress(css::LinearTiming(), 1.0)), "1", "and at the end");
    // `ease` is `cubic-bezier(0.25, 0.1, 0.25, 1)`, which is *ahead* of linear in the middle -- that is
    // the whole visual point of it, and it is the assertion that says the solver works.
    Expect(Progress(css::EaseTiming(), 0.5) > 0.5, "ease is past halfway at halfway");
    Expect(Progress(css::EaseInTiming(), 0.5) < 0.5, "and ease-in is behind");
    Expect(Progress(css::EaseOutTiming(), 0.5) > 0.5, "and ease-out ahead");
    // Both ends are exact for every curve, because a control point can only move the middle.
    for (const TimingFunction& timing :
         {css::EaseTiming(), css::EaseInTiming(), css::EaseOutTiming(), css::EaseInOutTiming()}) {
      ExpectEqString(Round(Progress(timing, 0.0)), "0", "every curve starts at zero");
      ExpectEqString(Round(Progress(timing, 1.0)), "1", "and ends at one");
    }
  });

  AddTest(tests, "Animation/ACurveMayOvershootAndIsNotClamped", [] {
    // `cubic-bezier(0, 1.5, 1, 1)` goes above 1 in the middle, which is what a page asking for a bounce
    // is asking for. **Clamping here would silently flatten every spring animation on the web** -- so
    // the clamp lives where a range is actually known: a colour channel, not a margin.
    TimingFunction bounce;
    Expect(css::ParseTimingFunction("cubic-bezier(0, 1.5, 1, 1)", bounce), "it parses");
    Expect(Progress(bounce, 0.4) > 1.0, "and overshoots");
    // The x coordinates must be in [0,1] and the y need not: x is *time*, and a control point outside
    // the range makes the curve run backwards rather than slowly.
    TimingFunction rejected;
    Expect(!css::ParseTimingFunction("cubic-bezier(-0.5, 0, 1, 1)", rejected),
           "an x below zero is invalid");
    Expect(!css::ParseTimingFunction("cubic-bezier(0, 0, 1.5, 1)", rejected),
           "and one above one is too");
    Expect(css::ParseTimingFunction("cubic-bezier(0, -3, 1, 4)", rejected),
           "while a y outside the range is fine");
  });

  AddTest(tests, "Animation/StepsAreFourRulesOverTheSameArithmetic", [] {
    const auto steps = [](const char* text, double t) {
      TimingFunction timing;
      Expect(css::ParseTimingFunction(text, timing), std::string("parses: ") + text);
      return Round(Progress(timing, t));
    };
    // `jump-end` is the default: four steps at 0, 1/4, 1/2, 3/4, reaching 1 only at the very end.
    ExpectEqString(steps("steps(4)", 0.0), "0", "jump-end starts at zero");
    ExpectEqString(steps("steps(4)", 0.3), "0.25", "and holds each step");
    ExpectEqString(steps("steps(4)", 1.0), "1", "and reaches one at the end");
    // `jump-start` jumps immediately, which is what `step-start` means.
    ExpectEqString(steps("steps(4, jump-start)", 0.0), "0.25", "jump-start jumps at once");
    ExpectEqString(steps("step-start", 0.0), "1", "and step-start is steps(1, jump-start)");
    ExpectEqString(steps("step-end", 0.5), "0", "while step-end holds until the end");
    // `jump-none` visits both ends and neither jump: 0, 1/3, 2/3, 1.
    ExpectEqString(steps("steps(4, jump-none)", 0.0), "0", "jump-none starts at zero");
    ExpectEqString(steps("steps(4, jump-none)", 0.5), "0.667", "and its middle is thirds");
    ExpectEqString(steps("steps(4, jump-none)", 1.0), "1", "and it ends at one");
    // `jump-both` takes both jumps: 1/5 .. 4/5 during, 1 at the end.
    ExpectEqString(steps("steps(4, jump-both)", 0.0), "0.2", "jump-both jumps at both ends");
    ExpectEqString(steps("steps(1, jump-none)", 0.5), "0",
                   "and a single jump-none step has nowhere to go, which the specification says is zero");
    TimingFunction rejected;
    Expect(!css::ParseTimingFunction("steps(0)", rejected), "zero steps is not a step function");
    Expect(!css::ParseTimingFunction("steps(2.5)", rejected), "and neither is a fractional count");
    Expect(!css::ParseTimingFunction("steps(4, sideways)", rejected), "nor an unknown position");
  });

  // --- Interpolation ---------------------------------------------------------------------------

  AddTest(tests, "Animation/ColoursInterpolateInPremultipliedAlpha", [] {
    const ComputedStyle from = StyleWith("color: rgb(0, 0, 0)");
    const ComputedStyle to = StyleWith("color: rgb(100, 200, 40)");
    ComputedStyle out = from;
    Expect(css::InterpolateProperty(AnimatableProperty::Color, from, to, 0.5, out), "it interpolates");
    ExpectEqInt(out.color.Red(), 50, "half the red");
    ExpectEqInt(out.color.Green(), 100, "half the green");
    ExpectEqInt(out.color.Blue(), 20, "half the blue");
    // The premultiplied case, which is the whole reason this is not three lerps: red fading to
    // transparent blue must not pass through purple. Halfway, the visible colour is still red.
    const ComputedStyle red = StyleWith("color: rgba(255, 0, 0, 1)");
    const ComputedStyle clear_blue = StyleWith("color: rgba(0, 0, 255, 0)");
    ComputedStyle mixed = red;
    css::InterpolateProperty(AnimatableProperty::Color, red, clear_blue, 0.5, mixed);
    Expect(mixed.color.Red() > mixed.color.Blue(),
           "the red is still red at half fade, not half purple");
    ExpectEqInt(mixed.color.Alpha(), 128, "and the alpha is halfway");
  });

  AddTest(tests, "Animation/LengthsInterpolateWhenTheirUnitsMatch", [] {
    const ComputedStyle from = StyleWith("width: 100px");
    const ComputedStyle to = StyleWith("width: 200px");
    ComputedStyle out = from;
    css::InterpolateProperty(AnimatableProperty::Width, from, to, 0.25, out);
    ExpectEqString(Round(out.width.value), "125", "a quarter of the way");
    // A negative margin is a real value, so a length is *not* clamped -- which is what lets an
    // overshooting curve overshoot.
    const ComputedStyle zero = StyleWith("margin-left: 0px");
    const ComputedStyle ten = StyleWith("margin-left: 10px");
    ComputedStyle past = zero;
    css::InterpolateProperty(AnimatableProperty::MarginLeft, zero, ten, 1.5, past);
    ExpectEqString(Round(past.margin.left.value), "15", "a length may pass its endpoint");
    // Mismatched units and `auto` both snap at the halfway point rather than mixing: `10px` to `50%`
    // needs a containing block the cascade does not have, and `auto` is not a number until layout runs.
    const ComputedStyle percent = StyleWith("width: 50%");
    ComputedStyle snapped = from;
    css::InterpolateProperty(AnimatableProperty::Width, from, percent, 0.4, snapped);
    Expect(snapped.width.unit == css::Length::Unit::Pixels, "before halfway it is still the start");
    css::InterpolateProperty(AnimatableProperty::Width, from, percent, 0.6, snapped);
    Expect(snapped.width.IsPercent(), "and after it, the end");
    const ComputedStyle automatic = StyleWith("width: auto");
    ComputedStyle from_auto = automatic;
    css::InterpolateProperty(AnimatableProperty::Width, automatic, to, 0.6, from_auto);
    Expect(!from_auto.width.IsAuto() && from_auto.width.value == 200.0f,
           "and auto snaps rather than counting as zero");
  });

  AddTest(tests, "Animation/APropertyThisBrowserWillNotInterpolateSaysSo", [] {
    // The list is short on purpose, and `None` is the answer that tells a caller to switch the value
    // discretely rather than to animate it. Answering "animatable" for something that then interpolated
    // wrongly would be worse: a `font-family` halfway is a font nobody named.
    Expect(css::AnimatablePropertyFromName("width") == AnimatableProperty::Width, "width");
    Expect(css::AnimatablePropertyFromName("BACKGROUND-COLOR") == AnimatableProperty::BackgroundColor,
           "and the name is case-insensitive");
    Expect(css::AnimatablePropertyFromName("font-family") == AnimatableProperty::None,
           "a font stack does not interpolate");
    Expect(css::AnimatablePropertyFromName("display") == AnimatableProperty::None, "nor does display");
    Expect(css::AnimatablePropertyFromName("opacity") == AnimatableProperty::None,
           "and neither does opacity, because this browser has no such property -- animating one that "
           "does not apply gains nothing, which is ADR 0014 §5's own argument");
    ComputedStyle out;
    Expect(!css::InterpolateProperty(AnimatableProperty::None, ComputedStyle{}, ComputedStyle{}, 0.5,
                                     out),
           "and interpolating None fails rather than doing nothing quietly");
  });

  // --- The shorthands --------------------------------------------------------------------------

  AddTest(tests, "Animation/TheFirstTimeIsTheDurationWhereverItAppears", [] {
    // The one rule worth knowing about this grammar: in `transition: 2s 1s ease` the first time is the
    // duration and the second the delay, and in `transition: ease 2s` the `2s` is still the duration.
    // The parser counts times rather than reading positions.
    const ComputedStyle both = StyleWith("transition: color 2s 1s ease-in");
    ExpectEqInt(static_cast<long long>(both.transitions.size()), 1, "one item");
    ExpectEqString(Round(both.transitions[0].duration_ms), "2000", "two seconds of duration");
    ExpectEqString(Round(both.transitions[0].delay_ms), "1000", "one of delay");
    Expect(both.transitions[0].property == AnimatableProperty::Color, "on color");
    Expect(both.transitions[0].timing == css::EaseInTiming(), "eased in");

    const ComputedStyle reordered = StyleWith("transition: ease-out 300ms width");
    ExpectEqString(Round(reordered.transitions[0].duration_ms), "300", "order does not decide");
    ExpectEqString(Round(reordered.transitions[0].delay_ms), "0", "and there was no delay");

    // A list, which is how a page transitions three properties at three speeds.
    const ComputedStyle list = StyleWith("transition: color 1s, width 2s, height 3s linear");
    ExpectEqInt(static_cast<long long>(list.transitions.size()), 3, "three items");
    ExpectEqString(Round(list.transitions[2].duration_ms), "3000", "and each keeps its own time");
    Expect(list.transitions[2].timing == css::LinearTiming(), "and its own curve");

    // No property named means every property, which is what `transition: 2s` says.
    const ComputedStyle everything = StyleWith("transition: 0.2s");
    Expect(everything.transitions[0].all_properties, "a bare time transitions everything");
    // `none` clears the list rather than adding an item that does nothing.
    Expect(StyleWith("transition: color 1s; transition: none").transitions.empty(), "none clears");
    // A bare number is not a time, so this is not a declaration and the property keeps its old value.
    Expect(StyleWith("transition: color 2").transitions.empty(),
           "`2` without a unit is invalid CSS and does not become two seconds");
  });

  AddTest(tests, "Animation/TheAnimationNameIsWhateverIsLeftOver", [] {
    const ComputedStyle spin = StyleWith("animation: spin 2s linear infinite");
    ExpectEqInt(static_cast<long long>(spin.animations.size()), 1, "one animation");
    ExpectEqString(spin.animations[0].name, "spin", "named");
    ExpectEqString(Round(spin.animations[0].duration_ms), "2000", "two seconds");
    Expect(spin.animations[0].iterations > 1e6, "infinite is a large finite count");
    // **The name has to be matched last.** A `@keyframes` block may legally be called `reverse`, so a
    // parser that took the first unrecognised word as the name would read this as an animation *named*
    // reverse instead of one running backwards.
    const ComputedStyle backwards = StyleWith("animation: reverse 2s slide");
    ExpectEqString(backwards.animations[0].name, "slide", "the keyword is a keyword");
    Expect(backwards.animations[0].direction == AnimationSpec::Direction::Reverse, "and it applies");
    const ComputedStyle filled = StyleWith("animation: grow 1s 0.5s forwards alternate paused");
    Expect(filled.animations[0].fill == AnimationSpec::Fill::Forwards, "forwards fill");
    Expect(filled.animations[0].direction == AnimationSpec::Direction::Alternate, "alternating");
    Expect(filled.animations[0].paused, "and paused, which asks for no frames at all");
    ExpectEqString(Round(filled.animations[0].delay_ms), "500", "with the second time as the delay");
    Expect(StyleWith("animation: 2s linear").animations.empty(),
           "an animation with no name animates nothing and is not a declaration");
  });

  // --- @keyframes ------------------------------------------------------------------------------

  AddTest(tests, "Animation/KeyframesAreParsedWithTheirOffsets", [] {
    const css::StyleSheet sheet = css::ParseStyleSheet(
        "@keyframes slide { from { left: 0px } 50% { left: 40px } to { left: 100px } }");
    ExpectEqInt(static_cast<long long>(sheet.keyframes.size()), 1, "one block");
    ExpectEqString(sheet.keyframes[0].name, "slide", "named");
    ExpectEqInt(static_cast<long long>(sheet.keyframes[0].frames.size()), 3, "three frames");
    ExpectEqString(Round(sheet.keyframes[0].frames[0].offset), "0", "`from` is zero");
    ExpectEqString(Round(sheet.keyframes[0].frames[1].offset), "0.5", "a percentage is a fraction");
    ExpectEqString(Round(sheet.keyframes[0].frames[2].offset), "1", "`to` is one");
    // Sorted, whatever order they were written in -- an animation reads them in offset order.
    const css::StyleSheet unsorted =
        css::ParseStyleSheet("@keyframes x { to { left: 9px } from { left: 1px } }");
    ExpectEqString(Round(unsorted.keyframes[0].frames[0].offset), "0", "sorted on the way in");
    // A selector list is two keyframes sharing one block, which is how a page writes a pause.
    const css::StyleSheet shared =
        css::ParseStyleSheet("@keyframes p { 0%, 50% { left: 0px } 100% { left: 9px } }");
    ExpectEqInt(static_cast<long long>(shared.keyframes[0].frames.size()), 3, "the list expands");
    // A later block with the same name *replaces* the earlier one rather than merging: a page that
    // redefines `spin` means the new one.
    const css::StyleSheet redefined = css::ParseStyleSheet(
        "@keyframes s { from { left: 0px } } @keyframes s { from { top: 5px } to { top: 9px } }");
    ExpectEqInt(static_cast<long long>(redefined.keyframes.size()), 1, "one block, not two");
    ExpectEqInt(static_cast<long long>(redefined.keyframes[0].frames.size()), 2, "and it is the second");
    // An offset outside 0..100 is invalid rather than clamped, or it would replace the real start.
    const css::StyleSheet bad = css::ParseStyleSheet("@keyframes b { -50% { left: 0px } }");
    Expect(bad.keyframes.empty(), "an out-of-range offset drops the frame and the empty block with it");
  });
}

}  // namespace microbrowser::tests
