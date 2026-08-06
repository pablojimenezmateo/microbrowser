#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace microbrowser::media {

// The five codecs this browser will decode, and the only names for them.
//
// ADR 0031 §2, and this table is the decision rather than an implementation detail of it: **the
// allowlist lives in our code, not in a build flag.** ADR 0013 says the container is ours because it
// is the layer that decides what the codec is asked to decode; a `--enable-decoder=...` build flag
// does not keep that true, because it is correct on the day it is written and drifts the first time
// somebody debugs a build. A table here fails a test instead.
//
// Five entries, from measurement rather than from a wish to be complete: `avc1` and `mp4a.40.2` are
// what the HLS stream this was measured against serves and what Plex direct-plays, VP9 and Opus are
// what YouTube's WebM carries, and AV1 is where the web is going.
enum class CodecId : std::uint8_t {
  H264,
  Vp9,
  Av1,
  Aac,
  Opus,
};

// The codec a container named, or nothing.
//
// **Nothing is the important answer.** A container can name anything -- a `MediaSource` type string is
// a page's own text, and a WebM's `CodecID` is a string a file chose -- and everything that is not one
// of the five is refused *before* a decoder library is configured, which is what the ADR means by
// constraining the codec's input. A library that would happily accept a sixth codec never gets asked.
//
// It takes both spellings because there are two: WebM says `V_VP9` and `A_OPUS`, MP4 says `vp09` and
// `Opus`, and an HLS playlist says `avc1.64001f` with a profile suffix. Reconciling them here is what
// stops four callers from each writing `codec.find("vp9")`.
std::optional<CodecId> CodecFromContainerName(std::string_view name);

// The name this browser uses for a codec in a message or a log. Stable, and deliberately not the
// container's spelling: a decoder process is configured with *this* enumeration, so a log line and a
// message name the same thing.
std::string_view CodecName(CodecId codec);

// Whether a full MSE type string is one this browser can play: `video/mp4; codecs="avc1.64001f,
// mp4a.40.2"`.
//
// **Every** codec in the list must be supported, not any of them -- a page asking for a stream with
// one codec this browser has and one it does not cannot play that stream, and answering yes would
// mean accepting bytes nothing will ever decode. The container must be one there is a demuxer for,
// which is the other half of the same question and the reason this lives beside the codec table
// rather than in the MSE code: `addSourceBuffer` and `canPlayType` must not answer differently.
bool IsSupportedMediaSourceType(std::string_view type);

// Whether this codec is audio. Not a property of the decoder -- a property of the *stream*, which
// decides which of the two pipelines a sample belongs to and therefore which clock it feeds.
bool IsAudioCodec(CodecId codec);

}  // namespace microbrowser::media
