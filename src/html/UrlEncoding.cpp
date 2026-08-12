#include "html/UrlEncoding.h"

#include <cstdint>

#include "util/StringUtil.h"

namespace microbrowser::html {

namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

void AppendPercentEncoded(std::string& out, unsigned char byte) {
  out.push_back('%');
  out.push_back(kHexDigits[byte >> 4]);
  out.push_back(kHexDigits[byte & 0x0F]);
}

}  // namespace

void DocumentQueryEncoder::EncodeQuery(std::string_view input, bool (*needs_escape)(unsigned char),
                                       std::string& out) const {
  Encoder encoder(encoding_);
  std::string bytes;

  // The percent-encode set applies to the bytes the *encoding* produced, which is the whole point of
  // the algorithm's name: `%81%40` and `%81@` are the same two bytes, and which one is written is
  // decided per byte after the character has stopped being a character.
  const auto flush = [&] {
    for (const char raw : bytes) {
      const auto byte = static_cast<unsigned char>(raw);
      if (needs_escape(byte)) {
        AppendPercentEncoded(out, byte);
      } else {
        out.push_back(raw);
      }
    }
    bytes.clear();
  };

  std::size_t at = 0;
  while (at < input.size()) {
    const std::uint32_t code_point = NextScalarValue(input, at);
    const bool encoded = encoder.Encode(code_point, bytes);
    // Flushed either way, because a *failed* encode can still have left the escape that takes
    // ISO-2022-JP out of its shift state -- and those bytes belong before the escape below, not
    // after it.
    flush();
    if (!encoded) {
      // The standard's spelling of an unencodable code point, and it is appended **already
      // percent-encoded** rather than run through the set above. `&` and `;` are not in the query
      // percent-encode set, so `&#1234;` written literally would read as two more query parameters
      // to whatever parses the URL next -- which is a parameter the user never typed appearing in a
      // request, and is why the standard writes the escape out in hex rather than composing it.
      out += "%26%23";
      out += std::to_string(code_point);
      out += "%3B";
    }
  }
  encoder.Finish(bytes);
  flush();
}

}  // namespace microbrowser::html
