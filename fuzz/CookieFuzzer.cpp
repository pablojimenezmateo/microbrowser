#include <cstddef>
#include <cstdint>
#include <string_view>

#include "net/CookieJar.h"
#include "url/PartitionKey.h"
#include "url/Url.h"

// Set-Cookie parsing and storage, fed arbitrary bytes.
//
// A cookie header is attacker-controlled by definition, and the storage rules
// it drives — domain matching against the Public Suffix List, the Secure and
// SameSite gates — are the ones that decide whether one site can write state
// another site reads.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view field(reinterpret_cast<const char*>(data), size);
  const auto url = microbrowser::url::Url::Parse("https://a.example.com/path/here");
  if (!url.has_value()) {
    return 0;
  }
  const auto key = microbrowser::url::PartitionKey::ForTopLevel(
      microbrowser::url::ContainerId::Default(), *url);

  microbrowser::net::CookieJar jar;
  jar.StoreFromHeader(key, *url, field, 1000);
  // Reading back must never produce a header the jar could not have stored.
  const std::string header = jar.HeaderFor(key, *url, true, true, 1000);
  if (header.find('\r') != std::string::npos || header.find('\n') != std::string::npos) {
    __builtin_trap();  // a cookie that can inject into the Cookie: header
  }
  return 0;
}
