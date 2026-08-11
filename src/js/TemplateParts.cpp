#include "js/TemplateParts.h"

#include <algorithm>

namespace microbrowser::js {

std::size_t ScanSubstitutionEnd(std::string_view source, std::size_t brace_at);

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

bool IsRegexFlag(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$';
}

bool CanStartRegex(std::string_view source, std::size_t slash_at) {
  while (slash_at > 0) {
    const char prev = source[slash_at - 1];
    if (prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r') {
      --slash_at;
      continue;
    }
    return prev == '(' || prev == ',' || prev == '=' || prev == ':' || prev == '!' || prev == '&' ||
           prev == '|' || prev == '?' || prev == '{' || prev == '[' || prev == '}' || prev == ';' ||
           prev == '+' || prev == '-' || prev == '*' || prev == '%' || prev == '~' || prev == '>';
  }
  return true;
}

std::size_t ScanRegexEnd(std::string_view source, std::size_t slash_at) {
  std::size_t scan = slash_at + 1;
  bool in_class = false;
  while (scan < source.size()) {
    const char c = source[scan];
    if (c == '\\') {
      scan = std::min(scan + 2, source.size());
      continue;
    }
    if (c == '[' && !in_class) {
      in_class = true;
    } else if (c == ']' && in_class) {
      in_class = false;
    } else if (c == '/' && !in_class) {
      ++scan;
      break;
    }
    ++scan;
  }
  while (scan < source.size() && IsRegexFlag(source[scan])) {
    ++scan;
  }
  return scan;
}

std::size_t ScanNestedTemplateEnd(std::string_view source, std::size_t backtick_at) {
  std::size_t scan = backtick_at + 1;
  while (scan < source.size()) {
    const char c = source[scan];
    if (c == '\\') {
      scan = std::min(scan + 2, source.size());
      continue;
    }
    if (IsLineTerminator(c)) {
      ++scan;
      continue;
    }
    if (c == '$' && scan + 1 < source.size() && source[scan + 1] == '{') {
      scan = ScanSubstitutionEnd(source, scan + 1);
      continue;
    }
    if (c == '`') {
      return scan + 1;
    }
    ++scan;
  }
  return scan;
}

}  // namespace

std::size_t ScanSubstitutionEnd(std::string_view source, std::size_t brace_at) {
  std::size_t depth = 1;
  std::size_t scan = brace_at + 1;
  while (scan < source.size() && depth > 0) {
    const char c = source[scan];
    if (c == '\\') {
      scan = std::min(scan + 2, source.size());
      continue;
    }
    if (c == '/' && CanStartRegex(source, scan)) {
      scan = ScanRegexEnd(source, scan);
      continue;
    }
    if (c == '\'' || c == '"') {
      const char quote = c;
      ++scan;
      while (scan < source.size()) {
        if (source[scan] == '\\') {
          scan = std::min(scan + 2, source.size());
          continue;
        }
        if (source[scan] == quote) {
          ++scan;
          break;
        }
        ++scan;
      }
      continue;
    }
    if (c == '`') {
      scan = ScanNestedTemplateEnd(source, scan);
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
    }
    ++scan;
  }
  return scan;
}

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
        // A surrogate *pair* written as two escapes is one code point, not two halves of one.
        // Strings here are UTF-8 indexed as UTF-16, so `"💩"` has to become U+1F4A9 at
        // the point it is read -- otherwise it is a string that compares unequal to the identical
        // `"\u{1F4A9}"`, encodes as six bytes where every other engine writes four, and reaches
        // `encodeURIComponent` as a pair of lone surrogates. Only a real pair combines; a lone
        // high surrogate stays lone, which is what a page that built one on purpose expects.
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF && at + 5 < source.size() &&
            source[at] == '\\' && source[at + 1] == 'u' && IsHexDigit(source[at + 2]) &&
            IsHexDigit(source[at + 3]) && IsHexDigit(source[at + 4]) &&
            IsHexDigit(source[at + 5])) {
          char32_t low = 0;
          for (std::size_t i = 0; i < 4; ++i) {
            low = low * 16 + static_cast<char32_t>(HexValue(source[at + 2 + i]));
          }
          if (low >= 0xDC00 && low <= 0xDFFF) {
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
            at += 6;
          }
        }
      }
      AppendUtf8(out, codepoint);
      return true;
    }
    default:
      if (IsLineTerminator(escape)) {
        if (escape == '\r' && at < source.size() && source[at] == '\n') {
          ++at;
        }
        ++lines;
        return true;
      }
      out.push_back(escape);
      return true;
  }
}

TemplateParts SplitTemplate(std::string_view raw) {
  TemplateParts parts;
  std::string literal;
  std::string raw_literal;
  const std::size_t end = raw.size() >= 2 ? raw.size() - 1 : raw.size();
  std::size_t at = raw.empty() ? 0 : 1;

  while (at < end) {
    if (raw[at] == '\\') {
      const std::size_t escape_start = at;
      ++at;
      std::size_t lines = 0;
      const std::size_t before = at;
      if (!DecodeEscape(raw, at, literal, lines)) {
        at = before;
        if (at < end) {
          literal.push_back(raw[at++]);
        }
      }
      raw_literal.append(raw.substr(escape_start, at - escape_start));
      continue;
    }
    if (raw[at] != '$' || at + 1 >= end || raw[at + 1] != '{') {
      raw_literal.push_back(raw[at]);
      literal.push_back(raw[at++]);
      continue;
    }

    const std::size_t begin = at + 2;
    const std::size_t scan = ScanSubstitutionEnd(raw, at + 1);
    if (scan <= begin || scan > end || raw[scan - 1] != '}') {
      raw_literal.push_back(raw[at]);
      literal.push_back(raw[at++]);
      continue;
    }
    parts.literals.push_back(std::move(literal));
    parts.raws.push_back(std::move(raw_literal));
    literal.clear();
    raw_literal.clear();
    parts.substitutions.push_back(raw.substr(begin, scan - begin - 1));
    at = scan;
  }

  parts.literals.push_back(std::move(literal));
  parts.raws.push_back(std::move(raw_literal));
  return parts;
}

}  // namespace microbrowser::js
