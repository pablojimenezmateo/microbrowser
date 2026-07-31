#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace microbrowser::util {

// Heterogeneous hash for string-keyed unordered containers: pairing it with
// std::equal_to<> lets find()/count()/erase() accept a std::string_view (or
// const char*) lookup key without allocating a temporary std::string per call.
// Stored std::string keys and string_view lookups both route through
// std::hash<std::string_view>, so their hashes stay consistent.
//
// Usage: std::unordered_map<std::string, V, TransparentStringHash, std::equal_to<>>
struct TransparentStringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
};

}  // namespace microbrowser::util
