#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace microbrowser::media {

// YouTube's UMP framing (`application/vnd.yt-ump`): varint type + varint size + payload.
//
// The page's player is supposed to demux this before `appendBuffer`. When it does not -- or when a
// binding hands the response body through as a single chunk the demuxer never saw -- the bytes that
// reach MSE are still UMP. Refusing them as "not fMP4" is correct for a strict demuxer and useless
// for a watch page: the MEDIA parts (type 21) already carry the fMP4/WebM the parser wants, after a
// one-byte header-id prefix.
//
// This extracts every complete type-21 payload's media bytes (skipping that prefix) into one
// contiguous buffer. Incomplete trailing parts are dropped rather than guessed -- a truncated
// download that mid-frames a part is a later request's problem, not ours to invent.
std::optional<std::vector<std::byte>> ExtractUmpMedia(std::span<const std::byte> input);

}  // namespace microbrowser::media
