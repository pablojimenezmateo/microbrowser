#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "net/Hpack.h"
#include "net/Http2Frames.h"
#include "net/Http2Session.h"
#include "net/Transport.h"

namespace microbrowser::tests {

using net::Http2Session;
using net::hpack::Header;
using namespace microbrowser::net::http2;  // NOLINT(google-build-using-namespace)

namespace {

// A socket a test writes the server's half of. Not the shared
// `ScriptedTransport`, which is built around one request meeting one response —
// the whole point of this protocol is that those are no longer the same shape.
class FakeTransport : public net::Transport {
 public:
  std::string inbound;   // what the server says next
  std::string outbound;  // what the client has written
  std::size_t read_at = 0;
  bool peer_closed = false;

  bool StartConnect(std::string_view, std::string_view, std::uint16_t, bool) override {
    return true;
  }
  net::IoStatus Advance() override { return net::IoStatus::Ready; }
  net::IoResult Send(std::span<const std::byte> data) override {
    outbound.append(reinterpret_cast<const char*>(data.data()), data.size());
    return {net::IoStatus::Ready, data.size()};
  }
  net::IoResult Receive(std::span<std::byte> out) override {
    // An offset rather than an erase. A flow-control test hands this megabytes,
    // and erasing the front of a buffer per read is what turns that into
    // quadratic time and a test nobody runs.
    if (read_at >= inbound.size()) {
      return {peer_closed ? net::IoStatus::Closed : net::IoStatus::Blocked, 0};
    }
    const std::size_t take = std::min(out.size(), inbound.size() - read_at);
    std::memcpy(out.data(), inbound.data() + read_at, take);
    read_at += take;
    return {net::IoStatus::Ready, take};
  }
  void Close() override {}
  std::optional<util::WaitDescriptor> Interest() const override { return std::nullopt; }
};

std::string Frame(FrameType type, std::uint8_t flags, std::uint32_t stream,
                  std::string_view payload) {
  std::string out;
  WriteFrameHeader(type, flags, stream, payload.size(), out);
  out += payload;
  return out;
}

std::string Block(const std::vector<Header>& fields) {
  std::string out;
  net::hpack::Encode(fields, out);
  return out;
}

// The server's opening SETTINGS. Empty, which is legal and means "the defaults".
std::string ServerSettings() {
  return Frame(FrameType::Settings, 0, 0, "");
}

std::span<const std::byte> Bytes(const std::string& text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string BodyOf(const net::HttpResponse& response) {
  return std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size());
}

// A session on a transport the test still owns. Handing over a `unique_ptr`
// while keeping the raw pointer is exactly what the production pool does, and
// it is what lets a test say what the server sent after the session took it.
struct Fixture {
  FakeTransport* wire = nullptr;
  std::unique_ptr<Http2Session> session;

  Fixture() {
    auto transport = std::make_unique<FakeTransport>();
    wire = transport.get();
    session = std::make_unique<Http2Session>(std::move(transport));
  }

  Http2Session::StreamId Get(std::string_view path) {
    net::HttpHeaders headers;
    Http2Session::Request request;
    request.method = "GET";
    request.scheme = "https";
    request.authority = "example.com";
    request.target = path;
    request.headers = &headers;
    const auto id = session->StartRequest(request);
    Expect(id.has_value(), "the session must accept a request");
    return *id;
  }

  void Deliver(std::string_view frames) {
    wire->inbound += frames;
    session->Advance();
  }
};

// A 200 with a body, on one stream, as a server would send it.
std::string OkOn(std::uint32_t stream, std::string_view body) {
  std::string out = Frame(FrameType::Headers, flag::kEndHeaders, stream,
                          Block({{":status", "200"}, {"content-type", "text/plain"}}));
  out += Frame(FrameType::Data, flag::kEndStream, stream, body);
  return out;
}

}  // namespace

void RegisterHttp2Tests(std::vector<TestCase>& tests) {
  AddTest(tests, "Http2/OpensWithThePrefaceSettingsAndAWindow", [] {
    Fixture fixture;
    fixture.session->Advance();
    const std::string& out = fixture.wire->outbound;
    Expect(out.rfind(std::string(kConnectionPreface), 0) == 0,
           "the connection preface goes first and unaltered");
    FrameHeader header;
    Expect(ParseFrameHeader(Bytes(out).subspan(kConnectionPreface.size()), header),
           "a frame must follow it");
    Expect(header.Is(FrameType::Settings) && header.stream == 0,
           "and that frame is our SETTINGS");
    // The connection window is not covered by SETTINGS_INITIAL_WINDOW_SIZE, so
    // it starts at 65535 whatever we advertise. A session that did not raise it
    // would pace every page after the first 64KB on round trips.
    FrameHeader window;
    const std::size_t at = kConnectionPreface.size() + kFrameHeaderBytes + header.length;
    Expect(ParseFrameHeader(Bytes(out).subspan(at), window), "and a third frame");
    Expect(window.Is(FrameType::WindowUpdate) && window.stream == 0,
           "which raises the connection's receive window explicitly");
  });

  AddTest(tests, "Http2/DeliversAResponse", [] {
    Fixture fixture;
    const auto id = fixture.Get("/index.html");
    fixture.Deliver(ServerSettings() + OkOn(id, "hello"));
    Expect(fixture.session->StateOf(id) == Http2Session::StreamState::Complete,
           "the stream must be complete");
    const net::HttpResponse response = fixture.session->TakeResponse(id);
    ExpectEqInt(response.status, 200, "the status comes from :status");
    ExpectEqString(BodyOf(response), "hello", "and the body from the DATA frame");
    ExpectEqString(std::string(response.headers.Get("content-type").value_or("")), "text/plain",
                   "with the regular fields intact");
    Expect(fixture.session->OpenStreams() == 0, "and taking the response frees the stream");
  });

  // The test this whole file exists for. Two requests on one socket, whose
  // responses arrive interleaved and out of order -- headers for the second
  // before the body of the first, then the two bodies in pieces. A
  // demultiplexer that kept a "current stream" anywhere passes every other test
  // in this file and fails this one.
  AddTest(tests, "Http2/MultiplexesInterleavedResponses", [] {
    Fixture fixture;
    const auto first = fixture.Get("/one");
    const auto second = fixture.Get("/two");
    Expect(first != second, "two requests get two streams");

    std::string wire = ServerSettings();
    wire += Frame(FrameType::Headers, flag::kEndHeaders, second, Block({{":status", "404"}}));
    wire += Frame(FrameType::Headers, flag::kEndHeaders, first, Block({{":status", "200"}}));
    wire += Frame(FrameType::Data, 0, second, "not");
    wire += Frame(FrameType::Data, 0, first, "AAA");
    wire += Frame(FrameType::Data, flag::kEndStream, second, "-found");
    wire += Frame(FrameType::Data, flag::kEndStream, first, "BBB");
    fixture.Deliver(wire);

    Expect(fixture.session->StateOf(first) == Http2Session::StreamState::Complete,
           "the first stream completes");
    Expect(fixture.session->StateOf(second) == Http2Session::StreamState::Complete,
           "and so does the second");
    const net::HttpResponse one = fixture.session->TakeResponse(first);
    const net::HttpResponse two = fixture.session->TakeResponse(second);
    ExpectEqInt(one.status, 200, "each response keeps its own status");
    ExpectEqInt(two.status, 404, "including the one that arrived first");
    ExpectEqString(BodyOf(one), "AAABBB", "and each body is only its own bytes");
    ExpectEqString(BodyOf(two), "not-found", "in the order they arrived on that stream");
  });

  AddTest(tests, "Http2/SplitsAResponseAcrossContinuations", [] {
    Fixture fixture;
    const auto id = fixture.Get("/big");
    const std::string block = Block({{":status", "200"}, {"x-a", std::string(400, 'a')},
                                     {"x-b", std::string(400, 'b')}});
    std::string wire = ServerSettings();
    wire += Frame(FrameType::Headers, 0, id, std::string_view(block).substr(0, 30));
    wire += Frame(FrameType::Continuation, 0, id, std::string_view(block).substr(30, 30));
    wire += Frame(FrameType::Continuation, flag::kEndHeaders, id,
                  std::string_view(block).substr(60));
    wire += Frame(FrameType::Data, flag::kEndStream, id, "ok");
    fixture.Deliver(wire);
    Expect(fixture.session->StateOf(id) == Http2Session::StreamState::Complete,
           "a header block split across CONTINUATIONs is one header block");
    const net::HttpResponse response = fixture.session->TakeResponse(id);
    ExpectEqInt(static_cast<long long>(response.headers.Get("x-a").value_or("").size()), 400,
                "and its fields survive the split");
  });

  AddTest(tests, "Http2/RefusesAFrameInsideAHeaderBlock", [] {
    Fixture fixture;
    const auto id = fixture.Get("/x");
    std::string wire = ServerSettings();
    wire += Frame(FrameType::Headers, 0, id, Block({{":status", "200"}}));
    // §6.10 allows nothing between a HEADERS and its CONTINUATION. A peer that
    // can interleave here can make two header blocks share one decoder.
    wire += Frame(FrameType::Data, 0, id, "x");
    fixture.Deliver(wire);
    Expect(fixture.session->Failed(), "the connection is over");
  });

  AddTest(tests, "Http2/BoundsAContinuationFlood", [] {
    Fixture fixture;
    const auto id = fixture.Get("/x");
    std::string wire = ServerSettings();
    wire += Frame(FrameType::Headers, 0, id, "");
    // Empty CONTINUATIONs add no bytes at all, so a byte bound never fires.
    // This is the flood, and the frame count is what closes it.
    for (int i = 0; i < 200; ++i) {
      wire += Frame(FrameType::Continuation, 0, id, "");
    }
    fixture.Deliver(wire);
    Expect(fixture.session->Failed(), "an endless run of CONTINUATIONs must be refused");
  });

  AddTest(tests, "Http2/RefusesServerPush", [] {
    Fixture fixture;
    const auto id = fixture.Get("/x");
    std::string payload;
    WriteUint32(2, payload);  // the promised stream
    payload += Block({{":method", "GET"}, {":path", "/pushed"}});
    fixture.Deliver(ServerSettings() +
                    Frame(FrameType::PushPromise, flag::kEndHeaders, id, payload));
    Expect(fixture.session->Failed(),
           "we advertised ENABLE_PUSH = 0; a push is a response to a request the user "
           "never made");
  });

  AddTest(tests, "Http2/AResetFailsOneStreamAndNotTheConnection", [] {
    Fixture fixture;
    const auto first = fixture.Get("/one");
    const auto second = fixture.Get("/two");
    std::string reset;
    WriteUint32(static_cast<std::uint32_t>(ErrorCode::InternalError), reset);
    fixture.Deliver(ServerSettings() + Frame(FrameType::RstStream, 0, first, reset) +
                    OkOn(second, "fine"));
    Expect(fixture.session->StateOf(first) == Http2Session::StreamState::Failed,
           "the reset stream failed");
    Expect(!fixture.session->Failed(), "and the connection did not");
    Expect(fixture.session->StateOf(second) == Http2Session::StreamState::Complete,
           "so the other nineteen requests on it still finish");
  });

  AddTest(tests, "Http2/GoAwayRefusesOnlyTheStreamsPastTheLastAccepted", [] {
    Fixture fixture;
    const auto first = fixture.Get("/one");
    const auto second = fixture.Get("/two");
    std::string payload;
    WriteUint32(first, payload);  // the last stream the server will process
    WriteUint32(static_cast<std::uint32_t>(ErrorCode::NoError), payload);
    fixture.Deliver(ServerSettings() + Frame(FrameType::GoAway, 0, 0, payload) +
                    OkOn(first, "kept"));
    Expect(fixture.session->StateOf(second) == Http2Session::StreamState::Refused,
           "a stream the server never promised to process is refused, and is safe to "
           "send again on a new connection");
    Expect(fixture.session->StateOf(first) == Http2Session::StreamState::Complete,
           "and one it did promise still finishes -- that is what makes a graceful "
           "shutdown graceful");
    Expect(!fixture.session->IsUsable(), "but nothing new may be started on it");
  });

  AddTest(tests, "Http2/ARefusedStreamIsDistinguishable", [] {
    Fixture fixture;
    const auto id = fixture.Get("/x");
    std::string reset;
    WriteUint32(static_cast<std::uint32_t>(ErrorCode::RefusedStream), reset);
    fixture.Deliver(ServerSettings() + Frame(FrameType::RstStream, 0, id, reset));
    Expect(fixture.session->StateOf(id) == Http2Session::StreamState::Refused,
           "REFUSED_STREAM says the request was never processed, which is the one "
           "failure a retry is safe after -- and it is a state rather than a string "
           "the caller has to match on");
    Expect(fixture.session->ErrorOf(id) != nullptr, "with a reason the caller can render");
  });

  AddTest(tests, "Http2/ClosingTheSocketIsNotCompletion", [] {
    Fixture fixture;
    const auto id = fixture.Get("/x");
    fixture.wire->inbound = ServerSettings() +
                            Frame(FrameType::Headers, flag::kEndHeaders, id,
                                  Block({{":status", "200"}})) +
                            Frame(FrameType::Data, 0, id, "half");
    fixture.wire->peer_closed = true;
    fixture.session->Advance();
    Expect(fixture.session->StateOf(id) == Http2Session::StreamState::Failed,
           "a response ends when END_STREAM says so and never because the socket did -- "
           "otherwise a truncated script is an accepted script");
  });

  AddTest(tests, "Http2/RefusesAResponseWithNoStatus", [] {
    Fixture fixture;
    const auto id = fixture.Get("/x");
    fixture.Deliver(ServerSettings() +
                    Frame(FrameType::Headers, flag::kEndHeaders | flag::kEndStream, id,
                          Block({{"content-type", "text/plain"}})));
    Expect(fixture.session->StateOf(id) == Http2Session::StreamState::Failed,
           "a response with no :status is not a response");
    Expect(!fixture.session->Failed(), "and that is the stream's problem, not the connection's");
  });

  AddTest(tests, "Http2/RefusesConnectionSpecificFields", [] {
    Fixture fixture;
    const auto id = fixture.Get("/x");
    fixture.Deliver(ServerSettings() +
                    Frame(FrameType::Headers, flag::kEndHeaders | flag::kEndStream, id,
                          Block({{":status", "200"}, {"transfer-encoding", "chunked"}})));
    Expect(fixture.session->StateOf(id) == Http2Session::StreamState::Failed,
           "transfer-encoding on this wire is a translating proxy that understands "
           "neither protocol, which is where smuggling lives");
  });

  AddTest(tests, "Http2/RefusesAnUppercaseFieldName", [] {
    Fixture fixture;
    const auto id = fixture.Get("/x");
    fixture.Deliver(ServerSettings() +
                    Frame(FrameType::Headers, flag::kEndHeaders | flag::kEndStream, id,
                          Block({{":status", "200"}, {"Content-Type", "text/plain"}})));
    Expect(fixture.session->StateOf(id) == Http2Session::StreamState::Failed,
           "field names are lowercase by the protocol, not by convention");
  });

  AddTest(tests, "Http2/RefusesABodyThatIsNotItsDeclaredLength", [] {
    Fixture fixture;
    const auto id = fixture.Get("/x");
    std::string wire = ServerSettings();
    wire += Frame(FrameType::Headers, flag::kEndHeaders, id,
                  Block({{":status", "200"}, {"content-length", "99"}}));
    wire += Frame(FrameType::Data, flag::kEndStream, id, "short");
    fixture.Deliver(wire);
    Expect(fixture.session->StateOf(id) == Http2Session::StreamState::Failed,
           "a body that is not the length it claimed is a response nobody should act on");
  });

  AddTest(tests, "Http2/PassesOverAnInformationalResponse", [] {
    Fixture fixture;
    const auto id = fixture.Get("/x");
    std::string wire = ServerSettings();
    // 103 Early Hints, which is what a real server sends before the real one.
    wire += Frame(FrameType::Headers, flag::kEndHeaders, id,
                  Block({{":status", "103"}, {"link", "</a.css>; rel=preload"}}));
    wire += OkOn(id, "real");
    fixture.Deliver(wire);
    Expect(fixture.session->StateOf(id) == Http2Session::StreamState::Complete,
           "an informational response is not the response");
    const net::HttpResponse response = fixture.session->TakeResponse(id);
    ExpectEqInt(response.status, 200, "the final status wins");
    Expect(!response.headers.Has("link"), "and the hints do not leak into it");
  });

  AddTest(tests, "Http2/AnswersAPing", [] {
    Fixture fixture;
    fixture.session->Advance();
    fixture.wire->outbound.clear();
    fixture.Deliver(ServerSettings() + Frame(FrameType::Ping, 0, 0, "12345678"));
    Expect(fixture.wire->outbound.find("12345678") != std::string::npos,
           "a PING is answered with exactly the eight bytes that arrived");
  });

  AddTest(tests, "Http2/GivesBackTheWindowAsItConsumes", [] {
    Fixture fixture;
    const auto id = fixture.Get("/big");
    std::string wire = ServerSettings();
    wire += Frame(FrameType::Headers, flag::kEndHeaders, id, Block({{":status", "200"}}));
    fixture.Deliver(wire);
    fixture.wire->outbound.clear();

    // Past half the stream window, in frames of the protocol's maximum size.
    // Without a WINDOW_UPDATE the server would stop here and the transfer would
    // be paced by round trips for the rest of its length.
    const std::string chunk(kMaxFrameSize, 'x');
    std::string data;
    // Past half of the *connection* window, which is the larger of the two, so
    // one run of frames exercises both top-ups.
    const std::size_t frames = Http2Session::kConnectionWindow / kMaxFrameSize / 2 + 2;
    for (std::size_t i = 0; i < frames; ++i) {
      data += Frame(FrameType::Data, 0, id, chunk);
    }
    fixture.Deliver(data);

    bool stream_update = false;
    bool connection_update = false;
    std::span<const std::byte> rest = Bytes(fixture.wire->outbound);
    while (true) {
      FrameHeader header;
      if (!ParseFrameHeader(rest, header) || rest.size() < kFrameHeaderBytes + header.length) {
        break;
      }
      if (header.Is(FrameType::WindowUpdate)) {
        (header.stream == 0 ? connection_update : stream_update) = true;
      }
      rest = rest.subspan(kFrameHeaderBytes + header.length);
    }
    Expect(stream_update, "the stream's window is given back as its body is consumed");
    Expect(connection_update, "and so is the connection's");
  });

  AddTest(tests, "Http2/SplitsARequestBodyAcrossTheSendWindow", [] {
    Fixture fixture;
    // A server that opens with a tiny initial window. Everything past it has to
    // wait for a WINDOW_UPDATE, and an implementation that ignored the window
    // would send the whole body and be reset for it.
    std::string settings;
    settings.push_back(0);
    settings.push_back(4);  // SETTINGS_INITIAL_WINDOW_SIZE
    WriteUint32(10, settings);
    fixture.wire->inbound = Frame(FrameType::Settings, 0, 0, settings);
    fixture.session->Advance();

    const std::string body(50, 'p');
    net::HttpHeaders headers;
    Http2Session::Request request;
    request.method = "POST";
    request.scheme = "https";
    request.authority = "example.com";
    request.target = "/post";
    request.headers = &headers;
    request.body = Bytes(body);
    const auto id = fixture.session->StartRequest(request);
    Expect(id.has_value(), "the request starts");
    fixture.session->Advance();

    std::size_t data_bytes = 0;
    std::span<const std::byte> rest = Bytes(fixture.wire->outbound);
    rest = rest.subspan(kConnectionPreface.size());
    while (true) {
      FrameHeader header;
      if (!ParseFrameHeader(rest, header) || rest.size() < kFrameHeaderBytes + header.length) {
        break;
      }
      if (header.Is(FrameType::Data)) {
        data_bytes += header.length;
      }
      rest = rest.subspan(kFrameHeaderBytes + header.length);
    }
    ExpectEqInt(static_cast<long long>(data_bytes), 10,
                "only what the window allows goes out; the rest waits to be invited");

    std::string update;
    WriteUint32(40, update);
    fixture.Deliver(Frame(FrameType::WindowUpdate, 0, *id, update));
    data_bytes = 0;
    rest = Bytes(fixture.wire->outbound);
    rest = rest.subspan(kConnectionPreface.size());
    while (true) {
      FrameHeader header;
      if (!ParseFrameHeader(rest, header) || rest.size() < kFrameHeaderBytes + header.length) {
        break;
      }
      if (header.Is(FrameType::Data)) {
        data_bytes += header.length;
      }
      rest = rest.subspan(kFrameHeaderBytes + header.length);
    }
    ExpectEqInt(static_cast<long long>(data_bytes), 50, "and the rest goes once it is");
  });

  AddTest(tests, "Http2/ARaisedInitialWindowMovesOpenStreamsToo", [] {
    // §6.9.2, and the bug it exists to prevent: a SETTINGS that changes
    // INITIAL_WINDOW_SIZE moves *every open stream's* send window by the delta,
    // retroactively. Applied to new streams only, a transfer already running
    // deadlocks -- against servers that change the setting mid-connection, so
    // only in production.
    PeerSettings peer;
    std::int64_t delta = 0;
    std::string payload;
    payload.push_back(0);
    payload.push_back(4);
    WriteUint32(1000, payload);
    Expect(peer.Apply(Bytes(payload), delta), "the settings apply");
    ExpectEqInt(delta, 1000 - 65535, "and the delta is signed and against the previous value");
  });

  AddTest(tests, "Http2/RefusesPaddingThatDoesNotFit", [] {
    // The one subtraction in the protocol that underflows into a span over the
    // heap if it is not checked: a pad length the peer chooses, taken off a
    // frame length the peer also chooses.
    FrameHeader header;
    header.type = static_cast<std::uint8_t>(FrameType::Data);
    header.flags = flag::kPadded;
    header.length = 3;
    const std::string payload = std::string("\xFF", 1) + "ab";
    std::span<const std::byte> out;
    Expect(!StripPadding(header, Bytes(payload), out),
           "padding longer than the frame is refused rather than wrapped");
    const std::string fits = std::string("\x01", 1) + "ab";
    Expect(StripPadding(header, Bytes(fits), out), "and padding that fits is stripped");
    ExpectEqInt(static_cast<long long>(out.size()), 1, "leaving the content");
  });

  AddTest(tests, "Http2/RefusesAnOversizedFrame", [] {
    Fixture fixture;
    fixture.Get("/x");
    std::string wire = ServerSettings();
    // A length past the SETTINGS_MAX_FRAME_SIZE we advertised. Refused on the
    // header, before a byte of the payload is buffered, which is the reason the
    // bound is advertised rather than assumed.
    WriteFrameHeader(FrameType::Data, 0, 1, kMaxFrameSize + 1, wire);
    fixture.Deliver(wire);
    Expect(fixture.session->Failed(), "a frame larger than we said we would take is refused");
  });

  AddTest(tests, "Http2/RefusesMalformedSettings", [] {
    PeerSettings peer;
    std::int64_t delta = 0;
    const std::string odd = "12345";  // not a multiple of six
    Expect(!peer.Apply(Bytes(odd), delta), "a SETTINGS payload is a list of six-byte entries");
    std::string too_big;
    too_big.push_back(0);
    too_big.push_back(5);  // MAX_FRAME_SIZE
    WriteUint32(1, too_big);
    Expect(!peer.Apply(Bytes(too_big), delta), "and a frame size below the protocol's floor");
  });
}

}  // namespace microbrowser::tests
