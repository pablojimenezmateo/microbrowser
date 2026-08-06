#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace microbrowser::ipc {

// The decoder process's messages. ADR 0031 §3, session 27.
//
// **This is a trust boundary in both directions**, which is what makes it different from every other
// message set here. The engine's messages to the decoder are ordinary; the decoder's messages *back* come
// from a process whose whole reason for existing is that it runs a third-party codec on attacker-supplied
// bytes and may therefore be compromised. So the reply deserializer is written the way a network parser
// is: every length is checked against the bytes that arrived, every dimension is bounded before anything
// is sized from it, and a malformed reply is a dead decoder rather than a partially-read frame.
//
// **What crosses, and what deliberately does not.** ADR 0031 §3 lists three absences and each is a
// property of this header rather than a rule someone follows:
//
//   * **No URLs.** There is no field for one. The decoder never learns where a sample came from, cannot
//     fetch, and has no reason to know a network exists.
//   * **No file paths.** A configure message carries the codec's configuration record as *bytes* -- an
//     `avcC`, a `vpcC`, an Opus head, all of which the demuxer already reports as byte ranges. A
//     compromised decoder cannot ask for a file it was not given, because there is no way to name one.
//   * **No display list, no DOM, no JavaScript.** A decoded frame is pixels and a descriptor. ADR 0013's
//     video surface already means a frame reaches the screen as a hole plus a surface, so there is no
//     path from here into the paint tree.
//
// Framed length-prefixed on a pipe rather than a socket, because a pipe is what a spawned child already
// has and the sandbox (ADR 0031 §4) allows `read` and `write` on descriptors it was handed and nothing
// else. One framing for both directions, so there is one length check rather than two.

// The protocol between the engine and a decoder. Versioned separately from `kProtocolVersion`: the two
// seams change for different reasons, and one number would make a UI change look like a decoder change
// to a decoder built from a different commit.
inline constexpr std::uint32_t kDecoderProtocolVersion = 1;

// The largest message either side will accept. A decoder's *reply* is the dangerous direction and this is
// the first bound applied to it: an encoded sample is a few hundred kilobytes and a decoded 4K frame is
// about 12MB, so 32MB is generous against both and small enough that a compromised decoder claiming a
// gigabyte is refused before a byte is allocated.
inline constexpr std::size_t kMaxDecoderMessageBytes = 32u * 1024u * 1024u;

// The largest frame the engine will accept a descriptor for. 8192x8192 is past any real stream and is the
// bound that stops `width * height * 4` from overflowing anything.
inline constexpr std::uint32_t kMaxFrameDimension = 8192;

// Which codec, as *this browser's* enumeration rather than a string from a container.
//
// The same five `media::CodecId` names, deliberately duplicated as an integer here rather than shared:
// `src/ipc` may not see `src/media`, and more to the point a wire enumeration and an internal one should
// be free to diverge -- a decoder built from a different commit reads this number, and it must mean what
// it meant then. The conversion is one switch in the engine and one in the decoder, and a value outside
// the five is a refused configure.
enum class WireCodec : std::uint8_t {
  H264 = 1,
  Vp9 = 2,
  Av1 = 3,
  Aac = 4,
  Opus = 5,
};

bool IsKnownWireCodec(std::uint8_t value);

// Engine -> decoder.
struct ConfigureMessage {
  WireCodec codec = WireCodec::Av1;
  // The codec's configuration record, as bytes. Never interpreted on this side.
  std::vector<std::uint8_t> extra_data;
};

struct SampleMessage {
  // Microseconds, which is finer than any container's timescale resolves to and coarse enough that a
  // 64-bit value covers half a million years. Not the container's own ticks: two containers with
  // different timescales feeding one decoder would make the decoder do the arithmetic, and the decoder is
  // the thing with the fewest reasons to be trusted with arithmetic.
  std::int64_t timestamp_us = 0;
  bool is_sync = false;
  std::vector<std::uint8_t> bytes;
};

// Decoder -> engine. **The dangerous direction.**
struct FrameMessage {
  std::int64_t timestamp_us = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Audio: samples per channel in this frame, and the channel count. Zero for video, and the two are
  // never both set -- one decoder instance is one stream, which ADR 0031 §5 makes a process each.
  std::uint32_t sample_count = 0;
  std::uint8_t channels = 0;
  // The pixels or the samples. **A copy, not a mapping.** ADR 0031 §3 says frames come back in shared
  // memory; this carries them inline instead, and the difference is deliberate for now: shared memory
  // means the engine maps a region a compromised decoder can write at any time, which needs a
  // double-buffer protocol and a fence to be safe. A copy is one memcpy per frame -- measurable, and
  // measurably safe. Recorded as the thing to revisit when the profile says so, and *not* silently.
  std::vector<std::uint8_t> bytes;
};

struct ErrorMessage {
  // A short reason, chosen from a fixed set by the decoder. Never echoed into a log without bound and
  // never derived from sample bytes: a decoder is a process that may be compromised, and a
  // format-string-shaped reason from it is a reason not to have free text here at all.
  std::string reason;
};

// One framed message, either direction. The tag is first so that a reader knows what it is holding before
// it reads a length that belongs to a different shape.
enum class DecoderMessageKind : std::uint8_t {
  Configure = 1,
  Sample = 2,
  Frame = 3,
  Error = 4,
  Flush = 5,
};

std::vector<std::uint8_t> EncodeConfigure(const ConfigureMessage& message);
std::vector<std::uint8_t> EncodeSample(const SampleMessage& message);
std::vector<std::uint8_t> EncodeFrame(const FrameMessage& message);
std::vector<std::uint8_t> EncodeError(const ErrorMessage& message);
std::vector<std::uint8_t> EncodeFlush();

// What one decoded message is, if it is anything.
//
// A tagged struct rather than a variant because both readers are loops over a byte stream and a variant
// would put a visit inside each: the tag is what they switch on anyway.
struct DecoderMessage {
  DecoderMessageKind kind = DecoderMessageKind::Error;
  ConfigureMessage configure;
  SampleMessage sample;
  FrameMessage frame;
  ErrorMessage error;
};

// Reads one message from the front of `bytes`.
//
// Three answers, and they are three because a caller does different things with each: a complete message
// (with `consumed` set), *not yet* (nothing consumed -- read more), and **malformed**, which for the
// reply direction means killing the decoder. Returning "not yet" for a malformed message would be an
// instruction to buffer forever, which is the WebSocket framing lesson applied here.
enum class DecoderDecode : std::uint8_t { Ok, Incomplete, Failed };

struct DecoderDecodeResult {
  DecoderDecode status = DecoderDecode::Incomplete;
  DecoderMessage message;
  std::size_t consumed = 0;
};

DecoderDecodeResult DecodeDecoderMessage(std::span<const std::uint8_t> bytes);

}  // namespace microbrowser::ipc
