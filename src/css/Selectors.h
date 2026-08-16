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

// What a nested selector list is allowed to be, which is not the same question
// at every call site.
//
// `relative` admits a leading combinator (`> .b`), which only `:has()` may
// write; everywhere else `> .b` is a syntax error rather than a selector whose
// first combinator nothing reads. `inside_has` forbids a second `:has()`, which
// the specification disallows at any depth, and it is a flag rather than a
// depth count because `:has(:is(:has(x)))` is just as forbidden as the direct
// spelling.
struct SelectorParseMode {
  // Nesting inside functional pseudo-classes, bounded by
  // `kMaxSelectorNestingDepth`. A caller starting from text passes zero.
  int depth = 0;
  bool relative = false;
  bool inside_has = false;
  // Null means no `@namespace` rules are in scope, which is the cascade: a
  // named prefix does not parse. CSSOM parse points this at the prefixes the
  // sheet has declared so far.
  const SelectorNamespaces* namespaces = nullptr;
};

// Parses a selector list out of `tokens[from, to)`. Returns empty on anything
// it does not understand, so the whole rule is dropped rather than applied to
// elements it was never written for.
std::vector<Selector> ParseSelectors(const std::vector<Token>& tokens, std::size_t from,
                                     std::size_t to, SelectorParseMode mode = {});

// The `An+B` of `:nth-child()`, over `tokens[from, to)`. Sets only `a` and `b`;
// which sequence they count over was decided by the function name, and the
// `of S` half is split off by the caller. Lives in `SelectorNth.cpp` -- see the
// note there for why it is a translation unit of its own.
bool ParseAnPlusB(const std::vector<Token>& tokens, std::size_t from, std::size_t to,
                  NthPattern& out);

}  // namespace microbrowser::css
