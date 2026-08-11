#pragma once

#include <string_view>

#include "css/StyleSheet.h"
#include "dom/Node.h"

// The four selector predicates whose answer is a question about *content*
// rather than about tree shape: which namespace a name is in, whether an
// attribute value matches under a case rule, what language an element is in,
// and which way its text runs.
//
// Private to the module, and split out of `SelectorMatch.cpp` because each one
// carries a specification of its own -- RFC 4647's extended filtering and
// HTML's directionality algorithm are both longer than the compound matcher
// that calls them. They stay pure functions of (element, part): `src/css` may
// see `src/text` for exactly one reason, which is that "which direction is this
// text?" is a character-property question (see `MODULE.deps`).

namespace microbrowser::css {

// Type-selector matching, namespace constraint included.
bool TypeSelectorMatches(const SelectorPart& part, const dom::Element& element);

// `[att]`, `[att=val]` and the rest, with the `i`/`s` flag and the namespace
// constraint applied.
bool AttributeSelectorMatches(const SelectorPart& part, const dom::Element& element);

// `:lang(a, b)`, with `ranges` as the parser left it: comma-separated, lower
// case. RFC 4647 §3.3.2 extended filtering.
bool LangSelectorMatches(const dom::Element& element, std::string_view ranges);

// `:dir(ltr)` / `:dir(rtl)`. Any other argument matches nothing, which is not
// the same as failing to parse -- see the parser.
bool DirSelectorMatches(const dom::Element& element, std::string_view direction);

// UAX #9's rule P2 applied to an element's own text: true when the first strong
// character is right-to-left. This is what `dir="auto"` means.
//
// Shared with the cascade, which needs the same answer for the `direction`
// property that presentational attributes produce. Two copies would be two
// answers, and the failure mode is a `:dir(rtl)` rule that does not apply to an
// element the cascade is already laying out right to left.
bool AutoDirectionIsRtl(const dom::Element& element);

}  // namespace microbrowser::css
