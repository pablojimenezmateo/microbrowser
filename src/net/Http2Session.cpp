#include "net/Http2Session.h"

#include <algorithm>
#include <array>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;
using namespace http2;  // NOLINT(google-build-using-namespace) — one file, one protocol

constexpr std::size_t kReadChunk = 32 * 1024;

}  // namespace

Http2Session::Http2Session(std::unique_ptr<Transport> connection)
    : connection_(std::move(connection)) {
  AddPerformanceCounter(PerfCounterId::NetHttp2Sessions);
  outgoing_ += kConnectionPreface;
  WriteSettings(kStreamWindow, outgoing_);
  // The connection window is *not* covered by SETTINGS_INITIAL_WINDOW_SIZE —
  // that setting applies to streams only — so it starts at the protocol's 65535
  // and has to be raised explicitly. Without this a page's whole load is paced
  // by round trips after the first 64KB, which is the shape of "HTTP/2 made it
  // slower" reports.
  WriteWindowUpdate(0, kConnectionWindow - 65535, outgoing_);
  recv_window_ = kConnectionWindow;
}

Http2Session::~Http2Session() {
  if (connection_ != nullptr) {
    connection_->Close();
  }
}

bool Http2Session::IsUsable() const {
  return error_ == nullptr && !going_away_ && connection_ != nullptr &&
         next_stream_ < 0x7FFFFFFFu;
}

std::size_t Http2Session::Capacity() const {
  if (!IsUsable()) {
    return 0;
  }
  const std::size_t limit = peer_.max_concurrent_streams;
  return streams_.size() >= limit ? 0 : limit - streams_.size();
}

std::optional<util::WaitDescriptor> Http2Session::Interest() const {
  if (connection_ == nullptr || error_ != nullptr) {
    return std::nullopt;
  }
  return connection_->Interest();
}

bool Http2Session::Flush() {
  if (connection_ == nullptr || sent_ >= outgoing_.size()) {
    return false;
  }
  bool moved = false;
  while (sent_ < outgoing_.size()) {
    const std::span<const std::byte> rest(
        reinterpret_cast<const std::byte*>(outgoing_.data()) + sent_, outgoing_.size() - sent_);
    const IoResult wrote = connection_->Send(rest);
    if (wrote.status == IoStatus::Blocked) {
      break;
    }
    if (wrote.status != IoStatus::Ready || wrote.bytes == 0) {
      error_ = error_ != nullptr ? error_ : "the connection failed while sending";
      return moved;
    }
    sent_ += wrote.bytes;
    moved = true;
  }
  if (sent_ == outgoing_.size()) {
    outgoing_.clear();
    sent_ = 0;
  } else if (sent_ > 64 * 1024) {
    outgoing_.erase(0, sent_);
    sent_ = 0;
  }
  return moved;
}

bool Http2Session::Read() {
  if (connection_ == nullptr || error_ != nullptr) {
    return false;
  }
  bool moved = false;
  std::array<std::byte, kReadChunk> buffer{};
  while (true) {
    const IoResult read = connection_->Receive(buffer);
    if (read.status == IoStatus::Blocked) {
      blocked_ = true;
      break;
    }
    if (read.status == IoStatus::Closed) {
      // A clean close with streams still open is a truncation, not an ending:
      // HTTP/2 says a response is over when END_STREAM says so and never
      // because the socket did. This is the same distinction the HTTP/1.1
      // parser draws between a self-delimited body and a close-delimited one,
      // and getting it wrong here would accept a half-received script.
      for (Stream& stream : streams_) {
        if (!stream.complete && stream.error == nullptr) {
          stream.error = "the connection closed before the response finished";
        }
      }
      error_ = error_ != nullptr ? error_ : "the connection closed";
      return true;
    }
    if (read.status != IoStatus::Ready) {
      for (Stream& stream : streams_) {
        if (!stream.complete && stream.error == nullptr) {
          stream.error = "the connection failed";
        }
      }
      error_ = error_ != nullptr ? error_ : "the connection failed";
      return true;
    }
    AddPerformanceCounter(PerfCounterId::NetBytesReceived, read.bytes);
    incoming_.insert(incoming_.end(), buffer.begin(),
                     buffer.begin() + static_cast<std::ptrdiff_t>(read.bytes));
    moved = true;
    if (!ConsumeFrames()) {
      return true;
    }
    if (read.bytes < buffer.size()) {
      // Short read: the socket had less than we asked for, so the next call
      // would only tell us what this one already did.
      blocked_ = true;
      break;
    }
  }
  return moved;
}

bool Http2Session::ConsumeFrames() {
  std::size_t at = 0;
  while (error_ == nullptr && incoming_.size() - at >= kFrameHeaderBytes) {
    const std::span<const std::byte> rest(incoming_.data() + at, incoming_.size() - at);
    FrameHeader header;
    if (!ParseFrameHeader(rest, header)) {
      break;
    }
    if (header.length > kMaxFrameSize) {
      // Larger than the SETTINGS_MAX_FRAME_SIZE we advertised. Refused before a
      // byte of it is buffered, which is the whole reason the bound is
      // advertised rather than assumed.
      Fail(ErrorCode::FrameSizeError, "frame larger than the advertised maximum");
      break;
    }
    if (rest.size() < kFrameHeaderBytes + header.length) {
      break;
    }
    const std::span<const std::byte> payload = rest.subspan(kFrameHeaderBytes, header.length);
    at += kFrameHeaderBytes + header.length;
    AddPerformanceCounter(PerfCounterId::NetHttp2FramesReceived);
    if (!HandleFrame(header, payload)) {
      break;
    }
  }
  // Erased once per call rather than once per frame: a page's load is thousands
  // of frames and each erase would move everything after it.
  incoming_.erase(incoming_.begin(), incoming_.begin() + static_cast<std::ptrdiff_t>(at));
  return error_ == nullptr;
}

bool Http2Session::HandleFrame(const FrameHeader& header, std::span<const std::byte> payload) {
  // §6.10: while a header block is being assembled the *only* frame that may
  // arrive is its own CONTINUATION. Nothing else, on any stream. Enforcing it
  // is not pedantry — a peer that can interleave here can make two header
  // blocks share a decoder, which is the HPACK desynchronisation this browser
  // spends a whole file avoiding.
  if (assembling_ != 0 && !header.Is(FrameType::Continuation)) {
    return Fail(ErrorCode::ProtocolError, "a frame arrived inside a header block");
  }
  switch (static_cast<FrameType>(header.type)) {
    case FrameType::Data:
      return HandleData(header, payload);
    case FrameType::Headers:
      return HandleHeaders(header, payload);
    case FrameType::Continuation:
      return HandleContinuation(header, payload);
    case FrameType::Settings:
      return HandleSettings(header, payload);
    case FrameType::WindowUpdate:
      return HandleWindowUpdate(header, payload);
    case FrameType::RstStream:
      return HandleRstStream(header, payload);
    case FrameType::GoAway:
      return HandleGoAway(payload);
    case FrameType::Ping:
      if (header.stream != 0 || header.length != 8) {
        return Fail(ErrorCode::ProtocolError, "malformed PING");
      }
      if (!header.Has(flag::kAck)) {
        WritePingAck(payload, outgoing_);
      }
      return true;
    case FrameType::Priority:
      // Deprecated by RFC 9113 and still sent by real servers. The length is
      // checked and the content is dropped: this browser has one loop and no
      // scheduler to prioritise against, so acting on it would be pretending.
      if (header.stream == 0 || header.length != 5) {
        return Fail(ErrorCode::ProtocolError, "malformed PRIORITY");
      }
      return true;
    case FrameType::PushPromise:
      // We advertised SETTINGS_ENABLE_PUSH = 0. A server that pushes anyway is
      // offering a response to a request the user never made, which is the one
      // thing `guidelines/privacy.md` says a browser must not accept.
      return Fail(ErrorCode::ProtocolError, "server push is not enabled");
  }
  // Unknown types are ignored, which §4.1 requires: it is how the protocol
  // grows, and it is safe because the length has already been checked and the
  // payload is being skipped rather than interpreted.
  return true;
}

void Http2Session::TopUpWindow(StreamId id, std::int64_t& window, std::uint32_t target) {
  if (window > target / 2) {
    return;
  }
  const std::int64_t increment = static_cast<std::int64_t>(target) - window;
  if (increment <= 0) {
    return;
  }
  window = target;
  WriteWindowUpdate(id, static_cast<std::uint32_t>(increment), outgoing_);
  AddPerformanceCounter(PerfCounterId::NetHttp2WindowUpdates);
}

bool Http2Session::HandleSettings(const FrameHeader& header, std::span<const std::byte> payload) {
  if (header.stream != 0) {
    return Fail(ErrorCode::ProtocolError, "SETTINGS on a stream");
  }
  if (header.Has(flag::kAck)) {
    if (header.length != 0) {
      return Fail(ErrorCode::FrameSizeError, "a SETTINGS acknowledgement with a payload");
    }
    return true;
  }
  std::int64_t window_delta = 0;
  if (!peer_.Apply(payload, window_delta)) {
    return Fail(ErrorCode::ProtocolError, "malformed SETTINGS");
  }
  if (window_delta != 0) {
    // §6.9.2, and the reason `Apply` hands back a delta rather than a value:
    // a change to the initial window size moves *every open stream's* send
    // window by that amount, retroactively. An implementation that applied it
    // to new streams only would deadlock a transfer already in flight, which
    // is a bug that only appears against servers that change the setting
    // mid-connection — so, only in production.
    for (Stream& stream : streams_) {
      stream.send_window += window_delta;
      if (stream.send_window > kMaxWindow) {
        return Fail(ErrorCode::FlowControlError, "a send window past its maximum");
      }
    }
  }
  WriteSettingsAck(outgoing_);
  return true;
}

bool Http2Session::HandleWindowUpdate(const FrameHeader& header,
                                      std::span<const std::byte> payload) {
  if (header.length != 4) {
    return Fail(ErrorCode::FrameSizeError, "a WINDOW_UPDATE that is not four bytes");
  }
  const std::int64_t increment = ReadUint32(payload) & 0x7FFFFFFFu;
  if (increment == 0) {
    if (header.stream == 0) {
      return Fail(ErrorCode::ProtocolError, "a WINDOW_UPDATE of zero");
    }
    Stream* stream = Find(header.stream);
    if (stream != nullptr) {
      FailStream(*stream, ErrorCode::ProtocolError, "a WINDOW_UPDATE of zero");
    }
    return true;
  }
  if (header.stream == 0) {
    send_window_ += increment;
    if (send_window_ > kMaxWindow) {
      return Fail(ErrorCode::FlowControlError, "the connection send window overflowed");
    }
    return true;
  }
  Stream* stream = Find(header.stream);
  if (stream == nullptr) {
    return true;  // for a stream we have already finished with
  }
  stream->send_window += increment;
  if (stream->send_window > kMaxWindow) {
    FailStream(*stream, ErrorCode::FlowControlError, "the stream send window overflowed");
  }
  return true;
}

bool Http2Session::HandleGoAway(std::span<const std::byte> payload) {
  if (payload.size() < 8) {
    return Fail(ErrorCode::FrameSizeError, "a GOAWAY shorter than its fixed fields");
  }
  AddPerformanceCounter(PerfCounterId::NetHttp2GoAways);
  going_away_ = true;
  last_accepted_ = ReadUint32(payload) & 0x7FFFFFFFu;
  // Streams above the last one the server promised to process were never
  // started as far as it is concerned, so they are safe to send again on a new
  // connection — which is what the caller's one retry is for. Streams at or
  // below it keep going: that is what makes a graceful shutdown graceful, and
  // failing them here would turn every server restart into a broken page.
  for (Stream& stream : streams_) {
    if (stream.id > last_accepted_ && !stream.complete && stream.error == nullptr) {
      stream.refused = true;
      stream.error = "the server refused the stream";
    }
  }
  return true;
}

bool Http2Session::Advance() {
  blocked_ = false;
  if (connection_ == nullptr || error_ != nullptr) {
    return false;
  }
  bool any = false;
  // Bounded rather than "until nothing moves", because every round can be made
  // to do work by a peer that keeps sending: this is the one loop in the
  // network stack a server could otherwise hold open indefinitely. The queue
  // calls `Advance` again on the next turn, so a bound costs latency and never
  // correctness.
  for (int round = 0; round < 16; ++round) {
    bool moved = false;
    moved |= PumpBodies();
    moved |= Flush();
    moved |= Read();
    if (!moved) {
      break;
    }
    any = true;
  }
  // One last flush, so the window updates and acknowledgements the frames above
  // produced go out on this turn rather than the next. A WINDOW_UPDATE held for
  // a turn is a round trip the transfer waits for.
  any |= Flush();
  return any;
}

}  // namespace microbrowser::net
