#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/EventStream.h"
#include "net/Transport.h"
#include "privacy/Verdict.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::net {

// An `EventSource`: one HTTP GET whose response never ends.
//
// ADR 0020 §5. Structurally this is `WebSocketConnection` with a GET instead of an
// upgrade, and it shares that class's zero-idle-CPU story -- the socket is a descriptor in
// the idle wait and a server that says nothing costs nothing.
//
// **Its one genuinely new problem is the reconnect**, which is the only request in this
// browser that the user did not cause. That makes it the one place a bug becomes a
// browser hammering a server, so the bound is not a nicety:
//
//   * The delay starts at the server's `retry:` value or three seconds, **doubles on each
//     consecutive failure, and is capped**. A server that drops every connection
//     immediately therefore gets slower, not faster.
//   * After `kMaxAttempts` consecutive failures the connection **stops trying**, and stays
//     closed until the page opens another. A page that wants to keep trying forever has to
//     say so, which is a page's decision to make and not a browser's.
//   * A response that is not a 200 with `text/event-stream` is a **permanent** failure and
//     does not reconnect at all. That is the specification, and it is also the only answer
//     that cannot loop: a URL that answers 404 will answer 404 again.
//   * It stops when the document goes away, which is the destructor -- the same property
//     WebSocketConnection has and for the same reason.
class EventSourceConnection {
 public:
  enum class State : std::uint8_t {
    Connecting,
    Open,
    Waiting,  // dropped, and a reconnect is due at `RetryAtMs`
    Closed,   // permanently: refused, given up, or closed by the page
  };

  // Three seconds is what the specification suggests and what servers expect; 30 is the
  // ceiling the doubling stops at, and 6 attempts is roughly two minutes of trying before
  // a page has to ask again.
  static constexpr std::uint32_t kDefaultRetryMs = 3000;
  static constexpr std::uint32_t kMaxRetryMs = 30000;
  static constexpr int kMaxAttempts = 6;

  struct Progress {
    bool opened = false;
    bool failed = false;   // a drop, whether or not it will be retried
    bool closed = false;   // permanently: no further reconnect will happen
    std::vector<ServerSentEvent> events;
  };

  EventSourceConnection(std::unique_ptr<Transport> transport, const privacy::Verdict& verdict,
                        std::string host, std::uint16_t port, std::string target);
  EventSourceConnection(const EventSourceConnection&) = delete;
  EventSourceConnection& operator=(const EventSourceConnection&) = delete;
  ~EventSourceConnection();

  State GetState() const { return state_; }
  Progress Advance(std::int64_t now_ms);

  // The page called `close()`. No reconnect follows, which is the difference between this
  // and a drop.
  void Close();

  std::optional<util::WaitDescriptor> Interest() const;
  // When a reconnect is due, for the loop's deadline. Absent unless waiting -- so an open
  // stream contributes no deadline at all and an idle page with one still blocks.
  std::optional<std::int64_t> RetryAtMs() const;
  // Whether it is time, and the transport it needs. Separate from `Advance` because the
  // engine owns the factory: a connection that could make its own transport would be one
  // that could reconnect after the page that opened it was gone.
  bool NeedsTransport(std::int64_t now_ms) const;
  void Restart(std::unique_ptr<Transport> transport, std::int64_t now_ms);

  // The last `id:` seen, which goes back out as `Last-Event-ID`. Sticky across events that
  // carry none, which is what lets a server resume a stream where it stopped.
  const std::string& LastEventId() const { return last_event_id_; }

 private:
  void Drop(std::int64_t now_ms, Progress& progress);
  bool ReadHead();

  std::unique_ptr<Transport> transport_;
  privacy::Verdict verdict_;
  std::string host_;
  std::uint16_t port_ = 443;
  std::string target_;
  State state_ = State::Connecting;
  bool sent_request_ = false;
  bool read_head_ = false;
  std::string incoming_;
  std::string last_event_id_;
  std::uint32_t retry_ms_ = kDefaultRetryMs;
  std::int64_t retry_at_ms_ = 0;
  int attempts_ = 0;
};

}  // namespace microbrowser::net
