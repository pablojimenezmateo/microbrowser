#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "css/ComputedStyle.h"
#include "css/StyleResolver.h"
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
namespace {

int NestingDepth(const microbrowser::css::Selector& selector) {
  int deepest = 0;
  for (const auto& compound : selector.compounds) {
    for (const auto& part : compound.parts) {
      for (const auto& argument : part.arguments) {
        deepest = std::max(deepest, 1 + NestingDepth(argument));
      }
    }
  }
  return deepest;
}

}  // namespace

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
  // Every declaration is also *applied*, which is the only way the value
  // parsers are reached at all: parsing a sheet stops at the text of a value,
  // and `calc()` is a second recursive descent that starts there. Before this
  // the fuzzer explored the grammar of a stylesheet and none of the grammar of
  // what is in one.
  const microbrowser::css::ComputedStyle initial;
  for (const microbrowser::css::Declaration& declaration :
       microbrowser::css::ParseDeclarationList(input)) {
    microbrowser::css::ComputedStyle style;
    microbrowser::css::ApplyDeclaration(declaration, initial, style);
  }

  // Selector lists nest, so the parser recurses over attacker-controlled input
  // and the recursion is bounded. Checking the bound here rather than only in a
  // unit test is the difference between "the shapes we thought of stay inside
  // it" and "no shape reachable from these bytes leaves it".
  for (const auto& selector : microbrowser::css::ParseSelectorList(input)) {
    if (NestingDepth(selector) > microbrowser::css::kMaxSelectorNestingDepth) {
      __builtin_trap();
    }
  }
  return 0;
}
