#include "css/DeclarationText.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "gfx/ColorText.h"
#include "util/StringUtil.h"

namespace microbrowser::css {

namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// The properties whose value is exactly `<color>`. An explicit list rather than a suffix test on
// `-color`, because `border-color` is a *shorthand* of four and serializes differently, and
// `-webkit-text-fill-color` would match a rule this module has no implementation for. A property
// wrongly on this list starts rejecting values a page relies on, which is a worse failure than the
// one being fixed.
constexpr std::string_view kColorProperties[] = {
    "color",
    "background-color",
    "border-top-color",
    "border-right-color",
    "border-bottom-color",
    "border-left-color",
    "outline-color",
    "text-decoration-color",
    "caret-color",
    "column-rule-color",
    "text-emphasis-color",
    "flood-color",
    "lighting-color",
    "stop-color",
};

// The four CSS-wide keywords, which are valid in every property and serialize as themselves.
constexpr std::string_view kWideKeywords[] = {"inherit", "initial", "unset", "revert",
                                              "revert-layer"};

bool Contains(const auto& list, std::string_view value) {
  return std::find(std::begin(list), std::end(list), value) != std::end(list);
}

// A colour as CSSOM serializes one: `rgb()` when opaque, `rgba()` otherwise, and the same form
// `getComputedStyle` reports. One function for both, in `gfx`, because two would be two answers to
// "how is a colour written down".
}  // namespace

DeclarationValidity CanonicaliseDeclaration(std::string_view property, std::string_view value,
                                            std::string* out) {
  const std::string name = util::AsciiLowerCase(Trim(property));
  const std::string_view trimmed = Trim(value);

  // An empty assignment is `removeProperty`, which every property accepts and which is not this
  // function's business.
  if (trimmed.empty()) {
    return DeclarationValidity::Unknown;
  }
  // `var()` and the other substitution functions defer the whole grammar to computed-value time, so
  // nothing here can say whether the result is valid. CSSOM says such a declaration is stored as
  // written, which is what `Unknown` already does.
  const std::string lowered = util::AsciiLowerCase(trimmed);
  if (lowered.find("var(") != std::string::npos ||
      lowered.find("env(") != std::string::npos) {
    return DeclarationValidity::Unknown;
  }
  if (Contains(kWideKeywords, std::string_view(lowered))) {
    if (out != nullptr) {
      *out = lowered;
    }
    return DeclarationValidity::Canonical;
  }

  if (Contains(kColorProperties, std::string_view(name))) {
    // `currentcolor` is a keyword rather than a colour: it has no red, green or blue until an
    // element is asked, so it serializes as itself.
    if (lowered == "currentcolor") {
      if (out != nullptr) {
        *out = lowered;
      }
      return DeclarationValidity::Canonical;
    }
    const std::optional<gfx::Color> color = gfx::ParseColorText(trimmed);
    if (!color.has_value()) {
      return DeclarationValidity::Invalid;
    }
    // **A named colour serializes as its name.** CSSOM serializes a specified value component by
    // component, and an identifier is an identifier: `el.style.color = 'red'` reads back `"red"` in
    // every browser, where `'#f00'` reads back `"rgb(255, 0, 0)"`. Only the numeric notations
    // collapse. The *computed* value is `rgb(255, 0, 0)` either way, and that is a different
    // question asked in a different place.
    const bool is_identifier =
        std::all_of(lowered.begin(), lowered.end(),
                    [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; });
    if (out != nullptr) {
      *out = is_identifier ? lowered : gfx::SerializeColorText(*color);
    }
    return DeclarationValidity::Canonical;
  }

  return DeclarationValidity::Unknown;
}

}  // namespace microbrowser::css
