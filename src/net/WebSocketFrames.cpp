#include "net/WebSocketFrames.h"

#include <algorithm>

#include "util/Base64.h"
#include "util/PerformanceCounters.h"
#include "util/Sha1.h"
#include "util/StringUtil.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// RFC 6455 §1.3. A fixed string, not a secret, and not a nonce: it exists so that a
// server which merely echoes headers cannot accidentally look like one that agreed to
// speak WebSocket.
constexpr std::string_view kHandshakeGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// §5.5: a control frame carries at most 125 bytes and is never fragmented.
constexpr std::size_t kMaxControlPayload = 125;

bool IsKnownOpcode(std::uint8_t opcode) {
  return opcode == 0x0 || opcode == 0x1 || opcode == 0x2 || opcode == 0x8 || opcode == 0x9 ||
         opcode == 0xA;
}

}  // namespace

WebSocketDecodeResult DecodeWebSocketFrame(std::span<const std::byte> input,
                                          std::size_t max_payload) {
  WebSocketDecodeResult result;
  const auto at = [&input](std::size_t index) {
    return static_cast<std::uint8_t>(input[index]);
  };
  if (input.size() < 2) {
    return result;  // Incomplete
  }

  const std::uint8_t first = at(0);
  const std::uint8_t second = at(1);
  const bool final = (first & 0x80u) != 0;
  const std::uint8_t reserved = first & 0x70u;
  const std::uint8_t opcode = first & 0x0Fu;
  const bool masked = (second & 0x80u) != 0;
  std::size_t length = second & 0x7Fu;
  std::size_t cursor = 2;

  // No extension has been negotiated -- this browser offers none -- so a reserved bit
  // set is a server using something we did not agree to. Failing is the specification's
  // answer and the safe one: the alternative is decoding a payload under rules nobody
  // named.
  if (reserved != 0 || !IsKnownOpcode(opcode)) {
    result.status = WebSocketDecode::Failed;
    AddPerformanceCounter(PerfCounterId::WebSocketProtocolErrors);
    return result;
  }
  // §5.1: a server must not mask. Accepting a masked server frame would mean accepting
  // a frame that a proxy could have rewritten -- the masking rule exists precisely so
  // that client-to-server traffic cannot be made to look like a cacheable request, and
  // a browser that ignored the direction would give that property away.
  if (masked) {
    result.status = WebSocketDecode::Failed;
    AddPerformanceCounter(PerfCounterId::WebSocketProtocolErrors);
    return result;
  }

  if (length == 126) {
    if (input.size() < cursor + 2) {
      return result;
    }
    length = (static_cast<std::size_t>(at(cursor)) << 8) | at(cursor + 1);
    cursor += 2;
    // The two-byte form must be used only for lengths it is needed for. A server that
    // spells 5 in two bytes is not malformed by the letter of §5.2, but it is a second
    // spelling of one length -- and this decoder refuses second spellings for the same
    // reason WOFF2's base-128 does.
    if (length < 126) {
      result.status = WebSocketDecode::Failed;
      AddPerformanceCounter(PerfCounterId::WebSocketProtocolErrors);
      return result;
    }
  } else if (length == 127) {
    if (input.size() < cursor + 8) {
      return result;
    }
    std::uint64_t wide = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      wide = (wide << 8) | at(cursor + i);
    }
    cursor += 8;
    // §5.2 says the high bit must be zero. It is checked before the ceiling because the
    // two answers differ: a 63-bit length is a length this decoder refuses, and a
    // 64-bit one is a server that is not speaking the protocol.
    if ((wide & 0x8000000000000000ull) != 0 || wide < 65536u) {
      result.status = WebSocketDecode::Failed;
      AddPerformanceCounter(PerfCounterId::WebSocketProtocolErrors);
      return result;
    }
    if (wide > max_payload) {
      // **Failed, not Incomplete.** This is the bound doing its work: answering
      // "incomplete" for a declared 9 exabytes would be an instruction to the
      // connection to buffer it.
      result.status = WebSocketDecode::Failed;
      AddPerformanceCounter(PerfCounterId::WebSocketOversizeRefusals);
      return result;
    }
    length = static_cast<std::size_t>(wide);
  }
  if (length > max_payload) {
    result.status = WebSocketDecode::Failed;
    AddPerformanceCounter(PerfCounterId::WebSocketOversizeRefusals);
    return result;
  }

  const bool control = opcode == 0x8 || opcode == 0x9 || opcode == 0xA;
  if (control && (!final || length > kMaxControlPayload)) {
    // §5.5 again, and both halves are refusals rather than accommodations: a fragmented
    // `close` is a state machine two implementations disagree about.
    result.status = WebSocketDecode::Failed;
    AddPerformanceCounter(PerfCounterId::WebSocketProtocolErrors);
    return result;
  }
  // A `close` carries either nothing or a two-byte code plus a reason. One byte is
  // neither, and a decoder that let it through would hand a caller half a code.
  if (opcode == 0x8 && length == 1) {
    result.status = WebSocketDecode::Failed;
    AddPerformanceCounter(PerfCounterId::WebSocketProtocolErrors);
    return result;
  }

  if (input.size() < cursor + length) {
    return result;  // Incomplete: the header is whole but the payload is not
  }
  result.frame.opcode = static_cast<WebSocketFrame::Opcode>(opcode);
  result.frame.final = final;
  result.frame.payload.assign(input.begin() + static_cast<std::ptrdiff_t>(cursor),
                              input.begin() + static_cast<std::ptrdiff_t>(cursor + length));
  result.consumed = cursor + length;
  result.status = WebSocketDecode::Ok;
  AddPerformanceCounter(PerfCounterId::WebSocketFramesReceived);
  return result;
}

std::vector<std::byte> EncodeWebSocketFrame(WebSocketFrame::Opcode opcode,
                                            std::span<const std::byte> payload,
                                            const std::uint8_t mask[4], bool final) {
  std::vector<std::byte> out;
  out.reserve(payload.size() + 14u);
  out.push_back(static_cast<std::byte>((final ? 0x80u : 0x00u) |
                                       static_cast<std::uint8_t>(opcode)));
  const std::size_t length = payload.size();
  // The shortest form that fits, which is the mirror of the decoder refusing longer
  // ones: one encoder and one decoder that disagree about which spelling is canonical
  // is a bug that only shows up against another implementation.
  if (length < 126) {
    out.push_back(static_cast<std::byte>(0x80u | length));
  } else if (length < 65536u) {
    out.push_back(static_cast<std::byte>(0x80u | 126u));
    out.push_back(static_cast<std::byte>((length >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(length & 0xFFu));
  } else {
    out.push_back(static_cast<std::byte>(0x80u | 127u));
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(length) >> shift) & 0xFFu));
    }
  }
  for (std::size_t i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>(mask[i]));
  }
  for (std::size_t i = 0; i < length; ++i) {
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4u]));
  }
  AddPerformanceCounter(PerfCounterId::WebSocketFramesSent);
  return out;
}

std::string WebSocketAcceptFor(std::string_view key) {
  std::string material(key);
  material += kHandshakeGuid;
  return util::Base64Encode(util::Sha1(material));
}

bool WebSocketHandshakeAccepted(int status, std::string_view upgrade, std::string_view connection,
                                std::string_view accept, std::string_view sent_key) {
  if (status != 101) {
    return false;
  }
  // Case-insensitive on both, because both are tokens and real servers vary: `Upgrade:
  // WebSocket` and `Connection: keep-alive, Upgrade` are both in the wild.
  if (!util::EqualsAsciiCaseInsensitive(upgrade, "websocket")) {
    return false;
  }
  if (util::AsciiLowerCase(std::string(connection)).find("upgrade") == std::string::npos) {
    return false;
  }
  // The digest, compared exactly. This is the whole reason the handshake is a handshake:
  // a server that returns 101 with the right token headers but the wrong accept value
  // has not computed anything, which means something other than a WebSocket server
  // answered -- a proxy, a cache, or a service that upgrades anything that asks.
  return !accept.empty() && accept == WebSocketAcceptFor(sent_key);
}

}  // namespace microbrowser::net
