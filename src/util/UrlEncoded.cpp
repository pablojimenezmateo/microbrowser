#include "util/UrlEncoded.h"

#include <cstddef>

#include "util/PercentEncoding.h"

namespace microbrowser::util {

namespace {

// The urlencoded byte serializer's keep-set, verbatim: ASCII alphanumeric plus
// `*`, `-`, `.` and `_`. Everything else is percent-encoded, and a space is
// `+` rather than `%20`.
bool IsUrlEncodedSafe(unsigned char byte) {
  return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
         (byte >= 'A' && byte <= 'Z') || byte == '*' || byte == '-' || byte == '.' ||
         byte == '_';
}

void AppendComponent(std::string_view text, std::string& out) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte == ' ') {
      out.push_back('+');
    } else if (IsUrlEncodedSafe(byte)) {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0F]);
    }
  }
}

}  // namespace

void AppendUrlEncodedPair(std::string_view name, std::string_view value, std::string& out) {
  if (!out.empty()) {
    out.push_back('&');
  }
  AppendComponent(name, out);
  out.push_back('=');
  AppendComponent(value, out);
}

std::string SerializeUrlEncoded(const std::vector<QueryPair>& pairs) {
  std::string out;
  for (const QueryPair& pair : pairs) {
    AppendUrlEncodedPair(pair.first, pair.second, out);
  }
  return out;
}

std::vector<QueryPair> ParseUrlEncoded(std::string_view input) {
  if (!input.empty() && input.front() == '?') {
    input.remove_prefix(1);
  }
  // `+` means a space, and only after the split: a name containing an encoded
  // `%2B` must survive as a plus sign, which is why the substitution happens on
  // the component rather than on the whole string.
  const auto decode = [](std::string_view component) {
    std::string plussed(component);
    for (char& c : plussed) {
      if (c == '+') {
        c = ' ';
      }
    }
    return PercentDecode(plussed);
  };

  std::vector<QueryPair> pairs;
  std::size_t at = 0;
  while (at <= input.size()) {
    const std::size_t end = input.find('&', at);
    const std::string_view component =
        input.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
    if (!component.empty()) {
      const std::size_t equals = component.find('=');
      if (equals == std::string_view::npos) {
        pairs.emplace_back(decode(component), std::string());
      } else {
        pairs.emplace_back(decode(component.substr(0, equals)),
                           decode(component.substr(equals + 1)));
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    at = end + 1;
  }
  return pairs;
}

}  // namespace microbrowser::util
