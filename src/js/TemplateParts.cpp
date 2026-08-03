#include "js/TemplateParts.h"

namespace microbrowser::js {

namespace {

bool IsLineTerminator(char c) { return c == '\n' || c == '\r'; }
bool IsDecimalDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHexDigit(char c) {
  return IsDecimalDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

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

}  // namespace

bool DecodeEscape(std::string_view source, std::size_t& at, std::string& out, std::size_t& lines) {
  if (at >= source.size()) {
    return false;
  }
  const char escape = source[at++];
  switch (escape) {
    case 'n': out.push_back('\n'); return true;
    case 't': out.push_back('\t'); return true;
    case 'r': out.push_back('\r'); return true;
    case 'b': out.push_back('\b'); return true;
    case 'f': out.push_back('\f'); return true;
    case 'v': out.push_back('\v'); return true;
    case '0':
      // `\0` is a NUL only when no digit follows; `\01` is a legacy octal
      // escape, which is a syntax error in strict mode and is refused here.
      if (at < source.size() && IsDecimalDigit(source[at])) {
        return false;
      }
      out.push_back('\0');
      return true;
    case 'x': {
      if (at + 1 >= source.size() || !IsHexDigit(source[at]) || !IsHexDigit(source[at + 1])) {
        return false;
      }
      const int high = HexValue(source[at]);
      const int low = HexValue(source[at + 1]);
      at += 2;
      AppendUtf8(out, static_cast<char32_t>(high * 16 + low));
      return true;
    }
    case 'u': {
      char32_t codepoint = 0;
      if (at < source.size() && source[at] == '{') {
        ++at;
        std::size_t seen = 0;
        while (at < source.size() && IsHexDigit(source[at])) {
          codepoint = codepoint * 16 + static_cast<char32_t>(HexValue(source[at]));
          if (codepoint > 0x10FFFF) {
            return false;
          }
          ++at;
          ++seen;
        }
        if (seen == 0 || at >= source.size() || source[at] != '}') {
          return false;
        }
        ++at;
      } else {
        if (at + 3 >= source.size()) {
          return false;
        }
        for (std::size_t i = 0; i < 4; ++i) {
          if (!IsHexDigit(source[at + i])) {
            return false;
          }
          codepoint = codepoint * 16 + static_cast<char32_t>(HexValue(source[at + i]));
        }
        at += 4;
      }
      AppendUtf8(out, codepoint);
      return true;
    }
    default:
      if (IsLineTerminator(escape)) {
        // A line continuation contributes nothing to the value.
        if (escape == '\r' && at < source.size() && source[at] == '\n') {
          ++at;
        }
        ++lines;
        return true;
      }
      // An unrecognised escape is the character itself: `\q` is q.
      out.push_back(escape);
      return true;
  }
}

TemplateParts SplitTemplate(std::string_view raw) {
  TemplateParts parts;
  std::string literal;
  // The backticks are part of the token. An unterminated template never
  // reaches here -- the lexer refuses it -- but the bounds are written so that
  // one would produce a short last chunk rather than read past the end.
  const std::size_t end = raw.size() >= 2 ? raw.size() - 1 : raw.size();
  std::size_t at = raw.empty() ? 0 : 1;

  while (at < end) {
    if (raw[at] == '\\') {
      ++at;
      std::size_t lines = 0;
      const std::size_t before = at;
      if (!DecodeEscape(raw, at, literal, lines)) {
        // The lexer already accepted this token, so a malformed escape here is
        // not a second chance to reject it. Emit the character and carry on --
        // the alternative is a template that lexes and then vanishes.
        at = before;
        if (at < end) {
          literal.push_back(raw[at++]);
        }
      }
      continue;
    }
    if (raw[at] != '$' || at + 1 >= end || raw[at + 1] != '{') {
      literal.push_back(raw[at++]);
      continue;
    }

    // A substitution. Its extent is the matching close brace, counted so that
    // an object literal or a nested template inside it does not end it early.
    std::size_t depth = 1;
    const std::size_t begin = at + 2;
    std::size_t scan = begin;
    for (; scan < end && depth > 0; ++scan) {
      if (raw[scan] == '\\') {
        ++scan;  // an escaped brace is not a brace
      } else if (raw[scan] == '{') {
        ++depth;
      } else if (raw[scan] == '}') {
        --depth;
      }
    }
    if (depth != 0) {
      // Unbalanced: the rest is literal text rather than a substitution that
      // silently swallows it.
      literal.push_back(raw[at++]);
      continue;
    }
    parts.literals.push_back(std::move(literal));
    literal.clear();
    parts.substitutions.push_back(raw.substr(begin, scan - begin - 1));
    at = scan;
  }

  parts.literals.push_back(std::move(literal));
  return parts;
}

}  // namespace microbrowser::js
