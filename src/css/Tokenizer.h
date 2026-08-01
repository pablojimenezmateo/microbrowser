#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "css/Token.h"

namespace microbrowser::css {

// Tokenizes a stylesheet, per CSS Syntax Level 3 §4.3.
//
// Whole-input rather than incremental: a stylesheet is a resource that arrives
// complete, unlike an HTML document that is parsed as it streams. The token
// vector is what the parser walks, and it is bounded by the input length.
std::vector<Token> Tokenize(std::string_view input);

// The pieces below are exposed because each is a place the specification is
// counter-intuitive and each deserves its own test.

// An identifier may start with `-` or `\`, and `--custom` is an ident rather
// than two delims. Getting this wrong silently loses every custom property.
bool WouldStartIdentifier(std::string_view input);

// A number may be `1`, `+1`, `-1`, `.5`, `1.5`, `1e3`, `1E-3`. The exponent
// forms appear in real stylesheets from preprocessors.
bool WouldStartNumber(std::string_view input);

// Consumes an escape sequence, which may be a hex code point of up to six
// digits optionally followed by one whitespace character. `\31 23` is the
// character `1` followed by `23`, not `\3123`.
std::size_t ConsumeEscape(std::string_view input, std::string& out);

}  // namespace microbrowser::css
