// The WebSocket wire format and its handshake.
//
// ADR 0020 §5. RFC 6455's own examples are the fixtures where it has them -- §1.3's
// handshake key and §5.7's frames -- because a codec checked only against itself is a
// codec that agrees with itself.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "net/WebSocketFrames.h"
#include "util/Sha1.h"

namespace microbrowser::tests {

namespace {

using net::DecodeWebSocketFrame;
using net::EncodeWebSocketFrame;
using net::WebSocketDecode;
using net::WebSocketFrame;

std::span<const std::byte> Bytes(const std::vector<std::uint8_t>& data) {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size());
}

std::string Text(const std::vector<std::byte>& payload) {
  std::string out;
  for (const std::byte byte : payload) {
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

std::string Hex(std::string_view raw) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const char c : raw) {
    out.push_back(kDigits[(static_cast<unsigned char>(c) >> 4) & 0xF]);
    out.push_back(kDigits[static_cast<unsigned char>(c) & 0xF]);
  }
  return out;
}

}  // namespace

void RegisterWebSocketTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WebSocket/Sha1MatchesThePublishedVectors", [] {
    // FIPS 180-4's examples plus the empty string, which is where a padding bug hides.
    ExpectEqString(Hex(util::Sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d", "abc");
    ExpectEqString(Hex(util::Sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709", "empty");
    ExpectEqString(Hex(util::Sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
                   "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "the 56-byte case");
    // 64 bytes exactly: the padding needs a whole extra block, which is the boundary a
    // hand-written implementation gets wrong.
    ExpectEqString(Hex(util::Sha1(std::string(64, 'a'))),
                   "0098ba824b5c16427bd7a1122a5a442a25ec644d", "one full block");
  });

  AddTest(tests, "WebSocket/TheHandshakeAcceptIsRfc6455sOwnExample", [] {
    // RFC 6455 §1.3, verbatim. This is the one check that makes a 101 a handshake: a
    // server that returns the status without computing this has not agreed to speak the
    // protocol, and something in between may have answered instead.
    ExpectEqString(net::WebSocketAcceptFor("dGhlIHNhbXBsZSBub25jZQ=="),
                   "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "the RFC's example");
    Expect(net::WebSocketHandshakeAccepted(101, "websocket", "Upgrade",
                                           "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
                                           "dGhlIHNhbXBsZSBub25jZQ=="),
           "the handshake is accepted");
    // The wrong digest is a refusal even with the right status and headers.
    Expect(!net::WebSocketHandshakeAccepted(101, "websocket", "Upgrade", "wrong",
                                            "dGhlIHNhbXBsZSBub25jZQ=="),
           "a server that echoed the headers without computing the accept is refused");
    Expect(!net::WebSocketHandshakeAccepted(200, "websocket", "Upgrade",
                                            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
                                            "dGhlIHNhbXBsZSBub25jZQ=="),
           "and 200 is not 101");
    // Both token headers are matched case-insensitively, and `Connection` is a list.
    Expect(net::WebSocketHandshakeAccepted(101, "WebSocket", "keep-alive, Upgrade",
                                           "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
                                           "dGhlIHNhbXBsZSBub25jZQ=="),
           "as real servers spell them");
  });

  AddTest(tests, "WebSocket/DecodesRfc6455sFrameExamples", [] {
    // §5.7. A single unmasked text frame holding "Hello".
    const std::vector<std::uint8_t> hello = {0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f};
    const net::WebSocketDecodeResult one = DecodeWebSocketFrame(Bytes(hello));
    Expect(one.status == WebSocketDecode::Ok, "one frame");
    Expect(one.frame.opcode == WebSocketFrame::Opcode::Text, "text");
    Expect(one.frame.final, "final");
    ExpectEqString(Text(one.frame.payload), "Hello", "the payload");
    ExpectEqInt(static_cast<long long>(one.consumed), 7, "and it consumed exactly its own bytes");

    // The same message as two fragments: `0x01` is a non-final text frame, `0x80`
    // marks the final continuation. Reassembly is the connection's business; the codec
    // reports the pieces.
    const std::vector<std::uint8_t> fragmented = {0x01, 0x03, 0x48, 0x65, 0x6c,
                                                  0x80, 0x02, 0x6c, 0x6f};
    const net::WebSocketDecodeResult first = DecodeWebSocketFrame(Bytes(fragmented));
    Expect(first.status == WebSocketDecode::Ok && !first.frame.final, "a non-final text frame");
    ExpectEqString(Text(first.frame.payload), "Hel", "with the first half");
    const net::WebSocketDecodeResult second =
        DecodeWebSocketFrame(Bytes(fragmented).subspan(first.consumed));
    Expect(second.status == WebSocketDecode::Ok && second.frame.final, "then a final one");
    Expect(second.frame.opcode == WebSocketFrame::Opcode::Continuation, "as a continuation");
    ExpectEqString(Text(second.frame.payload), "lo", "with the rest");
  });

  AddTest(tests, "WebSocket/AnIncompleteFrameIsNotAFailedOne", [] {
    // The distinction the connection depends on: one means wait, the other means close.
    // Every prefix of a valid frame must say "wait".
    const std::vector<std::uint8_t> hello = {0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f};
    for (std::size_t length = 0; length < hello.size(); ++length) {
      const net::WebSocketDecodeResult partial =
          DecodeWebSocketFrame(Bytes(hello).subspan(0, length));
      Expect(partial.status == WebSocketDecode::Incomplete,
             "a prefix of a frame is incomplete, never failed");
    }
  });

  AddTest(tests, "WebSocket/AMaskedServerFrameIsRefused", [] {
    // RFC 6455 §5.1: a server must not mask. Accepting one would mean accepting a frame
    // that a proxy could have rewritten -- the masking rule exists so that
    // client-to-server traffic cannot be made to look like a cacheable request, and a
    // browser that ignored the direction would give that property away.
    const std::vector<std::uint8_t> masked = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                              0x7f, 0x9f, 0x4d, 0x51, 0x58};
    Expect(DecodeWebSocketFrame(Bytes(masked)).status == WebSocketDecode::Failed,
           "refused, and not merely unmasked for us");
  });

  AddTest(tests, "WebSocket/AnAbsurdLengthFailsRatherThanAsksForMoreBytes", [] {
    // **The bound doing its work.** A 63-bit length answered with "incomplete" would be
    // an instruction to the connection to buffer nine exabytes.
    std::vector<std::uint8_t> huge = {0x82, 0x7F, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    Expect(DecodeWebSocketFrame(Bytes(huge), 1024).status == WebSocketDecode::Failed,
           "over the ceiling is a failure");
    // And the high bit set is a server that is not speaking the protocol at all, which
    // is a different answer from a length this decoder declines to buffer.
    std::vector<std::uint8_t> negative = {0x82, 0x7F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x01};
    Expect(DecodeWebSocketFrame(Bytes(negative)).status == WebSocketDecode::Failed,
           "and so is the reserved high bit");
  });

  AddTest(tests, "WebSocket/ARedundantLengthFormIsRefused", [] {
    // One length, one spelling. A server that writes 5 in the two-byte form is not
    // malformed by the letter of §5.2, and it is refused for the reason WOFF2's
    // base-128 refuses leading zeros: a second spelling of a length is a second way for
    // two implementations to disagree about what a frame is.
    const std::vector<std::uint8_t> padded = {0x81, 0x7E, 0x00, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f};
    Expect(DecodeWebSocketFrame(Bytes(padded)).status == WebSocketDecode::Failed,
           "the short form was available");
  });

  AddTest(tests, "WebSocket/ControlFramesCannotBeFragmentedOrLarge", [] {
    // §5.5, and both halves are refusals rather than accommodations: a fragmented
    // `close` is a state machine two implementations disagree about.
    const std::vector<std::uint8_t> fragmented_ping = {0x09, 0x01, 0x61};  // FIN clear
    Expect(DecodeWebSocketFrame(Bytes(fragmented_ping)).status == WebSocketDecode::Failed,
           "a non-final ping is refused");
    std::vector<std::uint8_t> large_ping = {0x89, 0x7E, 0x00, 0x80};
    large_ping.resize(4 + 128, 0x61);
    Expect(DecodeWebSocketFrame(Bytes(large_ping)).status == WebSocketDecode::Failed,
           "and a 128-byte one is too");
    // A `close` carries nothing, or a code and a reason. One byte is neither, and
    // letting it through would hand a caller half a status code.
    const std::vector<std::uint8_t> half_code = {0x88, 0x01, 0x03};
    Expect(DecodeWebSocketFrame(Bytes(half_code)).status == WebSocketDecode::Failed,
           "half a close code is not a close");
    const std::vector<std::uint8_t> empty_close = {0x88, 0x00};
    Expect(DecodeWebSocketFrame(Bytes(empty_close)).status == WebSocketDecode::Ok,
           "and an empty close is fine");
  });

  AddTest(tests, "WebSocket/AnUnknownOpcodeOrReservedBitIsRefused", [] {
    // No extension has been negotiated -- this browser offers none -- so a reserved bit
    // set is a server using rules nobody named.
    const std::vector<std::uint8_t> reserved = {0xC1, 0x01, 0x61};
    Expect(DecodeWebSocketFrame(Bytes(reserved)).status == WebSocketDecode::Failed, "RSV1 set");
    const std::vector<std::uint8_t> unknown = {0x83, 0x01, 0x61};
    Expect(DecodeWebSocketFrame(Bytes(unknown)).status == WebSocketDecode::Failed, "opcode 3");
  });

  AddTest(tests, "WebSocket/EncodedFramesAreMaskedAndRoundTripThroughTheirOwnMask", [] {
    // RFC 6455 §5.7's masked example, produced rather than read: the mask and the
    // payload are the RFC's, so the bytes have to be the RFC's too.
    const std::uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    const std::string hello = "Hello";
    const std::vector<std::byte> encoded = EncodeWebSocketFrame(
        WebSocketFrame::Opcode::Text,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(hello.data()), hello.size()),
        mask);
    const std::vector<std::uint8_t> expected = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                                0x7f, 0x9f, 0x4d, 0x51, 0x58};
    ExpectEqInt(static_cast<long long>(encoded.size()),
                static_cast<long long>(expected.size()), "the same length");
    for (std::size_t i = 0; i < expected.size(); ++i) {
      ExpectEqInt(static_cast<long long>(static_cast<std::uint8_t>(encoded[i])), expected[i],
                  "and the same bytes as the RFC's example");
    }
  });

  AddTest(tests, "WebSocket/TheEncoderUsesTheShortestLengthFormTheDecoderAccepts", [] {
    // The encoder and the decoder have to agree about which spelling is canonical, or
    // this browser produces frames it would itself refuse -- a bug that only shows up
    // against another implementation.
    const std::uint8_t mask[4] = {0, 0, 0, 0};
    for (const std::size_t length : {std::size_t{0}, std::size_t{125}, std::size_t{126},
                                     std::size_t{65535}, std::size_t{65536}}) {
      const std::vector<std::byte> payload(length, std::byte{0x61});
      const std::vector<std::byte> frame =
          EncodeWebSocketFrame(WebSocketFrame::Opcode::Binary, payload, mask);
      // Decoded with the mask bit cleared, since the decoder refuses masked frames --
      // which is what the direction rule means and is asserted above.
      std::vector<std::byte> as_server = frame;
      as_server[1] = static_cast<std::byte>(static_cast<std::uint8_t>(as_server[1]) & 0x7Fu);
      as_server.erase(as_server.begin() + static_cast<std::ptrdiff_t>(
                          static_cast<std::size_t>(frame.size() - length) - 4u),
                      as_server.begin() + static_cast<std::ptrdiff_t>(
                          static_cast<std::size_t>(frame.size() - length)));
      const net::WebSocketDecodeResult decoded = DecodeWebSocketFrame(
          std::span<const std::byte>(as_server), 1024u * 1024u);
      Expect(decoded.status == WebSocketDecode::Ok,
             "every length this encoder writes is one this decoder accepts");
      ExpectEqInt(static_cast<long long>(decoded.frame.payload.size()),
                  static_cast<long long>(length), "with its payload");
    }
  });
}

}  // namespace microbrowser::tests
