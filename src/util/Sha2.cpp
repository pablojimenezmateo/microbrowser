#include "util/Sha2.h"

#include <array>
#include <cstring>

namespace microbrowser::util {

namespace {

constexpr std::uint32_t kSha256Constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

constexpr std::uint64_t kSha512Constants[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

template <typename Word>
constexpr Word RotateRight(Word value, int by) {
  constexpr int kBits = static_cast<int>(sizeof(Word) * 8);
  return static_cast<Word>((value >> by) | (value << (kBits - by)));
}

// The four mixing functions, parameterised by the shift schedule so that the
// 32-bit and 64-bit rounds are one piece of code. The schedules are the only
// thing that differs between SHA-256 and SHA-512, which is the fact this
// template exists to make visible rather than to hide.
template <typename Word, int A, int B, int C>
constexpr Word Sigma(Word x) {
  return static_cast<Word>(RotateRight(x, A) ^ RotateRight(x, B) ^ RotateRight(x, C));
}

template <typename Word, int A, int B, int Shift>
constexpr Word LittleSigma(Word x) {
  return static_cast<Word>(RotateRight(x, A) ^ RotateRight(x, B) ^ (x >> Shift));
}

template <typename Word>
constexpr Word Choose(Word e, Word f, Word g) {
  return static_cast<Word>((e & f) ^ (~e & g));
}

template <typename Word>
constexpr Word Majority(Word a, Word b, Word c) {
  return static_cast<Word>((a & b) ^ (a & c) ^ (b & c));
}

void Sha256Block(std::array<std::uint32_t, 8>& state, const unsigned char* block) {
  std::array<std::uint32_t, 64> w{};
  for (int i = 0; i < 16; ++i) {
    w[static_cast<std::size_t>(i)] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                                     (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                                     (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                                     static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    w[i] = LittleSigma<std::uint32_t, 17, 19, 10>(w[i - 2]) + w[i - 7] +
           LittleSigma<std::uint32_t, 7, 18, 3>(w[i - 15]) + w[i - 16];
  }
  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t t1 =
        h + Sigma<std::uint32_t, 6, 11, 25>(e) + Choose(e, f, g) + kSha256Constants[i] + w[i];
    const std::uint32_t t2 = Sigma<std::uint32_t, 2, 13, 22>(a) + Majority(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

void Sha512Block(std::array<std::uint64_t, 8>& state, const unsigned char* block) {
  std::array<std::uint64_t, 80> w{};
  for (int i = 0; i < 16; ++i) {
    std::uint64_t value = 0;
    for (int byte = 0; byte < 8; ++byte) {
      value = (value << 8) | static_cast<std::uint64_t>(block[i * 8 + byte]);
    }
    w[static_cast<std::size_t>(i)] = value;
  }
  for (std::size_t i = 16; i < 80; ++i) {
    w[i] = LittleSigma<std::uint64_t, 19, 61, 6>(w[i - 2]) + w[i - 7] +
           LittleSigma<std::uint64_t, 1, 8, 7>(w[i - 15]) + w[i - 16];
  }
  std::uint64_t a = state[0];
  std::uint64_t b = state[1];
  std::uint64_t c = state[2];
  std::uint64_t d = state[3];
  std::uint64_t e = state[4];
  std::uint64_t f = state[5];
  std::uint64_t g = state[6];
  std::uint64_t h = state[7];
  for (std::size_t i = 0; i < 80; ++i) {
    const std::uint64_t t1 =
        h + Sigma<std::uint64_t, 14, 18, 41>(e) + Choose(e, f, g) + kSha512Constants[i] + w[i];
    const std::uint64_t t2 = Sigma<std::uint64_t, 28, 34, 39>(a) + Majority(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

// The padded message, block by block, without copying the input.
//
// A closure over the tail rather than a buffer of the whole message: an
// integrity check hashes a resource, and a resource is as large as a server
// says it is. The one allocation here is the two-block tail.
template <std::size_t kBlock, typename Consume>
void ForEachPaddedBlock(std::string_view data, std::size_t length_bytes, Consume consume) {
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data.data());
  std::size_t offset = 0;
  while (data.size() - offset >= kBlock) {
    consume(bytes + offset);
    offset += kBlock;
  }
  // The tail: what is left, a 0x80 byte, zeroes, and the bit length. Two blocks
  // when the remainder leaves no room for the length field, one otherwise.
  std::array<unsigned char, kBlock * 2> tail{};
  const std::size_t left = data.size() - offset;
  if (left > 0) {
    std::memcpy(tail.data(), bytes + offset, left);
  }
  tail[left] = 0x80;
  const std::size_t total = (left + 1 + length_bytes > kBlock) ? kBlock * 2 : kBlock;
  // The bit count, big-endian, in the last `length_bytes` of the tail. Only the
  // low 64 bits are written: SHA-512's field is 128 bits wide and no input here
  // is 2^64 bits long, so the high half is the zeroes it is initialised to.
  const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8U;
  for (std::size_t i = 0; i < 8; ++i) {
    tail[total - 1 - i] = static_cast<unsigned char>((bits >> (i * 8)) & 0xFFU);
  }
  consume(tail.data());
  if (total == kBlock * 2) {
    consume(tail.data() + kBlock);
  }
}

}  // namespace

std::size_t HashLength(HashAlgorithm algorithm) {
  switch (algorithm) {
    case HashAlgorithm::Sha256:
      return 32;
    case HashAlgorithm::Sha384:
      return 48;
    case HashAlgorithm::Sha512:
      return 64;
  }
  return 0;
}

std::string Sha2(HashAlgorithm algorithm, std::string_view data) {
  if (algorithm == HashAlgorithm::Sha256) {
    std::array<std::uint32_t, 8> state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                          0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    ForEachPaddedBlock<64>(data, 8, [&state](const unsigned char* block) {
      Sha256Block(state, block);
    });
    std::string out;
    out.reserve(32);
    for (const std::uint32_t word : state) {
      for (int i = 3; i >= 0; --i) {
        out.push_back(static_cast<char>((word >> (i * 8)) & 0xFFU));
      }
    }
    return out;
  }

  std::array<std::uint64_t, 8> state =
      algorithm == HashAlgorithm::Sha384
          ? std::array<std::uint64_t, 8>{0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL,
                                         0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
                                         0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
                                         0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL}
          : std::array<std::uint64_t, 8>{0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
                                         0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                         0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                         0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
  ForEachPaddedBlock<128>(data, 16, [&state](const unsigned char* block) {
    Sha512Block(state, block);
  });
  std::string out;
  out.reserve(64);
  for (const std::uint64_t word : state) {
    for (int i = 7; i >= 0; --i) {
      out.push_back(static_cast<char>((word >> (i * 8)) & 0xFFU));
    }
  }
  out.resize(HashLength(algorithm));
  return out;
}

}  // namespace microbrowser::util
