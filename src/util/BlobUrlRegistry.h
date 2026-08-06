#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace microbrowser::util {

// In-memory `blob:` URLs for `URL.createObjectURL(Blob)`. Cleared on navigation
// so a name from a previous document cannot be fetched on the next one.
class BlobUrlRegistry {
 public:
  void Clear() { entries_.clear(); next_id_ = 1; }

  std::string Register(std::string body, std::string mime_type);
  void Revoke(std::string_view url);
  std::optional<std::string_view> Lookup(std::string_view url) const;

 private:
  struct Entry {
    std::string body;
    std::string mime_type;
  };
  std::unordered_map<std::string, Entry> entries_;
  std::uint64_t next_id_ = 1;
};

}  // namespace microbrowser::util
