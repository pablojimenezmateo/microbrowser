#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microbrowser::util {

// MIME Sniffing Standard §4. Parameters are ordered and first-wins; serializing
// is a different operation from parsing, which is why this is a record.
struct MimeType {
  std::string type;
  std::string subtype;
  std::vector<std::pair<std::string, std::string>> parameters;
};

std::optional<MimeType> ParseMimeType(std::string_view input);
std::string SerializeMimeType(const MimeType& mime);

// Parse then serialize, or the empty string on failure -- `Blob.type`.
std::string BlobMimeType(std::string_view input);

// Fetch's header-name / header-value checks, which sit on the same token
// alphabet the MIME parser uses. A second copy would disagree about `'` vs `/`.
bool IsHttpToken(std::string_view text);
bool IsHttpHeaderValue(std::string_view text);

}  // namespace microbrowser::util
