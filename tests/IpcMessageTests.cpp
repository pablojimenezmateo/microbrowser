#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
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

  // --- Path commands on the wire -------------------------------------------

  AddTest(tests, "Ipc/PathCommandsRoundTrip", [] {
    gfx::Path shape;
    shape.MoveTo(gfx::FloatPoint{1.5f, 2.25f});
    shape.LineTo(gfx::FloatPoint{40.0f, 2.25f});
    shape.QuadTo(gfx::FloatPoint{50.0f, 12.0f}, gfx::FloatPoint{40.0f, 22.0f});
    shape.CubicTo(gfx::FloatPoint{30.0f, 30.0f}, gfx::FloatPoint{10.0f, 30.0f},
                  gfx::FloatPoint{1.5f, 22.0f});
    shape.Close();

    gfx::StrokeStyle style;
    style.width = 3.25f;
    style.cap = gfx::LineCap::Round;
    style.join = gfx::LineJoin::Bevel;
    style.miter_limit = 7.5f;

    ipc::PaintFrameMessage paint;
    paint.display_list.FillPath(shape, Color::Rgba(0x11, 0x22, 0x33, 0x44),
                                gfx::FillRule::EvenOdd);
    paint.display_list.StrokePath(shape, style, Color::Rgb(9, 9, 9));
    RoundTrip(ipc::EngineToUi{paint}, ipc::DeserializeEngineToUi, "PaintFrame(paths)");
  });

  AddTest(tests, "Ipc/TextCommandsRoundTrip", [] {
    ipc::PaintFrameMessage paint;
    paint.display_list.DrawText("Hello, world", 84.5f,
                                gfx::FontRequest{"DejaVu Sans", 16.0f, 400, false},
                                gfx::FloatPoint{12.0f, 30.5f}, Color::Rgb(0x11, 0x22, 0x33));
    paint.display_list.DrawText("italic", 30.0f, gfx::FontRequest{"Serif", 24.0f, 700, true},
                                gfx::FloatPoint{0.0f, 60.0f}, Color::Rgba(1, 2, 3, 4));
    RoundTrip(ipc::EngineToUi{paint}, ipc::DeserializeEngineToUi, "PaintFrame(text)");

    const auto decoded = ipc::DeserializeEngineToUi(ipc::Serialize(ipc::EngineToUi{paint}));
    Expect(decoded.has_value(), "the frame must decode");
    const auto& list = std::get<ipc::PaintFrameMessage>(*decoded).display_list;
    ExpectEqInt(static_cast<long long>(list.Texts().size()), 2, "both runs crossed");
    ExpectEqString(list.TextAt(0)->text, "Hello, world", "with their text");
    ExpectEqString(list.FontAt(0)->family, "DejaVu Sans",
                   "and the family they asked for -- a font handle could not cross a process "
                   "boundary, which is why the command names a family");
    Expect(list.FontAt(1)->italic && list.FontAt(1)->weight == 700, "and its slant and weight");
  });

  AddTest(tests, "Ipc/AHostileTextFrameIsRejected", [] {
    // Each of these is a value the encoder cannot produce. A font size near the
    // float maximum overflows FreeType's 26.6 conversion rather than producing
    // very large text, and a weight outside the CSS range makes the
    // nearest-weight search meaningless.
    const auto text_frame = [](float size, int weight, std::uint8_t italic, float advance,
                               float x) {
      ipc::ByteWriter writer;
      writer.WriteU32(ipc::kProtocolVersion);
      writer.WriteU8(1);   // PaintFrame
      writer.WriteU32(1);  // one command
      writer.WriteU8(6);   // DrawText
      writer.WriteU32(0xFF000000);
      writer.WriteF32(x);
      writer.WriteF32(0.0f);
      writer.WriteF32(advance);
      writer.WriteString("Family");
      writer.WriteF32(size);
      writer.WriteI32(weight);
      writer.WriteU8(italic);
      writer.WriteString("run");
      writer.WriteU32(0);  // damage rect count
      return writer.Take();
    };

    Expect(ipc::DeserializeEngineToUi(text_frame(16.0f, 400, 0, 10.0f, 1.0f)).has_value(),
           "the control frame decodes, or this test proves nothing");
    for (const auto& [bytes, why] : std::vector<std::pair<std::vector<std::byte>, const char*>>{
             {text_frame(0.0f, 400, 0, 10.0f, 1.0f), "a zero font size"},
             {text_frame(-16.0f, 400, 0, 10.0f, 1.0f), "a negative font size"},
             {text_frame(1e30f, 400, 0, 10.0f, 1.0f), "a font size past the bound"},
             {text_frame(std::numeric_limits<float>::infinity(), 400, 0, 10.0f, 1.0f),
              "an infinite font size"},
             {text_frame(std::numeric_limits<float>::quiet_NaN(), 400, 0, 10.0f, 1.0f),
              "a NaN font size"},
             {text_frame(16.0f, 0, 0, 10.0f, 1.0f), "a zero weight"},
             {text_frame(16.0f, 2000, 0, 10.0f, 1.0f), "a weight past 1000"},
             {text_frame(16.0f, -400, 0, 10.0f, 1.0f), "a negative weight"},
             {text_frame(16.0f, 400, 2, 10.0f, 1.0f), "a slant that is neither 0 nor 1"},
             {text_frame(16.0f, 400, 0, -1.0f, 1.0f), "a negative advance"},
             {text_frame(16.0f, 400, 0, std::numeric_limits<float>::infinity(), 1.0f),
              "an infinite advance"},
             {text_frame(16.0f, 400, 0, 10.0f, std::numeric_limits<float>::quiet_NaN()),
              "a NaN origin"},
         }) {
      Expect(!ipc::DeserializeEngineToUi(bytes).has_value(),
             std::string("the decoder accepted ") + why);
    }
  });

  AddTest(tests, "Ipc/ScalarFieldsAreRangeCheckedLikeEveryOtherField", [] {
    // Found by the IPC fuzzer, as a frame that decoded but did not survive a
    // re-encode: a NaN device scale. Decoding must be a fixed point of
    // encoding, or the wire format is whatever the current decoder happens to
    // accept rather than a format.
    const auto resize = [](int width, int height, float scale) {
      ipc::ByteWriter writer;
      writer.WriteU32(ipc::kProtocolVersion);
      writer.WriteU8(4);  // ResizeViewport
      writer.WriteI32(width);
      writer.WriteI32(height);
      writer.WriteF32(scale);
      return writer.Take();
    };
    Expect(ipc::DeserializeUiToEngine(resize(1280, 800, 2.0f)).has_value(),
           "the control frame decodes, or this test proves nothing");
    for (const auto& [bytes, why] : std::vector<std::pair<std::vector<std::byte>, const char*>>{
             {resize(-1, 800, 1.0f), "a negative width"},
             {resize(1280, -1, 1.0f), "a negative height"},
             {resize(1 << 20, 800, 1.0f), "a width past any real display"},
             {resize(1280, 800, std::numeric_limits<float>::quiet_NaN()), "a NaN device scale"},
             {resize(1280, 800, std::numeric_limits<float>::infinity()), "an infinite scale"},
             {resize(1280, 800, 0.0f), "a zero scale"},
             {resize(1280, 800, -2.0f), "a negative scale"},
         }) {
      Expect(!ipc::DeserializeUiToEngine(bytes).has_value(),
             std::string("the decoder accepted ") + why);
    }

    const auto progress = [](float fraction) {
      ipc::ByteWriter writer;
      writer.WriteU32(ipc::kProtocolVersion);
      writer.WriteU8(3);  // LoadProgress
      writer.WriteF32(fraction);
      return writer.Take();
    };
    Expect(ipc::DeserializeEngineToUi(progress(0.5f)).has_value(), "a real fraction decodes");
    for (const float fraction : {-0.1f, 1.5f, std::numeric_limits<float>::quiet_NaN(),
                                 std::numeric_limits<float>::infinity()}) {
      Expect(!ipc::DeserializeEngineToUi(progress(fraction)).has_value(),
             "a fraction is a fraction; NaN makes every clamp that would catch it compare "
             "false");
    }

    const auto pointer = [](int x, int y) {
      ipc::ByteWriter writer;
      writer.WriteU32(ipc::kProtocolVersion);
      writer.WriteU8(6);  // Pointer
      writer.WriteU8(0);  // Down
      writer.WriteI32(x);
      writer.WriteI32(y);
      writer.WriteU8(1);  // button
      return writer.Take();
    };
    Expect(ipc::DeserializeUiToEngine(pointer(10, 10)).has_value(), "a real position decodes");
    Expect(!ipc::DeserializeUiToEngine(pointer(2000000000, 10)).has_value(),
           "a pointer position is hit-tested against layout geometry, so it lives in the same "
           "coordinate range every rect does");
  });

  AddTest(tests, "Ipc/APathIsSerializedAsGeometryRatherThanAnIndex", [] {
    // Two commands naming the same geometry must both survive independently.
    // If the wire carried indices into a shared table, a hostile frame could
    // name a path that is not there, and every consumer would need a range
    // check it might forget.
    gfx::Path square;
    square.AddRect(gfx::FloatRect{0.0f, 0.0f, 4.0f, 4.0f});

    ipc::PaintFrameMessage paint;
    paint.display_list.FillPath(square, Color::Rgb(1, 1, 1));
    paint.display_list.FillPath(square, Color::Rgb(2, 2, 2));

    const auto decoded = ipc::DeserializeEngineToUi(ipc::Serialize(ipc::EngineToUi{paint}));
    Expect(decoded.has_value(), "the frame must decode");
    const auto& list = std::get<ipc::PaintFrameMessage>(*decoded).display_list;
    ExpectEqInt(static_cast<long long>(list.Paths().size()), 2,
                "each command brought its own geometry across");
    Expect(list.PathAt(0) != nullptr && list.PathAt(1) != nullptr, "and both resolve");
  });

  AddTest(tests, "Ipc/AHostilePathFrameIsRejectedRatherThanDecodedPartially", [] {
    // Every one of these is a frame no encoder can produce. The decoder must
    // reject each, not build a half-populated display list from it.
    const auto frame = [](auto&& fill_body) {
      ipc::ByteWriter writer;
      writer.WriteU32(ipc::kProtocolVersion);
      writer.WriteU8(1);   // PaintFrame
      writer.WriteU32(1);  // one command
      fill_body(writer);
      writer.WriteU32(0);  // damage rect count, which closes the message
      return writer.Bytes();
    };

    // A well-formed frame built the same way must decode. Without this the
    // rejections below would all pass for the wrong reason — a missing trailing
    // field rather than the malformation each one is about.
    Expect(ipc::DeserializeEngineToUi(frame([](ipc::ByteWriter& w) {
             w.WriteU8(4);
             w.WriteU32(0xFF000000);
             w.WriteU8(0);
             w.WriteU32(2);
             w.WriteU8(0);  // Move
             w.WriteF32(0.0f);
             w.WriteF32(0.0f);
             w.WriteU8(1);  // Line
             w.WriteF32(4.0f);
             w.WriteF32(4.0f);
           })).has_value(),
           "the control frame must decode, or every rejection below proves nothing");

    Expect(!ipc::DeserializeEngineToUi(frame([](ipc::ByteWriter& w) {
             w.WriteU8(4);           // FillPath
             w.WriteU32(0xFF000000); // color
             w.WriteU8(0);           // NonZero
             w.WriteU32(0xFFFFFFFF); // verb count larger than the frame
           })).has_value(),
           "a verb count beyond the remaining bytes must be rejected before any allocation");

    Expect(!ipc::DeserializeEngineToUi(frame([](ipc::ByteWriter& w) {
             w.WriteU8(4);
             w.WriteU32(0xFF000000);
             w.WriteU8(0);
             w.WriteU32(1);
             w.WriteU8(200);  // not a verb
           })).has_value(),
           "an unknown path verb must be rejected, not skipped");

    Expect(!ipc::DeserializeEngineToUi(frame([](ipc::ByteWriter& w) {
             w.WriteU8(4);
             w.WriteU32(0xFF000000);
             w.WriteU8(0);
             w.WriteU32(1);
             w.WriteU8(2);        // Quad, which needs two points
             w.WriteF32(1.0f);    // and gets half of one
           })).has_value(),
           "a verb truncated mid-point must be rejected");

    Expect(!ipc::DeserializeEngineToUi(frame([](ipc::ByteWriter& w) {
             w.WriteU8(4);
             w.WriteU32(0xFF000000);
             w.WriteU8(9);  // not a fill rule
             w.WriteU32(0);
           })).has_value(),
           "an out-of-range fill rule must be rejected rather than cast into the enum");

    Expect(!ipc::DeserializeEngineToUi(frame([](ipc::ByteWriter& w) {
             w.WriteU8(5);  // StrokePath
             w.WriteU32(0xFF000000);
             w.WriteF32(2.0f);
             w.WriteF32(4.0f);
             w.WriteU8(77);  // not a line cap
             w.WriteU8(0);
             w.WriteU32(0);
           })).has_value(),
           "an out-of-range line cap must be rejected");

    Expect(!ipc::DeserializeEngineToUi(frame([](ipc::ByteWriter& w) {
             w.WriteU8(5);
             w.WriteU32(0xFF000000);
             w.WriteF32(std::numeric_limits<float>::quiet_NaN());
             w.WriteF32(4.0f);
             w.WriteU8(0);
             w.WriteU8(0);
             w.WriteU32(0);
           })).has_value(),
           "a non-finite stroke width is not a value the encoder can produce");
  });

  AddTest(tests, "Ipc/ANonFiniteCoordinateOnTheWireNeverReachesTheRasterizer", [] {
    // Nothing rejects the frame — the coordinates are structurally valid
    // floats. The Path builder the decoder replays through is what drops them,
    // which is the point of decoding through the builder rather than into the
    // vectors.
    ipc::ByteWriter writer;
    writer.WriteU32(ipc::kProtocolVersion);
    writer.WriteU8(1);
    writer.WriteU32(1);
    writer.WriteU8(4);
    writer.WriteU32(0xFF000000);
    writer.WriteU8(0);
    writer.WriteU32(2);
    writer.WriteU8(0);  // Move
    writer.WriteF32(std::numeric_limits<float>::infinity());
    writer.WriteF32(0.0f);
    writer.WriteU8(1);  // Line
    writer.WriteF32(std::numeric_limits<float>::quiet_NaN());
    writer.WriteF32(1.0f);
    writer.WriteU32(0);  // damage rect count

    const auto decoded = ipc::DeserializeEngineToUi(writer.Bytes());
    Expect(decoded.has_value(), "the frame is well-formed, so it decodes");
    const auto& list = std::get<ipc::PaintFrameMessage>(*decoded).display_list;
    Expect(list.IsEmpty(),
           "every command in it collapsed to nothing, because a path that lost every "
           "coordinate is not a path");
  });

  // Regression: `IntRect::Right()` is `x + width`, so these values overflowed a
  // signed int inside Execute — undefined behavior driven straight from a
  // renderer the security model assumes is compromised. UBSan caught it on the
  // first hostile frame anyone wrote by hand.
  AddTest(tests, "Ipc/ARectWhoseCornerOverflowsIsRejected", [] {
    const auto frame_with_rect = [](std::int32_t x, std::int32_t y, std::int32_t width,
                                    std::int32_t height) {
      ipc::ByteWriter writer;
      writer.WriteU32(ipc::kProtocolVersion);
      writer.WriteU8(1);   // PaintFrame
      writer.WriteU32(1);  // one command
      writer.WriteU8(1);   // FillRect
      writer.WriteI32(x);
      writer.WriteI32(y);
      writer.WriteI32(width);
      writer.WriteI32(height);
      writer.WriteU32(0xFF000000);
      writer.WriteU32(0);  // damage count
      return writer.Bytes();
    };

    Expect(ipc::DeserializeEngineToUi(frame_with_rect(10, 20, 30, 40)).has_value(),
           "an ordinary rect still decodes, or the rejections below prove nothing");

    Expect(!ipc::DeserializeEngineToUi(frame_with_rect(2000000000, 0, 2000000000, 10)).has_value(),
           "x + width past INT_MAX is signed overflow the moment anything intersects it");
    Expect(!ipc::DeserializeEngineToUi(frame_with_rect(0, 2000000000, 10, 2000000000)).has_value(),
           "and so is y + height");
    Expect(!ipc::DeserializeEngineToUi(
                frame_with_rect(-2000000000, 0, -2000000000, 10)).has_value(),
           "underflow is the same bug with the sign flipped");
    Expect(!ipc::DeserializeEngineToUi(frame_with_rect(1500000000, 0, 10, 10)).has_value(),
           "a rect beyond the device range is one EnclosingIntRect could never have made, "
           "so it is tampering rather than an unusual page");
  });

  AddTest(tests, "Ipc/DamageRectsAreCheckedLikeEveryOtherRect", [] {
    // The damage list is read by a different code path from the display list,
    // and a bounds check applied to one and not the other is the shape most
    // input-validation bugs actually have.
    ipc::ByteWriter writer;
    writer.WriteU32(ipc::kProtocolVersion);
    writer.WriteU8(1);
    writer.WriteU32(0);  // no commands
    writer.WriteU32(1);  // one damage rect
    writer.WriteI32(2000000000);
    writer.WriteI32(0);
    writer.WriteI32(2000000000);
    writer.WriteI32(10);
    Expect(!ipc::DeserializeEngineToUi(writer.Bytes()).has_value(),
           "a damage rect that overflows must be rejected too");
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
