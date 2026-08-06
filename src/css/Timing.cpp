#include "css/Timing.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <optional>

#include "css/CssText.h"
#include "util/Parse.h"

namespace microbrowser::css {

namespace {

// A cubic Bézier's y for a given x, where the curve is the unit one with two control points.
//
// The awkward part is that a timing function is *y as a function of x* and a Bézier is both as
// functions of a parameter. So x has to be inverted first, and there is no closed form -- which is why
// every engine does what this does: Newton's method, falling back to bisection.
//
// Newton alone is not enough and the reason is worth stating: the derivative can be zero or nearly so
// -- `cubic-bezier(0, 0, 1, 1)`'s is fine but `cubic-bezier(1, 0, 1, 1)`'s is zero at t=0 -- and a
// Newton step there flies off the curve. Bisection cannot do that, and eight iterations of it are
// enough for a value that will be multiplied by a pixel count.
double SolveBezier(double x1, double x2, double x) {
  const auto sample = [](double a, double b, double t) {
    // The Bernstein form with the first and last control points at 0 and 1, which is where the
    // 3(1-t)²t and 3(1-t)t² coefficients come from.
    const double one_minus = 1.0 - t;
    return 3.0 * one_minus * one_minus * t * a + 3.0 * one_minus * t * t * b + t * t * t;
  };
  const auto slope = [](double a, double b, double t) {
    const double one_minus = 1.0 - t;
    return 3.0 * one_minus * one_minus * a + 6.0 * one_minus * t * (b - a) + 3.0 * t * t * (1.0 - b);
  };
  double t = x;
  for (int i = 0; i < 8; ++i) {
    const double error = sample(x1, x2, t) - x;
    if (std::abs(error) < 1e-7) {
      return t;
    }
    const double derivative = slope(x1, x2, t);
    if (std::abs(derivative) < 1e-7) {
      break;  // Newton cannot help here; bisection can.
    }
    t -= error / derivative;
  }
  double low = 0.0;
  double high = 1.0;
  t = x;
  for (int i = 0; i < 24; ++i) {
    const double value = sample(x1, x2, t);
    if (std::abs(value - x) < 1e-7) {
      return t;
    }
    if (value < x) {
      low = t;
    } else {
      high = t;
    }
    t = (low + high) * 0.5;
  }
  return t;
}

std::vector<std::string_view> SplitArguments(std::string_view inside) {
  std::vector<std::string_view> out;
  std::size_t at = 0;
  while (at <= inside.size()) {
    const std::size_t comma = inside.find(',', at);
    out.push_back(Trim(inside.substr(at, comma == std::string_view::npos ? comma : comma - at)));
    if (comma == std::string_view::npos) {
      break;
    }
    at = comma + 1;
  }
  return out;
}

bool ParseNumber(std::string_view text, double& out) {
  // Through `util::ParseFloat` rather than `std::stod`, and the architecture lint is right to insist:
  // `stod` throws on a non-number and reads the decimal separator from the process locale, so a browser
  // started under a comma-decimal locale would parse `cubic-bezier(0.42, 0, 1, 1)` as four integers.
  const std::optional<float> value = util::ParseFloat(text);
  if (!value.has_value()) {
    return false;
  }
  out = static_cast<double>(*value);
  return true;
}

}  // namespace

namespace {

// The keyword curves, as control points. `ease` is `cubic-bezier(0.25, 0.1, 0.25, 1)` and the others
// are the specification's too -- written once here so that a page spelling one out by hand and a page
// using the keyword animate identically, which they would not if one were a curve and the other a
// special case.
TimingFunction Bezier(float x1, float y1, float x2, float y2) {
  TimingFunction out;
  out.kind = TimingFunction::Kind::CubicBezier;
  out.x1 = x1;
  out.y1 = y1;
  out.x2 = x2;
  out.y2 = y2;
  return out;
}

}  // namespace

TimingFunction LinearTiming() { return Bezier(0.0f, 0.0f, 1.0f, 1.0f); }
TimingFunction EaseTiming() { return Bezier(0.25f, 0.1f, 0.25f, 1.0f); }
TimingFunction EaseInTiming() { return Bezier(0.42f, 0.0f, 1.0f, 1.0f); }
TimingFunction EaseOutTiming() { return Bezier(0.0f, 0.0f, 0.58f, 1.0f); }
TimingFunction EaseInOutTiming() { return Bezier(0.42f, 0.0f, 0.58f, 1.0f); }

double Progress(const TimingFunction& timing, double t) {
  const double clamped = std::clamp(t, 0.0, 1.0);
  if (timing.kind == TimingFunction::Kind::Steps) {
    const double count = static_cast<double>(std::max<std::uint32_t>(timing.steps, 1));
    // The four positions differ only in where the first and last jumps sit, which is why they are
    // arithmetic on the same two numbers rather than four branches of a state machine.
    double step = std::floor(clamped * count);
    double divisor = count;
    if (timing.position == TimingFunction::StepPosition::JumpStart ||
        timing.position == TimingFunction::StepPosition::JumpBoth) {
      step += 1.0;
    }
    if (timing.position == TimingFunction::StepPosition::JumpNone) {
      divisor = count - 1.0;
      if (divisor <= 0.0) {
        return 0.0;  // `steps(1, jump-none)` has no step to take, and the specification says zero.
      }
    } else if (timing.position == TimingFunction::StepPosition::JumpBoth) {
      divisor = count + 1.0;
    }
    // At exactly t=1 the floor lands one past the last step for two of the four positions. Clamping
    // is what makes the four one piece of arithmetic instead of four branches -- and it is why `steps`
    // never returns more than 1.
    return std::min(step, divisor) / divisor;
  }
  if (timing.x1 == 0.0f && timing.y1 == 0.0f && timing.x2 == 1.0f && timing.y2 == 1.0f) {
    return clamped;  // `linear`, which is worth short-circuiting: it is the common case in practice.
  }
  const double parameter = SolveBezier(static_cast<double>(timing.x1),
                                      static_cast<double>(timing.x2), clamped);
  const double one_minus = 1.0 - parameter;
  return 3.0 * one_minus * one_minus * parameter * static_cast<double>(timing.y1) +
         3.0 * one_minus * parameter * parameter * static_cast<double>(timing.y2) +
         parameter * parameter * parameter;
}

bool ParseTimingFunction(std::string_view text, TimingFunction& out) {
  const std::string lowered = Lowered(Trim(text));
  if (lowered == "linear") {
    out = LinearTiming();
    return true;
  }
  if (lowered == "ease") {
    out = EaseTiming();
    return true;
  }
  if (lowered == "ease-in") {
    out = EaseInTiming();
    return true;
  }
  if (lowered == "ease-out") {
    out = EaseOutTiming();
    return true;
  }
  if (lowered == "ease-in-out") {
    out = EaseInOutTiming();
    return true;
  }
  // `step-start` and `step-end` are `steps(1, jump-start)` and `steps(1, jump-end)`. Expressed in
  // terms of the general form rather than as their own cases, so the two cannot disagree.
  if (lowered == "step-start" || lowered == "step-end") {
    out = TimingFunction{};
    out.kind = TimingFunction::Kind::Steps;
    out.steps = 1;
    out.position = lowered == "step-start" ? TimingFunction::StepPosition::JumpStart
                                          : TimingFunction::StepPosition::JumpEnd;
    return true;
  }
  const std::size_t open = lowered.find('(');
  if (open == std::string::npos || lowered.back() != ')') {
    return false;
  }
  const std::string_view name(lowered.data(), open);
  const std::string_view inside(lowered.data() + open + 1, lowered.size() - open - 2);
  const std::vector<std::string_view> arguments = SplitArguments(inside);
  if (name == "cubic-bezier") {
    if (arguments.size() != 4) {
      return false;
    }
    double values[4] = {0.0, 0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < 4; ++i) {
      if (!ParseNumber(arguments[i], values[i])) {
        return false;
      }
    }
    // **The x coordinates must be in [0,1] and the y coordinates need not be.** That asymmetry is the
    // specification's and it is not arbitrary: x is *time*, and a control point outside [0,1] would
    // make the curve non-monotonic in time -- which is not a slow animation, it is one that runs
    // backwards. y is the value, and overshooting it is what a bounce is.
    if (values[0] < 0.0 || values[0] > 1.0 || values[2] < 0.0 || values[2] > 1.0) {
      return false;
    }
    out = TimingFunction{};
    out.kind = TimingFunction::Kind::CubicBezier;
    out.x1 = static_cast<float>(values[0]);
    out.y1 = static_cast<float>(values[1]);
    out.x2 = static_cast<float>(values[2]);
    out.y2 = static_cast<float>(values[3]);
    return true;
  }
  if (name == "steps") {
    if (arguments.empty() || arguments.size() > 2) {
      return false;
    }
    double count = 0.0;
    if (!ParseNumber(arguments[0], count) || count < 1.0 || count != std::floor(count) ||
        count > 1000.0) {
      // A bound on the count, because it is a page's number and a `steps(100000000)` is a division
      // this has no use for. A thousand steps at 60fps is sixteen seconds of one step per frame.
      return false;
    }
    out = TimingFunction{};
    out.kind = TimingFunction::Kind::Steps;
    out.steps = static_cast<std::uint32_t>(count);
    out.position = TimingFunction::StepPosition::JumpEnd;
    if (arguments.size() == 2) {
      const std::string_view where = arguments[1];
      if (where == "start" || where == "jump-start") {
        out.position = TimingFunction::StepPosition::JumpStart;
      } else if (where == "end" || where == "jump-end") {
        out.position = TimingFunction::StepPosition::JumpEnd;
      } else if (where == "jump-none") {
        out.position = TimingFunction::StepPosition::JumpNone;
      } else if (where == "jump-both") {
        out.position = TimingFunction::StepPosition::JumpBoth;
      } else {
        return false;
      }
    }
    return true;
  }
  return false;
}

}  // namespace microbrowser::css
