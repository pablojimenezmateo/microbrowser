#pragma once

#include <string>
#include <string_view>

namespace microbrowser::util {

// HMAC-SHA256 (FIPS 198-1) over util::Sha2.
//
// For Web Crypto `crypto.subtle.sign` and youtube's PES path — not a general
// crypto kitchen sink. Key and data in, 32 raw digest bytes out.

std::string HmacSha256(std::string_view key, std::string_view data);

}  // namespace microbrowser::util
