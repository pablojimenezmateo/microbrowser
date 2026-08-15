#include "css/Tokenizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "css/CssText.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::css {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

bool IsWhitespace(char c) {
  return c == '\n' || c == '\t' || c == ' ' || c == '\r' || c == '\f';
}

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

bool IsHexDigit(char c) {
  return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int HexValue(char c) {
  if (IsDigit(c)) {
    return c - '0';
  }
  return (c >= 'a' ? c - 'a' : c - 'A') + 10;
}

bool IsLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Everything above 0x7F counts as a name code point, which is how a stylesheet
// gets to use non-ASCII identifiers without the tokenizer decoding UTF-8.
bool IsNameStart(char c) {
  return IsLetter(c) || c == '_' || static_cast<unsigned char>(c) > 0x7F;
}

bool IsNameChar(char c) {
  return IsNameStart(c) || IsDigit(c) || c == '-';
}

bool IsValidEscape(std::string_view input) {
  return input.size() >= 2 && input[0] == '\\' && input[1] != '\n';
}

void AppendUtf8(std::uint32_t code_point, std::string& out) {
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

class Scanner {
 public:
  explicit Scanner(std::string_view input) : input_(input) {}

  std::vector<Token> Run();

 private:
  char At(std::size_t ahead = 0) const {
    return position_ + ahead < input_.size() ? input_[position_ + ahead] : '\0';
  }
  bool AtEnd() const { return position_ >= input_.size(); }
  std::string_view Rest(std::size_t ahead = 0) const {
    return position_ + ahead < input_.size() ? input_.substr(position_ + ahead) : std::string_view();
  }

  std::string ConsumeName();
  Token ConsumeNumeric();
  Token ConsumeIdentLike();
  bool ConsumeUnicodeRange(Token& token);
  Token ConsumeString(char quote);
  Token ConsumeUrl();
  void ConsumeComment();

  std::string_view input_;
  std::size_t position_ = 0;
};

std::string Scanner::ConsumeName() {
  std::string name;
  while (!AtEnd()) {
    if (IsNameChar(At())) {
      name.push_back(input_[position_++]);
    } else if (IsValidEscape(Rest())) {
      position_ += ConsumeEscape(Rest(), name);
    } else {
      break;
    }
  }
  return name;
}

Token Scanner::ConsumeNumeric() {
  Token token;
  std::string text;
  bool integer = true;

  if (At() == '+' || At() == '-') {
    token.has_sign = true;
    text.push_back(input_[position_++]);
  }
  while (IsDigit(At())) {
    text.push_back(input_[position_++]);
  }
  if (At() == '.' && IsDigit(At(1))) {
    integer = false;
    text.push_back(input_[position_++]);
    while (IsDigit(At())) {
      text.push_back(input_[position_++]);
    }
  }
  if ((At() == 'e' || At() == 'E') &&
      (IsDigit(At(1)) || ((At(1) == '+' || At(1) == '-') && IsDigit(At(2))))) {
    integer = false;
    text.push_back(input_[position_++]);
    if (At() == '+' || At() == '-') {
      text.push_back(input_[position_++]);
    }
    while (IsDigit(At())) {
      text.push_back(input_[position_++]);
    }
  }

  // std::strtod reads the decimal separator from the process locale, which SDL
  // changes behind our back — the repo bans it for exactly that reason. Parsed
  // by hand instead, which is also how the value stays exactly what the
  // stylesheet said.
  double value = 0.0;
  {
    std::size_t at = 0;
    double sign = 1.0;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
      sign = text[at] == '-' ? -1.0 : 1.0;
      ++at;
    }
    while (at < text.size() && IsDigit(text[at])) {
      value = value * 10.0 + static_cast<double>(text[at++] - '0');
    }
    if (at < text.size() && text[at] == '.') {
      ++at;
      double scale = 0.1;
      while (at < text.size() && IsDigit(text[at])) {
        value += static_cast<double>(text[at++] - '0') * scale;
        scale *= 0.1;
      }
    }
    if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
      ++at;
      double exponent_sign = 1.0;
      if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
        exponent_sign = text[at] == '-' ? -1.0 : 1.0;
        ++at;
      }
      double exponent = 0.0;
      while (at < text.size() && IsDigit(text[at])) {
        exponent = exponent * 10.0 + static_cast<double>(text[at++] - '0');
      }
      // Bounded: a stylesheet saying `1e999999` must not spin computing an
      // infinity, and must not produce one either.
      exponent = std::min(exponent, 300.0);
      value *= std::pow(10.0, exponent_sign * exponent);
    }
    value *= sign;
  }

  token.number = value;
  token.is_integer = integer;

  if (WouldStartIdentifier(Rest())) {
    token.kind = Token::Kind::Dimension;
    token.value = ConsumeName();
  } else if (At() == '%') {
    ++position_;
    token.kind = Token::Kind::Percentage;
  } else {
    token.kind = Token::Kind::Number;
  }
  return token;
}

Token Scanner::ConsumeString(char quote) {
  Token token;
  token.kind = Token::Kind::String;
  ++position_;  // the opening quote
  while (!AtEnd()) {
    const char c = input_[position_];
    if (c == quote) {
      ++position_;
      return token;
    }
    if (c == '\n') {
      // A newline in a string is a parse error and the token is a bad-string.
      // The parser discards the declaration it was in, which is the spec's
      // recovery rather than ours.
      token.kind = Token::Kind::BadString;
      return token;
    }
    if (c == '\\') {
      if (position_ + 1 >= input_.size()) {
        ++position_;
        return token;
      }
      if (input_[position_ + 1] == '\n') {
        position_ += 2;  // an escaped newline continues the string
        continue;
      }
      position_ += ConsumeEscape(Rest(), token.value);
      continue;
    }
    token.value.push_back(input_[position_++]);
  }
  return token;
}

Token Scanner::ConsumeUrl() {
  Token token;
  token.kind = Token::Kind::Url;
  while (IsWhitespace(At())) {
    ++position_;
  }
  while (!AtEnd()) {
    const char c = input_[position_];
    if (c == ')') {
      ++position_;
      return token;
    }
    if (IsWhitespace(c)) {
      while (IsWhitespace(At())) {
        ++position_;
      }
      if (At() == ')') {
        ++position_;
        return token;
      }
      token.kind = Token::Kind::BadUrl;
      break;
    }
    if (c == '"' || c == '\'' || c == '(') {
      token.kind = Token::Kind::BadUrl;
      break;
    }
    if (c == '\\') {
      if (IsValidEscape(Rest())) {
        position_ += ConsumeEscape(Rest(), token.value);
        continue;
      }
      token.kind = Token::Kind::BadUrl;
      break;
    }
    token.value.push_back(input_[position_++]);
  }
  // Bad url: consume to the closing paren so the parser resumes in a known
  // place rather than treating the rest of the sheet as a URL.
  while (!AtEnd() && input_[position_] != ')') {
    ++position_;
  }
  if (!AtEnd()) {
    ++position_;
  }
  return token;
}

// `u+`, and what follows it. CSS Values 4's <urange>: one to six hex digits, or
// hex digits followed by `?` wildcards, optionally a `-` and a second run of hex
// digits. The wildcard form is a range in disguise -- `U+4??` is U+400 to U+4FF --
// and expanding it here means nothing downstream has to know about `?`.
//
// Returns false and consumes nothing when what follows is not a range, because
// `u` is also a perfectly good identifier and `u + 1` is a perfectly good
// declaration value.
bool Scanner::ConsumeUnicodeRange(Token& token) {
  const std::size_t start = position_;
  ++position_;  // the u/U
  ++position_;  // the +
  std::string digits;
  std::size_t wildcards = 0;
  while (digits.size() + wildcards < 6) {
    const char c = At();
    if (IsHexDigit(c) && wildcards == 0) {
      digits.push_back(c);
    } else if (c == '?') {
      ++wildcards;
    } else {
      break;
    }
    ++position_;
  }
  if (digits.empty() && wildcards == 0) {
    position_ = start;
    return false;
  }
  const auto hex = [](const std::string& text, char fill, std::size_t pad) {
    std::string padded = text;
    padded.append(pad, fill);
    std::uint32_t value = 0;
    for (const char c : padded) {
      value = (value << 4) | static_cast<std::uint32_t>(HexValue(c));
    }
    return value;
  };
  token.kind = Token::Kind::UnicodeRange;
  token.range_start = hex(digits, '0', wildcards);
  token.range_end = hex(digits, 'F', wildcards);
  if (wildcards > 0) {
    return true;  // a wildcard form takes no second endpoint
  }
  if (At() != '-' || !IsHexDigit(At(1))) {
    return true;  // a single code point: the range is itself
  }
  ++position_;
  std::string second;
  while (second.size() < 6 && IsHexDigit(At())) {
    second.push_back(At());
    ++position_;
  }
  token.range_end = hex(second, '0', 0);
  // A range whose end is below its start is dropped rather than swapped: the
  // author wrote something they did not mean, and swapping it would silently
  // fetch a subset the page never asked for.
  if (token.range_end < token.range_start) {
    token.range_end = token.range_start;
  }
  return true;
}

Token Scanner::ConsumeIdentLike() {
  const std::string name = ConsumeName();
  // `url(` is a token of its own, but `url("x")` is a function whose argument
  // is a string — the difference is whether a quote follows.
  if (name.size() == 3 && (name[0] == 'u' || name[0] == 'U') &&
      (name[1] == 'r' || name[1] == 'R') && (name[2] == 'l' || name[2] == 'L') && At() == '(') {
    std::size_t ahead = 1;
    while (IsWhitespace(At(ahead))) {
      ++ahead;
    }
    if (At(ahead) != '"' && At(ahead) != '\'') {
      ++position_;
      return ConsumeUrl();
    }
  }
  Token token;
  if (At() == '(') {
    ++position_;
    token.kind = Token::Kind::Function;
  } else {
    token.kind = Token::Kind::Ident;
  }
  token.value = name;
  return token;
}

void Scanner::ConsumeComment() {
  position_ += 2;
  while (position_ + 1 < input_.size()) {
    if (input_[position_] == '*' && input_[position_ + 1] == '/') {
      position_ += 2;
      return;
    }
    ++position_;
  }
  // An unterminated comment swallows the rest of the sheet, which is what the
  // spec says and what every browser does.
  position_ = input_.size();
}

std::vector<Token> Scanner::Run() {
  std::vector<Token> tokens;
  while (true) {
    if (AtEnd()) {
      Token eof;
      eof.kind = Token::Kind::EndOfFile;
      tokens.push_back(eof);
      break;
    }

    const char c = input_[position_];
    if (c == '/' && At(1) == '*') {
      ConsumeComment();
      continue;
    }
    if (IsWhitespace(c)) {
      while (IsWhitespace(At())) {
        ++position_;
      }
      Token token;
      token.kind = Token::Kind::Whitespace;
      tokens.push_back(token);
      continue;
    }

    Token token;
    switch (c) {
      case '"':
      case '\'':
        tokens.push_back(ConsumeString(c));
        continue;
      case '#': {
        if (IsNameChar(At(1)) || IsValidEscape(Rest(1))) {
          ++position_;
          token.kind = Token::Kind::Hash;
          // Whether it could be an id decides whether `#1x` parses as a
          // selector. A hash that is not an id is still a valid colour.
          token.hash_is_id = WouldStartIdentifier(Rest());
          token.value = ConsumeName();
          tokens.push_back(token);
          continue;
        }
        break;
      }
      case '(':
        token.kind = Token::Kind::LeftParen;
        break;
      case ')':
        token.kind = Token::Kind::RightParen;
        break;
      case '[':
        token.kind = Token::Kind::LeftSquare;
        break;
      case ']':
        token.kind = Token::Kind::RightSquare;
        break;
      case '{':
        token.kind = Token::Kind::LeftBrace;
        break;
      case '}':
        token.kind = Token::Kind::RightBrace;
        break;
      case ',':
        token.kind = Token::Kind::Comma;
        break;
      case ':':
        token.kind = Token::Kind::Colon;
        break;
      case ';':
        token.kind = Token::Kind::Semicolon;
        break;
      case '@': {
        if (WouldStartIdentifier(Rest(1))) {
          ++position_;
          token.kind = Token::Kind::AtKeyword;
          token.value = ConsumeName();
          tokens.push_back(token);
          continue;
        }
        break;
      }
      case '<': {
        if (Rest().rfind("<!--", 0) == 0) {
          position_ += 4;
          token.kind = Token::Kind::Cdo;
          tokens.push_back(token);
          continue;
        }
        break;
      }
      case '-': {
        if (WouldStartNumber(Rest())) {
          tokens.push_back(ConsumeNumeric());
          continue;
        }
        if (Rest().rfind("-->", 0) == 0) {
          position_ += 3;
          token.kind = Token::Kind::Cdc;
          tokens.push_back(token);
          continue;
        }
        if (WouldStartIdentifier(Rest())) {
          tokens.push_back(ConsumeIdentLike());
          continue;
        }
        break;
      }
      case '+':
      case '.': {
        if (WouldStartNumber(Rest())) {
          tokens.push_back(ConsumeNumeric());
          continue;
        }
        break;
      }
      case '\\': {
        if (IsValidEscape(Rest())) {
          tokens.push_back(ConsumeIdentLike());
          continue;
        }
        break;
      }
      default:
        break;
    }

    if (IsDigit(c)) {
      tokens.push_back(ConsumeNumeric());
      continue;
    }
    if (IsNameStart(c)) {
      if ((c == 'u' || c == 'U') && At(1) == '+') {
        Token range;
        if (ConsumeUnicodeRange(range)) {
          tokens.push_back(range);
          continue;
        }
      }
      tokens.push_back(ConsumeIdentLike());
      continue;
    }
    if (token.kind != Token::Kind::EndOfFile) {
      ++position_;
      tokens.push_back(token);
      continue;
    }

    token.kind = Token::Kind::Delim;
    token.value.assign(1, c);
    ++position_;
    tokens.push_back(token);
  }
  return tokens;
}

}  // namespace

std::size_t ConsumeEscape(std::string_view input, std::string& out) {
  if (input.size() < 2 || input[0] != '\\') {
    return input.empty() ? 0 : 1;
  }
  if (!IsHexDigit(input[1])) {
    out.push_back(input[1]);
    return 2;
  }

  std::uint32_t code_point = 0;
  std::size_t consumed = 1;
  int digits = 0;
  while (consumed < input.size() && digits < 6 && IsHexDigit(input[consumed])) {
    const char c = input[consumed];
    const std::uint32_t value = IsDigit(c) ? static_cast<std::uint32_t>(c - '0')
                                           : static_cast<std::uint32_t>(
                                                 (c | 0x20) - 'a' + 10);
    code_point = code_point * 16 + value;
    ++consumed;
    ++digits;
  }
  // One whitespace character after the digits is part of the escape, not
  // content: `\31 23` is `1` then `23`, not the code point 0x3123.
  if (consumed < input.size() && IsWhitespace(input[consumed])) {
    ++consumed;
  }
  if (code_point == 0 || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
    code_point = 0xFFFD;
  }
  AppendUtf8(code_point, out);
  return consumed;
}

bool WouldStartIdentifier(std::string_view input) {
  if (input.empty()) {
    return false;
  }
  if (input[0] == '-') {
    if (input.size() < 2) {
      return false;
    }
    // `--custom` is an identifier. Without this every custom property is
    // silently lost.
    return IsNameStart(input[1]) || input[1] == '-' || IsValidEscape(input.substr(1));
  }
  if (IsNameStart(input[0])) {
    return true;
  }
  return IsValidEscape(input);
}

bool WouldStartNumber(std::string_view input) {
  if (input.empty()) {
    return false;
  }
  if (input[0] == '+' || input[0] == '-') {
    if (input.size() >= 2 && IsDigit(input[1])) {
      return true;
    }
    return input.size() >= 3 && input[1] == '.' && IsDigit(input[2]);
  }
  if (input[0] == '.') {
    return input.size() >= 2 && IsDigit(input[1]);
  }
  return IsDigit(input[0]);
}

std::vector<Token> Tokenize(std::string_view input) {
  Scanner scanner(input);
  std::vector<Token> tokens = scanner.Run();
  AddPerformanceCounter(PerfCounterId::CssTokens, tokens.size());
  return tokens;
}

std::string ReconstructTokens(const std::vector<Token>& tokens, std::size_t from, std::size_t to) {
  std::string out;
  for (std::size_t i = from; i < to && i < tokens.size(); ++i) {
    const Token& token = tokens[i];
    switch (token.kind) {
      case Token::Kind::Whitespace:
        if (!out.empty()) {
          out.push_back(' ');
        }
        break;
      case Token::Kind::Ident:
        out += token.value;
        break;
      case Token::Kind::Url:
        out += "url(\"";
        out += token.value;
        out += "\")";
        break;
      case Token::Kind::Function:
        out += token.value;
        out.push_back('(');
        break;
      case Token::Kind::Hash:
        out.push_back('#');
        out += token.value;
        break;
      case Token::Kind::String:
        out.push_back('"');
        out += token.value;
        out.push_back('"');
        break;
      case Token::Kind::AtKeyword:
        out.push_back('@');
        out += token.value;
        break;
      case Token::Kind::Number:
      case Token::Kind::Percentage:
      case Token::Kind::Dimension: {
        if (token.is_integer) {
          out += std::to_string(static_cast<long long>(token.number));
        } else {
          std::string text = std::to_string(token.number);
          while (text.size() > 1 && text.back() == '0') {
            text.pop_back();
          }
          if (!text.empty() && text.back() == '.') {
            text.pop_back();
          }
          out += text;
        }
        if (token.kind == Token::Kind::Percentage) {
          out.push_back('%');
        } else if (token.kind == Token::Kind::Dimension) {
          out += token.value;
        }
        break;
      }
      case Token::Kind::UnicodeRange: {
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "U+%X-%X", token.range_start, token.range_end);
        out += buffer;
        break;
      }
      case Token::Kind::Delim:
        out += token.value;
        break;
      case Token::Kind::Colon:
        out.push_back(':');
        break;
      case Token::Kind::Comma:
        out.push_back(',');
        break;
      case Token::Kind::Semicolon:
        out.push_back(';');
        break;
      case Token::Kind::LeftParen:
        out.push_back('(');
        break;
      case Token::Kind::RightParen:
        out.push_back(')');
        break;
      case Token::Kind::LeftSquare:
        out.push_back('[');
        break;
      case Token::Kind::RightSquare:
        out.push_back(']');
        break;
      case Token::Kind::LeftBrace:
        out.push_back('{');
        break;
      case Token::Kind::RightBrace:
        out.push_back('}');
        break;
      default:
        break;
    }
  }
  return std::string(Trim(out));
}

}  // namespace microbrowser::css
