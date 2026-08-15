#include <cstddef>
#include <cstdint>
#include <string_view>

#include "util/MimeType.h"

// MIME type parse/serialize, fed arbitrary bytes.
//
// Content-Type is a header a stranger wrote, and Blob.type is a string a page
// wrote; both go through this parser. A parse that succeeds must serialize to
// something that parses back to the same serialization -- two components that
// disagree about a MIME type is how a sniffed HTML document becomes an image.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const auto parsed = microbrowser::util::ParseMimeType(input);
  if (!parsed.has_value()) {
    return 0;
  }
  const std::string serialized = microbrowser::util::SerializeMimeType(*parsed);
  const auto reparsed = microbrowser::util::ParseMimeType(serialized);
  if (!reparsed.has_value() || microbrowser::util::SerializeMimeType(*reparsed) != serialized) {
    __builtin_trap();
  }
  return 0;
}
