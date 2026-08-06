#include "util/Sha1.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace microbrowser::util {

namespace {

std::uint32_t RotateLeft(std::uint32_t value, int by) {
  return (value << by) | (value >> (32 - by));
}

}  // namespace

std::string Sha1(std::string_view data) {
  std::array<std::uint32_t, 5> state = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                                        0xC3D2E1F0u};

  // The message, padded: a 0x80 byte, zeros, and the bit length as a big-endian 64-bit
  // number. Built once rather than streamed because the one caller hashes 60 bytes and
  // a streaming interface would be an API nobody needs and a state machine to be wrong
  // about.
  std::string message(data);
  const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8u;
  message.push_back(static_cast<char>(0x80));
  while (message.size() % 64u != 56u) {
    message.push_back('\0');
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<char>((bits >> shift) & 0xFFu));
  }

  for (std::size_t at = 0; at < message.size(); at += 64u) {
    std::array<std::uint32_t, 80> schedule = {};
    for (std::size_t i = 0; i < 16; ++i) {
      const std::size_t base = at + i * 4u;
      schedule[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(message[base])) << 24) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(message[base + 1])) << 16) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(message[base + 2])) << 8) |
                    static_cast<std::uint32_t>(static_cast<unsigned char>(message[base + 3]));
    }
    for (std::size_t i = 16; i < 80; ++i) {
      schedule[i] = RotateLeft(
          schedule[i - 3] ^ schedule[i - 8] ^ schedule[i - 14] ^ schedule[i - 16], 1);
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    for (std::size_t i = 0; i < 80; ++i) {
      std::uint32_t f = 0;
      std::uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const std::uint32_t temp = RotateLeft(a, 5) + f + e + k + schedule[i];
      e = d;
      d = c;
      c = RotateLeft(b, 30);
      b = a;
      a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
  }

  std::string digest;
  digest.reserve(20);
  for (const std::uint32_t word : state) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      digest.push_back(static_cast<char>((word >> shift) & 0xFFu));
    }
  }
  return digest;
}

}  // namespace microbrowser::util
