#include <cstddef>
#include <vector>

#include "TestSupport.h"
#include "ipc/ByteStream.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"

namespace microbrowser::tests {

namespace {

using gfx::Color;
using gfx::IntRect;

// Round-tripping every message on every build is what keeps the wire format
// alive while the in-process transport is the one actually in use. A message
// that stops serializing is a message that has quietly started holding a
// pointer, and that is exactly what must not reach the process split.
template <typename Message, typename Deserializer>
void RoundTrip(const Message& message, Deserializer deserialize, std::string_view label) {
  const std::vector<std::byte> bytes = ipc::Serialize(message);
  Expect(!bytes.empty(), std::string(label) + ": serialization produced no bytes");

  const auto decoded = deserialize(bytes);
  Expect(decoded.has_value(), std::string(label) + ": failed to decode its own output");
  Expect(*decoded == message, std::string(label) + ": round trip changed the message");
}

}  // namespace

void RegisterIpcMessageTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Ipc/UiToEngineRoundTripsEveryMessage", [] {
    RoundTrip(ipc::UiToEngine{ipc::NavigateMessage{"https://example.org/a?b=c"}},
              ipc::DeserializeUiToEngine, "Navigate");
    RoundTrip(ipc::UiToEngine{ipc::NavigateMessage{""}}, ipc::DeserializeUiToEngine,
              "Navigate(empty)");
    RoundTrip(ipc::UiToEngine{ipc::ReloadMessage{true}}, ipc::DeserializeUiToEngine, "Reload");
    RoundTrip(ipc::UiToEngine{ipc::StopLoadMessage{}}, ipc::DeserializeUiToEngine, "StopLoad");
    RoundTrip(ipc::UiToEngine{ipc::ResizeViewportMessage{gfx::IntSize{1280, 800}, 2.0f}},
              ipc::DeserializeUiToEngine, "ResizeViewport");
    RoundTrip(ipc::UiToEngine{ipc::ScrollMessage{-3, 42}}, ipc::DeserializeUiToEngine, "Scroll");
    RoundTrip(ipc::UiToEngine{ipc::PointerMessage{ipc::PointerMessage::Kind::Down,
                                                  gfx::IntPoint{5, 7}, 1}},
              ipc::DeserializeUiToEngine, "Pointer");
  });

  AddTest(tests, "Ipc/EngineToUiRoundTripsEveryMessage", [] {
    RoundTrip(ipc::EngineToUi{ipc::TitleChangedMessage{"A Title"}}, ipc::DeserializeEngineToUi,
              "TitleChanged");
    RoundTrip(ipc::EngineToUi{ipc::LoadProgressMessage{0.5f}}, ipc::DeserializeEngineToUi,
              "LoadProgress");
    RoundTrip(ipc::EngineToUi{ipc::NavigationCommittedMessage{"https://example.org/"}},
              ipc::DeserializeEngineToUi, "NavigationCommitted");

    ipc::PaintFrameMessage paint;
    paint.display_list.FillRect(IntRect{0, 0, 100, 50}, Color::Rgb(0x10, 0x20, 0x30));
    paint.display_list.PushClip(IntRect{5, 5, 10, 10});
    paint.display_list.FillRect(IntRect{6, 6, 2, 2}, Color::Rgba(0, 0, 0, 0x80));
    paint.display_list.PopClip();
    paint.damage.push_back(IntRect{0, 0, 100, 50});
    RoundTrip(ipc::EngineToUi{paint}, ipc::DeserializeEngineToUi, "PaintFrame");
  });

  AddTest(tests, "Ipc/NegativeCoordinatesSurviveTheWire", [] {
    // Scroll offsets and off-viewport boxes are routinely negative; an unsigned
    // encoding bug here would be invisible until a page scrolled.
    ipc::PaintFrameMessage paint;
    paint.display_list.FillRect(IntRect{-40, -80, 10, 10}, Color::Rgb(1, 1, 1));
    paint.damage.push_back(IntRect{-40, -80, 10, 10});
    RoundTrip(ipc::EngineToUi{paint}, ipc::DeserializeEngineToUi, "PaintFrame(negative)");
  });

  AddTest(tests, "Ipc/TruncatedFrameIsRejected", [] {
    std::vector<std::byte> bytes =
        ipc::Serialize(ipc::UiToEngine{ipc::NavigateMessage{"https://example.org/"}});
    for (std::size_t drop = 1; drop < bytes.size(); ++drop) {
      std::vector<std::byte> truncated(bytes.begin(), bytes.end() - static_cast<long>(drop));
      Expect(!ipc::DeserializeUiToEngine(truncated).has_value(),
             "a truncated frame must decode to nullopt, never a half-populated message");
    }
  });

  AddTest(tests, "Ipc/TrailingBytesAreRejected", [] {
    std::vector<std::byte> bytes = ipc::Serialize(ipc::UiToEngine{ipc::StopLoadMessage{}});
    bytes.push_back(std::byte{0xAB});
    Expect(!ipc::DeserializeUiToEngine(bytes).has_value(),
           "leftover bytes mean the two ends disagree about the payload; that must surface as "
           "a decode failure, not a silently dropped field");
  });

  AddTest(tests, "Ipc/UnknownTagIsRejected", [] {
    std::vector<std::byte> bytes = ipc::Serialize(ipc::UiToEngine{ipc::StopLoadMessage{}});
    bytes[4] = std::byte{0xFE};  // the tag byte follows the u32 version
    Expect(!ipc::DeserializeUiToEngine(bytes).has_value(), "an unknown tag must be rejected");
  });

  AddTest(tests, "Ipc/VersionMismatchIsRejected", [] {
    std::vector<std::byte> bytes = ipc::Serialize(ipc::UiToEngine{ipc::StopLoadMessage{}});
    bytes[0] = std::byte{0xFF};
    Expect(!ipc::DeserializeUiToEngine(bytes).has_value(),
           "a version mismatch must be rejected rather than reinterpreted");
  });

  AddTest(tests, "Ipc/EmptyInputIsRejected", [] {
    Expect(!ipc::DeserializeUiToEngine({}).has_value(), "an empty frame must decode to nullopt");
    Expect(!ipc::DeserializeEngineToUi({}).has_value(), "an empty frame must decode to nullopt");
  });

  AddTest(tests, "Ipc/HostileLengthPrefixDoesNotAllocate", [] {
    // A frame claiming a 4 GiB string with four bytes left must fail, not
    // attempt the allocation. This is the reader's whole reason for existing.
    ipc::ByteWriter writer;
    writer.WriteU32(ipc::kProtocolVersion);
    writer.WriteU8(1);           // Navigate
    writer.WriteU32(0xFFFFFFFF); // claimed length
    writer.WriteU8('x');
    Expect(!ipc::DeserializeUiToEngine(writer.Bytes()).has_value(),
           "an oversized length prefix must be rejected against the bytes that remain");
  });

  AddTest(tests, "Ipc/ByteReaderFailureIsSticky", [] {
    const std::vector<std::byte> empty;
    ipc::ByteReader reader(empty);
    reader.ReadU32();
    Expect(!reader.Ok(), "reading past the end must fail");
    reader.ReadU8();
    Expect(!reader.Ok(), "failure must stay failed so a decoder can check once at the end");
  });

  AddTest(tests, "Ipc/InProcessChannelDeliversInOrder", [] {
    ipc::InProcessChannel channel;
    channel.Ui().Send(ipc::NavigateMessage{"first"});
    channel.Ui().Send(ipc::NavigateMessage{"second"});
    ExpectEqInt(static_cast<long long>(channel.PendingForEngine()), 2, "both messages queued");

    const auto a = channel.Engine().TryReceive();
    const auto b = channel.Engine().TryReceive();
    Expect(a.has_value() && b.has_value(), "both messages must arrive");
    ExpectEqString(std::get<ipc::NavigateMessage>(*a).url, "first", "FIFO order");
    ExpectEqString(std::get<ipc::NavigateMessage>(*b).url, "second", "FIFO order");
    Expect(!channel.Engine().TryReceive().has_value(), "an empty queue must yield nullopt");
  });

  AddTest(tests, "Ipc/InProcessChannelDirectionsAreIndependent", [] {
    ipc::InProcessChannel channel;
    channel.Ui().Send(ipc::StopLoadMessage{});
    Expect(!channel.Ui().TryReceive().has_value(),
           "a message sent by the UI must not be receivable by the UI");
    ExpectEqInt(static_cast<long long>(channel.PendingForUi()), 0, "the reverse queue is empty");
  });
}

}  // namespace microbrowser::tests
