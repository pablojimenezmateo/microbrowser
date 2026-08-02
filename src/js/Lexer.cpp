#include "js/Lexer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

#include "util/Parse.h"

namespace microbrowser::js {

namespace {

// The reserved words, sorted so the lookup is a binary search. Contextual
// keywords are absent on purpose -- see Token.h.
constexpr std::array<std::string_view, 36> kReservedWords = {
    "await",   "break",  "case",     "catch",  "class",  "const",      "continue", "debugger",
    "default", "delete", "do",       "else",   "enum",   "export",     "extends",  "false",
    "finally", "for",    "function", "if",     "import", "in",         "instanceof", "let",
    "new",     "null",   "return",   "super",  "switch", "this",       "throw",    "true",
    "try",     "typeof", "var",      "void",
};

// Not in the sorted table above because they would break its ordering if
// appended carelessly; kept separate and checked explicitly.
constexpr std::array<std::string_view, 3> kMoreReservedWords = {"while", "with", "yield"};

bool IsLineTerminator(char c) {
  // U+2028 and U+2029 are line terminators too. They are multi-byte in UTF-8
  // and are not handled here; the parser sees them as identifier characters,
  // which is wrong for ASI and right for everything else. Handling them needs
  // the lexer to decode UTF-8, which it does not yet do.
  return c == '\n' || c == '\r';
}

bool IsWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\v' || c == '\f' ||
         static_cast<unsigned char>(c) == 0xA0;
}

bool IsDecimalDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHexDigit(char c) {
  return IsDecimalDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool IsOctalDigit(char c) { return c >= '0' && c <= '7'; }
bool IsBinaryDigit(char c) { return c == '0' || c == '1'; }

// The ASCII subset of IdentifierStart, plus the bytes that begin a multi-byte
// UTF-8 sequence. Treating every non-ASCII byte as an identifier character is
// wrong for the few code points that are not ID_Start, and right for the vast
// majority; the alternative is a Unicode property table, which is a dependency
// and a decision of its own.
bool IsIdentifierStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
         static_cast<unsigned char>(c) >= 0x80;
}

bool IsIdentifierPart(char c) { return IsIdentifierStart(c) || IsDecimalDigit(c); }

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return c - 'A' + 10;
}

void AppendUtf8(std::string& out, char32_t codepoint) {
  if (codepoint < 0x80) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

// Punctuators, longest first. Order is the whole algorithm: matching `>` before
// `>>>=` would lex four tokens where there is one.
// `??=` is spelled with an escape because `??=` in a C++ source file is a
// trigraph for `#`, and a compiler that warns about it is right to.
constexpr std::string_view kNullishAssign = "?\?=";
constexpr std::string_view kNullish = "?\?";

constexpr std::array<std::string_view, 57> kPunctuators = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>",
    "&&=", "||=", kNullishAssign, "=>", "==", "!=", "<=", ">=",
    "&&", "||", kNullish, "?.", "++", "--", "+=", "-=",
    "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>",
    "**", "{", "}", "(", ")", "[", "]", ";",
    ",", "<", ">", "+", "-", "*", "/", "%",
    "&", "|", "^", "!", "~", ":", "?", ".",
    "=",
};

}  // namespace

bool IsReservedWord(std::string_view text) {
  return std::binary_search(kReservedWords.begin(), kReservedWords.end(), text) ||
         std::find(kMoreReservedWords.begin(), kMoreReservedWords.end(), text) !=
             kMoreReservedWords.end();
}

void Lexer::SeekTo(std::size_t offset, std::size_t line) {
  offset_ = std::min(offset, source_.size());
  line_ = line;
}

bool Lexer::SkipWhitespaceAndComments() {
  bool newline = false;
  while (offset_ < source_.size()) {
    const char c = source_[offset_];
    if (IsLineTerminator(c)) {
      newline = true;
      // CRLF is one terminator, not two, or every line number is doubled on
      // Windows-authored source.
      if (c == '\r' && Peek(1) == '\n') {
        ++offset_;
      }
      ++line_;
      ++offset_;
      continue;
    }
    if (IsWhitespace(c)) {
      ++offset_;
      continue;
    }
    if (c == '/' && Peek(1) == '/') {
      offset_ += 2;
      while (offset_ < source_.size() && !IsLineTerminator(source_[offset_])) {
        ++offset_;
      }
      continue;
    }
    if (c == '/' && Peek(1) == '*') {
      offset_ += 2;
      bool closed = false;
      while (offset_ < source_.size()) {
        if (source_[offset_] == '*' && Peek(1) == '/') {
          offset_ += 2;
          closed = true;
          break;
        }
        if (IsLineTerminator(source_[offset_])) {
          // A block comment containing a line terminator counts as one for
          // ASI. This is a real rule and the reason `a = b /*
          // */ ++c` parses differently from the same source on one line.
          newline = true;
          ++line_;
        }
        ++offset_;
      }
      if (!closed) {
        // Unterminated: consume to the end rather than looping. The parser sees
        // EndOfFile and reports it.
        offset_ = source_.size();
      }
      continue;
    }
    break;
  }
  return newline;
}

Token Lexer::MakeToken(TokenType type, std::size_t start, bool newline) const {
  Token token;
  token.type = type;
  token.lexeme = source_.substr(start, offset_ - start);
  token.start = start;
  token.end = offset_;
  token.line = line_;
  token.newline_before = newline;
  return token;
}

Token Lexer::LexIdentifierOrKeyword(std::size_t start, bool newline) {
  const bool is_private = source_[offset_] == '#';
  if (is_private) {
    ++offset_;
  }
  while (offset_ < source_.size() && IsIdentifierPart(source_[offset_])) {
    ++offset_;
  }
  if (is_private && offset_ == start + 1) {
    return MakeToken(TokenType::Invalid, start, newline);
  }
  Token token = MakeToken(is_private ? TokenType::PrivateIdentifier : TokenType::Identifier, start,
                          newline);
  if (!is_private && IsReservedWord(token.lexeme)) {
    token.type = TokenType::Keyword;
  }
  return token;
}

Token Lexer::LexNumber(std::size_t start, bool newline) {
  const auto digits = [&](bool (*predicate)(char)) {
    std::size_t count = 0;
    while (offset_ < source_.size()) {
      if (source_[offset_] == '_') {
        // Numeric separators. Legal between digits only, but accepting them
        // anywhere here and letting the value conversion ignore them is the
        // same result for every program that is not already a syntax error.
        ++offset_;
        continue;
      }
      if (!predicate(source_[offset_])) {
        break;
      }
      ++offset_;
      ++count;
    }
    return count;
  };

  int base = 10;
  if (source_[offset_] == '0' && offset_ + 1 < source_.size()) {
    const char marker = source_[offset_ + 1];
    if (marker == 'x' || marker == 'X') {
      base = 16;
      offset_ += 2;
      if (digits(IsHexDigit) == 0) {
        return MakeToken(TokenType::Invalid, start, newline);
      }
    } else if (marker == 'o' || marker == 'O') {
      base = 8;
      offset_ += 2;
      if (digits(IsOctalDigit) == 0) {
        return MakeToken(TokenType::Invalid, start, newline);
      }
    } else if (marker == 'b' || marker == 'B') {
      base = 2;
      offset_ += 2;
      if (digits(IsBinaryDigit) == 0) {
        return MakeToken(TokenType::Invalid, start, newline);
      }
    }
  }

  if (base == 10) {
    digits(IsDecimalDigit);
    if (offset_ < source_.size() && source_[offset_] == '.') {
      ++offset_;
      digits(IsDecimalDigit);
    }
    if (offset_ < source_.size() && (source_[offset_] == 'e' || source_[offset_] == 'E')) {
      const std::size_t exponent_start = offset_;
      ++offset_;
      if (offset_ < source_.size() && (source_[offset_] == '+' || source_[offset_] == '-')) {
        ++offset_;
      }
      if (digits(IsDecimalDigit) == 0) {
        // `1e` and `1e+` are not numbers. Rewinding puts the `e` back, where
        // the "a number may not touch an identifier" check below turns the
        // whole thing into one Invalid token -- which is what the spec says it
        // is, and is better than two tokens that look separately fine.
        offset_ = exponent_start;
      }
    }
  }

  // A number immediately followed by an identifier character is an error --
  // `3in` is not `3 in`. Catching it here means the parser never sees a token
  // pair that could not have been written.
  if (offset_ < source_.size() && IsIdentifierStart(source_[offset_])) {
    ++offset_;
    return MakeToken(TokenType::Invalid, start, newline);
  }

  Token token = MakeToken(TokenType::NumericLiteral, start, newline);
  std::string cleaned;
  cleaned.reserve(token.lexeme.size());
  for (const char c : token.lexeme) {
    if (c != '_') {
      cleaned.push_back(c);
    }
  }
  if (base == 10) {
    token.number = util::ParseDouble(cleaned).value_or(std::nan(""));
  } else {
    token.number = static_cast<double>(std::strtoull(cleaned.c_str() + 2, nullptr, base));
  }
  return token;
}

Token Lexer::LexString(std::size_t start, bool newline) {
  const char quote = source_[offset_];
  ++offset_;
  std::string value;

  while (true) {
    if (offset_ >= source_.size()) {
      return MakeToken(TokenType::Invalid, start, newline);  // unterminated
    }
    const char c = source_[offset_];
    if (c == quote) {
      ++offset_;
      break;
    }
    if (IsLineTerminator(c)) {
      // A raw newline ends a string. Consuming it would make one unterminated
      // string swallow the rest of the file.
      return MakeToken(TokenType::Invalid, start, newline);
    }
    if (c != '\\') {
      value.push_back(c);
      ++offset_;
      continue;
    }

    ++offset_;
    if (offset_ >= source_.size()) {
      return MakeToken(TokenType::Invalid, start, newline);
    }
    const char escape = source_[offset_++];
    switch (escape) {
      case 'n': value.push_back('\n'); break;
      case 't': value.push_back('\t'); break;
      case 'r': value.push_back('\r'); break;
      case 'b': value.push_back('\b'); break;
      case 'f': value.push_back('\f'); break;
      case 'v': value.push_back('\v'); break;
      case '0':
        // `\0` is a NUL only when no digit follows; `\01` is a legacy octal
        // escape, which is a syntax error in strict mode and is refused here.
        if (offset_ < source_.size() && IsDecimalDigit(source_[offset_])) {
          return MakeToken(TokenType::Invalid, start, newline);
        }
        value.push_back('\0');
        break;
      case 'x': {
        if (offset_ + 1 >= source_.size() || !IsHexDigit(source_[offset_]) ||
            !IsHexDigit(source_[offset_ + 1])) {
          return MakeToken(TokenType::Invalid, start, newline);
        }
        const int high = HexValue(source_[offset_]);
        const int low = HexValue(source_[offset_ + 1]);
        offset_ += 2;
        AppendUtf8(value, static_cast<char32_t>(high * 16 + low));
        break;
      }
      case 'u': {
        char32_t codepoint = 0;
        if (offset_ < source_.size() && source_[offset_] == '{') {
          ++offset_;
          std::size_t seen = 0;
          while (offset_ < source_.size() && IsHexDigit(source_[offset_])) {
            codepoint = codepoint * 16 + static_cast<char32_t>(HexValue(source_[offset_]));
            if (codepoint > 0x10FFFF) {
              return MakeToken(TokenType::Invalid, start, newline);
            }
            ++offset_;
            ++seen;
          }
          if (seen == 0 || offset_ >= source_.size() || source_[offset_] != '}') {
            return MakeToken(TokenType::Invalid, start, newline);
          }
          ++offset_;
        } else {
          if (offset_ + 3 >= source_.size()) {
            return MakeToken(TokenType::Invalid, start, newline);
          }
          for (int i = 0; i < 4; ++i) {
            if (!IsHexDigit(source_[offset_ + static_cast<std::size_t>(i)])) {
              return MakeToken(TokenType::Invalid, start, newline);
            }
            codepoint = codepoint * 16 +
                        static_cast<char32_t>(HexValue(source_[offset_ + static_cast<std::size_t>(i)]));
          }
          offset_ += 4;
        }
        AppendUtf8(value, codepoint);
        break;
      }
      default:
        if (IsLineTerminator(escape)) {
          // A line continuation contributes nothing to the value.
          if (escape == '\r' && offset_ < source_.size() && source_[offset_] == '\n') {
            ++offset_;
          }
          ++line_;
          break;
        }
        value.push_back(escape);
        break;
    }
  }

  Token token = MakeToken(TokenType::StringLiteral, start, newline);
  token.value = std::move(value);
  return token;
}

Token Lexer::LexTemplate(std::size_t start, bool newline) {
  // The whole literal is one token, substitutions included and unparsed. The
  // parser re-lexes the inside of each `${}`, which keeps the nesting rules
  // (a template inside a substitution inside a template) in one place instead
  // of duplicating a brace counter here and a parser there.
  ++offset_;  // the backtick
  int depth = 0;
  while (offset_ < source_.size()) {
    const char c = source_[offset_];
    if (c == '\\') {
      // Clamped, not `+= 2`. A backslash as the last byte of the input would
      // otherwise put the offset one past the end, and every token carries its
      // end offset -- so an error message that slices the source by it reads
      // out of bounds. Found by the fuzzer.
      offset_ = std::min(offset_ + 2, source_.size());
      continue;
    }
    if (IsLineTerminator(c)) {
      ++line_;
      ++offset_;
      continue;
    }
    if (c == '$' && Peek(1) == '{') {
      ++depth;
      offset_ += 2;
      continue;
    }
    if (c == '}' && depth > 0) {
      --depth;
      ++offset_;
      continue;
    }
    if (c == '`' && depth == 0) {
      ++offset_;
      return MakeToken(TokenType::TemplateString, start, newline);
    }
    ++offset_;
  }
  return MakeToken(TokenType::Invalid, start, newline);  // unterminated
}

Token Lexer::LexPunctuator(std::size_t start, bool newline) {
  const std::string_view rest = source_.substr(offset_);
  for (const std::string_view punctuator : kPunctuators) {
    if (rest.size() >= punctuator.size() && rest.substr(0, punctuator.size()) == punctuator) {
      // `?.3` is a ternary followed by a number, not optional chaining: the
      // spec says so, because `a?.3:0` has to keep working.
      if (punctuator == "?." && rest.size() > 2 && IsDecimalDigit(rest[2])) {
        continue;
      }
      offset_ += punctuator.size();
      return MakeToken(TokenType::Punctuator, start, newline);
    }
  }
  ++offset_;
  return MakeToken(TokenType::Invalid, start, newline);
}

Token Lexer::Next() {
  const bool newline = SkipWhitespaceAndComments();
  const std::size_t start = offset_;
  if (offset_ >= source_.size()) {
    Token token = MakeToken(TokenType::EndOfFile, start, newline);
    return token;
  }

  const char c = source_[offset_];
  if (IsIdentifierStart(c) || c == '#') {
    return LexIdentifierOrKeyword(start, newline);
  }
  if (IsDecimalDigit(c) || (c == '.' && IsDecimalDigit(Peek(1)))) {
    return LexNumber(start, newline);
  }
  if (c == '"' || c == '\'') {
    return LexString(start, newline);
  }
  if (c == '`') {
    return LexTemplate(start, newline);
  }
  return LexPunctuator(start, newline);
}

Token Lexer::RescanAsRegExp(const Token& slash) {
  // Restart from the slash itself: the token that was handed back may have been
  // `/` or `/=`, and both begin a regex body.
  offset_ = slash.start;
  const std::size_t start = offset_;
  ++offset_;  // the opening slash

  bool in_class = false;
  while (true) {
    if (offset_ >= source_.size()) {
      return MakeToken(TokenType::Invalid, start, slash.newline_before);
    }
    const char c = source_[offset_];
    if (IsLineTerminator(c)) {
      // A regex may not span lines. Without this an unterminated one would eat
      // the rest of the program.
      return MakeToken(TokenType::Invalid, start, slash.newline_before);
    }
    if (c == '\\') {
      offset_ = std::min(offset_ + 2, source_.size());
      continue;
    }
    if (c == '[') {
      in_class = true;
    } else if (c == ']') {
      in_class = false;
    } else if (c == '/' && !in_class) {
      // A `/` inside a character class is a literal slash, not the end -- which
      // is why the class state is tracked at all.
      ++offset_;
      break;
    }
    ++offset_;
  }
  while (offset_ < source_.size() && IsIdentifierPart(source_[offset_])) {
    ++offset_;  // flags
  }
  return MakeToken(TokenType::RegExpLiteral, start, slash.newline_before);
}

std::vector<Token> TokenizeAll(std::string_view source) {
  std::vector<Token> tokens;
  Lexer lexer(source);
  while (true) {
    Token token = lexer.Next();
    const bool done = token.type == TokenType::EndOfFile;
    tokens.push_back(std::move(token));
    if (done) {
      break;
    }
  }
  return tokens;
}

}  // namespace microbrowser::js
