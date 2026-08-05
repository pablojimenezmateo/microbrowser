#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace microbrowser::util {

// Base64, both alphabets, in one place.
//
// Here rather than beside its first caller because it already had two: `data:`
// URLs decode base64 in `src/engine`, and both Subresource Integrity and CSP's
// hash-sources decode it in modules that may not see the engine. Three copies
// of a decoder that disagree about what `-` means is the class of bug
// PercentEncoding.h exists to have deleted once already.
//
// `Base64Decode` accepts the standard alphabet (`+/`) and the URL-safe one
// (`-_`), because SRI and CSP both name base64 and pages use both. It does
// **not** accept a mix of the two in one string: `a+b_c` is not a value anyone
// meant to write, and accepting it would mean two spellings of one digest
// compare unequal as text and equal as bytes.
//
// Padding is optional and is checked rather than skipped: `=` may appear only
// at the end, and only in the quantity the length implies. A decoder that
// ignores padding accepts `YQ==Yg==` as "ab", which is two values for one
// digest -- exactly what an integrity check must not have.
std::optional<std::string> Base64Decode(std::string_view text);

// The standard alphabet with padding. Only used to *report* a digest -- an
// integrity failure that says which hash it computed is the difference between
// a debuggable page and a blank one.
std::string Base64Encode(std::string_view bytes);

}  // namespace microbrowser::util
