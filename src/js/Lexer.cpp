#include "js/Lexer.h"

#include "js/TemplateParts.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "util/Parse.h"
#include "util/StringUtil.h"

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

double ParseBasedInteger(std::string_view digits, int base) {
  double value = 0.0;
  for (const char c : digits) {
    value = value * static_cast<double>(base) + static_cast<double>(HexValue(c));
  }
  return value;
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

// kPunctuators grouped by first byte, so that lexing one does not walk the
// whole table.
//
// The table is ordered longest-first and that order *is* the maximal-munch
// rule, so this must not reorder it -- it only skips the entries whose first
// byte cannot match. The grouping is stable, so within a byte the entries keep
// their relative order and `>>>=` is still tried before `>>>`, `>>` and `>`.
//
// Worth the table because of where the common punctuators sit: `.`, `(`, `)`,
// `,`, `=` and `;` are the last two rows, so the linear scan cost 34 to 57
// substring comparisons each. Minified script is mostly punctuation -- 1.28
// million of youtube's 2.05 million tokens -- and this was the single hottest
// function in the parse.
struct PunctuatorIndex {
  // Indices into kPunctuators, grouped by first byte and stable within a group.
  std::array<std::uint8_t, kPunctuators.size()> order{};
  std::array<std::uint8_t, 128> begin{};
  std::array<std::uint8_t, 128> count{};
};

constexpr PunctuatorIndex BuildPunctuatorIndex() {
  PunctuatorIndex index{};
  std::uint8_t next = 0;
  for (std::size_t lead = 0; lead < index.begin.size(); ++lead) {
    index.begin[lead] = next;
    for (std::size_t i = 0; i < kPunctuators.size(); ++i) {
      if (static_cast<unsigned char>(kPunctuators[i].front()) == lead) {
        index.order[next++] = static_cast<std::uint8_t>(i);
        ++index.count[lead];
      }
    }
  }
  return index;
}

constexpr PunctuatorIndex kPunctuatorIndex = BuildPunctuatorIndex();

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
    // Annex B §B.1.3, HTML-like comments: `<!--` is a line comment, and so is `-->` when nothing but
    // whitespace and comments precedes it on its line.
    //
    // This is not a curiosity. `<script type="text/javascript"><!--` is how a page written before 1998
    // hid its script from a browser that did not have one, and the pattern is still on a great many
    // documents -- aozora.gr.jp's front page among them, where without this the *whole* first script
    // fails with `SyntaxError: unexpected token '<'` rather than one line of it.
    //
    // **Accepted unconditionally rather than behind a script-versus-module flag**, which is the same
    // decision ParserImpl.h makes about `import`: whether a file is a module is the *host's* answer,
    // and the parser is asked what the file needs before the host has decided what the file is. The
    // cost of being wrong is narrow in one direction and wide in the other -- a module containing
    // `<!--` is a file nobody has written, and a classic script containing it is on the web today.
    if (c == '<' && Peek(1) == '!' && Peek(2) == '-' && Peek(3) == '-') {
      offset_ += 4;
      while (offset_ < source_.size() && !IsLineTerminator(source_[offset_])) {
        ++offset_;
      }
      continue;
    }
    // The closing form, and the line condition is the whole of what keeps it from breaking arithmetic:
    // `a-->b` on one line is `a-- > b` and must stay that way. `newline` is true exactly when this skip
    // crossed a terminator, and a skip always begins where the previous token ended -- so it answers
    // "is this the start of a line?" without a second cursor. At offset zero there is no previous line.
    if (c == '-' && Peek(1) == '-' && Peek(2) == '>' && (newline || offset_ == 0)) {
      offset_ += 3;
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
  // An identifier may be written with `\uXXXX` escapes, and the escaped and
  // unescaped spellings are the same name. Angular's output uses it -- the
  // `ɵ` prefix it marks its own properties with is emitted as `ɵ` --
  // so a bundle carrying any Angular-derived code fails to parse without it.
  //
  // Two passes rather than one: nearly every identifier has no escape at all,
  // and that case must stay a pointer bump per character with nothing owned.
  bool escaped = false;
  while (offset_ < source_.size()) {
    const char c = source_[offset_];
    if (IsIdentifierPart(c)) {
      ++offset_;
    } else if (c == '\\' && offset_ + 1 < source_.size() && source_[offset_ + 1] == 'u') {
      escaped = true;
      offset_ += 2;
      // The digits themselves are validated on the decoding pass; here it is
      // enough to get past them, and `IsHexDigit` is the only thing that can
      // end the escape either way.
      if (offset_ < source_.size() && source_[offset_] == '{') {
        while (offset_ < source_.size() && source_[offset_] != '}') {
          ++offset_;
        }
        offset_ += offset_ < source_.size() ? std::size_t{1} : std::size_t{0};
      } else {
        while (offset_ < source_.size() && IsHexDigit(source_[offset_])) {
          ++offset_;
        }
      }
    } else {
      break;
    }
  }
  if (is_private && offset_ == start + 1) {
    return MakeToken(TokenType::Invalid, start, newline);
  }
  Token token = MakeToken(is_private ? TokenType::PrivateIdentifier : TokenType::Identifier, start,
                          newline);
  if (escaped) {
    if (!DecodeIdentifier(token)) {
      token.type = TokenType::Invalid;
    }
    // An escaped identifier is never a keyword. The spec makes `if` an
    // early error rather than `if`; treating it as an ordinary name is the
    // lenient half of that and cannot turn one construct into another.
    return token;
  }
  if (!is_private && IsReservedWord(token.lexeme)) {
    token.type = TokenType::Keyword;
  }
  return token;
}

bool Lexer::DecodeIdentifier(Token& token) {
  std::string decoded;
  decoded.reserve(token.lexeme.size());
  const std::string_view raw = token.lexeme;
  for (std::size_t i = 0; i < raw.size();) {
    if (raw[i] != '\\') {
      decoded.push_back(raw[i++]);
      continue;
    }
    // `\` in an identifier introduces a unicode escape and nothing else: there
    // is no `\n` here the way there is in a string.
    if (i + 1 >= raw.size() || raw[i + 1] != 'u') {
      return false;
    }
    i += 2;
    std::uint32_t code = 0;
    if (i < raw.size() && raw[i] == '{') {
      ++i;
      std::size_t digits = 0;
      for (; i < raw.size() && raw[i] != '}'; ++i, ++digits) {
        if (!IsHexDigit(raw[i]) || code > 0x10FFFF) {
          return false;  // saturates rather than wraps: the input is hostile
        }
        code = code * 16 + static_cast<std::uint32_t>(HexValue(raw[i]));
      }
      if (digits == 0 || i >= raw.size() || code > 0x10FFFF) {
        return false;
      }
      ++i;  // the '}'
    } else {
      if (i + 4 > raw.size()) {
        return false;
      }
      for (std::size_t digit = 0; digit < 4; ++digit, ++i) {
        if (!IsHexDigit(raw[i])) {
          return false;
        }
        code = code * 16 + static_cast<std::uint32_t>(HexValue(raw[i]));
      }
    }
    util::AppendUtf8(decoded, code);
  }
  decoded_names_.push_back(std::move(decoded));
  token.lexeme = decoded_names_.back();
  return true;
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

  // `123n` is a bigint. The suffix is checked before the
  // number-touching-an-identifier rule below, which would otherwise reject it
  // -- `n` is an identifier character.
  bool is_big = false;
  if (offset_ < source_.size() && source_[offset_] == 'n') {
    // Only an integer may carry it: `1.5n` and `1e3n` are not bigints, and
    // saying so here is better than rounding one silently.
    const std::string_view so_far = source_.substr(start, offset_ - start);
    if (so_far.find('.') != std::string_view::npos ||
        (base == 10 && (so_far.find('e') != std::string_view::npos ||
                        so_far.find('E') != std::string_view::npos))) {
      ++offset_;
      return MakeToken(TokenType::Invalid, start, newline);
    }
    is_big = true;
    ++offset_;
  }

  // A number immediately followed by an identifier character is an error --
  // `3in` is not `3 in`. Catching it here means the parser never sees a token
  // pair that could not have been written.
  if (offset_ < source_.size() && IsIdentifierStart(source_[offset_])) {
    ++offset_;
    return MakeToken(TokenType::Invalid, start, newline);
  }

  Token token = MakeToken(is_big ? TokenType::BigIntLiteral : TokenType::NumericLiteral, start,
                          newline);
  std::string cleaned;
  cleaned.reserve(token.lexeme.size());
  for (const char c : token.lexeme) {
    if (c != '_' && c != 'n') {
      cleaned.push_back(c);
    }
  }
  if (is_big) {
    // The digits, for the parser to turn into a value. The lexer does not do
    // the arithmetic: a token carries text and a number, and a bigint is
    // neither.
    token.value = std::move(cleaned);
    return token;
  }
  if (base == 10) {
    token.number = util::ParseDouble(cleaned).value_or(std::nan(""));
  } else {
    token.number = ParseBasedInteger(std::string_view(cleaned).substr(2), base);
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
    // The escape forms live with the template splitter, because a template's
    // `\n` has to be the same newline a string's is. They were two
    // implementations and only one of them was right.
    std::size_t lines = 0;
    if (!DecodeEscape(source_, offset_, value, lines)) {
      return MakeToken(TokenType::Invalid, start, newline);
    }
    line_ += lines;
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
  const auto lead = static_cast<unsigned char>(rest.empty() ? 0 : rest.front());
  if (lead < kPunctuatorIndex.begin.size()) {
    const std::size_t from = kPunctuatorIndex.begin[lead];
    const std::size_t to = from + kPunctuatorIndex.count[lead];
    for (std::size_t i = from; i < to; ++i) {
      const std::string_view punctuator = kPunctuators[kPunctuatorIndex.order[i]];
      if (rest.size() < punctuator.size() || rest.substr(0, punctuator.size()) != punctuator) {
        continue;
      }
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
  // `ɵprov` is an identifier that begins with its escape, which is how
  // Angular's `ɵ` prefix reaches a bundle. Without the third test here the
  // backslash falls through to the punctuators and is simply invalid.
  if (IsIdentifierStart(c) || c == '#' || (c == '\\' && Peek(1) == 'u')) {
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
