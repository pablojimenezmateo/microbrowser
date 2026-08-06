#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "net/Hpack.h"

namespace microbrowser::tests {

using net::hpack::Decoder;
using net::hpack::Header;

namespace {

std::vector<std::byte> FromHex(std::string_view hex) {
  std::vector<std::byte> out;
  int high = -1;
  for (const char c : hex) {
    int digit = 0;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      continue;  // spaces, so a vector can be pasted from the RFC as written
    }
    if (high < 0) {
      high = digit;
      continue;
    }
    out.push_back(static_cast<std::byte>((high << 4) | digit));
    high = -1;
  }
  return out;
}

std::string ToHex(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const char c : bytes) {
    const auto byte = static_cast<unsigned char>(c);
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0F]);
  }
  return out;
}

// The header list, flattened, so one string compare covers a whole block.
std::string Flatten(const std::vector<Header>& headers) {
  std::string out;
  for (const Header& field : headers) {
    out += field.name;
    out += ": ";
    out += field.value;
    out += "\n";
  }
  return out;
}

std::vector<Header> DecodeOrFail(Decoder& decoder, std::string_view hex,
                                 std::string_view what) {
  std::vector<Header> out;
  const std::vector<std::byte> block = FromHex(hex);
  Expect(decoder.Decode(block, out), std::string("must decode: ") + std::string(what));
  return out;
}

}  // namespace

void RegisterHpackTests(std::vector<TestCase>& tests) {
  // The one property that says the 257-entry length table in Hpack.cpp is not
  // mistyped. A canonical prefix code is complete exactly when the sum of
  // 2^-length over its symbols is one, and a single wrong entry breaks it --
  // which is the whole reason the source holds lengths rather than codes.
  //
  // Computed in integer arithmetic against a common denominator of 2^30, the
  // longest code, because a floating-point sum of 257 terms would be a test
  // that passes with the wrong table often enough to be useless.
  AddTest(tests, "Hpack/HuffmanCodeIsComplete", [] {
    // Denominator 2^30, the longest code, so the whole sum is integer
    // arithmetic. A floating-point version of this would pass with a table
    // that is wrong by one bit somewhere, which is exactly the mistake it
    // exists to catch.
    std::uint64_t kraft = 0;
    for (std::uint16_t symbol = 0; symbol <= 256; ++symbol) {
      const unsigned length = net::hpack::HuffmanCodeLength(symbol);
      Expect(length >= 5 && length <= 30, "every symbol has a code between five and thirty bits");
      kraft += std::uint64_t{1} << (30 - length);
    }
    ExpectEqInt(static_cast<long long>(kraft), 1LL << 30,
                "Kraft's equality: the RFC 7541 Huffman code is complete, and a single "
                "mistyped length in Hpack.cpp breaks this and nothing else");

    // Every symbol, round-tripped: this exercises the same table from both
    // sides and would catch a length that is self-consistently wrong in a way
    // Kraft alone cannot see.
    std::string all;
    for (int byte = 0; byte < 256; ++byte) {
      all.push_back(static_cast<char>(byte));
    }
    std::string coded;
    net::hpack::HuffmanEncode(all, coded);
    std::string back;
    const auto* at = reinterpret_cast<const std::byte*>(coded.data());
    Expect(net::hpack::HuffmanDecode({at, coded.size()}, 1024, back),
           "every byte must survive a Huffman round trip");
    Expect(back == all, "and come back identical");
  });

  // RFC 7541 Appendix C.4.1, C.6.1 and C.6.2. Known-answer vectors, which is
  // the only kind of test that can tell a correct Huffman table from a
  // self-consistent wrong one.
  AddTest(tests, "Hpack/HuffmanMatchesTheRfcVectors", [] {
    struct Vector {
      std::string_view text;
      std::string_view hex;
    };
    constexpr Vector kVectors[] = {
        {"www.example.com", "f1e3c2e5f23a6ba0ab90f4ff"},
        {"no-cache", "a8eb10649cbf"},
        {"custom-key", "25a849e95ba97d7f"},
        {"custom-value", "25a849e95bb8e8b4bf"},
        {"private", "aec3771a4b"},
        {"Mon, 21 Oct 2013 20:13:21 GMT", "d07abe941054d444a8200595040b8166e082a62d1bff"},
        {"https://www.example.com", "9d29ad171863c78f0b97c8e9ae82ae43d3"},
    };
    for (const Vector& vector : kVectors) {
      std::string coded;
      net::hpack::HuffmanEncode(vector.text, coded);
      ExpectEqString(ToHex(coded), vector.hex, "the RFC's own encoding of this string");
      std::string back;
      const std::vector<std::byte> raw = FromHex(vector.hex);
      Expect(net::hpack::HuffmanDecode(raw, 1024, back), "and it must decode again");
      ExpectEqString(back, vector.text, "back to what it was");
    }
  });

  AddTest(tests, "Hpack/RejectsMalformedHuffman", [] {
    std::string out;
    // Padding that is not all ones. §5.2 calls this malformed rather than
    // tolerable, because a decoder that accepts it and one that does not
    // disagree about the string -- and both are talking to the same page.
    const std::vector<std::byte> bad_padding = FromHex("f1e3c2e5f23a6ba0ab90f4fe");
    Expect(!net::hpack::HuffmanDecode(bad_padding, 1024, out),
           "padding that is not the EOS prefix must be refused");
    out.clear();
    // A padding longer than seven bits: the last symbol ends on a byte
    // boundary and a whole further byte of ones follows.
    const std::vector<std::byte> long_padding = FromHex("ff");
    Expect(!net::hpack::HuffmanDecode(long_padding, 1024, out),
           "a padding of eight bits or more must be refused");
    out.clear();
    // An encoded EOS, which is thirty one bits. Accepting it as a terminator
    // is how a value's end becomes a matter of opinion.
    const std::vector<std::byte> eos = FromHex("ffffffff");
    Expect(!net::hpack::HuffmanDecode(eos, 1024, out), "an encoded EOS must be refused");
    out.clear();
    Expect(!net::hpack::HuffmanDecode(FromHex("f1e3c2e5f23a6ba0ab90f4ff"), 4, out),
           "and the output bound must hold before the string is finished");
  });

  // RFC 7541 Appendix C.3 — three requests on one connection, without Huffman,
  // with incremental indexing. The point is the *third*: it is short only
  // because the first two filled the dynamic table, so a decoder that got
  // eviction or ordering wrong produces different headers rather than an error.
  AddTest(tests, "Hpack/DecodesTheRfcRequestSequence", [] {
    Decoder decoder;
    const std::vector<Header> first =
        DecodeOrFail(decoder, "8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d", "C.3.1");
    ExpectEqString(Flatten(first),
                   ":method: GET\n:scheme: http\n:path: /\n:authority: www.example.com\n",
                   "the first request");
    ExpectEqInt(static_cast<long long>(decoder.TableBytes()), 57, "one entry, 57 bytes");

    const std::vector<Header> second =
        DecodeOrFail(decoder, "8286 84be 5808 6e6f 2d63 6163 6865", "C.3.2");
    ExpectEqString(Flatten(second),
                   ":method: GET\n:scheme: http\n:path: /\n:authority: www.example.com\n"
                   "cache-control: no-cache\n",
                   "the second, whose authority came out of the dynamic table");
    ExpectEqInt(static_cast<long long>(decoder.TableCount()), 2, "two entries now");

    const std::vector<Header> third = DecodeOrFail(
        decoder, "8287 85bf 400a 6375 7374 6f6d 2d6b 6579 0c63 7573 746f 6d2d 7661 6c75 65",
        "C.3.3");
    ExpectEqString(Flatten(third),
                   ":method: GET\n:scheme: https\n:path: /index.html\n"
                   ":authority: www.example.com\ncustom-key: custom-value\n",
                   "the third");
    ExpectEqInt(static_cast<long long>(decoder.TableCount()), 3, "three entries");
  });

  // Appendix C.5 — responses, with the table capacity set to 256 so that the
  // third one *evicts*. This is the case where a decoder that keeps the wrong
  // end of the table produces plausible headers from the wrong entries.
  AddTest(tests, "Hpack/EvictsWhenTheTableIsFull", [] {
    Decoder decoder;
    // A dynamic table size update to 256, then the RFC's first response.
    const std::vector<Header> first = DecodeOrFail(
        decoder,
        "3fe1 0148 0333 3032 5807 7072 6976 6174 6561 1d4d 6f6e 2c20 3231 204f 6374 2032 "
        "3031 3320 3230 3a31 333a 3231 2047 4d54 6e17 6874 7470 733a 2f2f 7777 772e 6578 "
        "616d 706c 652e 636f 6d",
        "C.5.1");
    ExpectEqString(Flatten(first),
                   ":status: 302\ncache-control: private\n"
                   "date: Mon, 21 Oct 2013 20:13:21 GMT\nlocation: https://www.example.com\n",
                   "the first response");
    ExpectEqInt(static_cast<long long>(decoder.TableBytes()), 222, "222 bytes of table");

    const std::vector<Header> second = DecodeOrFail(decoder, "4803 3330 37c1 c0bf", "C.5.2");
    ExpectEqString(Flatten(second),
                   ":status: 307\ncache-control: private\n"
                   "date: Mon, 21 Oct 2013 20:13:21 GMT\nlocation: https://www.example.com\n",
                   "the second, three of whose four fields came from the table");
    ExpectEqInt(static_cast<long long>(decoder.TableBytes()), 222,
                "and the oldest entry was evicted to make room");
    ExpectEqInt(static_cast<long long>(decoder.TableCount()), 4, "four entries");
  });

  AddTest(tests, "Hpack/RefusesAnIndexPastTheTable", [] {
    Decoder decoder;
    std::vector<Header> out;
    // Index 62 is the first dynamic entry, and there are none.
    const std::vector<std::byte> block = FromHex("be");
    Expect(!decoder.Decode(block, out), "an index past the end of the table is not decodable");
    Expect(decoder.Failed(), "and the decoder stays failed");
    std::vector<Header> again;
    Expect(!decoder.Decode(FromHex("82"), again),
           "a decoder that failed never decodes again -- its table no longer matches the peer's");
  });

  AddTest(tests, "Hpack/RefusesIndexZero", [] {
    Decoder decoder;
    std::vector<Header> out;
    Expect(!decoder.Decode(FromHex("80"), out), "indexed field 0 does not exist");
  });

  AddTest(tests, "Hpack/RefusesATableSizeAboveWhatWeAdvertised", [] {
    Decoder decoder;
    std::vector<Header> out;
    // 001 followed by 8193 in the five-bit prefix form: above the 4096 this
    // browser advertises, which is the one message whose entire content is
    // "hold more state for me".
    Expect(!decoder.Decode(FromHex("3fe2 3f"), out),
           "a dynamic table size update above the advertised maximum is a protocol error");
  });

  AddTest(tests, "Hpack/RefusesATruncatedBlock", [] {
    Decoder decoder;
    std::vector<Header> out;
    // A literal whose declared name length runs past the block.
    Expect(!decoder.Decode(FromHex("400f 7777 77"), out),
           "a string longer than the bytes that arrived must be refused, not reserved for");
  });

  AddTest(tests, "Hpack/BoundsTheHeaderCount", [] {
    Decoder decoder;
    std::vector<Header> out;
    // `:method: GET` repeated past the bound, which is the cheapest header
    // block there is: one byte in, 39 bytes of accounted list out.
    std::string block;
    for (std::size_t i = 0; i < net::hpack::kMaxHeaderCount + 10; ++i) {
      block += "82";
    }
    Expect(!decoder.Decode(FromHex(block), out), "a header list past the count bound is refused");
  });

  AddTest(tests, "Hpack/EncodesThroughTheStaticTable", [] {
    const std::vector<Header> headers = {
        {":method", "GET"}, {":scheme", "https"}, {":path", "/"},
        {":authority", "www.example.com"},
    };
    std::string block;
    net::hpack::Encode(headers, block);
    // Three exact static hits are one byte each; the authority is a literal
    // without indexing (0x01: name index 1) and a Huffman value (0x8c: twelve
    // coded bytes).
    ExpectEqString(ToHex(block), "828784018cf1e3c2e5f23a6ba0ab90f4ff",
                   "the encoding this browser puts on the wire");

    Decoder decoder;
    std::vector<Header> back;
    const auto* at = reinterpret_cast<const std::byte*>(block.data());
    Expect(decoder.Decode({at, block.size()}, back), "and our own decoder must read it");
    ExpectEqString(Flatten(back),
                   ":method: GET\n:scheme: https\n:path: /\n:authority: www.example.com\n",
                   "round trip");
  });

  AddTest(tests, "Hpack/SendsCookiesNeverIndexed", [] {
    const std::vector<Header> headers = {{"cookie", "session=abc"}};
    std::string block;
    net::hpack::Encode(headers, block);
    // 0x1f is the never-indexed form with a four-bit prefix at its maximum,
    // meaning the name index continues into the next byte: 15 + 17 = 32,
    // which is `cookie` in the static table.
    Expect(!block.empty() && (static_cast<unsigned char>(block[0]) & 0xF0u) == 0x10u,
           "a cookie goes out never-indexed, which forbids every intermediary from "
           "putting it in a table too");
    Decoder decoder;
    std::vector<Header> back;
    const auto* at = reinterpret_cast<const std::byte*>(block.data());
    Expect(decoder.Decode({at, block.size()}, back), "and it still decodes");
    ExpectEqString(Flatten(back), "cookie: session=abc\n", "to the same field");
    ExpectEqInt(static_cast<long long>(decoder.TableCount()), 0,
                "and nothing was inserted into the table");
  });

  AddTest(tests, "Hpack/EncoderNeverIndexes", [] {
    // Two identical header lists in a row. An encoder with a dynamic table
    // would make the second one shorter; this one must not, because it holds
    // no table and a peer that indexed on its behalf would be holding state
    // this side cannot reproduce.
    const std::vector<Header> headers = {{"x-thing", "a-fairly-long-value-here"}};
    std::string first;
    std::string second;
    net::hpack::Encode(headers, first);
    net::hpack::Encode(headers, second);
    ExpectEqString(ToHex(second), ToHex(first),
                   "the same headers encode to the same bytes, every time");
  });
}

}  // namespace microbrowser::tests
