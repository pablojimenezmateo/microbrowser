#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/Hpack.h"
#include "net/Http2Frames.h"
#include "net/HttpMessage.h"
#include "net/Transport.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::net {

// One HTTP/2 connection, carrying many requests at once.
//
// **This is the object that makes HTTP/2 worth having, and it is also the one
// place in the network stack where two requests share state.** Under HTTP/1.1 a
// request owns its socket for the whole exchange, which is why six concurrent
// images meant six connections and six handshakes — and, on
// `upload.wikimedia.org`, six requests of which two were answered 429 (TD-0008).
// Here one socket carries all six, interleaved, and the sharing is the feature.
//
// It is also what makes this file dangerous. A demultiplexer that puts a DATA
// frame on the wrong stream serves one page's bytes into another page's
// response, and every such bug is silent. So:
//
//  - A stream is found by id or it does not exist. There is no "current
//    stream" anywhere in the implementation.
//  - A header block is *always* decoded, even for a stream that is closed,
//    unknown or was reset by us — HPACK is stateful and skipping a block puts
//    the two ends' dynamic tables permanently out of step.
//  - DATA for an unknown stream still counts against the connection's flow
//    control window, for the same reason: the window is the connection's, and a
//    receiver that stops accounting for bytes it did not want stalls the ones it
//    did.
//
// The session owns its transport and nothing else. It does not know what a URL
// is, what a cookie is, or what CORS is; `Fetch` keeps all of that and hands
// this a method, a target and a header list. That is the same seam
// `ResponseParser` sits on for HTTP/1.1.
class Http2Session {
 public:
  using StreamId = std::uint32_t;

  // What this browser advertises as its per-stream receive window, and the
  // connection window it raises itself to immediately after the preface.
  //
  // Both are far above the protocol's 65535 default, and the reason is that the
  // default makes a transfer round-trip-bound: a server may send 64KB and then
  // must wait to be told it may send more, so a 2MB script over a 40ms link
  // costs a second in nothing but waiting. These are accounting numbers rather
  // than buffers — nothing is preallocated — and the real bound on what one
  // response may hold is `HttpLimits::max_body`, which is checked as the bytes
  // arrive.
  static constexpr std::uint32_t kStreamWindow = 8u * 1024u * 1024u;
  static constexpr std::uint32_t kConnectionWindow = 32u * 1024u * 1024u;

  // One request, already decided. Strings rather than a URL because this module
  // may not see `url` — and should not: every question about who may ask for
  // this was answered before it got here.
  //
  // **Owned strings rather than views, and that is not a style preference.**
  // Three of the four are *built* by the caller — the authority is host plus
  // port, the target is path plus query — so a view field invites
  // `request.authority = AuthorityFor(url)`, which binds to a temporary that
  // dies at the semicolon. That is precisely the bug this browser shipped for
  // one commit: every server on the web reset the stream or answered 400,
  // because `:authority` was whatever the stack slot held next. Four small
  // allocations per request, against a network round trip, buys the class of
  // bug being impossible instead of merely absent.
  struct Request {
    std::string method;
    std::string scheme;
    std::string authority;
    std::string target;
    // Borrowed, and obviously so: a pointer, to something the caller keeps for
    // the length of the call.
    const HttpHeaders* headers = nullptr;
    std::span<const std::byte> body;
  };

  enum class StreamState : std::uint8_t {
    Open,      // still going
    Complete,  // a whole response is waiting to be taken
    Failed,    // this stream is over; `ErrorOf` says why
    // Failed, and the server said it never processed the request:
    // REFUSED_STREAM, or a stream past the last one a GOAWAY promised. Its own
    // state rather than a flag beside `Failed`, because it is the only failure
    // after which sending the request again is safe -- and "is this safe to
    // repeat" is not a question to answer by matching on an error string.
    Refused,
    Unknown,   // never opened, or already taken
  };

  // Takes a transport that is already open and already past its TLS handshake,
  // and one whose ALPN said `h2`. Being handed a connected socket rather than
  // connecting itself is what keeps the coalescing decision — "is anybody
  // already opening one of these?" — in the pool, where the partition key is.
  explicit Http2Session(std::unique_ptr<Transport> connection);
  ~Http2Session();

  Http2Session(const Http2Session&) = delete;
  Http2Session& operator=(const Http2Session&) = delete;

  // Opens a stream and queues the request on it. Nothing when this session
  // cannot take another: at the peer's concurrency limit, past a GOAWAY, out of
  // stream ids, or already broken.
  std::optional<StreamId> StartRequest(const Request& request);

  // Carries every stream forward as far as the socket allows, without blocking.
  // True when anything moved, which is what an inactivity deadline is measured
  // against.
  bool Advance();

  // True when the last `Advance` stopped because the transport had nothing.
  bool IsBlocked() const { return blocked_; }

  StreamState StateOf(StreamId id) const;
  // Per-stream accounting for stall detection. Multiplexed `Advance()` can move
  // other streams while this one is stuck behind a send window — RequestQueue
  // must not treat that as progress for *this* request (TD-0042).
  struct StreamAccounting {
    std::size_t body_sent = 0;
    std::size_t body_total = 0;
    std::size_t response_bytes = 0;
    StreamState state = StreamState::Unknown;
  };
  StreamAccounting AccountingOf(StreamId id) const;
  // Null unless that stream failed. A literal, so it can be copied into a
  // FetchResult without an allocation on the failure path.
  const char* ErrorOf(StreamId id) const;
  // Only valid once `StateOf` says Complete. Taking it forgets the stream.
  HttpResponse TakeResponse(StreamId id);

  // Forgets a stream, and tells the server to stop if it was still open. Called
  // when a request is abandoned — an `AbortController`, or a navigation.
  void CloseStream(StreamId id);

  // True when a new request may be started on it. False makes the pool open a
  // second connection, which is right for a session that has been told to go
  // away and wrong for one that is merely busy — so "busy" is not one of the
  // reasons this answers false, `Capacity` is.
  bool IsUsable() const;
  // How many more streams this session would accept right now.
  std::size_t Capacity() const;
  std::size_t OpenStreams() const { return streams_.size(); }
  bool Failed() const { return error_ != nullptr; }
  const char* Error() const { return error_; }

  std::optional<util::WaitDescriptor> Interest() const;

 private:
  // One request and the response coming back for it. `send_window` and
  // `recv_window` are per stream *and* the connection has its own pair, which
  // is the shape of RFC 9113 §6.9 and the reason a large download does not
  // starve a small one.
  struct Stream {
    StreamId id = 0;
    HttpResponse response;
    // The body still to go out, and how much of it has.
    std::vector<std::byte> body;
    std::size_t body_sent = 0;
    std::int64_t send_window = 65535;
    std::int64_t recv_window = kStreamWindow;
    std::size_t declared_length = 0;
    bool has_declared_length = false;
    bool headers_done = false;
    bool complete = false;
    // Set with `error` when the server said this request was never processed.
    bool refused = false;
    const char* error = nullptr;
  };

  Stream* Find(StreamId id);
  const Stream* Find(StreamId id) const;
  void Forget(StreamId id);

  // A connection error: everything on it fails, a GOAWAY goes out, and no
  // further frame is read. Returns false so a caller can `return Fail(...)`.
  bool Fail(http2::ErrorCode code, const char* reason);
  // A stream error: only that request fails, and the peer is told with
  // RST_STREAM. The connection stays up, which is the whole difference between
  // the two and the reason one bad response does not cost a page its other
  // twenty.
  void FailStream(Stream& stream, http2::ErrorCode code, const char* reason);

  bool Flush();
  bool Read();
  bool ConsumeFrames();
  bool HandleFrame(const http2::FrameHeader& header, std::span<const std::byte> payload);
  bool HandleData(const http2::FrameHeader& header, std::span<const std::byte> payload);
  bool HandleHeaders(const http2::FrameHeader& header, std::span<const std::byte> payload);
  bool HandleContinuation(const http2::FrameHeader& header, std::span<const std::byte> payload);
  bool HandleSettings(const http2::FrameHeader& header, std::span<const std::byte> payload);
  bool HandleWindowUpdate(const http2::FrameHeader& header, std::span<const std::byte> payload);
  bool HandleRstStream(const http2::FrameHeader& header, std::span<const std::byte> payload);
  bool HandleGoAway(std::span<const std::byte> payload);
  // Decodes the assembled block and applies it to its stream. Always called,
  // even when the stream is gone, because the decoder is the connection's.
  bool DeliverHeaderBlock();
  // Turns a decoded field list into a response, or says why it is malformed.
  bool ApplyHeaders(Stream& stream, const std::vector<hpack::Header>& fields, bool end_stream);
  // Moves as much of every stream's body onto the wire as the two windows allow.
  bool PumpBodies();
  // Gives back what has been consumed, once it is worth a frame.
  void TopUpWindow(StreamId id, std::int64_t& window, std::uint32_t target);

  std::unique_ptr<Transport> connection_;
  hpack::Decoder decoder_;
  http2::PeerSettings peer_;
  HttpLimits limits_;

  std::string outgoing_;
  std::size_t sent_ = 0;
  std::vector<std::byte> incoming_;

  std::vector<Stream> streams_;
  // The header block being assembled across HEADERS and its CONTINUATIONs, and
  // whose it is. While this is non-zero **no other frame may arrive** — §6.10
  // — and enforcing that is also what stops a CONTINUATION flood: an endless
  // run of empty CONTINUATION frames that a receiver keeps accepting is a
  // denial of service with no frame ever completing.
  StreamId assembling_ = 0;
  std::vector<std::byte> header_block_;
  bool assembling_end_stream_ = false;
  std::size_t continuations_ = 0;

  std::int64_t send_window_ = 65535;
  std::int64_t recv_window_ = 65535;
  StreamId next_stream_ = 1;
  // Round-robin cursor for `PumpBodies`: without it, earlier streams keep the
  // connection send window and later SABR POSTs never get END_STREAM (TD-0042).
  std::size_t pump_cursor_ = 0;
  // The last stream the server promised to process, from a GOAWAY. Once this is
  // set no new stream may be opened, but the ones already running finish — that
  // is what makes a graceful shutdown graceful.
  StreamId last_accepted_ = 0;
  bool going_away_ = false;
  bool blocked_ = false;
  const char* error_ = nullptr;
};

}  // namespace microbrowser::net
