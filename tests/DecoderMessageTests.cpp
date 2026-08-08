#include <cstdint>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "ipc/DecoderMessage.h"

namespace microbrowser::tests {

namespace {

void ExpectRoundTripConfigure(const ipc::ConfigureMessage& message) {
  const std::vector<std::uint8_t> encoded = ipc::EncodeConfigure(message);
  const ipc::DecoderDecodeResult decoded = ipc::DecodeDecoderMessage(encoded);
  Expect(decoded.status == ipc::DecoderDecode::Ok, "configure decode status");
  Expect(decoded.consumed == encoded.size(), "configure consumed");
  Expect(decoded.message.kind == ipc::DecoderMessageKind::Configure, "configure kind");
  Expect(decoded.message.configure.codec == message.codec, "configure codec");
  Expect(decoded.message.configure.extra_data == message.extra_data, "configure extra_data");
}

void ExpectRoundTripSample(const ipc::SampleMessage& message) {
  const std::vector<std::uint8_t> encoded = ipc::EncodeSample(message);
  const ipc::DecoderDecodeResult decoded = ipc::DecodeDecoderMessage(encoded);
  Expect(decoded.status == ipc::DecoderDecode::Ok, "sample decode status");
  Expect(decoded.consumed == encoded.size(), "sample consumed");
  Expect(decoded.message.kind == ipc::DecoderMessageKind::Sample, "sample kind");
  Expect(decoded.message.sample.timestamp_us == message.timestamp_us, "sample timestamp");
  Expect(decoded.message.sample.is_sync == message.is_sync, "sample sync");
  Expect(decoded.message.sample.bytes == message.bytes, "sample bytes");
}

void ExpectRoundTripFrame(const ipc::FrameMessage& message) {
  const std::vector<std::uint8_t> encoded = ipc::EncodeFrame(message);
  const ipc::DecoderDecodeResult decoded = ipc::DecodeDecoderMessage(encoded);
  Expect(decoded.status == ipc::DecoderDecode::Ok, "frame decode status");
  Expect(decoded.consumed == encoded.size(), "frame consumed");
  Expect(decoded.message.kind == ipc::DecoderMessageKind::Frame, "frame kind");
  Expect(decoded.message.frame.timestamp_us == message.timestamp_us, "frame timestamp");
  Expect(decoded.message.frame.width == message.width, "frame width");
  Expect(decoded.message.frame.height == message.height, "frame height");
  Expect(decoded.message.frame.sample_count == message.sample_count, "frame sample_count");
  Expect(decoded.message.frame.channels == message.channels, "frame channels");
  Expect(decoded.message.frame.bytes == message.bytes, "frame bytes");
}

void ExpectRoundTripError(const ipc::ErrorMessage& message) {
  const std::vector<std::uint8_t> encoded = ipc::EncodeError(message);
  const ipc::DecoderDecodeResult decoded = ipc::DecodeDecoderMessage(encoded);
  Expect(decoded.status == ipc::DecoderDecode::Ok, "error decode status");
  Expect(decoded.consumed == encoded.size(), "error consumed");
  Expect(decoded.message.kind == ipc::DecoderMessageKind::Error, "error kind");
  Expect(decoded.message.error.reason == message.reason, "error reason");
}

std::vector<std::uint8_t> Truncate(std::vector<std::uint8_t> bytes, std::size_t keep) {
  if (keep < bytes.size()) {
    bytes.resize(keep);
  }
  return bytes;
}

}  // namespace

void RegisterDecoderMessageTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DecoderMessage/RoundTripsConfigure", [] {
    ipc::ConfigureMessage message;
    message.codec = ipc::WireCodec::H264;
    message.extra_data = {0x01, 0x00, 0x00, 0x00, 0x42};
    ExpectRoundTripConfigure(message);
    message.codec = ipc::WireCodec::Opus;
    message.extra_data = {0x4F, 0x70, 0x75, 0x73};
    ExpectRoundTripConfigure(message);
  });

  AddTest(tests, "DecoderMessage/RoundTripsSample", [] {
    ipc::SampleMessage message;
    message.timestamp_us = 1'234'567;
    message.is_sync = true;
    message.bytes = {0x00, 0x00, 0x01, 0x09};
    ExpectRoundTripSample(message);
  });

  AddTest(tests, "DecoderMessage/RoundTripsFrame", [] {
    ipc::FrameMessage video;
    video.timestamp_us = 42;
    video.width = 2;
    video.height = 2;
    video.bytes = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    ExpectRoundTripFrame(video);

    ipc::FrameMessage audio;
    audio.timestamp_us = -1;
    audio.sample_count = 960;
    audio.channels = 2;
    audio.bytes = {0xAA, 0xBB, 0xCC};
    ExpectRoundTripFrame(audio);
  });

  AddTest(tests, "DecoderMessage/RoundTripsErrorAndFlush", [] {
    ExpectRoundTripError(ipc::ErrorMessage{"decode_failed"});
    const std::vector<std::uint8_t> flush = ipc::EncodeFlush();
    const ipc::DecoderDecodeResult decoded = ipc::DecodeDecoderMessage(flush);
    Expect(decoded.status == ipc::DecoderDecode::Ok, "flush decode status");
    Expect(decoded.consumed == flush.size(), "flush consumed");
    Expect(decoded.message.kind == ipc::DecoderMessageKind::Flush, "flush kind");
  });

  AddTest(tests, "DecoderMessage/IsKnownWireCodec", [] {
    Expect(ipc::IsKnownWireCodec(static_cast<std::uint8_t>(ipc::WireCodec::Av1)), "av1 known");
    Expect(!ipc::IsKnownWireCodec(0), "zero unknown");
    Expect(!ipc::IsKnownWireCodec(99), "large unknown");
  });

  AddTest(tests, "DecoderMessage/IncompleteOnTruncated", [] {
    const std::vector<std::uint8_t> encoded = ipc::EncodeSample(ipc::SampleMessage{});
    for (std::size_t keep = 0; keep < encoded.size(); ++keep) {
      const ipc::DecoderDecodeResult decoded = ipc::DecodeDecoderMessage(Truncate(encoded, keep));
      Expect(decoded.status == ipc::DecoderDecode::Incomplete, "truncated sample incomplete");
      Expect(decoded.consumed == 0, "truncated sample consumes nothing");
    }
  });

  AddTest(tests, "DecoderMessage/FailedOnBadLength", [] {
    std::vector<std::uint8_t> bytes = {0xFF, 0xFF, 0xFF, 0xFF};
    const ipc::DecoderDecodeResult oversized = ipc::DecodeDecoderMessage(bytes);
    Expect(oversized.status == ipc::DecoderDecode::Failed, "oversized length failed");
    Expect(oversized.consumed == 0, "oversized length consumes nothing");

    bytes = ipc::EncodeConfigure(ipc::ConfigureMessage{});
    bytes.push_back(0x00);
    const ipc::DecoderDecodeResult trailing = ipc::DecodeDecoderMessage(bytes);
    Expect(trailing.status == ipc::DecoderDecode::Ok, "first message still decodes");
    Expect(trailing.consumed == bytes.size() - 1, "trailing byte left behind");

    ipc::ConfigureMessage bad_codec;
    bad_codec.codec = ipc::WireCodec::Av1;
    bytes = ipc::EncodeConfigure(bad_codec);
    bytes[9] = 0x7F;
    const ipc::DecoderDecodeResult unknown_codec = ipc::DecodeDecoderMessage(bytes);
    Expect(unknown_codec.status == ipc::DecoderDecode::Failed, "unknown codec failed");
  });

  AddTest(tests, "DecoderMessage/RefusesDimensionOverflow", [] {
    ipc::FrameMessage frame;
    frame.width = ipc::kMaxFrameDimension + 1;
    frame.height = 16;
    frame.bytes = {0x01, 0x02, 0x03, 0x04};
    const ipc::DecoderDecodeResult wide = ipc::DecodeDecoderMessage(ipc::EncodeFrame(frame));
    Expect(wide.status == ipc::DecoderDecode::Failed, "wide frame failed");

    frame.width = 16;
    frame.height = ipc::kMaxFrameDimension + 1;
    const ipc::DecoderDecodeResult tall = ipc::DecodeDecoderMessage(ipc::EncodeFrame(frame));
    Expect(tall.status == ipc::DecoderDecode::Failed, "tall frame failed");

    frame.width = 4096;
    frame.height = 4096;
    frame.bytes.assign(4096 * 4096 * 4 - 1, 0xAB);
    const ipc::DecoderDecodeResult short_blob = ipc::DecodeDecoderMessage(ipc::EncodeFrame(frame));
    Expect(short_blob.status == ipc::DecoderDecode::Failed, "short blob failed");

    frame.bytes.assign(4096 * 4096 * 4 + 1, 0xAB);
    const ipc::DecoderDecodeResult long_blob = ipc::DecodeDecoderMessage(ipc::EncodeFrame(frame));
    Expect(long_blob.status == ipc::DecoderDecode::Failed, "long blob failed");

    ipc::ErrorMessage error;
    error.reason.assign(65, 'x');
    const ipc::DecoderDecodeResult long_reason = ipc::DecodeDecoderMessage(ipc::EncodeError(error));
    Expect(long_reason.status == ipc::DecoderDecode::Failed, "long error reason failed");
  });
}

}  // namespace microbrowser::tests
