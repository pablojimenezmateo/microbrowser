// HMAC-SHA256 and AES-128-CTR, against published vectors rather than against
// this project's reading of the specification.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "util/AesCtr.h"
#include "util/Hmac.h"

namespace microbrowser::tests {

namespace {

std::string Hex(std::string_view bytes) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const char c : bytes) {
    const auto byte = static_cast<unsigned char>(c);
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0F]);
  }
  return out;
}

template <std::size_t N>
std::array<std::uint8_t, N> ParseHex(std::string_view hex) {
  std::array<std::uint8_t, N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    const char hi_ch = hex[i * 2];
    const char lo_ch = hex[i * 2 + 1];
    auto hex_val = [](char ch) -> int {
      if (ch >= '0' && ch <= '9') {
        return ch - '0';
      }
      if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
      }
      if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
      }
      return -1;
    };
    const int hi = hex_val(hi_ch);
    const int lo = hex_val(lo_ch);
    Expect(hi >= 0 && lo >= 0, "hex digit");
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

}  // namespace

void RegisterCryptoPrimitiveTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Hmac/MatchesRfc4231TestCase1", [] {
    const std::string key(20, static_cast<char>(0x0b));
    const std::string digest = util::HmacSha256(key, "Hi There");
    ExpectEqString(Hex(digest), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
                   "RFC 4231 test case 1");
  });

  AddTest(tests, "AesCtr/MatchesNistSp80038aF51FirstBlock", [] {
    const auto key = ParseHex<16>("2b7e151628aed2a6abf7158809cf4f3c");
    const auto counter = ParseHex<16>("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    const auto plaintext = ParseHex<16>("6bc1bee22e409f96e93d7e117393172a");
    std::array<std::uint8_t, 16> ciphertext{};

    const bool ok = util::Aes128CtrXor(key, counter, plaintext, ciphertext);
    Expect(ok, "AES-128-CTR encrypt");
    ExpectEqString(Hex(std::string_view(reinterpret_cast<const char*>(ciphertext.data()), 16)),
                   "874d6191b620e3261bef6864990db6ce", "NIST SP 800-38A F.5.1 first block");
  });

  AddTest(tests, "AesCtr/RejectsOutputSizeMismatch", [] {
    const auto key = ParseHex<16>("2b7e151628aed2a6abf7158809cf4f3c");
    const auto counter = ParseHex<16>("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    const auto plaintext = ParseHex<16>("6bc1bee22e409f96e93d7e117393172a");
    std::array<std::uint8_t, 8> short_output{};
    Expect(!util::Aes128CtrXor(key, counter, plaintext, short_output), "size mismatch");
  });
}

}  // namespace microbrowser::tests
