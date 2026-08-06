#include "util/BlobUrlRegistry.h"

namespace microbrowser::util {

std::string BlobUrlRegistry::Register(std::string body, std::string mime_type) {
  const std::string url = "blob:null/" + std::to_string(next_id_++);
  entries_.emplace(url, Entry{std::move(body), std::move(mime_type)});
  return url;
}

void BlobUrlRegistry::Revoke(std::string_view url) { entries_.erase(std::string(url)); }

std::optional<std::string_view> BlobUrlRegistry::Lookup(std::string_view url) const {
  const auto found = entries_.find(std::string(url));
  if (found == entries_.end()) {
    return std::nullopt;
  }
  return found->second.body;
}

}  // namespace microbrowser::util
