#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace microbrowser::css {

// What `el.style.color = '#000'` is allowed to store, and what it reads back as.
//
// CSSOM says an assignment to a `CSSStyleDeclaration` property **parses** the value and does
// nothing at all when it does not parse -- so `el.style.color = 'nonsense'` leaves the declaration
// as it was rather than storing the word -- and that reading a property back gives the value's
// *serialization*, which is canonical rather than what the page wrote. `#000` reads back as
// `rgb(0, 0, 0)` in every browser, and `css/CSS2/syntax/colors-007.html` is 904 subtests of
// exactly that.
//
// This engine stored whatever was assigned and read it back verbatim, which is wrong in both
// directions at once and is invisible until something asks.
//
// **The answer has three cases and the third is the honest one.** A property whose grammar this
// module can check gets `Invalid` or `Canonical`; a property it cannot gets `Unknown`, and the
// caller keeps the old behaviour of storing the text as written. That is a deliberate partial
// implementation rather than a guess: a property wrongly canonicalised silently changes what a page
// reads back, while one left alone behaves exactly as it did before this existed.
enum class DeclarationValidity {
  // Not a property this module canonicalises. Store the text as written.
  Unknown,
  // Parsed, and it did not. The assignment does nothing.
  Invalid,
  // Parsed. `out` holds the serialization to store.
  Canonical,
};

// `property` is the CSS name (`background-color`, not `backgroundColor`).
// On `Unknown`, `out` still receives the token serialization (`.1em` → `0.1em`)
// so a caller that keeps the declaration stores the canonical form.
DeclarationValidity CanonicaliseDeclaration(std::string_view property, std::string_view value,
                                            std::string* out);

}  // namespace microbrowser::css
