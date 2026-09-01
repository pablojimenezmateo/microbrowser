#include "css/DeclarationText.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "css/Tokenizer.h"
#include "css/ComputedStyle.h"
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

int HexDigitValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

struct UrangeCursor {
  std::string_view text;
  std::size_t at = 0;

  void SkipComments() {
    while (at + 1 < text.size() && text[at] == '/' && text[at + 1] == '*') {
      at += 2;
      while (at + 1 < text.size() && !(text[at] == '*' && text[at + 1] == '/')) {
        ++at;
      }
      at = at + 1 < text.size() ? at + 2 : text.size();
    }
  }
  char Peek() {
    SkipComments();
    return at < text.size() ? text[at] : '\0';
  }
  char Take() {
    SkipComments();
    return at < text.size() ? text[at++] : '\0';
  }
  bool Ended() {
    SkipComments();
    return at >= text.size();
  }
};

bool ParseOneUrange(UrangeCursor& cursor, std::uint32_t& first, std::uint32_t& last) {
  const char u = cursor.Take();
  if (u != 'u' && u != 'U') {
    return false;
  }
  if (cursor.Take() != '+') {
    return false;
  }
  std::string hex;
  std::size_t questions = 0;
  while (hex.size() + questions < 6) {
    const char c = cursor.Peek();
    if (HexDigitValue(c) >= 0 && questions == 0) {
      hex.push_back(cursor.Take());
    } else if (c == '?') {
      ++questions;
      cursor.Take();
    } else {
      break;
    }
  }
  if (hex.empty() && questions == 0) {
    return false;
  }
  const auto hex_value = [](const std::string& digits, char fill, std::size_t pad) {
    std::string padded = digits;
    padded.append(pad, fill);
    std::uint32_t value = 0;
    for (const char c : padded) {
      value = (value << 4) | static_cast<std::uint32_t>(HexDigitValue(c));
    }
    return value;
  };
  first = hex_value(hex, '0', questions);
  last = hex_value(hex, 'F', questions);
  if (questions == 0 && cursor.Peek() == '-') {
    cursor.Take();
    std::string second;
    while (second.size() < 6 && HexDigitValue(cursor.Peek()) >= 0) {
      second.push_back(cursor.Take());
    }
    if (second.empty()) {
      return false;
    }
    last = hex_value(second, '0', 0);
  }
  if (first > 0x10FFFF || last > 0x10FFFF) {
    return false;
  }
  return true;
}

bool ParseUnicodeRangeList(std::string_view value, std::string* out) {
  UrangeCursor cursor{Trim(value)};
  if (cursor.Ended()) {
    return false;
  }
  std::string serialized;
  while (!cursor.Ended()) {
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    if (!ParseOneUrange(cursor, first, last)) {
      return false;
    }
    if (!serialized.empty()) {
      serialized += ", ";
    }
    char buffer[32] = {};
    if (first == last) {
      std::snprintf(buffer, sizeof(buffer), "U+%X", first);
    } else {
      std::snprintf(buffer, sizeof(buffer), "U+%X-%X", first, last);
    }
    serialized += buffer;
    if (cursor.Ended()) {
      break;
    }
    if (cursor.Peek() != ',') {
      return false;
    }
    cursor.Take();
  }
  if (out != nullptr) {
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
// One component of an individual transform property, as CSS serializes a
// specified value: the number in its shortest form, with the unit it was
// written with.
std::string ComponentText(const Token& token, bool negate = false) {
  double number = token.number;
  if (negate) {
    number = -number;
  }
  std::string text;
  if (number == static_cast<double>(static_cast<long long>(number))) {
    text = std::to_string(static_cast<long long>(number));
  } else {
    text = std::to_string(number);
    while (text.size() > 1 && text.back() == '0') {
      text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  if (token.kind == Token::Kind::Percentage) {
    return text + "%";
  }
  if (token.kind == Token::Kind::Dimension) {
    return text + util::AsciiLowerCase(token.value);
  }
  return text;
}

bool IsZero(const Token& token) { return token.number == 0.0; }

// Whether a token is a length rather than a percentage -- `translate`'s third
// component must be one, and its second is dropped only when it is a zero
// *length*: `100px 0%` keeps its `0%` and `100px 0px` does not.
bool IsLengthToken(const Token& token) {
  return token.kind == Token::Kind::Dimension ||
         (token.kind == Token::Kind::Number && token.number == 0.0);
}

// `translate`, `rotate` and `scale`, each canonicalised the way the suite's
// `*-parsing-valid.html` states. Three shapes rather than one, because the rule
// for dropping a component differs in each: `translate` drops trailing zero
// lengths, `scale` drops a trailing 1 and then a y equal to x, and `rotate`
// collapses an axis vector to the keyword it is parallel to -- carrying the
// sign into the angle when it points the other way.
bool CanonicaliseIndividualTransform(const std::string& name, const std::vector<Token>& parts,
                                     std::string* out) {
  const auto write = [out](std::string text) {
    if (out != nullptr) {
      *out = std::move(text);
    }
    return true;
  };
  if (name == "translate") {
    if (parts.size() > 3) {
      return false;
    }
    std::size_t count = parts.size();
    // A trailing zero length disappears; a trailing zero *percentage* does not.
    while (count > 1 && IsZero(parts[count - 1]) && IsLengthToken(parts[count - 1])) {
      --count;
    }
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
      if (i != 0) {
        text += ' ';
      }
      // A bare `0` is a length and serializes with its unit.
      text += parts[i].kind == Token::Kind::Number && parts[i].number == 0.0
                  ? "0px"
                  : ComponentText(parts[i]);
    }
    return write(std::move(text));
  }
  if (name == "scale") {
    if (parts.size() > 3) {
      return false;
    }
    std::vector<std::string> numbers;
    for (const Token& token : parts) {
      if (token.kind == Token::Kind::Percentage) {
        Token as_number = token;
        as_number.kind = Token::Kind::Number;
        as_number.number = token.number / 100.0;
        numbers.push_back(ComponentText(as_number));
      } else if (token.kind == Token::Kind::Number) {
        numbers.push_back(ComponentText(token));
      } else {
        return false;
      }
    }
    if (numbers.size() == 3 && numbers[2] == "1") {
      numbers.pop_back();
    }
    if (numbers.size() == 2 && numbers[1] == numbers[0]) {
      numbers.pop_back();
    }
    std::string text;
    for (std::size_t i = 0; i < numbers.size(); ++i) {
      if (i != 0) {
        text += ' ';
      }
      text += numbers[i];
    }
    return write(std::move(text));
  }
  // rotate. The angle may come before or after the axis, and there is exactly
  // one of it.
  std::size_t angle = parts.size();
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (parts[i].kind == Token::Kind::Dimension ||
        (parts[i].kind == Token::Kind::Number && parts[i].number == 0.0 && parts.size() == 1)) {
      if (angle != parts.size()) {
        return false;
      }
      angle = i;
    }
  }
  if (angle == parts.size()) {
    return false;
  }
  std::vector<const Token*> axis;
  std::string keyword;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i == angle) {
      continue;
    }
    if (parts[i].kind == Token::Kind::Ident) {
      const std::string word = util::AsciiLowerCase(parts[i].value);
      if (!keyword.empty() || !axis.empty() || (word != "x" && word != "y" && word != "z")) {
        return false;
      }
      keyword = word;
      continue;
    }
    if (parts[i].kind != Token::Kind::Number || !keyword.empty()) {
      return false;
    }
    axis.push_back(&parts[i]);
  }
  if (!axis.empty() && axis.size() != 3) {
    return false;
  }
  double components[3] = {0.0, 0.0, 1.0};
  if (!axis.empty()) {
    for (std::size_t i = 0; i < 3; ++i) {
      components[i] = axis[i]->number;
    }
  } else if (keyword == "x") {
    components[0] = 1.0;
    components[2] = 0.0;
  } else if (keyword == "y") {
    components[1] = 1.0;
    components[2] = 0.0;
  }
  // An axis parallel to a basis vector collapses to that keyword, and a
  // negative one carries its sign into the angle -- which is what makes
  // `0 0 -1 400grad` and `-400grad` the same declaration.
  const bool only_x = components[1] == 0.0 && components[2] == 0.0 && components[0] != 0.0;
  const bool only_y = components[0] == 0.0 && components[2] == 0.0 && components[1] != 0.0;
  const bool only_z = components[0] == 0.0 && components[1] == 0.0 && components[2] != 0.0;
  if (only_z) {
    return write(ComponentText(parts[angle], components[2] < 0.0));
  }
  if (only_x) {
    return write("x " + ComponentText(parts[angle], components[0] < 0.0));
  }
  if (only_y) {
    return write("y " + ComponentText(parts[angle], components[1] < 0.0));
  }
  std::string text;
  for (std::size_t i = 0; i < 3; ++i) {
    Token number;
    number.kind = Token::Kind::Number;
    number.number = components[i];
    text += ComponentText(number) + " ";
  }
  return write(text + ComponentText(parts[angle]));
}

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

  // CSS Transforms 2's individual transform properties. Canonicalised on the
  // *tokens* rather than by round-tripping through a parsed value, because the
  // specified value keeps the unit the page wrote -- `400grad` reads back as
  // `400grad`, not as its degree equivalent -- and a value that went through
  // radians and back would not.
  if (name == "translate" || name == "rotate" || name == "scale") {
    if (lowered == "none") {
      if (out != nullptr) {
        *out = "none";
      }
      return DeclarationValidity::Canonical;
    }
    std::vector<Token> parts;
    for (const Token& token : Tokenize(trimmed)) {
      if (token.kind != Token::Kind::Whitespace && token.kind != Token::Kind::EndOfFile) {
        parts.push_back(token);
      }
    }
    if (parts.empty()) {
      return DeclarationValidity::Invalid;
    }
    if (!CanonicaliseIndividualTransform(name, parts, out)) {
      return DeclarationValidity::Invalid;
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

  if (name == "unicode-range") {
    if (!ParseUnicodeRangeList(trimmed, out)) {
      return DeclarationValidity::Invalid;
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

namespace {

bool ExpandCustomTokens(std::string_view value, const ComputedStyle& style, int depth,
                        std::vector<Token>& out) {
  if (depth > 32) {
    return false;
  }
  const std::vector<Token> tokens = Tokenize(value);
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token& token = tokens[i];
    if (token.kind == Token::Kind::EndOfFile) {
      break;
    }
    if (token.kind == Token::Kind::Function && util::AsciiLowerCase(token.value) == "var") {
      int paren = 1;
      std::size_t close = i + 1;
      std::size_t comma = tokens.size();
      while (close < tokens.size() && paren > 0) {
        if (tokens[close].kind == Token::Kind::Function ||
            tokens[close].kind == Token::Kind::LeftParen) {
          ++paren;
        } else if (tokens[close].kind == Token::Kind::RightParen) {
          --paren;
        } else if (tokens[close].kind == Token::Kind::Comma && paren == 1 &&
                   comma == tokens.size()) {
          comma = close;
        }
        if (paren > 0) {
          ++close;
        }
      }
      if (paren != 0) {
        return false;
      }
      std::size_t name_at = i + 1;
      while (name_at < close && tokens[name_at].kind == Token::Kind::Whitespace) {
        ++name_at;
      }
      if (name_at >= close || tokens[name_at].kind != Token::Kind::Ident) {
        return false;
      }
      if (const std::string* found = style.CustomProperty(tokens[name_at].value)) {
        if (!ExpandCustomTokens(*found, style, depth + 1, out)) {
          return false;
        }
      } else if (comma < tokens.size()) {
        const std::string fallback = ReconstructTokens(tokens, comma + 1, close);
        if (!ExpandCustomTokens(fallback, style, depth + 1, out)) {
          return false;
        }
      } else {
        return false;
      }
      i = close;
      continue;
    }
    out.push_back(token);
  }
  return true;
}

}  // namespace

std::string ComputedCustomProperty(const ComputedStyle& style, std::string_view name) {
  const std::string* specified = style.CustomProperty(name);
  if (specified == nullptr) {
    return {};
  }
  const std::string lowered = util::AsciiLowerCase(*specified);
  if (lowered.find("var(") == std::string::npos) {
    return std::string(Trim(*specified));
  }
  std::vector<Token> tokens;
  if (!ExpandCustomTokens(*specified, style, 0, tokens)) {
    return {};
  }
  return ReconstructTokens(tokens, 0, tokens.size(), true);
}

}  // namespace microbrowser::css
