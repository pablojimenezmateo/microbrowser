#include "util/PercentEncoding.h"

namespace microbrowser::util {

namespace {

bool IsC0ControlOrAbove7E(unsigned char byte) {
  return byte <= 0x1F || byte > 0x7E;
}

bool InFragmentSet(unsigned char byte) {
  return IsC0ControlOrAbove7E(byte) || byte == ' ' || byte == '"' || byte == '<' || byte == '>' ||
         byte == '`';
}

bool InQuerySet(unsigned char byte) {
  return IsC0ControlOrAbove7E(byte) || byte == ' ' || byte == '"' || byte == '#' || byte == '<' ||
         byte == '>';
}

bool InPathSet(unsigned char byte) {
  return InQuerySet(byte) || byte == '?' || byte == '`' || byte == '{' || byte == '}';
}

bool InUserinfoSet(unsigned char byte) {
  return InPathSet(byte) || byte == '/' || byte == ':' || byte == ';' || byte == '=' ||
         byte == '@' || (byte >= '[' && byte <= '^') || byte == '|';
}

bool InComponentSet(unsigned char byte) {
  return InUserinfoSet(byte) || byte == '$' || byte == '%' || byte == '&' || byte == '+' ||
         byte == ',';
}

int HexValue(char c) {
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

}  // namespace

bool ShouldPercentEncode(unsigned char byte, PercentEncodeSet set) {
  switch (set) {
    case PercentEncodeSet::C0Control:
      return IsC0ControlOrAbove7E(byte);
    case PercentEncodeSet::Fragment:
      return InFragmentSet(byte);
    case PercentEncodeSet::Query:
      return InQuerySet(byte);
    case PercentEncodeSet::SpecialQuery:
      return InQuerySet(byte) || byte == '\'';
    case PercentEncodeSet::Path:
      return InPathSet(byte);
    case PercentEncodeSet::Userinfo:
      return InUserinfoSet(byte);
    case PercentEncodeSet::Component:
      return InComponentSet(byte);
  }
  return true;
}

void PercentEncodeInto(std::string_view input, PercentEncodeSet set, std::string& out) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const char c : input) {
    const auto byte = static_cast<unsigned char>(c);
    if (ShouldPercentEncode(byte, set)) {
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0F]);
    } else {
      out.push_back(c);
    }
  }
}

std::string PercentEncode(std::string_view input, PercentEncodeSet set) {
  std::string out;
  out.reserve(input.size());
  PercentEncodeInto(input, set, out);
  return out;
}

std::string PercentDecode(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] != '%' || i + 2 >= input.size()) {
      out.push_back(input[i]);
      continue;
    }
    const int high = HexValue(input[i + 1]);
    const int low = HexValue(input[i + 2]);
    if (high < 0 || low < 0) {
      out.push_back(input[i]);
      continue;
    }
    out.push_back(static_cast<char>(high * 16 + low));
    i += 2;
  }
  return out;
}

}  // namespace microbrowser::util
