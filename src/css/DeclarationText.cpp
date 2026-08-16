#include "css/DeclarationText.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "css/Tokenizer.h"
#include "gfx/ColorText.h"
#include "util/StringUtil.h"

namespace microbrowser::css {

namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// The properties whose value is exactly `<color>`. An explicit list rather than a suffix test on
// `-color`, because `border-color` is a *shorthand* of four and serializes differently, and
// `-webkit-text-fill-color` would match a rule this module has no implementation for. A property
// wrongly on this list starts rejecting values a page relies on, which is a worse failure than the
// one being fixed.
constexpr std::string_view kColorProperties[] = {
    "color",
    "background-color",
    "border-top-color",
    "border-right-color",
    "border-bottom-color",
    "border-left-color",
    "outline-color",
    "text-decoration-color",
    "caret-color",
    "column-rule-color",
    "text-emphasis-color",
    "flood-color",
    "lighting-color",
    "stop-color",
};

// The four CSS-wide keywords, which are valid in every property and serialize as themselves.
constexpr std::string_view kWideKeywords[] = {"inherit", "initial", "unset", "revert",
                                              "revert-layer"};

// Unquoted family names that are CSS-wide keywords or the reserved `default`.
// A quoted `"inherit"` is a name; an unquoted `inherit` is not.
constexpr std::string_view kReservedFamilyNames[] = {
    "inherit", "initial", "unset", "revert", "revert-layer", "default",
};

constexpr std::string_view kGenericFamilies[] = {
    "serif",         "sans-serif", "cursive",       "fantasy",     "monospace",
    "system-ui",     "math",       "ui-serif",      "ui-sans-serif",
    "ui-monospace",  "ui-rounded",
};

bool Contains(const auto& list, std::string_view value) {
  return std::find(std::begin(list), std::end(list), value) != std::end(list);
}

bool IsIdentStartByte(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

bool IsIdentByte(unsigned char c) {
  return IsIdentStartByte(c) || (c >= '0' && c <= '9') || c == '-';
}

bool CanUnquoteFamilyName(std::string_view name) {
  if (name.empty() || name.find("  ") != std::string_view::npos) {
    return false;
  }
  bool word_start = true;
  bool any = false;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c == ' ') {
      if (word_start) {
        return false;
      }
      word_start = true;
      continue;
    }
    if (word_start) {
      const bool dash_ident = c == '-' && i + 1 < name.size() &&
                              IsIdentStartByte(static_cast<unsigned char>(name[i + 1]));
      if (!IsIdentStartByte(c) && !dash_ident) {
        return false;
      }
      word_start = false;
      any = true;
    } else if (!IsIdentByte(c)) {
      return false;
    }
  }
  return any && !word_start;
}

std::string QuoteCssString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string SerializeFamilyName(std::string_view name, bool was_quoted) {
  const std::string lowered = util::AsciiLowerCase(name);
  if (was_quoted && (Contains(kGenericFamilies, std::string_view(lowered)) ||
                     Contains(kReservedFamilyNames, std::string_view(lowered)))) {
    return QuoteCssString(name);
  }
  if (!CanUnquoteFamilyName(name)) {
    return QuoteCssString(name);
  }
  return std::string(name);
}

struct ParsedFamily {
  std::string name;
  bool quoted = false;
};

bool ParseFontFamily(std::string_view value, std::string* out) {
  const std::vector<Token> tokens = Tokenize(value);
  std::vector<ParsedFamily> families;
  std::size_t at = 0;
  const auto skip_ws = [&]() {
    while (at < tokens.size() && tokens[at].kind == Token::Kind::Whitespace) {
      ++at;
    }
  };
  skip_ws();
  if (at >= tokens.size() || tokens[at].kind == Token::Kind::EndOfFile) {
    return false;
  }
  while (at < tokens.size() && tokens[at].kind != Token::Kind::EndOfFile) {
    skip_ws();
    if (at >= tokens.size() || tokens[at].kind == Token::Kind::EndOfFile) {
      return false;
    }
    ParsedFamily family;
    if (tokens[at].kind == Token::Kind::String) {
      family.name = tokens[at].value;
      family.quoted = true;
      ++at;
    } else if (tokens[at].kind == Token::Kind::Ident) {
      family.name = tokens[at].value;
      ++at;
      while (at < tokens.size() && (tokens[at].kind == Token::Kind::Whitespace ||
                                    tokens[at].kind == Token::Kind::Ident)) {
        if (tokens[at].kind == Token::Kind::Ident) {
          if (!family.name.empty()) {
            family.name.push_back(' ');
          }
          family.name += tokens[at].value;
        }
        ++at;
      }
      const std::string lowered_family = util::AsciiLowerCase(family.name);
      if (Contains(kReservedFamilyNames, std::string_view(lowered_family))) {
        return false;
      }
    } else {
      return false;
    }
    if (family.name.empty()) {
      return false;
    }
    families.push_back(std::move(family));
    skip_ws();
    if (at < tokens.size() && tokens[at].kind == Token::Kind::Comma) {
      ++at;
      skip_ws();
      if (at >= tokens.size() || tokens[at].kind == Token::Kind::EndOfFile) {
        return false;
      }
      continue;
    }
    break;
  }
  skip_ws();
  if (at < tokens.size() && tokens[at].kind != Token::Kind::EndOfFile) {
    return false;
  }
  if (families.empty()) {
    return false;
  }
  if (out != nullptr) {
    std::string serialized;
    for (std::size_t i = 0; i < families.size(); ++i) {
      if (i != 0) {
        serialized += ", ";
      }
      serialized += SerializeFamilyName(families[i].name, families[i].quoted);
    }
    *out = std::move(serialized);
  }
  return true;
}

// The family half of `font: <size> <family>`. Everything after the first
// length, percentage, or absolute/relative-size keyword is the family.
bool ParseFontShorthandFamily(std::string_view value, std::string* out) {
  const std::vector<Token> tokens = Tokenize(value);
  std::size_t family_at = tokens.size();
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token& token = tokens[i];
    if (token.kind == Token::Kind::Dimension || token.kind == Token::Kind::Percentage) {
      family_at = i + 1;
      break;
    }
    if (token.kind == Token::Kind::Ident) {
      const std::string word = util::AsciiLowerCase(token.value);
      if (word == "xx-small" || word == "x-small" || word == "small" || word == "medium" ||
          word == "large" || word == "x-large" || word == "xx-large" || word == "larger" ||
          word == "smaller") {
        family_at = i + 1;
        break;
      }
    }
  }
  while (family_at < tokens.size() && tokens[family_at].kind == Token::Kind::Whitespace) {
    ++family_at;
  }
  if (family_at < tokens.size() && tokens[family_at].kind == Token::Kind::Delim &&
      tokens[family_at].value == "/") {
    ++family_at;
    while (family_at < tokens.size() && tokens[family_at].kind != Token::Kind::EndOfFile &&
           tokens[family_at].kind != Token::Kind::Ident &&
           tokens[family_at].kind != Token::Kind::String) {
      ++family_at;
    }
  }
  if (family_at >= tokens.size()) {
    return false;
  }
  return ParseFontFamily(ReconstructTokens(tokens, family_at, tokens.size()), out);
}

// A colour as CSSOM serializes one: `rgb()` when opaque, `rgba()` otherwise, and the same form
// `getComputedStyle` reports. One function for both, in `gfx`, because two would be two answers to
// "how is a colour written down".
}  // namespace

DeclarationValidity CanonicaliseDeclaration(std::string_view property, std::string_view value,
                                            std::string* out) {
  const std::string name = util::AsciiLowerCase(Trim(property));
  const std::string_view trimmed = Trim(value);

  // An empty assignment is `removeProperty`, which every property accepts and which is not this
  // function's business.
  if (trimmed.empty()) {
    return DeclarationValidity::Unknown;
  }
  // `var()` and the other substitution functions defer the whole grammar to computed-value time, so
  // nothing here can say whether the result is valid. CSSOM says such a declaration is stored as
  // written, which is what `Unknown` already does.
  const std::string lowered = util::AsciiLowerCase(trimmed);
  if (lowered.find("var(") != std::string::npos ||
      lowered.find("env(") != std::string::npos) {
    return DeclarationValidity::Unknown;
  }
  if (Contains(kWideKeywords, std::string_view(lowered))) {
    if (out != nullptr) {
      *out = lowered;
    }
    return DeclarationValidity::Canonical;
  }

  if (Contains(kColorProperties, std::string_view(name))) {
    // `currentcolor` is a keyword rather than a colour: it has no red, green or blue until an
    // element is asked, so it serializes as itself. `invert` is outline-color's extra keyword.
    if (lowered == "currentcolor" || (name == "outline-color" && lowered == "invert")) {
      if (out != nullptr) {
        *out = lowered;
      }
      return DeclarationValidity::Canonical;
    }
    const std::optional<gfx::Color> color = gfx::ParseColorText(trimmed);
    if (!color.has_value()) {
      return DeclarationValidity::Invalid;
    }
    // **A named colour serializes as its name.** CSSOM serializes a specified value component by
    // component, and an identifier is an identifier: `el.style.color = 'red'` reads back `"red"` in
    // every browser, where `'#f00'` reads back `"rgb(255, 0, 0)"`. Only the numeric notations
    // collapse. Functional notation keeps its specified numbers -- `rgba(..., 0.5)` is 0.5, not the
    // 8-bit channel 128/255 -- because a Color *is* eight bits and cannot round-trip 0.5.
    const bool is_identifier =
        std::all_of(lowered.begin(), lowered.end(),
                    [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; });
    if (out != nullptr) {
      if (is_identifier) {
        *out = lowered;
      } else if (!trimmed.empty() && trimmed.front() == '#') {
        *out = gfx::SerializeColorText(*color);
      } else {
        const std::vector<Token> tokens = Tokenize(trimmed);
        *out = ReconstructTokens(tokens, 0, tokens.size());
      }
    }
    return DeclarationValidity::Canonical;
  }

  if (name == "font-family") {
    if (!ParseFontFamily(trimmed, out)) {
      return DeclarationValidity::Invalid;
    }
    return DeclarationValidity::Canonical;
  }
  if (name == "font") {
    std::string family;
    if (!ParseFontShorthandFamily(trimmed, &family)) {
      return DeclarationValidity::Invalid;
    }
    // Valid family, but this module does not yet serialize the whole shorthand
    // canonically. Store the text so a valid assignment still reads back.
    if (out != nullptr) {
      *out = std::string(trimmed);
    }
    return DeclarationValidity::Canonical;
  }

  if (name == "content") {
    const std::vector<Token> tokens = Tokenize(trimmed);
    std::string serialized = ReconstructTokens(tokens, 0, tokens.size());
    constexpr std::string_view kDefaultList = ", decimal)";
    if (serialized.starts_with("counter(") && serialized.ends_with(kDefaultList)) {
      serialized.resize(serialized.size() - kDefaultList.size());
      serialized.push_back(')');
    }
    if (out != nullptr) {
      *out = std::move(serialized);
    }
    return DeclarationValidity::Canonical;
  }

  // Custom properties preserve the token stream the page wrote. Reconstructing
  // them would turn a 25-digit integer into a double.
  if (name.size() >= 2 && name[0] == '-' && name[1] == '-') {
    return DeclarationValidity::Unknown;
  }

  // Token-level specified-value serialization: `.1em` → `0.1em`, `'x'` → `"x"`,
  // `url(x)` → `url("x")`. The property grammar is not checked, so this stays
  // Unknown -- a name that is not a CSS property is still dropped by the caller.
  if (out != nullptr) {
    const std::vector<Token> tokens = Tokenize(trimmed);
    *out = ReconstructTokens(tokens, 0, tokens.size());
  }
  return DeclarationValidity::Unknown;
}

}  // namespace microbrowser::css
