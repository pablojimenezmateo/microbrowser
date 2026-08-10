#include "net/Http2Session.h"

#include <algorithm>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

// The stream half of `Http2Session`, split from the connection half in
// `Http2Session.cpp` because the two are different jobs and the file was over
// this module's line cap -- the same split `Engine` and `Page` already carry.
//
// The division is by *whose state is being touched*. Everything here concerns
// one stream: opening it, framing a request onto it, assembling a response off
// it, and failing it without taking the connection down. The other file owns
// the socket, the frame loop, and the settings and windows that belong to the
// connection as a whole.
//
// That is not a filing convenience. A stream error costs one request and a
// connection error costs all nineteen on the same socket, and keeping the two
// kinds of failure in two files is what makes it obvious which one a new
// `return` is choosing.

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;
using namespace http2;  // NOLINT(google-build-using-namespace) — one file, one protocol

// The largest header block this browser will assemble across a HEADERS frame
// and its CONTINUATIONs.
//
// The bound is on the *encoded* bytes, which is the only quantity available
// before the block has been decoded, and it is what closes the CONTINUATION
// flood: a peer that sends an endless run of CONTINUATION frames, none of them
// carrying END_HEADERS, makes a receiver buffer forever while no frame ever
// completes. HPACK's own list bound cannot help, because nothing has been
// decoded yet.
constexpr std::size_t kMaxHeaderBlockBytes = 64 * 1024;
constexpr std::size_t kMaxContinuations = 64;

// The header fields HTTP/2 forbids outright (§8.2.2). They are the HTTP/1.1
// connection-management fields, and their presence in an HTTP/2 message is not
// a quirk to tolerate — it is the signature of a proxy or an origin that is
// translating between the two protocols without understanding either, which is
// where request smuggling lives.
bool IsConnectionSpecific(std::string_view name) {
  return util::EqualsAsciiCaseInsensitive(name, "connection") ||
         util::EqualsAsciiCaseInsensitive(name, "proxy-connection") ||
         util::EqualsAsciiCaseInsensitive(name, "keep-alive") ||
         util::EqualsAsciiCaseInsensitive(name, "transfer-encoding") ||
         util::EqualsAsciiCaseInsensitive(name, "upgrade");
}

// A field name on this wire is lowercase, by the protocol rather than by
// convention (§8.2.1). An uppercase byte means the sender is not speaking
// HTTP/2, whatever else it got right.
bool IsLowercaseFieldName(std::string_view name) {
  for (const char c : name) {
    if (c >= 'A' && c <= 'Z') {
      return false;
    }
  }
  return !name.empty();
}

bool ParseStatus(std::string_view text, int& out) {
  if (text.size() != 3) {
    return false;
  }
  int value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  out = value;
  return true;
}

bool ParseLength(std::string_view text, std::size_t& out) {
  if (text.empty() || text.size() > 19) {
    return false;
  }
  std::size_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + static_cast<std::size_t>(c - '0');
  }
  out = value;
  return true;
}

}  // namespace

Http2Session::Stream* Http2Session::Find(StreamId id) {
  for (Stream& stream : streams_) {
    if (stream.id == id) {
      return &stream;
    }
  }
  return nullptr;
}

const Http2Session::Stream* Http2Session::Find(StreamId id) const {
  return const_cast<Http2Session*>(this)->Find(id);
}

void Http2Session::Forget(StreamId id) {
  const auto end = std::remove_if(streams_.begin(), streams_.end(),
                                  [id](const Stream& stream) { return stream.id == id; });
  streams_.erase(end, streams_.end());
}

bool Http2Session::Fail(ErrorCode code, const char* reason) {
  if (error_ != nullptr) {
    return false;
  }
  error_ = reason;
  AddPerformanceCounter(PerfCounterId::NetHttp2ProtocolErrors);
  for (Stream& stream : streams_) {
    if (!stream.complete && stream.error == nullptr) {
      stream.error = reason;
    }
  }
  // A literal from this source and nothing else. The debug field goes to the
  // server, and a URL or a header value in it would be this browser telling a
  // stranger about the page.
  WriteGoAway(last_accepted_, code, reason, outgoing_);
  Flush();
  return false;
}

void Http2Session::FailStream(Stream& stream, ErrorCode code, const char* reason) {
  if (stream.error != nullptr) {
    return;
  }
  stream.error = reason;
  stream.complete = false;
  WriteRstStream(stream.id, code, outgoing_);
}

std::optional<Http2Session::StreamId> Http2Session::StartRequest(const Request& request) {
  if (Capacity() == 0) {
    return std::nullopt;
  }
  const StreamId id = next_stream_;
  next_stream_ += 2;

  // The pseudo-header fields, in order and before everything else. §8.3 is
  // explicit that they come first; a server is entitled to reject a message
  // where they do not, and the ordering is free to get right here and
  // expensive to discover in the field.
  std::vector<hpack::Header> fields;
  fields.push_back({":method", request.method});
  fields.push_back({":scheme", request.scheme});
  fields.push_back({":authority", request.authority});
  fields.push_back({":path", request.target});
  if (request.headers != nullptr) {
    for (const HttpHeaders::Field& field : request.headers->Fields()) {
      // `Host` is `:authority` on this wire and sending both is malformed;
      // the connection-management fields have no meaning here at all. Dropped
      // rather than refused, because the caller builds one header set for both
      // protocols and this is the one place that knows which one is in use.
      if (util::EqualsAsciiCaseInsensitive(field.name, "host") ||
          IsConnectionSpecific(field.name)) {
        continue;
      }
      fields.push_back({util::AsciiLowerCase(field.name), field.value});
    }
  }

  std::string block;
  hpack::Encode(fields, block);

  const bool has_body = !request.body.empty();
  // Split across CONTINUATIONs when the block does not fit one frame. Rare for
  // a request — but a page with a large cookie jar reaches it, and a browser
  // that could not send its own headers would fail exactly on the sites that
  // set the most cookies.
  const std::size_t first = std::min<std::size_t>(block.size(), peer_.max_frame_size);
  std::uint8_t flags = first == block.size() ? flag::kEndHeaders : 0;
  if (!has_body && first == block.size()) {
    flags |= flag::kEndStream;
  }
  WriteFrameHeader(FrameType::Headers, flags, id, first, outgoing_);
  outgoing_.append(block, 0, first);
  for (std::size_t at = first; at < block.size();) {
    const std::size_t take = std::min<std::size_t>(block.size() - at, peer_.max_frame_size);
    std::uint8_t more = at + take == block.size() ? flag::kEndHeaders : 0;
    WriteFrameHeader(FrameType::Continuation, more, id, take, outgoing_);
    outgoing_.append(block, at, take);
    at += take;
  }

  Stream stream;
  stream.id = id;
  stream.send_window = peer_.initial_window_size;
  stream.body.assign(request.body.begin(), request.body.end());
  streams_.push_back(std::move(stream));
  AddPerformanceCounter(PerfCounterId::NetHttp2Streams);
  return id;
}

Http2Session::StreamState Http2Session::StateOf(StreamId id) const {
  const Stream* stream = Find(id);
  if (stream == nullptr) {
    return StreamState::Unknown;
  }
  if (stream->error != nullptr) {
    return stream->refused ? StreamState::Refused : StreamState::Failed;
  }
  return stream->complete ? StreamState::Complete : StreamState::Open;
}

Http2Session::StreamAccounting Http2Session::AccountingOf(StreamId id) const {
  StreamAccounting out;
  out.state = StateOf(id);
  const Stream* stream = Find(id);
  if (stream == nullptr) {
    return out;
  }
  out.body_sent = stream->body_sent;
  out.body_total = stream->body.size();
  out.response_bytes = stream->response.body.size();
  return out;
}

const char* Http2Session::ErrorOf(StreamId id) const {
  const Stream* stream = Find(id);
  return stream != nullptr ? stream->error : error_;
}

HttpResponse Http2Session::TakeResponse(StreamId id) {
  Stream* stream = Find(id);
  if (stream == nullptr) {
    return HttpResponse{};
  }
  HttpResponse response = std::move(stream->response);
  Forget(id);
  return response;
}

void Http2Session::CloseStream(StreamId id) {
  Stream* stream = Find(id);
  if (stream == nullptr) {
    return;
  }
  if (!stream->complete && stream->error == nullptr && error_ == nullptr) {
    // The server is still sending. Telling it to stop is the difference between
    // an abort that frees the connection and one that leaves a download running
    // into a stream nobody reads.
    WriteRstStream(id, ErrorCode::Cancel, outgoing_);
    Flush();
  }
  Forget(id);
}

bool Http2Session::HandleData(const FrameHeader& header, std::span<const std::byte> payload) {
  if (header.stream == 0) {
    return Fail(ErrorCode::ProtocolError, "DATA on the connection stream");
  }
  // The *whole* payload counts against the connection window, padding
  // included, and it counts whether or not this browser wants the stream. A
  // receiver that stopped accounting for bytes it discarded would stall every
  // other stream on the connection behind a window that never reopens.
  recv_window_ -= static_cast<std::int64_t>(header.length);
  if (recv_window_ < 0) {
    return Fail(ErrorCode::FlowControlError, "the peer exceeded the connection window");
  }
  TopUpWindow(0, recv_window_, kConnectionWindow);

  std::span<const std::byte> body;
  if (!StripPadding(header, payload, body)) {
    return Fail(ErrorCode::ProtocolError, "DATA padding does not fit its frame");
  }

  Stream* stream = Find(header.stream);
  if (stream == nullptr || stream->error != nullptr) {
    return true;  // accounted for above, and dropped
  }
  if (!stream->headers_done) {
    FailStream(*stream, ErrorCode::ProtocolError, "DATA arrived before the response headers");
    return true;
  }
  stream->recv_window -= static_cast<std::int64_t>(header.length);
  if (stream->recv_window < 0) {
    FailStream(*stream, ErrorCode::FlowControlError, "the peer exceeded the stream window");
    return true;
  }
  if (stream->response.body.size() + body.size() > limits_.max_body) {
    FailStream(*stream, ErrorCode::Cancel, "the response body exceeds its bound");
    return true;
  }
  stream->response.body.insert(stream->response.body.end(), body.begin(), body.end());
  TopUpWindow(stream->id, stream->recv_window, kStreamWindow);

  if (header.Has(flag::kEndStream)) {
    if (stream->has_declared_length && stream->response.body.size() != stream->declared_length) {
      // A body that is not the length it said it was. Under HTTP/1.1 this is
      // where request smuggling starts; under HTTP/2 it cannot desynchronise
      // the connection, but it is still a response nobody should act on.
      FailStream(*stream, ErrorCode::ProtocolError, "the body is not its declared length");
      return true;
    }
    stream->complete = true;
  }
  return true;
}

bool Http2Session::HandleHeaders(const FrameHeader& header, std::span<const std::byte> payload) {
  if (header.stream == 0) {
    return Fail(ErrorCode::ProtocolError, "HEADERS on the connection stream");
  }
  if ((header.stream & 1u) == 0) {
    // Even ids are server-initiated, which means push, which is off.
    return Fail(ErrorCode::ProtocolError, "HEADERS on a server-initiated stream");
  }
  std::span<const std::byte> block;
  if (!StripPadding(header, payload, block)) {
    return Fail(ErrorCode::ProtocolError, "HEADERS padding does not fit its frame");
  }
  if (block.size() > kMaxHeaderBlockBytes) {
    return Fail(ErrorCode::FrameSizeError, "header block exceeds its bound");
  }
  assembling_ = header.stream;
  assembling_end_stream_ = header.Has(flag::kEndStream);
  continuations_ = 0;
  header_block_.assign(block.begin(), block.end());
  if (header.Has(flag::kEndHeaders)) {
    return DeliverHeaderBlock();
  }
  return true;
}

bool Http2Session::HandleContinuation(const FrameHeader& header,
                                      std::span<const std::byte> payload) {
  if (assembling_ == 0 || header.stream != assembling_) {
    return Fail(ErrorCode::ProtocolError, "CONTINUATION without a header block");
  }
  // Two bounds, and they catch different attacks. The byte bound catches one
  // enormous block; the frame-count bound catches the flood, which is an
  // unbounded run of *empty* CONTINUATIONs that adds no bytes at all and so
  // slips past a byte bound forever.
  if (++continuations_ > kMaxContinuations) {
    return Fail(ErrorCode::EnhanceYourCalm, "too many CONTINUATION frames");
  }
  if (header_block_.size() + payload.size() > kMaxHeaderBlockBytes) {
    return Fail(ErrorCode::FrameSizeError, "header block exceeds its bound");
  }
  header_block_.insert(header_block_.end(), payload.begin(), payload.end());
  if (header.Has(flag::kEndHeaders)) {
    return DeliverHeaderBlock();
  }
  return true;
}

bool Http2Session::DeliverHeaderBlock() {
  const StreamId id = assembling_;
  const bool end_stream = assembling_end_stream_;
  assembling_ = 0;
  continuations_ = 0;

  // **Decoded whatever happens to the stream.** The decoder is the
  // connection's, not the stream's: skipping a block because nobody wants it
  // leaves this side's dynamic table one entry behind the server's, and every
  // header after that on every stream is wrong rather than missing.
  std::vector<hpack::Header> fields;
  const bool decoded = decoder_.Decode(header_block_, fields);
  header_block_.clear();
  if (!decoded) {
    return Fail(ErrorCode::CompressionError,
                decoder_.Error() != nullptr ? decoder_.Error() : "HPACK failed");
  }

  Stream* stream = Find(id);
  if (stream == nullptr || stream->error != nullptr || stream->complete) {
    return true;
  }
  return ApplyHeaders(*stream, fields, end_stream);
}

bool Http2Session::ApplyHeaders(Stream& stream, const std::vector<hpack::Header>& fields,
                                bool end_stream) {
  if (stream.headers_done) {
    // Trailers. Every field must be a regular one — a pseudo-header here is
    // malformed — and the values are dropped: nothing in this browser reads a
    // trailer, and delivering them as though they were response headers would
    // let a server change `content-type` after the body it applied to.
    for (const hpack::Header& field : fields) {
      if (!field.name.empty() && field.name[0] == ':') {
        FailStream(stream, ErrorCode::ProtocolError, "a pseudo-header field in the trailers");
        return true;
      }
    }
    if (end_stream) {
      stream.complete = true;
    }
    return true;
  }

  int status = 0;
  bool saw_status = false;
  bool saw_regular = false;
  HttpResponse response;
  response.version_minor = 1;
  for (const hpack::Header& field : fields) {
    if (!field.name.empty() && field.name[0] == ':') {
      if (saw_regular) {
        FailStream(stream, ErrorCode::ProtocolError,
                   "a pseudo-header field after a regular one");
        return true;
      }
      if (field.name != ":status" || saw_status || !ParseStatus(field.value, status)) {
        // The only pseudo-header a response may carry is `:status`, exactly
        // once. Anything else is a message this browser has no agreed meaning
        // for, and guessing is how one implementation's response becomes
        // another's request.
        FailStream(stream, ErrorCode::ProtocolError, "a malformed response pseudo-header");
        return true;
      }
      saw_status = true;
      continue;
    }
    saw_regular = true;
    if (!IsLowercaseFieldName(field.name) || IsConnectionSpecific(field.name)) {
      FailStream(stream, ErrorCode::ProtocolError, "a field name HTTP/2 forbids");
      return true;
    }
    if (!response.headers.Add(field.name, field.value)) {
      // `HttpHeaders::Add` refuses a CR or an LF, which is header injection.
      // It cannot split a response on this wire the way it can on HTTP/1.1,
      // but the value would be handed to code that has no idea which protocol
      // it came over.
      FailStream(stream, ErrorCode::ProtocolError, "an invalid header value");
      return true;
    }
  }
  if (!saw_status) {
    FailStream(stream, ErrorCode::ProtocolError, "a response with no :status");
    return true;
  }

  if (status >= 100 && status < 200) {
    // Informational. `103 Early Hints` is the one that turns up in practice.
    // It is not the response — another HEADERS follows — so nothing is kept,
    // and an END_STREAM on one is malformed because it would end a message
    // that never had a final status.
    if (end_stream) {
      FailStream(stream, ErrorCode::ProtocolError, "an informational response ended the stream");
    }
    return true;
  }

  stream.response = std::move(response);
  stream.response.status = status;
  stream.headers_done = true;
  if (const std::optional<std::string_view> length = stream.response.headers.Get("content-length")) {
    if (!ParseLength(*length, stream.declared_length)) {
      FailStream(stream, ErrorCode::ProtocolError, "a malformed content-length");
      return true;
    }
    stream.has_declared_length = true;
  }
  if (end_stream) {
    if (stream.has_declared_length && stream.declared_length != 0) {
      FailStream(stream, ErrorCode::ProtocolError, "the body is not its declared length");
      return true;
    }
    stream.complete = true;
  }
  return true;
}

bool Http2Session::HandleRstStream(const FrameHeader& header,
                                   std::span<const std::byte> payload) {
  if (header.stream == 0) {
    return Fail(ErrorCode::ProtocolError, "RST_STREAM on the connection stream");
  }
  if (header.length != 4) {
    return Fail(ErrorCode::FrameSizeError, "an RST_STREAM that is not four bytes");
  }
  AddPerformanceCounter(PerfCounterId::NetHttp2StreamResets);
  Stream* stream = Find(header.stream);
  if (stream == nullptr) {
    return true;
  }
  // No RST_STREAM back: the peer already knows, and answering a reset with a
  // reset is how two implementations spend a connection talking to each other.
  const std::uint32_t code = ReadUint32(payload);
  stream->refused = code == static_cast<std::uint32_t>(ErrorCode::RefusedStream);
  stream->error = stream->refused ? "the server refused the stream"
                                  : "the server reset the stream";
  stream->complete = false;
  return true;
}

bool Http2Session::PumpBodies() {
  bool moved = false;
  const std::size_t n = streams_.size();
  if (n == 0) {
    return false;
  }
  // One DATA frame per stream per call, starting at `pump_cursor_`. A greedy
  // drain of stream[0] until the connection window is empty left later SABR
  // POSTs with `body_sent < body.size()` and no END_STREAM forever (TD-0042).
  for (std::size_t i = 0; i < n; ++i) {
    Stream& stream = streams_[(pump_cursor_ + i) % n];
    if (stream.error != nullptr || stream.body_sent >= stream.body.size()) {
      continue;
    }
    const std::int64_t remaining =
        static_cast<std::int64_t>(stream.body.size() - stream.body_sent);
    const std::int64_t allowed = std::min(
        {remaining, stream.send_window, send_window_,
         static_cast<std::int64_t>(peer_.max_frame_size)});
    if (allowed <= 0) {
      util::AddPerformanceCounter(util::PerfCounterId::NetHttp2StreamStalls);
      continue;
    }
    const auto take = static_cast<std::size_t>(allowed);
    const bool last = stream.body_sent + take == stream.body.size();
    WriteFrameHeader(FrameType::Data, last ? flag::kEndStream : 0, stream.id, take, outgoing_);
    outgoing_.append(reinterpret_cast<const char*>(stream.body.data() + stream.body_sent), take);
    stream.body_sent += take;
    stream.send_window -= allowed;
    send_window_ -= allowed;
    moved = true;
  }
  pump_cursor_ = (pump_cursor_ + 1) % n;
  return moved;
}

}  // namespace microbrowser::net
