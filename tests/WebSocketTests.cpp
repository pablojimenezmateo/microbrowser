// The WebSocket wire format and its handshake.
//
// ADR 0020 §5. RFC 6455's own examples are the fixtures where it has them -- §1.3's
// handshake key and §5.7's frames -- because a codec checked only against itself is a
// codec that agrees with itself.

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <utility>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "engine/Engine.h"
#include "gfx/FontCatalog.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "net/EventSourceConnection.h"
#include "net/EventStream.h"
#include "net/WebSocketConnection.h"
#include "net/WebSocketFrames.h"
#include "privacy/PrivacyPolicy.h"
#include "support/DriveLoop.h"
#include "support/ScriptedTransport.h"
#include "support/SyntheticFont.h"
#include "url/Url.h"
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

// A connection over a scripted transport. The handshake response has to carry the
// accept value for the key the connection sent, so the key is fixed and the accept is
// computed rather than pasted -- a test with a hard-coded pair would pass with a broken
// digest.
constexpr std::string_view kKey = "dGhlIHNhbXBsZSBub25jZQ==";

std::string Handshake(std::string_view extra = {}) {
  std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                         "Connection: Upgrade\r\nSec-WebSocket-Accept: ";
  response += net::WebSocketAcceptFor(kKey);
  response += "\r\n\r\n";
  response += extra;
  return response;
}

std::string Frame(std::initializer_list<std::uint8_t> bytes) {
  std::string out;
  for (const std::uint8_t byte : bytes) {
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

// A transport that stays open.
//
// The shared `ScriptedTransport` hangs up as soon as its canned response has been read,
// which is a real server behaviour and the wrong one for testing a connection whose
// whole point is to *stay* open: with it, the socket is Closed by the end of the first
// `Advance` and nothing about `send`, `close` or a second message can be observed. So
// this one answers `Blocked` when it has nothing more to give and keeps everything the
// connection wrote.
class OpenTransport : public net::Transport {
 public:
  // What a test can still look at after this object is gone.
  //
  // A closed socket is usually a *destroyed* one here -- the connection owns its
  // transport and a navigation erases the connection -- so a test holding a raw pointer
  // reads freed memory, which is how ASan caught the first version of this. The state is
  // shared and outlives the transport, and `destroyed` counts as closed because the
  // destructor closes.
  struct Observed {
    bool closed = false;
    bool destroyed = false;

    bool IsClosed() const { return closed || destroyed; }
  };

  explicit OpenTransport(std::shared_ptr<Observed> observed = nullptr)
      : observed_(std::move(observed)) {}

  ~OpenTransport() override {
    if (observed_ != nullptr) {
      observed_->destroyed = true;
    }
  }

  bool StartConnect(std::string_view, std::uint16_t, bool) override { return true; }
  net::IoStatus Advance() override { return net::IoStatus::Ready; }

  net::IoResult Send(std::span<const std::byte> data) override {
    written_.append(reinterpret_cast<const char*>(data.data()), data.size());
    return net::IoResult{net::IoStatus::Ready, data.size()};
  }

  net::IoResult Receive(std::span<std::byte> out) override {
    if (closed_) {
      return net::IoResult{net::IoStatus::Closed, 0};
    }
    // A *server* answers the key it was actually sent. The engine generates its own
    // `Sec-WebSocket-Key` per socket, so a fixture with a hard-coded accept value tests
    // nothing but its own paste -- and fails, which is how this was found.
    if (answer_handshake_ && pending_.empty()) {
      const std::size_t at = written_.find("Sec-WebSocket-Key: ");
      const std::size_t end = at == std::string::npos ? at : written_.find("\r\n", at);
      if (end != std::string::npos) {
        const std::string key = written_.substr(at + 19, end - at - 19);
        pending_ = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                   net::WebSocketAcceptFor(key) + "\r\n\r\n" + after_handshake_;
        answer_handshake_ = false;
      }
    }
    if (pending_.empty()) {
      // Blocked, not Closed: the connection is open and the server has not spoken yet,
      // which is exactly the state an idle WebSocket spends its life in.
      return net::IoResult{net::IoStatus::Blocked, 0};
    }
    const std::size_t take = std::min(out.size(), pending_.size());
    std::memcpy(out.data(), pending_.data(), take);
    pending_.erase(0, take);
    return net::IoResult{net::IoStatus::Ready, take};
  }

  void Close() override {
    closed_ = true;
    if (observed_ != nullptr) {
      observed_->closed = true;
    }
  }

  std::optional<util::WaitDescriptor> Interest() const override {
    if (closed_) {
      return std::nullopt;
    }
    util::WaitDescriptor descriptor;
    descriptor.descriptor = 7;  // a number, not a socket: nothing here waits on it
    descriptor.readable = true;
    return descriptor;
  }

  // What the server says next, appended so a test can deliver in stages.
  void Deliver(std::string_view bytes) { pending_ += bytes; }
  // Answer whatever key this connection sends, then these bytes. What a server does.
  void AnswerHandshake(std::string after) {
    answer_handshake_ = true;
    after_handshake_ = std::move(after);
  }
  const std::string& Written() const { return written_; }
  bool IsClosed() const { return closed_; }

 private:
  std::string pending_;
  std::string written_;
  std::string after_handshake_;
  std::shared_ptr<Observed> observed_;
  bool answer_handshake_ = false;
  bool closed_ = false;
};

struct LiveSocket {
  OpenTransport* transport = nullptr;
  std::unique_ptr<net::WebSocketConnection> connection;

  explicit LiveSocket(std::string_view first) {
    auto owned = std::make_unique<OpenTransport>();
    transport = owned.get();
    transport->Deliver(first);
    const std::optional<url::Url> url = url::Url::Parse("wss://chat.example/socket");
    privacy::PrivacyPolicy policy;
    privacy::Request request;
    request.url = *url;
    request.container = url::ContainerId::Default();
    request.top_level_site = url::Site::FromUrl(*url);
    request.type = privacy::ResourceType::Other;
    connection = std::make_unique<net::WebSocketConnection>(
        std::move(owned), policy.Decide(request), "chat.example", 443, "/socket", true,
        std::string(kKey));
  }
};

// A page that opens a socket, over a factory whose transports stay open.
//
// The document response comes first and then every socket takes an `OpenTransport`, so
// the page's `WebSocket` behaves the way one does on a real network: it opens, it waits,
// and it is still there on the next turn.
class PageFactory : public net::TransportFactory {
 public:
  std::unique_ptr<net::Transport> Create() override {
    if (!document_taken_) {
      document_taken_ = true;
      auto scripted = std::make_unique<OpenTransport>();
      scripted->Deliver(document_);
      return scripted;
    }
    auto observed = std::make_shared<OpenTransport::Observed>();
    auto connection = std::make_unique<OpenTransport>(observed);
    if (stream_.empty()) {
      connection->AnswerHandshake(frames_);
    } else {
      // An event stream answers with an ordinary HTTP response rather than an upgrade, so
      // there is no key to compute anything from.
      connection->Deliver(stream_);
    }
    sockets_.push_back(std::move(observed));
    return connection;
  }

  void SetDocument(std::string html) {
    document_ = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                std::to_string(html.size()) + "\r\n\r\n" + html;
  }
  // What the socket says *after* its handshake. The handshake itself is computed from
  // the key the connection sent, which is what a server does.
  void SetFrames(std::string frames) { frames_ = std::move(frames); }
  // For an `EventSource`: a whole HTTP response, since a stream has no handshake.
  void SetStream(std::string stream) { stream_ = std::move(stream); }
  const std::vector<std::shared_ptr<OpenTransport::Observed>>& Sockets() const {
    return sockets_;
  }

 private:
  std::string document_;
  std::string frames_;
  std::string stream_;
  bool document_taken_ = false;
  std::vector<std::shared_ptr<OpenTransport::Observed>> sockets_;
};

struct Socket {
  ScriptedFactory factory;
  std::unique_ptr<net::WebSocketConnection> connection;

  // Everything this connection wrote, in order. The harness gives a write after the
  // canned response its own log slot, so the frames a socket sends later -- a pong, a
  // close, a message -- are in the second one.
  std::string Written() const {
    std::string joined;
    for (const std::string& written : factory.log.requests) {
      joined += written;
    }
    return joined;
  }

  explicit Socket(std::string_view response) {
    factory.script.push_back(
        ScriptedTransport::Exchange{"chat.example", 443, true, std::string(response)});
    // A second, empty exchange so that writes *after* the canned response -- a pong, a
    // close frame, a message the page sent -- are absorbed rather than read as a
    // connection that has run out of script. A real socket takes writes for as long as
    // it is open; this is the harness's way of saying the same thing.
    factory.script.push_back(ScriptedTransport::Exchange{"", 0, true, std::string()});
    const std::optional<url::Url> url = url::Url::Parse("wss://chat.example/socket");
    privacy::PrivacyPolicy policy;
    privacy::Request request;
    request.url = *url;
    request.container = url::ContainerId::Default();
    request.top_level_site = url::Site::FromUrl(*url);
    request.type = privacy::ResourceType::Other;
    const privacy::Verdict verdict = policy.Decide(request);
    connection = std::make_unique<net::WebSocketConnection>(
        factory.Create(), verdict, "chat.example", 443, "/socket", true, std::string(kKey));
  }
};

}  // namespace

namespace {

// An EventSource over a transport that stays open, for the reason a WebSocket needs one.
struct LiveStream {
  std::shared_ptr<OpenTransport::Observed> observed = std::make_shared<OpenTransport::Observed>();
  OpenTransport* transport = nullptr;
  std::unique_ptr<net::EventSourceConnection> connection;

  explicit LiveStream(std::string_view first) {
    auto owned = std::make_unique<OpenTransport>(observed);
    transport = owned.get();
    transport->Deliver(first);
    const std::optional<url::Url> url = url::Url::Parse("https://feed.example/events");
    privacy::PrivacyPolicy policy;
    privacy::Request request;
    request.url = *url;
    request.container = url::ContainerId::Default();
    request.top_level_site = url::Site::FromUrl(*url);
    request.type = privacy::ResourceType::Other;
    connection = std::make_unique<net::EventSourceConnection>(
        std::move(owned), policy.Decide(request), "feed.example", 443, "/events");
  }

  // A fresh transport for a reconnect, which the engine would otherwise supply.
  OpenTransport* Reconnect(std::string_view bytes, std::int64_t now_ms) {
    auto owned = std::make_unique<OpenTransport>();
    OpenTransport* raw = owned.get();
    raw->Deliver(bytes);
    connection->Restart(std::move(owned), now_ms);
    return raw;
  }
};

std::string StreamHead(std::string_view body) {
  return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n") +
         std::string(body);
}

}  // namespace

void RegisterWebSocketTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EventSource/OpensOnATwoHundredEventStreamAndDeliversEvents", [] {
    LiveStream stream(StreamHead("data: first\n\n"));
    const net::EventSourceConnection::Progress progress = stream.connection->Advance(0);
    Expect(stream.connection->GetState() == net::EventSourceConnection::State::Open, "open");
    ExpectEqInt(static_cast<long long>(progress.events.size()), 1, "one event");
    ExpectEqString(progress.events.at(0).data, "first", "with its data");
    // The two headers that make this a stream request rather than a document one.
    Expect(stream.transport->Written().find("Accept: text/event-stream") != std::string::npos,
           "it asked for a stream");
    Expect(stream.transport->Written().find("Cache-Control: no-cache") != std::string::npos,
           "and promised not to take a cached one");
    // Still open with nothing outstanding, and contributing no deadline: an idle stream
    // costs a descriptor and nothing else.
    Expect(stream.connection->Interest().has_value(), "it waits on its socket");
    Expect(!stream.connection->RetryAtMs().has_value(), "and asks for no timer");
  });

  AddTest(tests, "EventSource/AnythingButATwoHundredEventStreamIsPermanent", [] {
    // A 404 will answer 404 again, so retrying it six times with backoff is six requests
    // nobody asked for. The specification makes this failure permanent and so does this.
    LiveStream missing("HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n");
    const net::EventSourceConnection::Progress progress = missing.connection->Advance(0);
    Expect(progress.failed && progress.closed, "failed, permanently");
    Expect(!missing.connection->RetryAtMs().has_value(), "with no reconnect scheduled");
    // A 200 with the wrong type is the same answer: a page that gets HTML where it asked
    // for a stream is a page whose URL is wrong, not a page with a flaky server.
    LiveStream wrong_type("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\ndata: x\n\n");
    Expect(wrong_type.connection->Advance(0).closed, "and so is the wrong content type");
  });

  AddTest(tests, "EventSource/ADropBacksOffAndResumesFromTheLastEventId", [] {
    // The only request in this browser the user did not cause, so this is the test that
    // matters: the delay doubles, and the reconnect carries `Last-Event-ID`.
    LiveStream stream(StreamHead("id: 42\ndata: one\n\n"));
    stream.connection->Advance(0);
    ExpectEqString(stream.connection->LastEventId(), "42", "the id is remembered");
    // The transport hangs up: OpenTransport closes when told, and a closed transport
    // reports Closed.
    stream.transport->Close();
    const net::EventSourceConnection::Progress dropped = stream.connection->Advance(1000);
    Expect(dropped.failed && !dropped.closed, "a drop is a failure that will be retried");
    Expect(stream.connection->RetryAtMs().has_value(), "with a time to try again");
    ExpectEqInt(static_cast<long long>(*stream.connection->RetryAtMs()), 4000,
                "three seconds after the drop, which is the specification's default");
    Expect(!stream.connection->NeedsTransport(3999), "not before then");
    Expect(stream.connection->NeedsTransport(4000), "and then yes");

    OpenTransport* second = stream.Reconnect(StreamHead("data: two\n\n"), 4000);
    const net::EventSourceConnection::Progress resumed = stream.connection->Advance(4000);
    ExpectEqInt(static_cast<long long>(resumed.events.size()), 1, "the stream resumed");
    Expect(second->Written().find("Last-Event-ID: 42") != std::string::npos,
           "and told the server where it stopped, which is what makes it a resume");
  });

  AddTest(tests, "EventSource/GivesUpAfterABoundedNumberOfFailures", [] {
    // A server that drops every connection immediately gets slower and then gets left
    // alone. Without the cap this is a browser hammering a server on a page nobody is
    // looking at.
    LiveStream stream("");
    // A server that hangs up the instant it is asked: the transport is closed before the
    // connection ever reads from it.
    stream.transport->Close();
    std::vector<std::int64_t> delays;
    std::int64_t now = 0;
    for (int attempt = 0; attempt < net::EventSourceConnection::kMaxAttempts + 2; ++attempt) {
      const net::EventSourceConnection::Progress progress = stream.connection->Advance(now);
      if (progress.closed) {
        break;
      }
      if (const std::optional<std::int64_t> at = stream.connection->RetryAtMs()) {
        delays.push_back(*at - now);
        now = *at;
        stream.Reconnect("", now)->Close();
      }
    }
    Expect(stream.connection->GetState() == net::EventSourceConnection::State::Closed,
           "it stopped trying");
    Expect(delays.size() >= 3, "after several attempts");
    Expect(delays.at(1) > delays.at(0) && delays.at(2) > delays.at(1),
           "each wait longer than the last, which is what backoff means");
    Expect(delays.back() <= net::EventSourceConnection::kMaxRetryMs,
           "and never longer than the cap");
  });

  AddTest(tests, "EventSource/AServersRetryFieldSetsTheDelay", [] {
    // The server's only say over it, and it resets the backoff -- a stream that ran fine
    // and then dropped should not inherit a doubling from an earlier failure.
    LiveStream stream(StreamHead("retry: 500\ndata: x\n\n"));
    stream.connection->Advance(0);
    stream.transport->Close();
    stream.connection->Advance(100);
    ExpectEqInt(static_cast<long long>(*stream.connection->RetryAtMs()), 600,
                "half a second after the drop, because the server asked for that");
  });

  AddTest(tests, "EventSource/ClosingIsNotADropAndScheduesNothing", [] {
    LiveStream stream(StreamHead("data: x\n\n"));
    stream.connection->Advance(0);
    stream.connection->Close();
    Expect(stream.connection->GetState() == net::EventSourceConnection::State::Closed, "closed");
    Expect(!stream.connection->RetryAtMs().has_value(),
           "and nothing is scheduled: a page that closed a stream did not lose one");
    Expect(!stream.connection->Interest().has_value(), "with nothing left in the wait");
  });

  AddTest(tests, "EventStream/AnEventIsFieldsUntilABlankLineAndNotALine", [] {
    // The rule the whole format turns on: fields accumulate and a *blank line*
    // dispatches. Three `data:` lines are one event with two newlines in it.
    const net::EventStreamResult result = net::ParseEventStream(
        "event: tick\ndata: one\ndata: two\nid: 7\n\ndata: next\n\n");
    ExpectEqInt(static_cast<long long>(result.events.size()), 2, "two events");
    ExpectEqString(result.events.at(0).type, "tick", "the type");
    ExpectEqString(result.events.at(0).data, "one\ntwo", "joined by a newline");
    ExpectEqString(result.events.at(0).id.value_or(""), "7", "and the id");
    ExpectEqString(result.events.at(1).data, "next", "then the second");
    Expect(result.events.at(1).type.empty(),
           "whose type is empty, which the binding turns into `message`");
  });

  AddTest(tests, "EventStream/APartialEventIsKeptRatherThanDispatched", [] {
    // What `consumed` is for. A packet boundary inside an event must not deliver half a
    // message, and the bytes after the last blank line are the caller's to keep.
    const net::EventStreamResult result = net::ParseEventStream("data: whole\n\ndata: par");
    ExpectEqInt(static_cast<long long>(result.events.size()), 1, "one whole event");
    // "data: whole\n\n" is thirteen bytes; everything after it is the caller's to keep.
    ExpectEqInt(static_cast<long long>(result.consumed), 13,
                "and the partial one is left in the buffer");
  });

  AddTest(tests, "EventStream/CommentsAndKeepAlivesCostNothing", [] {
    // A server holds a stream open for hours with `:keep-alive` comments, and a
    // dispatch with no `data` field fires nothing -- which is the difference between a
    // keep-alive and a message, and the reason a long-lived stream is not a message
    // storm.
    const net::EventStreamResult result =
        net::ParseEventStream(":keep-alive\n\n: another\n\ndata: real\n\n");
    ExpectEqInt(static_cast<long long>(result.events.size()), 1, "only the real one");
    ExpectEqString(result.events.at(0).data, "real", "with its data");
  });

  AddTest(tests, "EventStream/OneSpaceIsStrippedAndOnlyOne", [] {
    // `data:  x` is " x". Stripping both corrupts every payload a server indents, which
    // is most JSON pretty-printed onto a stream.
    const net::EventStreamResult result =
        net::ParseEventStream("data:  indented\n\ndata:tight\n\ndata\n\n");
    ExpectEqInt(static_cast<long long>(result.events.size()), 3, "three events");
    ExpectEqString(result.events.at(0).data, " indented", "one space stripped, one kept");
    ExpectEqString(result.events.at(1).data, "tight", "no space to strip");
    Expect(result.events.at(2).data.empty(),
           "and a field with no colon is an empty value rather than an error");
  });

  AddTest(tests, "EventStream/EveryLineTerminatorIsOneTerminator", [] {
    // A stream may mix `\r\n`, `\n` and `\r`. Splitting `\r\n` into two would produce a
    // spurious blank line -- which in this format means *dispatch*, so it would fire an
    // event per line.
    const net::EventStreamResult result =
        net::ParseEventStream("data: a\r\ndata: b\r\n\r\ndata: c\rdata: d\r\r");
    ExpectEqInt(static_cast<long long>(result.events.size()), 2, "two events, not four");
    ExpectEqString(result.events.at(0).data, "a\nb", "crlf joined one event");
    ExpectEqString(result.events.at(1).data, "c\nd", "and so did bare cr");
  });

  AddTest(tests, "EventStream/RetryIsTheServersOnlySayOverReconnecting", [] {
    const net::EventStreamResult result = net::ParseEventStream("retry: 4500\ndata: x\n\n");
    Expect(result.retry_ms.has_value() && *result.retry_ms == 4500u, "the delay it asked for");
    // A `retry` that is not a number changes nothing rather than resetting the delay: a
    // stream that sent `retry: soon` must not become a reconnect storm.
    const net::EventStreamResult bad = net::ParseEventStream("retry: soon\ndata: x\n\n");
    Expect(!bad.retry_ms.has_value(), "and nonsense is ignored");
  });

  AddTest(tests, "EventStream/AnEventOverTheBoundIsDroppedRatherThanHeldForever", [] {
    // A server that sends `data:` forever without a blank line would otherwise grow the
    // buffer without limit. Dropping one message is better than closing the stream and
    // better than holding it in memory -- and it is a bound on memory a *peer* controls,
    // which is why it is in the parser rather than at a caller.
    std::string stream = "data: " + std::string(200, 'x') + "\n\ndata: ok\n\n";
    const net::EventStreamResult result = net::ParseEventStream(stream, 64);
    ExpectEqInt(static_cast<long long>(result.events.size()), 1, "the oversized one is gone");
    ExpectEqString(result.events.at(0).data, "ok", "and the stream carries on");
  });

  AddTest(tests, "EventSource/APageOpensAStreamAndItsHandlersRun", [] {
    // The end of the EventSource path: `new EventSource(...)`, the engine's table, the
    // connection, and back as `onopen` and `onmessage` -- including a *named* event, which
    // is how servers label them.
    gfx::FontLibrary library;
    gfx::FontCatalog fonts{library};
    fonts.Register("Test", 400, false, BuildSyntheticFont());
    fonts.SetDefaultFamily("Test");
    ipc::InProcessChannel channel;
    engine::Engine engine{channel.Engine(), fonts};
    PageFactory factory;
    factory.SetStream(
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"
        "data: one\n\nevent: ping\ndata: two\nid: 9\n\n");
    factory.SetDocument(
        "<html><body><script>"
        "const es = new EventSource('https://page.example/events');"
        "es.onopen = () => console.log('open:' + es.readyState);"
        "es.onmessage = (e) => console.log('msg:' + e.type + ':' + e.data + ':' + e.lastEventId);"
        "console.log('ctor:' + es.readyState);"
        "</script></body></html>");
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
    for (int turn = 0; turn < 4; ++turn) {
      engine.Advance();
    }
    std::string console;
    for (const std::string& line : engine.ConsoleOutput()) {
      console += line + "|";
    }
    Expect(console.find("ctor:0") != std::string::npos, "CONNECTING at construction");
    Expect(console.find("open:1") != std::string::npos, "then OPEN");
    Expect(console.find("msg:message:one:") != std::string::npos,
           "an unnamed event arrives as `message`");
    Expect(console.find("msg:ping:two:9") != std::string::npos,
           "and a named one keeps its name and carries the id");
  });

  AddTest(tests, "WebSocket/APageOpensASocketAndItsHandlersRun", [] {
    // The end of ADR 0020 §5's path: `new WebSocket(...)` through the binding, the
    // engine's table, the connection, and back as `onopen` and `onmessage`.
    gfx::FontLibrary library;
    gfx::FontCatalog fonts{library};
    fonts.Register("Test", 400, false, BuildSyntheticFont());
    fonts.SetDefaultFamily("Test");
    ipc::InProcessChannel channel;
    engine::Engine engine{channel.Engine(), fonts};
    PageFactory factory;
    factory.SetFrames(Frame({0x81, 0x05, 'h', 'e', 'l', 'l', 'o'}));
    factory.SetDocument(
        "<html><body><script>"
        "const s = new WebSocket('wss://page.example/live');"
        "s.onopen = () => console.log('open:' + s.readyState);"
        "s.onmessage = (e) => console.log('message:' + e.data);"
        "s.onclose = (e) => console.log('close:' + e.code + ':' + e.wasClean);"
        "console.log('ctor:' + s.readyState + ':' + s.url);"
        "</script></body></html>");
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
    // The load is over; the socket's turns are after it. On a real loop each of these is
    // a wake from the descriptor the connection put in the wait -- which is what the next
    // test asserts is there.
    for (int turn = 0; turn < 4; ++turn) {
      engine.Advance();
    }

    std::string console;
    for (const std::string& line : engine.ConsoleOutput()) {
      console += line + "|";
    }
    // CONNECTING at construction, which is what a page switches on before `onopen`.
    Expect(console.find("ctor:0:wss://page.example/live") != std::string::npos,
           "the constructor answers CONNECTING with the url it was given");
    Expect(console.find("open:1") != std::string::npos, "onopen ran with readyState OPEN");
    Expect(console.find("message:hello") != std::string::npos, "and the message arrived");
  });

  AddTest(tests, "WebSocket/AnIdlePageWithAnOpenSocketHasNoWorkToDo", [] {
    // **This is session 23's check.** An open connection is a descriptor in the idle wait
    // and nothing else: no timer, no poll, no keepalive. So a page with one open and a
    // server saying nothing must report *no runnable work* -- if it reported work, the
    // loop would spin for as long as the page held the socket, which is the failure ADR
    // 0020 §5 exists to prevent.
    gfx::FontLibrary library;
    gfx::FontCatalog fonts{library};
    fonts.Register("Test", 400, false, BuildSyntheticFont());
    fonts.SetDefaultFamily("Test");
    ipc::InProcessChannel channel;
    engine::Engine engine{channel.Engine(), fonts};
    PageFactory factory;
    factory.SetFrames(std::string());
    factory.SetDocument(
        "<html><body><script>new WebSocket('wss://page.example/live');</script></body></html>");
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
    for (int turn = 0; turn < 4; ++turn) {
      engine.Advance();
    }

    Expect(!engine.HasRunnableWork(),
           "an open socket with nothing queued is not work: it is a descriptor to wait on");
    // And it *is* in the wait, which is the other half -- a connection nobody waits on is
    // a connection whose messages arrive whenever something else happens to wake the loop.
    util::WaitDescriptorList descriptors;
    engine.AppendWaitDescriptors(descriptors);
    Expect(!descriptors.empty(), "and the loop is told to watch it");
    // A navigation closes it, which is what makes "no socket outlives its document" true.
    // Asserted on the transport rather than on the descriptor list, because the new
    // navigation puts a descriptor of its own in there: what matters is that the *socket*
    // was closed, and the transport is what knows.
    Expect(!factory.Sockets().empty(), "the page opened one");
    Expect(!factory.Sockets().at(0)->IsClosed(), "and it is open before the navigation");
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/second"});
    engine.HandlePendingMessages();
    Expect(factory.Sockets().at(0)->IsClosed(),
           "and the navigation closed it: erasing the table is closing the connection");
  });

  AddTest(tests, "WebSocket/APlaintextSocketIsRefusedRatherThanOpened", [] {
    // `ws://` is refused, and the reason is not squeamishness: the masking key and the
    // handshake key are both counters in this implementation, and both are safe *because*
    // the transport is TLS. Accepting a plaintext socket would quietly invalidate two
    // decisions made elsewhere -- so the refusal is here, where they are relied on.
    gfx::FontLibrary library;
    gfx::FontCatalog fonts{library};
    fonts.Register("Test", 400, false, BuildSyntheticFont());
    fonts.SetDefaultFamily("Test");
    ipc::InProcessChannel channel;
    engine::Engine engine{channel.Engine(), fonts};
    PageFactory factory;
    factory.SetFrames(std::string());
    factory.SetDocument(
        "<html><body><script>"
        "const s = new WebSocket('ws://page.example/live');"
        "console.log('state:' + s.readyState);"
        "</script></body></html>");
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
    std::string console;
    for (const std::string& line : engine.ConsoleOutput()) {
      console += line;
    }
    // CLOSED, and the object still exists: a policy refusal is a socket that closes
    // rather than a constructor that throws, which is what the specification says and
    // what stops a page from probing the user's policy with a try/catch.
    ExpectEqString(console, "state:3", "the socket exists and is closed");
  });

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


  AddTest(tests, "WebSocket/OpensThroughTheHandshakeAndDeliversAMessage", [] {
    // The whole path: connect, send the upgrade request, check the accept value, then
    // frame whatever follows. A server is allowed to put the first frame in the same
    // packet as the handshake, so the fixture does -- a reader that dropped the tail
    // would lose the first message of every fast server.
    Socket socket(Handshake(Frame({0x81, 0x02, 0x68, 0x69})));
    const net::WebSocketConnection::Progress progress = socket.connection->Advance();
    Expect(progress.opened, "the handshake was accepted");
    // The scripted transport hangs up after its canned bytes, which is what a server
    // that closes promptly does -- so the assertion is that the message arrived
    // *before* the close was noticed, which is the bug this file had first.
    Expect(progress.closed, "and the peer's hang-up was noticed on the same turn");
    ExpectEqInt(static_cast<long long>(progress.messages.size()), 1, "one message");
    Expect(progress.messages.at(0).first, "text");
    ExpectEqString(progress.messages.at(0).second, "hi", "with its payload");
    // The request it sent is the protocol's, including the version the server keys its
    // accept value off.
    Expect(socket.Written().find("Upgrade: websocket") != std::string::npos, "the upgrade header");
    Expect(socket.Written().find("Sec-WebSocket-Version: 13") != std::string::npos,
           "and the version");
  });

  AddTest(tests, "WebSocket/AWrongAcceptValueIsARefusalRatherThanAnOpenSocket", [] {
    // A server that returns 101 with the right token headers but the wrong digest has
    // not computed anything, which means something other than a WebSocket server
    // answered -- a proxy, a cache, or a service that upgrades whatever asks.
    Socket socket("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Accept: not-the-digest\r\n\r\n");
    const net::WebSocketConnection::Progress progress = socket.connection->Advance();
    Expect(!progress.opened, "not opened");
    Expect(progress.failed, "and reported as a failure");
    Expect(socket.connection->GetState() == net::WebSocketConnection::State::Closed, "closed");
    ExpectEqInt(socket.connection->CloseCode(), 1006,
                "with the code the specification reserves for a connection that never opened");
  });

  AddTest(tests, "WebSocket/AnswersAPingAndNeverOriginatesOne", [] {
    // ADR 0020 §5's zero-idle-CPU clause, asserted rather than described: a keepalive on
    // a timer is the one thing that would turn an idle page into a waking one, so the
    // ping is *responsive only*. The pong echoes the ping's payload, per §5.5.3.
    Socket socket(Handshake(Frame({0x89, 0x04, 0x70, 0x69, 0x6e, 0x67})));
    socket.connection->Advance();
    // The pong is in what went out, and nothing else did: no ping of our own.
    const std::string sent = socket.Written();
    Expect(sent.find("Upgrade: websocket") != std::string::npos, "the handshake went out");
    // Opcode 0x8A is a pong; 0x89 would be a ping we originated.
    Expect(sent.find(static_cast<char>(0x8A)) != std::string::npos, "a pong went out");
    Expect(sent.find(static_cast<char>(0x89)) == std::string::npos,
           "and no ping: we never originate one");
  });

  AddTest(tests, "WebSocket/ReassemblesAFragmentedMessageAndKeepsItsKind", [] {
    // A continuation frame's opcode says nothing, so the *first* frame is the only place
    // text-versus-binary is known. Guessing later is how a binary message becomes
    // mojibake -- so the fixture fragments a *binary* message and the kind has to
    // survive.
    Socket socket(Handshake(Frame({0x02, 0x02, 0x01, 0x02, 0x80, 0x02, 0x03, 0x04})));
    const net::WebSocketConnection::Progress progress = socket.connection->Advance();
    ExpectEqInt(static_cast<long long>(progress.messages.size()), 1, "one message, not two");
    Expect(!progress.messages.at(0).first, "and it is still binary");
    ExpectEqInt(static_cast<long long>(progress.messages.at(0).second.size()), 4, "four bytes");
  });

  AddTest(tests, "WebSocket/ACloseFrameIsAnsweredAndReportsItsCode", [] {
    // 1001 is "going away", which is what a server sends when it is shutting down. The
    // closing handshake is their frame, then ours, then the transport.
    Socket socket(Handshake(Frame({0x88, 0x07, 0x03, 0xE9, 'b', 'y', 'e', '!', '!'})));
    const net::WebSocketConnection::Progress progress = socket.connection->Advance();
    Expect(progress.closed, "closed");
    Expect(socket.connection->ClosedCleanly(), "cleanly, because a close frame was exchanged");
    ExpectEqInt(socket.connection->CloseCode(), 1001, "with the peer's code");
    ExpectEqString(socket.connection->CloseReason(), "bye!!", "and its reason");
    Expect(!socket.connection->Interest().has_value(),
           "and it asks the idle wait for nothing, which is what takes it out of the wait");
  });

  AddTest(tests, "WebSocket/AFramingErrorClosesRatherThanResynchronises", [] {
    // A masked server frame, which is refused by the codec -- and at the connection level
    // a framing error has no recovery: the stream's structure is no longer trusted, so
    // the specification closes and so does this. "Skip a byte and try again" is how a
    // decoder is taught to accept a frame a proxy wrote.
    Socket socket(Handshake(Frame({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f})));
    const net::WebSocketConnection::Progress progress = socket.connection->Advance();
    Expect(progress.opened, "the handshake still opened");
    Expect(progress.closed && progress.failed, "then the frame closed it as a failure");
  });

  AddTest(tests, "WebSocket/SendRefusesBeforeOpenAndMasksAfterwards", [] {
    LiveSocket socket(Handshake());
    Expect(!socket.connection->Send("early", true),
           "a send before the handshake is refused, which script sees as InvalidStateError");
    Expect(socket.connection->Advance().opened, "the handshake completed");
    Expect(socket.connection->GetState() == net::WebSocketConnection::State::Open,
           "and the socket stays open with nothing outstanding, which is the whole point");
    Expect(socket.connection->Send("hello", true), "a send is accepted");
    // Masked, so the payload is not on the wire in the clear -- and this is the assertion
    // that a client frame cannot be spelled unmasked here.
    Expect(socket.transport->Written().find("hello") == std::string::npos,
           "the payload is masked");
    Expect(socket.transport->Written().find(static_cast<char>(0x81)) != std::string::npos,
           "and it went out as a final text frame");
  });

  AddTest(tests, "WebSocket/AnOpenSocketWithNothingOutstandingStillAsksToBeWokenOnce", [] {
    // ADR 0020 §5 against the zero-idle-CPU invariant. An open connection is *one
    // descriptor* in the idle wait and nothing else: no timer, no poll, no keepalive. A
    // server that says nothing therefore costs nothing, and the way to check that here is
    // that the connection asks for a descriptor and produces no work.
    LiveSocket socket(Handshake());
    socket.connection->Advance();
    Expect(socket.connection->Interest().has_value(), "it waits on its socket");
    const net::WebSocketConnection::Progress quiet = socket.connection->Advance();
    Expect(!quiet.opened && !quiet.closed && quiet.messages.empty(),
           "a turn where the server said nothing produces nothing");
    Expect(!socket.connection->HasPendingWrites(),
           "and nothing is queued, so the loop has no reason to come back");
    socket.connection->Close();
    Expect(socket.connection->GetState() == net::WebSocketConnection::State::Closing,
           "a close we started is a handshake rather than a hang-up");
    socket.transport->Deliver(Frame({0x88, 0x02, 0x03, 0xE8}));
    const net::WebSocketConnection::Progress done = socket.connection->Advance();
    Expect(done.closed, "and the peer's answer finishes it");
    Expect(socket.connection->ClosedCleanly(), "cleanly");
    Expect(!socket.connection->Interest().has_value(), "with nothing left in the wait");
  });

  AddTest(tests, "WebSocket/TwoMessagesArriveInOrderAcrossTwoTurns", [] {
    // A long-lived connection's real shape: bytes arrive on separate turns, and a frame
    // split across them has to be held rather than dropped or re-read.
    LiveSocket socket(Handshake());
    socket.connection->Advance();
    // The first frame, and *half* of the second.
    socket.transport->Deliver(Frame({0x81, 0x01, 'a', 0x81, 0x03, 'b'}));
    const net::WebSocketConnection::Progress first = socket.connection->Advance();
    ExpectEqInt(static_cast<long long>(first.messages.size()), 1, "only the whole frame");
    ExpectEqString(first.messages.at(0).second, "a", "which is the first");
    socket.transport->Deliver("cd");
    const net::WebSocketConnection::Progress second = socket.connection->Advance();
    ExpectEqInt(static_cast<long long>(second.messages.size()), 1, "then the second");
    ExpectEqString(second.messages.at(0).second, "bcd", "reassembled across the turn boundary");
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
