#include "net/Http2Frames.h"

#include "net/Hpack.h"

namespace microbrowser::net::http2 {

namespace {

// The largest a flow-control window may ever be (RFC 9113 §6.9.1). A peer that
// asks for more has to be refused rather than clamped: clamping would leave the
// two ends with different numbers for the same window, and that disagreement is
// what a flow-control deadlock is made of.
constexpr std::uint32_t kMaxWindow = 0x7FFFFFFF;

void WriteUint16(std::uint16_t value, std::string& out) {
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

void WriteSetting(Setting id, std::uint32_t value, std::string& out) {
  WriteUint16(static_cast<std::uint16_t>(id), out);
  WriteUint32(value, out);
}

}  // namespace

void WriteUint32(std::uint32_t value, std::string& out) {
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

std::uint32_t ReadUint32(std::span<const std::byte> data) {
  if (data.size() < 4) {
    return 0;
  }
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

bool ParseFrameHeader(std::span<const std::byte> data, FrameHeader& out) {
  if (data.size() < kFrameHeaderBytes) {
    return false;
  }
  out.length = (static_cast<std::uint32_t>(data[0]) << 16) |
               (static_cast<std::uint32_t>(data[1]) << 8) | static_cast<std::uint32_t>(data[2]);
  out.type = static_cast<std::uint8_t>(data[3]);
  out.flags = static_cast<std::uint8_t>(data[4]);
  out.stream = ReadUint32(data.subspan(5, 4)) & 0x7FFFFFFFu;
  return true;
}

bool StripPadding(const FrameHeader& header, std::span<const std::byte> payload,
                  std::span<const std::byte>& out) {
  std::size_t at = 0;
  std::size_t pad = 0;
  if (header.Has(flag::kPadded)) {
    if (payload.empty()) {
      return false;
    }
    pad = static_cast<unsigned>(payload[0]);
    at = 1;
  }
  // Only HEADERS carries the priority fields, and only when it says so. Reading
  // them off a DATA frame because the bit happened to be set is how a body's
  // first five bytes go missing.
  if (header.Is(FrameType::Headers) && header.Has(flag::kPriority)) {
    if (payload.size() < at + 5) {
      return false;
    }
    at += 5;
  }
  // The subtraction, and the whole reason this function exists. `pad` is a byte
  // the peer chose; if it is not checked against what is left, the length below
  // wraps and the span covers memory the frame never contained.
  if (payload.size() < at + pad) {
    return false;
  }
  out = payload.subspan(at, payload.size() - at - pad);
  return true;
}

bool PeerSettings::Apply(std::span<const std::byte> payload, std::int64_t& window_delta) {
  window_delta = 0;
  if (payload.size() % 6 != 0) {
    return false;
  }
  for (std::size_t at = 0; at + 6 <= payload.size(); at += 6) {
    const auto id = static_cast<std::uint16_t>((static_cast<std::uint32_t>(payload[at]) << 8) |
                                               static_cast<std::uint32_t>(payload[at + 1]));
    const std::uint32_t value = ReadUint32(payload.subspan(at + 2, 4));
    switch (static_cast<Setting>(id)) {
      case Setting::HeaderTableSize:
        // What the peer will let *us* index up to. This browser's encoder never
        // indexes, so there is nothing to store: the value is accepted and
        // ignored, which is different from being rejected.
        break;
      case Setting::EnablePush:
        // A client sending this would be a protocol error; a server sending it
        // is meaningless. Either way it decides nothing here, because push is
        // refused unconditionally -- see Http2Session.
        break;
      case Setting::MaxConcurrentStreams:
        max_concurrent_streams = value;
        break;
      case Setting::InitialWindowSize:
        if (value > kMaxWindow) {
          return false;
        }
        // Signed, and applied to every open stream by the caller. §6.9.2 is
        // explicit that this is retroactive, and an implementation that applies
        // it to new streams only stalls a transfer that was already running.
        window_delta = static_cast<std::int64_t>(value) - static_cast<std::int64_t>(initial_window_size);
        initial_window_size = value;
        break;
      case Setting::MaxFrameSize:
        if (value < 16384 || value > 16777215) {
          return false;
        }
        max_frame_size = value;
        break;
      case Setting::MaxHeaderListSize:
        max_header_list_size = value;
        break;
      default:
        // Required by §6.5.2. An unknown setting is how the protocol grows, and
        // it is safe to ignore here only because this is a fixed set of named
        // fields rather than a table the peer gets to add rows to.
        break;
    }
  }
  return true;
}

void WriteFrameHeader(FrameType type, std::uint8_t flags, std::uint32_t stream,
                      std::size_t length, std::string& out) {
  out.push_back(static_cast<char>((length >> 16) & 0xFF));
  out.push_back(static_cast<char>((length >> 8) & 0xFF));
  out.push_back(static_cast<char>(length & 0xFF));
  out.push_back(static_cast<char>(static_cast<std::uint8_t>(type)));
  out.push_back(static_cast<char>(flags));
  WriteUint32(stream & 0x7FFFFFFFu, out);
}

void WriteSettings(std::uint32_t initial_window_size, std::string& out) {
  std::string payload;
  WriteSetting(Setting::HeaderTableSize, hpack::kDynamicTableBytes, payload);
  // **Zero, and this is a security decision rather than a performance one.**
  // Server push lets a server put a response into this browser's cache for a
  // URL nothing asked for -- a request the user did not cause, which is the
  // rule `guidelines/privacy.md` is built on. Every major browser has since
  // removed it; this one never had it.
  WriteSetting(Setting::EnablePush, 0, payload);
  WriteSetting(Setting::MaxFrameSize, kMaxFrameSize, payload);
  WriteSetting(Setting::InitialWindowSize, initial_window_size, payload);
  // The bound HPACK enforces anyway, told to the peer so a server that would
  // have sent a header list this browser must refuse can send something else
  // instead. Advertising it is what turns a failed load into a smaller one.
  WriteSetting(Setting::MaxHeaderListSize, hpack::kMaxHeaderListBytes, payload);
  WriteFrameHeader(FrameType::Settings, 0, 0, payload.size(), out);
  out += payload;
}

void WriteSettingsAck(std::string& out) {
  WriteFrameHeader(FrameType::Settings, flag::kAck, 0, 0, out);
}

void WriteWindowUpdate(std::uint32_t stream, std::uint32_t increment, std::string& out) {
  WriteFrameHeader(FrameType::WindowUpdate, 0, stream, 4, out);
  WriteUint32(increment & kMaxWindow, out);
}

void WriteRstStream(std::uint32_t stream, ErrorCode code, std::string& out) {
  WriteFrameHeader(FrameType::RstStream, 0, stream, 4, out);
  WriteUint32(static_cast<std::uint32_t>(code), out);
}

void WriteGoAway(std::uint32_t last_stream, ErrorCode code, std::string_view debug,
                 std::string& out) {
  // The debug field is free-form and goes to the server. It carries the reason
  // this browser is closing the connection and nothing about the user or the
  // page: a literal from our own source, never a URL and never a header value.
  WriteFrameHeader(FrameType::GoAway, 0, 0, 8 + debug.size(), out);
  WriteUint32(last_stream & 0x7FFFFFFFu, out);
  WriteUint32(static_cast<std::uint32_t>(code), out);
  out += debug;
}

void WritePingAck(std::span<const std::byte> opaque, std::string& out) {
  WriteFrameHeader(FrameType::Ping, flag::kAck, 0, 8, out);
  // Exactly the eight bytes that arrived, back unchanged. Padding a short one
  // or truncating a long one would be answering a different question from the
  // one asked, and the caller has already checked the length is eight.
  out.append(reinterpret_cast<const char*>(opaque.data()), opaque.size());
}

}  // namespace microbrowser::net::http2
