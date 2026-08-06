// `transition-*` and `animation-*`, parsed.
//
// ADR 0014 §5, session 35. Its own translation unit for the reason TransformDeclarations.cpp is one:
// Declarations.cpp is at the module's line cap, and these two shorthands have a grammar that is
// genuinely awkward -- a comma-separated list of space-separated lists, where the *order* inside each
// item is mostly free and one position is decided by which of two numbers came first.
//
// **The one rule worth reading before the code**: in `transition: 2s 1s ease`, the first time is the
// duration and the second is the delay, whatever else is around them. In `transition: ease 2s`, the
// `2s` is still the duration. So the parser tracks how many times it has seen, not where they were.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "css/Animation.h"
#include "css/ComputedStyle.h"
#include "css/CssText.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microbrowser::css {

namespace {

// One comma-separated item's worth of space-separated words.
std::vector<std::string_view> Words(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && IsCssWhitespace(text[at])) {
      ++at;
    }
    if (at >= text.size()) {
      break;
    }
    const std::size_t start = at;
    // A function's parentheses hold spaces and commas of their own -- `cubic-bezier(0.4, 0, 0.2, 1)`
    // is one word. Depth-tracked rather than split on the comma first, which is why the comma split
    // below has to do the same.
    int depth = 0;
    while (at < text.size() && (depth > 0 || !IsCssWhitespace(text[at]))) {
      if (text[at] == '(') {
        ++depth;
      } else if (text[at] == ')') {
        --depth;
      }
      ++at;
    }
    out.push_back(text.substr(start, at - start));
  }
  return out;
}

// The comma-separated items, respecting parentheses for the same reason.
std::vector<std::string_view> Items(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t at = 0;
  std::size_t start = 0;
  int depth = 0;
  for (; at < text.size(); ++at) {
    if (text[at] == '(') {
      ++depth;
    } else if (text[at] == ')') {
      --depth;
    } else if (text[at] == ',' && depth == 0) {
      out.push_back(Trim(text.substr(start, at - start)));
      start = at + 1;
    }
  }
  out.push_back(Trim(text.substr(start)));
  return out;
}

// A `<time>`: seconds or milliseconds, as milliseconds. Nothing else is a time, and a bare number is
// deliberately not one -- `transition: 2 1s` is invalid CSS and treating the `2` as two seconds would
// make an invalid declaration apply.
bool ParseTime(std::string_view text, double& out_ms) {
  if (text.size() < 2) {
    return false;
  }
  std::string_view number = text;
  double scale = 1.0;
  if (text.ends_with("ms")) {
    number = text.substr(0, text.size() - 2);
  } else if (text.ends_with("s")) {
    number = text.substr(0, text.size() - 1);
    scale = 1000.0;
  } else {
    return false;
  }
  const std::optional<float> value = util::ParseFloat(number);
  if (!value.has_value()) {
    return false;
  }
  out_ms = static_cast<double>(*value) * scale;
  return true;
}

bool ParseCount(std::string_view text, double& out) {
  if (util::EqualsAsciiCaseInsensitive(text, "infinite")) {
    // A large finite number rather than an infinity. Every reader of this wants "how many", and an
    // infinity would make each of them check for one -- and one multiplication by a duration away
    // from a NaN. Ten million iterations of a one-second animation is four months.
    out = 1e7;
    return true;
  }
  const std::optional<float> value = util::ParseFloat(text);
  if (!value.has_value() || *value < 0.0f) {
    return false;
  }
  out = static_cast<double>(*value);
  return true;
}

}  // namespace

bool ApplyTransitionDeclaration(std::string_view property, std::string_view value,
                                ComputedStyle& style) {
  const std::string name = Lowered(Trim(property));
  if (name == "transition") {
    // The shorthand. Every item resets the whole spec, which is what a shorthand means -- so
    // `transition: color` after `transition-duration: 2s` has a zero duration and animates nothing,
    // and that is correct rather than surprising.
    std::vector<TransitionSpec> parsed;
    for (const std::string_view item : Items(value)) {
      if (util::EqualsAsciiCaseInsensitive(item, "none")) {
        style.transitions.clear();
        return true;
      }
      TransitionSpec spec;
      spec.timing = EaseTiming();
      int times_seen = 0;
      bool named_property = false;
      for (const std::string_view word : Words(item)) {
        double time_ms = 0.0;
        if (ParseTime(word, time_ms)) {
          // The first time is the duration and the second is the delay, wherever they appear. A third
          // makes the item invalid rather than being ignored: a page that wrote three times meant
          // something this cannot express.
          if (times_seen == 0) {
            spec.duration_ms = time_ms;
          } else if (times_seen == 1) {
            spec.delay_ms = time_ms;
          } else {
            return false;
          }
          ++times_seen;
          continue;
        }
        TimingFunction timing;
        if (ParseTimingFunction(word, timing)) {
          spec.timing = timing;
          continue;
        }
        if (named_property) {
          return false;  // two property names in one item is not a transition
        }
        if (util::EqualsAsciiCaseInsensitive(word, "all")) {
          spec.all_properties = true;
          named_property = true;
          continue;
        }
        const AnimatableProperty which = AnimatablePropertyFromName(word);
        // **A property this browser does not interpolate is not an error.** It is a transition that
        // does nothing, which is exactly what the specification says for a non-animatable property --
        // and refusing the declaration would throw away the *other* items in the list.
        spec.property = which;
        named_property = true;
      }
      if (!named_property) {
        spec.all_properties = true;  // `transition: 2s` means every property
      }
      parsed.push_back(spec);
    }
    style.transitions = std::move(parsed);
    return true;
  }
  if (name == "transition-property" || name == "transition-duration" ||
      name == "transition-delay" || name == "transition-timing-function") {
    const std::vector<std::string_view> items = Items(value);
    if (items.empty()) {
      return false;
    }
    // The longhands are *per item*, and the list may be shorter than the property list -- in which case
    // it repeats. That is the specification's rule and it is what makes
    // `transition-property: color, width; transition-duration: 2s` give both properties two seconds.
    if (style.transitions.size() < items.size()) {
      style.transitions.resize(items.size());
      for (TransitionSpec& spec : style.transitions) {
        if (spec.property == AnimatableProperty::None && !spec.all_properties) {
          spec.all_properties = true;
        }
      }
    }
    for (std::size_t i = 0; i < style.transitions.size(); ++i) {
      const std::string_view item = items[i % items.size()];
      TransitionSpec& spec = style.transitions[i];
      if (name == "transition-property") {
        if (util::EqualsAsciiCaseInsensitive(item, "none")) {
          style.transitions.clear();
          return true;
        }
        spec.all_properties = util::EqualsAsciiCaseInsensitive(item, "all");
        spec.property = spec.all_properties ? AnimatableProperty::None
                                            : AnimatablePropertyFromName(item);
      } else if (name == "transition-duration") {
        double time_ms = 0.0;
        if (!ParseTime(item, time_ms) || time_ms < 0.0) {
          return false;  // a negative duration is invalid, unlike a negative delay
        }
        spec.duration_ms = time_ms;
      } else if (name == "transition-delay") {
        double time_ms = 0.0;
        if (!ParseTime(item, time_ms)) {
          return false;
        }
        spec.delay_ms = time_ms;
      } else {
        TimingFunction timing;
        if (!ParseTimingFunction(item, timing)) {
          return false;
        }
        spec.timing = timing;
      }
    }
    return true;
  }
  return false;
}

bool ApplyAnimationDeclaration(std::string_view property, std::string_view value,
                               ComputedStyle& style) {
  const std::string name = Lowered(Trim(property));
  if (name == "animation") {
    std::vector<AnimationSpec> parsed;
    for (const std::string_view item : Items(value)) {
      if (util::EqualsAsciiCaseInsensitive(item, "none")) {
        style.animations.clear();
        return true;
      }
      AnimationSpec spec;
      spec.timing = EaseTiming();
      int times_seen = 0;
      for (const std::string_view word : Words(item)) {
        double time_ms = 0.0;
        if (ParseTime(word, time_ms)) {
          if (times_seen == 0) {
            spec.duration_ms = time_ms;
          } else if (times_seen == 1) {
            spec.delay_ms = time_ms;
          } else {
            return false;
          }
          ++times_seen;
          continue;
        }
        TimingFunction timing;
        if (ParseTimingFunction(word, timing)) {
          spec.timing = timing;
          continue;
        }
        double count = 0.0;
        if (ParseCount(word, count)) {
          spec.iterations = count;
          continue;
        }
        if (util::EqualsAsciiCaseInsensitive(word, "normal")) {
          continue;  // both `animation-direction` and `animation-fill-mode`'s initial value
        }
        if (util::EqualsAsciiCaseInsensitive(word, "reverse")) {
          spec.direction = AnimationSpec::Direction::Reverse;
          continue;
        }
        if (util::EqualsAsciiCaseInsensitive(word, "alternate")) {
          spec.direction = AnimationSpec::Direction::Alternate;
          continue;
        }
        if (util::EqualsAsciiCaseInsensitive(word, "alternate-reverse")) {
          spec.direction = AnimationSpec::Direction::AlternateReverse;
          continue;
        }
        if (util::EqualsAsciiCaseInsensitive(word, "forwards")) {
          spec.fill = AnimationSpec::Fill::Forwards;
          continue;
        }
        if (util::EqualsAsciiCaseInsensitive(word, "backwards")) {
          spec.fill = AnimationSpec::Fill::Backwards;
          continue;
        }
        if (util::EqualsAsciiCaseInsensitive(word, "both")) {
          spec.fill = AnimationSpec::Fill::Both;
          continue;
        }
        if (util::EqualsAsciiCaseInsensitive(word, "paused")) {
          spec.paused = true;
          continue;
        }
        if (util::EqualsAsciiCaseInsensitive(word, "running")) {
          continue;
        }
        // **Whatever is left is the name**, and it has to be last in this chain rather than first: a
        // `@keyframes` block may legally be called `reverse`, and a parser that took the first
        // unrecognised word as the name would read `animation: reverse 2s` as an animation named
        // `reverse` instead of one running backwards. The specification resolves it this way too.
        if (spec.name.empty()) {
          spec.name = std::string(word);
          continue;
        }
        return false;
      }
      if (spec.name.empty()) {
        return false;  // an animation with no name animates nothing and is not a declaration
      }
      parsed.push_back(std::move(spec));
    }
    style.animations = std::move(parsed);
    return true;
  }
  static constexpr std::string_view kLonghands[] = {
      "animation-name",      "animation-duration",  "animation-delay",
      "animation-timing-function", "animation-iteration-count", "animation-direction",
      "animation-fill-mode", "animation-play-state"};
  bool is_longhand = false;
  for (const std::string_view candidate : kLonghands) {
    is_longhand = is_longhand || name == candidate;
  }
  if (!is_longhand) {
    return false;
  }
  const std::vector<std::string_view> items = Items(value);
  if (items.empty()) {
    return false;
  }
  if (name == "animation-name") {
    if (items.size() == 1 && util::EqualsAsciiCaseInsensitive(items[0], "none")) {
      style.animations.clear();
      return true;
    }
    style.animations.resize(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
      style.animations[i].name = std::string(items[i]);
      if (style.animations[i].duration_ms == 0.0 && style.animations[i].iterations == 0.0) {
        style.animations[i].iterations = 1.0;
      }
    }
    return true;
  }
  if (style.animations.empty()) {
    // A longhand before any name. The specification keeps it, and so does this -- a page that writes
    // `animation-duration` above `animation-name` in the same rule must not lose the duration, and
    // declaration order inside a rule is not something the cascade preserves for us.
    style.animations.resize(items.size());
  }
  for (std::size_t i = 0; i < style.animations.size(); ++i) {
    const std::string_view item = items[i % items.size()];
    AnimationSpec& spec = style.animations[i];
    if (name == "animation-duration") {
      double time_ms = 0.0;
      if (!ParseTime(item, time_ms) || time_ms < 0.0) {
        return false;
      }
      spec.duration_ms = time_ms;
    } else if (name == "animation-delay") {
      double time_ms = 0.0;
      if (!ParseTime(item, time_ms)) {
        return false;
      }
      spec.delay_ms = time_ms;
    } else if (name == "animation-timing-function") {
      TimingFunction timing;
      if (!ParseTimingFunction(item, timing)) {
        return false;
      }
      spec.timing = timing;
    } else if (name == "animation-iteration-count") {
      double count = 0.0;
      if (!ParseCount(item, count)) {
        return false;
      }
      spec.iterations = count;
    } else if (name == "animation-direction") {
      if (util::EqualsAsciiCaseInsensitive(item, "normal")) {
        spec.direction = AnimationSpec::Direction::Normal;
      } else if (util::EqualsAsciiCaseInsensitive(item, "reverse")) {
        spec.direction = AnimationSpec::Direction::Reverse;
      } else if (util::EqualsAsciiCaseInsensitive(item, "alternate")) {
        spec.direction = AnimationSpec::Direction::Alternate;
      } else if (util::EqualsAsciiCaseInsensitive(item, "alternate-reverse")) {
        spec.direction = AnimationSpec::Direction::AlternateReverse;
      } else {
        return false;
      }
    } else if (name == "animation-fill-mode") {
      if (util::EqualsAsciiCaseInsensitive(item, "none")) {
        spec.fill = AnimationSpec::Fill::None;
      } else if (util::EqualsAsciiCaseInsensitive(item, "forwards")) {
        spec.fill = AnimationSpec::Fill::Forwards;
      } else if (util::EqualsAsciiCaseInsensitive(item, "backwards")) {
        spec.fill = AnimationSpec::Fill::Backwards;
      } else if (util::EqualsAsciiCaseInsensitive(item, "both")) {
        spec.fill = AnimationSpec::Fill::Both;
      } else {
        return false;
      }
    } else {
      if (util::EqualsAsciiCaseInsensitive(item, "paused")) {
        spec.paused = true;
      } else if (util::EqualsAsciiCaseInsensitive(item, "running")) {
        spec.paused = false;
      } else {
        return false;
      }
    }
  }
  return true;
}

}  // namespace microbrowser::css
