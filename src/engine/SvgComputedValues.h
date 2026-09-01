#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "css/ComputedStyle.h"

namespace microbrowser::engine {

// The computed value of one SVG presentation property, as `getComputedStyle`
// reports it (ADR 0043 §2). `std::nullopt` when the property is not one of
// them, which is what lets the caller carry on down its own chain.
std::optional<std::string> SvgComputedValueOf(const css::ComputedStyle& style,
                                              std::string_view property);

}  // namespace microbrowser::engine
