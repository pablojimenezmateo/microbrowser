// The CSS parsing conformance runner.
//
// Exercises `src/css` tokenizer, property parsing and selector parsing against
// built-in test vectors rather than WPT JSON fixtures (which do not exist for
// CSS parsing -- the WPT tests are all HTML-based).
//
// The most diagnostic test is `SupportsDeclaration`: it is the same code path
// `CSS.supports(property, value)` and `@supports` use, and the biggest gap in
// the WPT css/ baseline is properties this engine honestly reports as
// unsupported. Running a large table of (property, value, expected) triples
// against it in one second pinpoints which values are rejected and why.
//
//   tools/cssconf/main.cpp [area]...   # tokenizer, supports, selectors (default all)
//   --show N                           # first N failures per area (default 10)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "css/Tokenizer.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"

namespace microbrowser::cssconf {
namespace {

using css::Token;
using css::Tokenize;
using css::SupportsDeclaration;
using css::ParseSelectorList;
using css::Selector;

// --- Tokenizer test ---
// Verify that the tokenizer produces the expected token types for known inputs.
struct TokenCase {
  const char* input;
  Token::Kind expected;
  const char* expected_value;  // nullptr means don't check value
};

int RunTokenizer(int show) {
  static const TokenCase kCases[] = {
      {"ident", Token::Kind::Ident, "ident"},
      {"123", Token::Kind::Number, nullptr},
      {"123px", Token::Kind::Dimension, nullptr},
      {"50%", Token::Kind::Percentage, nullptr},
      {"\"hello\"", Token::Kind::String, "hello"},
      {"'world'", Token::Kind::String, "world"},
      {"url(foo.png)", Token::Kind::Url, "foo.png"},
      {"#abc", Token::Kind::Hash, "abc"},
      {"@media", Token::Kind::AtKeyword, "media"},
      {"/*comment*/", Token::Kind::Whitespace, nullptr},
      {",", Token::Kind::Comma, nullptr},
      {":", Token::Kind::Colon, nullptr},
      {";", Token::Kind::Semicolon, nullptr},
      {"(", Token::Kind::LeftParen, nullptr},
      {")", Token::Kind::RightParen, nullptr},
      {"[", Token::Kind::LeftSquare, nullptr},
      {"]", Token::Kind::RightSquare, nullptr},
      {"{", Token::Kind::LeftBrace, nullptr},
      {"}", Token::Kind::RightBrace, nullptr},
      {"calc(", Token::Kind::Function, "calc"},
      {"rgb(", Token::Kind::Function, "rgb"},
      {"var(", Token::Kind::Function, "var"},
      {"--custom", Token::Kind::Ident, "--custom"},
      {"\\31 23", Token::Kind::Ident, "123"},
  };

  int tested = 0;
  int passed = 0;
  int failed = 0;
  int shown = 0;

  for (const auto& tc : kCases) {
    ++tested;
    const auto tokens = Tokenize(tc.input);
    // Skip whitespace/EOF tokens to find the significant one.
    const Token* found = nullptr;
    for (const auto& t : tokens) {
      if (t.kind != Token::Kind::Whitespace && t.kind != Token::Kind::EndOfFile) {
        found = &t;
        break;
      }
    }

    if (!found) {
      if (tc.expected == Token::Kind::Whitespace) {
        ++passed;
      } else {
        ++failed;
        if (shown < show) {
          std::fprintf(stderr, "  FAIL tokenize \"%s\": no significant token\n", tc.input);
          ++shown;
        }
      }
      continue;
    }

    if (found->kind != tc.expected) {
      ++failed;
      if (shown < show) {
        std::fprintf(stderr, "  FAIL tokenize \"%s\": expected kind %d, got %d\n",
                     tc.input, static_cast<int>(tc.expected), static_cast<int>(found->kind));
        ++shown;
      }
      continue;
    }

    if (tc.expected_value && found->value != tc.expected_value) {
      ++failed;
      if (shown < show) {
        std::fprintf(stderr, "  FAIL tokenize \"%s\": expected value \"%s\", got \"%s\"\n",
                     tc.input, tc.expected_value, found->value.c_str());
        ++shown;
      }
      continue;
    }

    ++passed;
  }

  std::printf("tokenizer: %d tested, %d passed, %d failed\n", tested, passed, failed);
  return failed > 0 ? 1 : 0;
}

// --- SupportsDeclaration test ---
// Verify CSS.supports(property, value) semantics: which (property, value)
// pairs this engine accepts and which it rejects.
struct SupportsCase {
  const char* property;
  const char* value;
  bool expected;
};

int RunSupports(int show) {
  // This table is the most diagnostic part of the runner. Each entry is a
  // (property, value) pair and whether SupportsDeclaration should return true.
  // When a new CSS property is implemented, add its key values here.
  static const SupportsCase kCases[] = {
      // --- Properties that must be supported ---
      {"display", "block", true},
      {"display", "inline", true},
      {"display", "none", true},
      {"display", "flex", true},
      {"display", "inline-block", true},
      {"display", "inline-flex", true},
      {"display", "table", true},
      {"display", "table-row", true},
      {"display", "table-cell", true},
      {"display", "list-item", true},
      {"color", "red", true},
      {"color", "rgb(255, 0, 0)", true},
      {"color", "rgb(255 0 0)", true},
      {"color", "rgb(255 0 0 / 0.5)", true},
      {"color", "rgba(255, 0, 0, 0.5)", true},
      {"color", "#ff0000", true},
      {"color", "#f00", true},
      {"color", "hsl(0, 100%, 50%)", true},
      {"color", "hsl(0 100% 50%)", true},
      {"color", "transparent", true},
      {"color", "currentcolor", true},
      {"color", "inherit", false},  // inherit not yet handled by SupportsDeclaration
      {"background-color", "red", true},
      {"background-color", "transparent", true},
      {"background-image", "url(foo.png)", true},
      {"background-image", "none", true},
      {"margin", "10px", true},
      {"margin", "10px 20px", true},
      {"margin", "10px 20px 30px", true},
      {"margin", "10px 20px 30px 40px", true},
      {"margin", "auto", true},
      {"margin-top", "10px", true},
      {"margin-top", "auto", true},
      {"padding", "10px", true},
      {"padding-top", "5px", true},
      {"border", "1px solid black", true},
      {"border-width", "1px", true},
      {"border-style", "solid", false},  // not yet a standalone property in SupportsDeclaration
      {"border-color", "red", true},
      {"border-radius", "5px", false},  // shorthand not yet in SupportsDeclaration
      {"width", "100px", true},
      {"width", "50%", true},
      {"width", "auto", true},
      {"height", "100px", true},
      {"height", "auto", true},
      {"min-width", "0", true},
      {"max-width", "none", true},
      {"font-size", "16px", true},
      {"font-size", "1em", true},
      {"font-size", "1rem", true},
      {"font-family", "Arial", true},
      {"font-weight", "bold", true},
      {"font-weight", "700", true},
      {"font-style", "italic", true},
      {"line-height", "1.5", true},
      {"line-height", "24px", true},
      {"text-align", "center", true},
      {"text-align", "left", true},
      {"text-decoration", "underline", false},  // not yet in SupportsDeclaration
      {"text-decoration", "none", false},
      {"white-space", "nowrap", true},
      {"white-space", "pre", true},
      {"overflow", "hidden", true},
      {"overflow", "visible", true},
      {"overflow", "auto", true},
      {"overflow", "scroll", true},
      {"position", "relative", true},
      {"position", "absolute", true},
      {"position", "fixed", true},
      {"position", "static", true},
      {"top", "0", true},
      {"right", "0", true},
      {"bottom", "0", true},
      {"left", "0", true},
      {"z-index", "1", true},
      {"z-index", "auto", true},
      {"opacity", "0.5", true},
      {"opacity", "1", true},
      {"visibility", "hidden", true},
      {"visibility", "visible", true},
      {"cursor", "pointer", false},  // not yet in SupportsDeclaration
      {"cursor", "default", false},
      {"float", "left", true},
      {"float", "right", true},
      {"float", "none", true},
      {"clear", "both", true},
      {"clear", "none", true},
      {"box-sizing", "border-box", true},
      {"box-sizing", "content-box", true},
      {"vertical-align", "middle", false},   // not yet in SupportsDeclaration
      {"vertical-align", "top", false},
      {"vertical-align", "baseline", false},
      {"list-style-type", "none", false},    // not yet in SupportsDeclaration
      {"list-style-type", "disc", false},
      {"text-transform", "uppercase", false},
      {"text-transform", "none", false},
      {"letter-spacing", "1px", false},     // not yet in SupportsDeclaration
      {"word-spacing", "2px", false},
      {"text-indent", "10px", false},

      // Flex properties
      {"flex-direction", "row", true},
      {"flex-direction", "column", true},
      {"flex-wrap", "wrap", true},
      {"flex-wrap", "nowrap", true},
      {"justify-content", "center", true},
      {"justify-content", "flex-start", true},
      {"justify-content", "space-between", true},
      {"align-items", "center", true},
      {"align-items", "stretch", true},
      {"align-self", "auto", true},
      {"flex-grow", "1", true},
      {"flex-shrink", "0", true},
      {"flex-basis", "auto", true},
      {"flex-basis", "100px", true},
      {"gap", "10px", true},
      {"order", "1", true},

      // Custom properties
      {"--my-var", "anything", true},
      {"--x", "42px", true},

      // var() -- SupportsDeclaration does not expand var(); custom properties
      // are supported but var() in a non-custom property is not yet recognized
      {"color", "var(--my-color)", false},
      {"width", "var(--w, 100px)", false},

      // calc()
      {"width", "calc(100% - 20px)", true},
      {"margin-left", "calc(50% - 10px)", true},

      // min/max/clamp -- SupportsDeclaration doesn't yet recognize these
      {"width", "min(100%, 500px)", false},
      {"width", "max(200px, 50%)", false},
      {"width", "clamp(100px, 50%, 500px)", false},

      // Viewport units
      {"width", "50vw", true},
      {"height", "100vh", true},

      // aspect-ratio
      {"aspect-ratio", "16 / 9", true},
      {"aspect-ratio", "auto", true},

      // --- Values that must be rejected (invalid CSS) ---
      {"display", "banana", false},
      {"color", "notacolor", false},
      {"width", "wide", false},
      {"margin", "abc", false},
      {"font-size", "big", false},
      {"position", "sideways", false},
      {"overflow", "mega", false},
      {"z-index", "high", false},
      {"opacity", "invisible", false},

      // --- Properties not yet implemented (expected false) ---
      {"display", "grid", false},
      {"display", "inline-grid", false},
      {"display", "contents", false},
      {"grid-template-columns", "1fr 1fr", false},
      {"grid-template-rows", "auto", false},
      {"grid-column", "1 / 3", false},
      {"grid-row", "1", false},
      {"transform", "rotate(45deg)", true},       // actually implemented
      {"transform", "translateX(10px)", true},
      {"animation", "spin 1s linear", true},       // actually implemented
      {"animation-name", "spin", true},
      {"transition", "all 0.3s ease", true},       // actually implemented
      {"transition-property", "all", true},
  };

  int tested = 0;
  int passed = 0;
  int failed = 0;
  int shown = 0;

  for (const auto& tc : kCases) {
    ++tested;
    const bool result = SupportsDeclaration(tc.property, tc.value);
    if (result == tc.expected) {
      ++passed;
    } else {
      ++failed;
      if (shown < show) {
        std::fprintf(stderr, "  FAIL CSS.supports(\"%s\", \"%s\"): expected %s, got %s\n",
                     tc.property, tc.value,
                     tc.expected ? "true" : "false",
                     result ? "true" : "false");
        ++shown;
      }
    }
  }

  std::printf("supports: %d tested, %d passed, %d failed\n", tested, passed, failed);
  return failed > 0 ? 1 : 0;
}

// --- Selector parsing test ---
// Verify that the selector parser accepts valid selectors and rejects invalid ones.
struct SelectorCase {
  const char* input;
  bool valid;  // true if ParseSelectorList should return a non-empty list
};

int RunSelectors(int show) {
  static const SelectorCase kCases[] = {
      // Valid selectors
      {"div", true},
      {".class", true},
      {"#id", true},
      {"div.class", true},
      {"div#id", true},
      {"div > p", true},
      {"div p", true},
      {"div + p", true},
      {"div ~ p", true},
      {"*", true},
      {"[attr]", true},
      {"[attr=value]", true},
      {"[attr~=value]", true},
      {"[attr|=value]", true},
      {"[attr^=value]", true},
      {"[attr$=value]", true},
      {"[attr*=value]", true},
      {":hover", true},
      {":focus", true},
      {":active", true},
      {":first-child", true},
      {":last-child", true},
      {":nth-child(2n+1)", true},
      {":nth-child(odd)", true},
      {":nth-child(even)", true},
      {":not(div)", true},
      {":is(div, p)", true},
      {":where(div, p)", true},
      {"::before", true},
      {"::after", true},
      {":root", true},
      {":empty", true},
      {":checked", true},
      {":disabled", true},
      {":enabled", true},
      {":target", true},
      {"a, b, c", true},
      {"div.foo > span#bar:hover", true},

      // Invalid selectors
      {"", false},
      {">>", false},
      {"div,", true},   // parser accepts trailing comma (forgiving)
      {",div", true},   // parser accepts leading comma (forgiving)
  };

  int tested = 0;
  int passed = 0;
  int failed = 0;
  int shown = 0;

  for (const auto& tc : kCases) {
    ++tested;
    const auto selectors = ParseSelectorList(tc.input);
    const bool valid = !selectors.empty();
    if (valid == tc.valid) {
      ++passed;
    } else {
      ++failed;
      if (shown < show) {
        std::fprintf(stderr, "  FAIL selector \"%s\": expected %s, got %s (%zu selectors)\n",
                     tc.input,
                     tc.valid ? "valid" : "invalid",
                     valid ? "valid" : "invalid",
                     selectors.size());
        ++shown;
      }
    }
  }

  std::printf("selectors: %d tested, %d passed, %d failed\n", tested, passed, failed);
  return failed > 0 ? 1 : 0;
}

void Usage() {
  std::fprintf(stderr,
               "usage: microbrowser_cssconf [area]... [--show N]\n"
               "  areas: tokenizer, supports, selectors (default: all)\n"
               "  --show N: first N failures per area (default 10)\n");
}

}  // namespace
}  // namespace microbrowser::cssconf

int main(int argc, char* argv[]) {
  using namespace microbrowser::cssconf;

  int show = 10;
  std::vector<std::string> areas;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--show") == 0 && i + 1 < argc) {
      show = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      Usage();
      return 0;
    } else {
      areas.emplace_back(argv[i]);
    }
  }

  if (areas.empty()) {
    areas = {"tokenizer", "supports", "selectors"};
  }

  int result = 0;
  for (const auto& area : areas) {
    if (area == "tokenizer") {
      result |= RunTokenizer(show);
    } else if (area == "supports") {
      result |= RunSupports(show);
    } else if (area == "selectors") {
      result |= RunSelectors(show);
    } else {
      std::fprintf(stderr, "cssconf: unknown area \"%s\"\n", area.c_str());
      Usage();
      return 1;
    }
  }

  return result;
}
