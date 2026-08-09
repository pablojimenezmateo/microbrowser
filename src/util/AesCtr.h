#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace microbrowser::util {

// AES-128 in CTR mode (NIST SP 800-38A).
//
// For Web Crypto `subtle.encrypt` AES-CTR on youtube — hostile input is
// fuzzable. AES-128 only; the block function is FIPS 197 in AesCtr.cpp.

// XOR `input` with the AES-128 keystream starting at `counter`. The counter is
// incremented as a big-endian 128-bit integer after each 16-byte block.
// Returns false when `output` is not exactly `input.size()`.
[[nodiscard]] bool Aes128CtrXor(std::span<const std::uint8_t, 16> key,
                                std::span<const std::uint8_t, 16> counter,
                                std::span<const std::uint8_t> input,
                                std::span<std::uint8_t> output);

}  // namespace microbrowser::util
