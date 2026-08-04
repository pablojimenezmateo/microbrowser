#pragma once

#include <optional>
#include <string_view>

#include "css/ComputedStyle.h"

// `calc()`, evaluated to a Length.
//
// Private to the module: nothing outside it asks for a calc, it asks for a
// length, and ParseLength is where the two meet.

namespace microbrowser::css {

// Evaluates `calc(...)` — or the parenthesised sub-expressions inside one — to
// the length it stands for.
//
// Nullopt for three different failures, deliberately not distinguished, because
// every one of them means the same thing to CSS: the declaration is invalid and
// is dropped. The text is not a calc; it is a calc this engine cannot represent
// (`calc(100% - 1em)` mixes two relative terms, and a Length carries one); or it
// is a calc that is invalid outright — a unit mismatch, a division by zero, a
// multiplication of two lengths, or a result that is not a finite number.
std::optional<Length> ParseCalc(std::string_view text);

}  // namespace microbrowser::css
