#include "util/Hmac.h"

#include <array>
#include <cstring>

#include "util/Sha2.h"

namespace microbrowser::util {

std::string HmacSha256(std::string_view key, std::string_view data) {
  constexpr std::size_t kBlockSize = 64;

  std::array<unsigned char, kBlockSize> key_block{};
  if (key.size() > kBlockSize) {
    const std::string hashed = Sha2(HashAlgorithm::Sha256, key);
    std::memcpy(key_block.data(), hashed.data(), hashed.size());
  } else {
    std::memcpy(key_block.data(), key.data(), key.size());
  }

  std::array<unsigned char, kBlockSize> ipad{};
  std::array<unsigned char, kBlockSize> opad{};
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    ipad[i] = static_cast<unsigned char>(key_block[i] ^ 0x36);
    opad[i] = static_cast<unsigned char>(key_block[i] ^ 0x5c);
  }

  std::string inner;
  inner.reserve(kBlockSize + data.size());
  inner.append(reinterpret_cast<const char*>(ipad.data()), kBlockSize);
  inner.append(data.data(), data.size());
  const std::string inner_hash = Sha2(HashAlgorithm::Sha256, inner);

  std::string outer;
  outer.reserve(kBlockSize + inner_hash.size());
  outer.append(reinterpret_cast<const char*>(opad.data()), kBlockSize);
  outer.append(inner_hash);
  return Sha2(HashAlgorithm::Sha256, outer);
}

}  // namespace microbrowser::util
