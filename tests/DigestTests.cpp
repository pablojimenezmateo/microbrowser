// SHA-2 and base64, against published vectors rather than against this
// project's reading of the specification.
//
// The vectors below are FIPS 180-4's own examples plus the RFC 4648 base64 test
// set. That is the entire justification for writing SHA-2 by hand instead of
// calling the TLS stack's (see src/util/Sha2.h): the algorithm has an
// independent, authoritative answer for every input, so "did we get it right"
// is a question with a citation rather than an opinion.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "TestSupport.h"
#include "util/Base64.h"
#include "util/Sha2.h"

namespace microbrowser::tests {

namespace {

using util::HashAlgorithm;

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

std::string HashHex(HashAlgorithm algorithm, std::string_view data) {
  return Hex(util::Sha2(algorithm, data));
}

}  // namespace

void RegisterDigestTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Sha2/MatchesTheFips180Vectors", [] {
    // FIPS 180-4 §D.1 and §D.2: the one-block and two-block examples.
    ExpectEqString(HashHex(HashAlgorithm::Sha256, "abc"),
                   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                   "SHA-256 of \"abc\"");
    ExpectEqString(
        HashHex(HashAlgorithm::Sha256, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "SHA-256 of the 448-bit example");
    ExpectEqString(HashHex(HashAlgorithm::Sha384, "abc"),
                   "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc"
                   "2358baeca134c825a7",
                   "SHA-384 of \"abc\"");
    ExpectEqString(HashHex(HashAlgorithm::Sha512, "abc"),
                   "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a"
                   "836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
                   "SHA-512 of \"abc\"");
    ExpectEqString(
        HashHex(HashAlgorithm::Sha512,
                "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqkl"
                "mnopqrlmnopqrsmnopqrstnopqrstu"),
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b"
        "5433ac7d329eeb6dd26545e96e55b874be909",
        "SHA-512 of the 896-bit example");
  });

  AddTest(tests, "Sha2/HashesTheEmptyStringAndEveryPaddingBoundary", [] {
    ExpectEqString(HashHex(HashAlgorithm::Sha256, ""),
                   "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                   "SHA-256 of the empty string");
    ExpectEqString(HashHex(HashAlgorithm::Sha512, ""),
                   "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0"
                   "ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
                   "SHA-512 of the empty string");
    // 55, 56 and 64 bytes are the three cases the SHA-256 tail has: room for
    // the length, no room (so a second block), and an exact block. A hash that
    // gets any of them wrong is right for almost every input, which is why they
    // are named rather than covered by accident.
    ExpectEqString(HashHex(HashAlgorithm::Sha256, std::string(55, 'a')),
                   "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
                   "SHA-256 of 55 'a'");
    ExpectEqString(HashHex(HashAlgorithm::Sha256, std::string(56, 'a')),
                   "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
                   "SHA-256 of 56 'a'");
    ExpectEqString(HashHex(HashAlgorithm::Sha256, std::string(64, 'a')),
                   "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
                   "SHA-256 of 64 'a'");
    // The same three for SHA-512, whose block is 128 bytes and whose length
    // field is 16.
    ExpectEqString(HashHex(HashAlgorithm::Sha512, std::string(111, 'a')),
                   "fa9121c7b32b9e01733d034cfc78cbf67f926c7ed83e82200ef86818196921760b4beff48404df811"
                   "b953828274461673c68d04e297b0eb7b2b4d60fc6b566a2",
                   "SHA-512 of 111 'a'");
    ExpectEqString(HashHex(HashAlgorithm::Sha512, std::string(112, 'a')),
                   "c01d080efd492776a1c43bd23dd99d0a2e626d481e16782e75d54c2503b5dc32bd05f0f1ba33e568"
                   "b88fd2d970929b719ecbb152f58f130a407c8830604b70ca",
                   "SHA-512 of 112 'a'");
    ExpectEqString(HashHex(HashAlgorithm::Sha512, std::string(128, 'a')),
                   "b73d1929aa615934e61a871596b3f3b33359f42b8175602e89f7e06e5f658a243667807ed300314b"
                   "95cacdd579f3e33abdfbe351909519a846d465c59582f321",
                   "SHA-512 of 128 'a'");
  });

  AddTest(tests, "Sha2/Sha384IsSha512Truncated", [] {
    ExpectEqInt(static_cast<long long>(util::HashLength(HashAlgorithm::Sha256)), 32, "sha256 length");
    ExpectEqInt(static_cast<long long>(util::HashLength(HashAlgorithm::Sha384)), 48, "sha384 length");
    ExpectEqInt(static_cast<long long>(util::HashLength(HashAlgorithm::Sha512)), 64, "sha512 length");
    Expect(util::Sha2(HashAlgorithm::Sha384, "abc").size() == 48,
           "SHA-384 is 48 bytes, not the 64 its state holds");
  });

  AddTest(tests, "Base64/RoundTripsTheRfc4648Vectors", [] {
    const std::pair<std::string_view, std::string_view> kVectors[] = {
        {"", ""},        {"f", "Zg=="},      {"fo", "Zm8="},        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    for (const auto& [plain, encoded] : kVectors) {
      ExpectEqString(util::Base64Encode(plain), encoded, "encode");
      const std::optional<std::string> decoded = util::Base64Decode(encoded);
      Expect(decoded.has_value(), "the vector decodes");
      ExpectEqString(*decoded, plain, "decode");
    }
  });

  AddTest(tests, "Base64/AcceptsBothAlphabetsButNotAMixture", [] {
    // `-_` is what a JWT and half the CSP hash-sources on the web are written
    // in; `+/` is what SRI uses.
    const std::optional<std::string> url_safe = util::Base64Decode("-_8");
    const std::optional<std::string> standard = util::Base64Decode("+/8");
    Expect(url_safe.has_value() && standard.has_value(), "both alphabets decode");
    ExpectEqString(*url_safe, *standard, "and to the same bytes");
    Expect(!util::Base64Decode("+_8").has_value(), "one string cannot be both");
  });

  AddTest(tests, "Base64/RejectsWhatWouldGiveOneDigestTwoSpellings", [] {
    Expect(!util::Base64Decode("YQ==Yg==").has_value(), "padding is not a separator");
    Expect(!util::Base64Decode("Z").has_value(), "six bits is never a byte");
    Expect(!util::Base64Decode("Zg=").has_value(), "the padding must match the length");
    // "Zh" and "Zg" differ only in bits the decoder throws away; accepting both
    // would mean one resource has two integrity strings that both verify.
    Expect(!util::Base64Decode("Zh==").has_value(), "leftover bits must be zero");
    Expect(util::Base64Decode("Zg==").has_value(), "and the canonical spelling still decodes");
    Expect(!util::Base64Decode("Zm9v!").has_value(), "an unknown character is a refusal");
  });

  AddTest(tests, "Base64/StripsTheWhitespaceAWrappedBlobArrivesWith", [] {
    const std::optional<std::string> decoded = util::Base64Decode("Zm9v\n  YmFy\r\n");
    Expect(decoded.has_value(), "a wrapped blob decodes");
    ExpectEqString(*decoded, "foobar", "and to the same bytes as the unwrapped one");
  });
}

}  // namespace microbrowser::tests
