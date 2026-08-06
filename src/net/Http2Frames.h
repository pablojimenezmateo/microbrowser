#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace microbrowser::net::http2 {

// HTTP/2's framing layer (RFC 9113 §4-6), on its own.
//
// Separate from the session for the reason the HTTP/1.1 response parser is
// separate from `Fetch`: this half is a pure function of bytes and can be
// fuzzed without a socket, and the half above it is a state machine that can be
// tested without one. Everything here reads or writes exactly one frame and
// holds no state between calls — the connection's state lives in
// `Http2Session`, which is where the interesting mistakes are.

// The client's opening bytes. Deliberately not a valid HTTP/1.1 request:
// its purpose is that a server which does *not* speak HTTP/2 rejects it
// immediately rather than half-understanding it.
inline constexpr std::string_view kConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

inline constexpr std::size_t kFrameHeaderBytes = 9;

// The largest frame this browser will accept, and what it advertises in
// SETTINGS_MAX_FRAME_SIZE. The protocol's floor and default; raising it buys
// throughput at the cost of a bigger buffer that a peer chooses the size of,
// and there is no measurement here saying the throughput is missing.
inline constexpr std::uint32_t kMaxFrameSize = 16384;

// The largest a flow-control window may ever hold (RFC 9113 §6.9.1).
//
// Here rather than beside each of the two places that check it, because a peer
// that asks for more has to be *refused* rather than clamped -- clamped, the
// two ends hold different numbers for the same window, and that disagreement is
// what a flow-control deadlock is. Two spellings of that limit is one that can
// drift.
inline constexpr std::int64_t kMaxWindow = 0x7FFFFFFF;

// Frames are only ever read after their whole payload has arrived, so this is
// also the ceiling on what one read can be asked to hold.
inline constexpr std::size_t kMaxFrameBytes = kFrameHeaderBytes + kMaxFrameSize;

enum class FrameType : std::uint8_t {
  Data = 0,
  Headers = 1,
  Priority = 2,
  RstStream = 3,
  Settings = 4,
  PushPromise = 5,
  Ping = 6,
  GoAway = 7,
  WindowUpdate = 8,
  Continuation = 9,
};

// Flags share bit values across frame types — 0x1 is ACK on SETTINGS and PING
// and END_STREAM on DATA and HEADERS — which is exactly the kind of overlap
// that turns into a confusion bug when the type is not checked first. Named
// separately so a call site has to say which it means.
namespace flag {
inline constexpr std::uint8_t kAck = 0x01;
inline constexpr std::uint8_t kEndStream = 0x01;
inline constexpr std::uint8_t kEndHeaders = 0x04;
inline constexpr std::uint8_t kPadded = 0x08;
inline constexpr std::uint8_t kPriority = 0x20;
}  // namespace flag

enum class ErrorCode : std::uint32_t {
  NoError = 0,
  ProtocolError = 1,
  InternalError = 2,
  FlowControlError = 3,
  SettingsTimeout = 4,
  StreamClosed = 5,
  FrameSizeError = 6,
  RefusedStream = 7,
  Cancel = 8,
  CompressionError = 9,
  ConnectError = 10,
  EnhanceYourCalm = 11,
  InadequateSecurity = 12,
  Http11Required = 13,
};

enum class Setting : std::uint16_t {
  HeaderTableSize = 1,
  EnablePush = 2,
  MaxConcurrentStreams = 3,
  InitialWindowSize = 4,
  MaxFrameSize = 5,
  MaxHeaderListSize = 6,
};

struct FrameHeader {
  std::uint32_t length = 0;
  std::uint8_t type = 0;
  std::uint8_t flags = 0;
  // The reserved high bit is masked off here rather than checked. RFC 9113
  // §4.1 says to ignore it, and a browser that closed connections over it
  // would be the only one on the web that did.
  std::uint32_t stream = 0;

  bool Is(FrameType wanted) const { return type == static_cast<std::uint8_t>(wanted); }
  bool Has(std::uint8_t bit) const { return (flags & bit) != 0; }
};

// The peer's half of the connection's parameters, with RFC 9113's defaults.
//
// A struct of plain numbers rather than a map, because every one of them is
// load-bearing on a path a peer controls and an unnamed setting is one nobody
// audits. Unknown identifiers are ignored, which the RFC requires — that is how
// the protocol is extended — and is safe precisely because this is a fixed set
// rather than a table the peer can grow.
struct PeerSettings {
  // The RFC's default is "unlimited", which is not a number. A hundred is what
  // is assumed until the server says otherwise, and it is what browsers assume:
  // treating unlimited as unlimited would mean a page could open as many
  // streams as it has subresources with no bound at all.
  std::uint32_t max_concurrent_streams = 100;
  std::uint32_t initial_window_size = 65535;
  std::uint32_t max_frame_size = 16384;
  // What the peer will accept from us. Zero means it did not say.
  std::uint32_t max_header_list_size = 0;

  // Applies a SETTINGS payload. `window_delta` comes back as the *signed*
  // change to SETTINGS_INITIAL_WINDOW_SIZE, because §6.9.2 requires every
  // existing stream's send window to move by exactly that amount — a change
  // that is applied to new streams only is the classic way a connection
  // deadlocks halfway through a large upload.
  //
  // False on a payload whose length is not a multiple of six, on an
  // INITIAL_WINDOW_SIZE above 2^31-1, or on a MAX_FRAME_SIZE outside the
  // protocol's range. All three are connection errors.
  bool Apply(std::span<const std::byte> payload, std::int64_t& window_delta);
};

// Reads a frame header. False only when fewer than nine bytes are present:
// every field is total over its bits, so a header either exists or has not
// arrived, and there is no third answer for a caller to get wrong.
bool ParseFrameHeader(std::span<const std::byte> data, FrameHeader& out);

// Strips the padding and, on HEADERS, the priority fields — leaving the part of
// the payload that is actually a header block or body.
//
// This is where a length underflows if it is going to. `pad_length` is a byte
// the peer chooses and the subtraction that follows it is the one place in the
// whole protocol where an unsigned wrap turns into a span over the heap. False
// when the padding does not fit; the caller treats that as a connection error.
bool StripPadding(const FrameHeader& header, std::span<const std::byte> payload,
                  std::span<const std::byte>& out);

// --- Writing ----------------------------------------------------------------
//
// Every writer appends to a string, because a connection sends into one
// outgoing buffer that a non-blocking socket drains at its own pace. A writer
// that returned a buffer would make every send a copy and every partial write a
// bug.

void WriteFrameHeader(FrameType type, std::uint8_t flags, std::uint32_t stream,
                      std::size_t length, std::string& out);
// This browser's own SETTINGS: a bounded HPACK table, push disabled, the
// protocol's frame size, and a receive window large enough that a transfer is
// not paced by round trips. See the constants in Http2Session.h.
void WriteSettings(std::uint32_t initial_window_size, std::string& out);
void WriteSettingsAck(std::string& out);
void WriteWindowUpdate(std::uint32_t stream, std::uint32_t increment, std::string& out);
void WriteRstStream(std::uint32_t stream, ErrorCode code, std::string& out);
void WriteGoAway(std::uint32_t last_stream, ErrorCode code, std::string_view debug,
                 std::string& out);
void WritePingAck(std::span<const std::byte> opaque, std::string& out);

// Big-endian, which is what every integer on this wire is. Exposed because the
// session writes a DATA frame's header itself and a second spelling of "four
// bytes, most significant first" is a second chance to get it wrong.
void WriteUint32(std::uint32_t value, std::string& out);
std::uint32_t ReadUint32(std::span<const std::byte> data);

}  // namespace microbrowser::net::http2
