#pragma once

#include <optional>
#include <string_view>

#include "css/ComputedStyle.h"
#include "css/MediaQuery.h"
#include "css/MediaQuery.h"

// Math functions evaluated to a Length: `calc()`, `min()`, `max()`, `clamp()`.
//
// Private to the module: nothing outside it asks for a calc, it asks for a
// length, and ParseLength is where the two meet.

namespace microbrowser::css {

// Evaluates `calc(...)` / `min(...)` / `max(...)` / `clamp(...)` — and the
// nested forms inside them — to the length it stands for.
//
// Nullopt for three different failures, deliberately not distinguished, because
// every one of them means the same thing to CSS: the declaration is invalid and
// is dropped. The text is not a math function; it is one this engine cannot
// represent (`calc(100% - 1em)` mixes two relative terms, and a Length carries
// one; `min(50px, 70%)` needs a used value to compare); or it is invalid
// outright — a unit mismatch, a division by zero, a multiplication of two
// lengths, or a result that is not a finite number.
std::optional<Length> ParseCalc(std::string_view text, const MediaContext& context = {},
                               float root_font_size = kRootFontSize);

}  // namespace microbrowser::css
