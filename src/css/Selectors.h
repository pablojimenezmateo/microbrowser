#pragma once

#include <cstddef>
#include <vector>

#include "css/StyleSheet.h"
#include "css/Token.h"

// The selector parser, over a token run rather than over text.
//
// Private to the module: `ParseSelectorList` is the public spelling of the same
// thing, and the rule parser needs the range form because a rule's prelude is a
// slice of the stylesheet's tokens rather than a string of its own.

namespace microbrowser::css {

// Parses a selector list out of `tokens[from, to)`. Returns empty on anything
// it does not understand, so the whole rule is dropped rather than applied to
// elements it was never written for. `depth` counts selector lists nested
// inside functional pseudo-classes and is what `kMaxSelectorNestingDepth`
// bounds; a caller starting from text passes zero.
std::vector<Selector> ParseSelectors(const std::vector<Token>& tokens, std::size_t from,
                                     std::size_t to, int depth = 0);

}  // namespace microbrowser::css
