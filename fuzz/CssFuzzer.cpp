#include <cstddef>
#include <cstdint>
#include <string_view>

#include "css/StyleSheet.h"
#include "css/Tokenizer.h"

// The CSS tokenizer and parser, fed arbitrary bytes.
//
// CSS has no failure mode: a stylesheet with a syntax error is one whose bad
// declarations are dropped and whose good ones still apply, and the recovery is
// normative. So the property is that parsing terminates and produces a bounded
// result for any input — a parser that can loop, or that can emit more rules
// than the input could describe, is a denial of service reachable by anyone who
// can serve a stylesheet.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  const auto tokens = microbrowser::css::Tokenize(input);
  if (tokens.size() > size + 1) {
    __builtin_trap();  // more tokens than bytes plus the EOF token
  }

  const microbrowser::css::StyleSheet sheet = microbrowser::css::ParseStyleSheet(input);
  if (sheet.rules.size() + sheet.skipped > size + 1) {
    __builtin_trap();  // more rules than the input could possibly describe
  }
  microbrowser::css::ParseDeclarationList(input);
  microbrowser::css::ParseSelectorList(input);
  return 0;
}
