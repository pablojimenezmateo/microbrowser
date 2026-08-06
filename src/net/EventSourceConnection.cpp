#include "net/EventSourceConnection.h"

#include <algorithm>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/UserAgent.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// How much of a response head, and how much unparsed body, may pile up. Both are bounds on
// memory the server controls; the body bound is generous because an event may legitimately
// be large, and `ParseEventStream` has its own per-event ceiling underneath it.
constexpr std::size_t kMaxHead = 64u * 1024u;
constexpr std::size_t kMaxBody = 8u * 1024u * 1024u;

}  // namespace

EventSourceConnection::EventSourceConnection(std::unique_ptr<Transport> transport,
                                             const privacy::Verdict& verdict, std::string host,
                                             std::uint16_t port, std::string target)
    : transport_(std::move(transport)),
      verdict_(verdict),
      host_(std::move(host)),
      port_(port),
      target_(std::move(target)) {
  if (transport_ == nullptr ||
      !transport_->StartConnect(verdict_.Partition().Serialize(), host_, port_, true)) {
    state_ = State::Closed;
    return;
  }
  AddPerformanceCounter(PerfCounterId::EventSourceConnectionsOpened);
}

EventSourceConnection::~EventSourceConnection() {
  if (transport_ != nullptr) {
    transport_->Close();
  }
}

std::optional<util::WaitDescriptor> EventSourceConnection::Interest() const {
  if (state_ == State::Closed || state_ == State::Waiting || transport_ == nullptr) {
    return std::nullopt;
  }
  return transport_->Interest();
}

std::optional<std::int64_t> EventSourceConnection::RetryAtMs() const {
  if (state_ != State::Waiting) {
    return std::nullopt;
  }
  return retry_at_ms_;
}

bool EventSourceConnection::NeedsTransport(std::int64_t now_ms) const {
  return state_ == State::Waiting && now_ms >= retry_at_ms_;
}

void EventSourceConnection::Close() {
  state_ = State::Closed;
  if (transport_ != nullptr) {
    transport_->Close();
  }
}

void EventSourceConnection::Drop(std::int64_t now_ms, Progress& progress) {
  if (transport_ != nullptr) {
    transport_->Close();
    transport_.reset();
  }
  incoming_.clear();
  read_head_ = false;
  sent_request_ = false;
  progress.failed = true;
  ++attempts_;
  if (attempts_ >= kMaxAttempts) {
    // Stops trying, and stays stopped. A page that wants to keep trying forever has to
    // say so -- which is a page's decision and not a browser's, and is the difference
    // between a reconnect and a retry storm.
    state_ = State::Closed;
    progress.closed = true;
    AddPerformanceCounter(PerfCounterId::EventSourceGaveUp);
    return;
  }
  state_ = State::Waiting;
  // Doubling from the current delay, capped. A server that drops every connection
  // immediately gets slower rather than faster.
  const std::uint64_t doubled = static_cast<std::uint64_t>(retry_ms_) * 2u;
  retry_at_ms_ = now_ms + static_cast<std::int64_t>(retry_ms_);
  retry_ms_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(doubled, kMaxRetryMs));
  AddPerformanceCounter(PerfCounterId::EventSourceReconnects);
}

void EventSourceConnection::Restart(std::unique_ptr<Transport> transport, std::int64_t now_ms) {
  (void)now_ms;
  if (state_ != State::Waiting) {
    return;
  }
  transport_ = std::move(transport);
  if (transport_ == nullptr ||
      !transport_->StartConnect(verdict_.Partition().Serialize(), host_, port_, true)) {
    state_ = State::Closed;
    return;
  }
  state_ = State::Connecting;
}

bool EventSourceConnection::ReadHead() {
  const std::size_t end = incoming_.find("\r\n\r\n");
  if (end == std::string::npos) {
    return incoming_.size() <= kMaxHead;
  }
  const std::string_view head(incoming_.data(), end);
  int status = 0;
  std::string content_type;
  std::size_t at = 0;
  bool first = true;
  while (at <= head.size()) {
    const std::size_t line_end = std::min(head.find("\r\n", at), head.size());
    const std::string_view line = head.substr(at, line_end - at);
    if (first) {
      const std::size_t space = line.find(' ');
      for (std::size_t i = space + 1; space != std::string_view::npos && i < line.size() &&
                                      line[i] >= '0' && line[i] <= '9';
           ++i) {
        status = status * 10 + (line[i] - '0');
      }
      first = false;
    } else if (const std::size_t colon = line.find(':'); colon != std::string_view::npos) {
      if (util::EqualsAsciiCaseInsensitive(line.substr(0, colon), "content-type")) {
        content_type = util::AsciiLowerCase(std::string(util::TrimAscii(line.substr(colon + 1))));
      }
    }
    if (line_end >= head.size()) {
      break;
    }
    at = line_end + 2;
  }
  incoming_.erase(0, end + 4);
  read_head_ = true;
  // A 200 with `text/event-stream`, or nothing. Anything else is a **permanent** failure
  // that does not reconnect: the specification says so, and it is the only answer that
  // cannot loop -- a URL that answers 404 will answer 404 again, and retrying it six times
  // with backoff is six requests nobody asked for.
  if (status != 200 || content_type.rfind("text/event-stream", 0) != 0) {
    AddPerformanceCounter(PerfCounterId::EventSourceRefusals);
    state_ = State::Closed;
    return false;
  }
  state_ = State::Open;
  return true;
}

EventSourceConnection::Progress EventSourceConnection::Advance(std::int64_t now_ms) {
  Progress progress;
  if (state_ == State::Closed || state_ == State::Waiting || transport_ == nullptr) {
    return progress;
  }

  const IoStatus ready = transport_->Advance();
  if (ready == IoStatus::Failed) {
    Drop(now_ms, progress);
    return progress;
  }
  if (ready == IoStatus::Blocked) {
    return progress;
  }

  if (!sent_request_) {
    std::string request = "GET " + target_ + " HTTP/1.1\r\n";
    request += "Host: " + host_ + "\r\n";
    // The two headers that make this an event stream rather than a document: the type it
    // will accept, and the promise not to hand it a cached copy of a stream.
    request += "Accept: text/event-stream\r\n";
    request += "Cache-Control: no-cache\r\n";
    if (!last_event_id_.empty()) {
      // Where the last connection stopped. This is what makes a reconnect a *resume*
      // rather than a replay, and it is the whole reason the id is sticky.
      request += "Last-Event-ID: " + last_event_id_ + "\r\n";
    }
    request += "User-Agent: " + std::string(util::kUserAgent) + "\r\n";
    request += "Connection: keep-alive\r\n\r\n";
    const IoResult sent = transport_->Send(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(request.data()),
                                   request.size()));
    if (sent.status != IoStatus::Ready) {
      Drop(now_ms, progress);
      return progress;
    }
    sent_request_ = true;
  }

  bool peer_closed = false;
  std::byte chunk[8192];
  while (true) {
    const IoResult read = transport_->Receive(chunk);
    if (read.status == IoStatus::Blocked) {
      break;
    }
    if (read.status == IoStatus::Closed) {
      peer_closed = true;
      break;
    }
    if (read.status != IoStatus::Ready || read.bytes == 0 ||
        incoming_.size() + read.bytes > kMaxBody) {
      Drop(now_ms, progress);
      return progress;
    }
    incoming_.append(reinterpret_cast<const char*>(chunk), read.bytes);
  }

  // Everything that arrived is parsed before the close is acted on, for the reason
  // WebSocketConnection does the same: a server may answer and hang up in one packet, and
  // returning on the close would lose every event in it.
  const bool had_head = read_head_;
  if (!read_head_ && !ReadHead()) {
    if (state_ == State::Closed) {
      progress.failed = true;
      progress.closed = true;
      return progress;
    }
  }
  if (state_ == State::Open) {
    // `open` fires on the turn the head was accepted, and only then -- including after a
    // reconnect, which is what tells a page the stream is back.
    progress.opened = !had_head && read_head_;
    const EventStreamResult parsed = ParseEventStream(incoming_);
    if (parsed.retry_ms.has_value()) {
      // The server's only say over the delay, and it resets the backoff: a stream that
      // ran fine and then dropped should not inherit the doubling from an earlier failure.
      retry_ms_ = std::min(*parsed.retry_ms, kMaxRetryMs);
    }
    for (const ServerSentEvent& event : parsed.events) {
      if (event.id.has_value()) {
        last_event_id_ = *event.id;
      }
      progress.events.push_back(event);
    }
    incoming_.erase(0, parsed.consumed);
    if (attempts_ != 0 && !parsed.events.empty()) {
      // A stream that delivered something is a working stream: the consecutive-failure
      // count is about a server that cannot hold a connection, not about one that
      // eventually drops a healthy one.
      attempts_ = 0;
    }
  }

  if (peer_closed && state_ != State::Closed) {
    Drop(now_ms, progress);
  }
  return progress;
}

}  // namespace microbrowser::net
