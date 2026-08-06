#include "media/CodecId.h"

#include <algorithm>
#include <string>

#include "util/StringUtil.h"

namespace microbrowser::media {

namespace {

// Every spelling of every codec this browser decodes, and nothing else.
//
// A prefix match rather than equality, because a codec string carries parameters: `avc1.64001f` is
// H.264 at a profile and level, `mp4a.40.2` is AAC-LC and `mp4a.40.5` is HE-AAC, and `vp09.00.10.08`
// is VP9 with a profile. The *parameters* are the decoder's business -- it reads them from the
// configuration record, not from the name -- and this layer's job is only to answer which of the five
// it is.
//
// `mp4a.40` is the one entry that needs care: `mp4a` alone is an MPEG-4 audio object type this table
// cannot resolve (it could be anything the ISO registry lists), so only the `.40` family -- MPEG-4
// audio, which is AAC -- matches. `mp4a.40.34` is MP3-in-MP4 and is *not* accepted, which is why the
// table lists the two profiles rather than the family.
struct Entry {
  std::string_view prefix;
  CodecId codec;
};

constexpr Entry kAllowed[] = {
    // H.264, in the three spellings the containers use.
    {"avc1", CodecId::H264},
    {"avc3", CodecId::H264},
    {"v_mpeg4/iso/avc", CodecId::H264},
    // VP9.
    {"vp09", CodecId::Vp9},
    {"vp9", CodecId::Vp9},
    {"v_vp9", CodecId::Vp9},
    // AV1.
    {"av01", CodecId::Av1},
    {"v_av1", CodecId::Av1},
    // AAC. Only the two profiles a browser plays: LC and HE.
    {"mp4a.40.2", CodecId::Aac},
    {"mp4a.40.5", CodecId::Aac},
    {"mp4a.40.29", CodecId::Aac},
    {"a_aac", CodecId::Aac},
    // Opus.
    {"opus", CodecId::Opus},
    {"a_opus", CodecId::Opus},
};

}  // namespace

std::optional<CodecId> CodecFromContainerName(std::string_view name) {
  const std::string lowered = util::AsciiLowerCase(std::string(name));
  const std::string_view trimmed = util::TrimAscii(lowered);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  // Longest match first, so `mp4a.40.2` is AAC and a bare `mp4a` is nothing. Sorting by length at
  // the point of use rather than keeping the table sorted, because a table whose *order* is load
  // bearing is a table the next person breaks by adding a row in the obvious place.
  const Entry* best = nullptr;
  for (const Entry& entry : kAllowed) {
    if (trimmed.rfind(entry.prefix, 0) != 0) {
      continue;
    }
    if (best == nullptr || entry.prefix.size() > best->prefix.size()) {
      best = &entry;
    }
  }
  return best == nullptr ? std::nullopt : std::optional<CodecId>(best->codec);
}

std::string_view CodecName(CodecId codec) {
  switch (codec) {
    case CodecId::H264:
      return "h264";
    case CodecId::Vp9:
      return "vp9";
    case CodecId::Av1:
      return "av1";
    case CodecId::Aac:
      return "aac";
    case CodecId::Opus:
      return "opus";
  }
  return "unknown";
}

bool IsSupportedMediaSourceType(std::string_view type) {
  // The container comes first, and an unknown one is refused before the codec list is even looked at.
  // `video/mp4` and `audio/mp4` are the fragmented-MP4 path ADR 0007 measured; `video/webm` and
  // `audio/webm` are the Matroska one. Everything else -- `video/mp2t`, `application/x-mpegurl`,
  // anything a page invents -- has no demuxer behind it.
  const std::size_t semicolon = type.find(';');
  std::string container = util::AsciiLowerCase(util::TrimAscii(type.substr(0, semicolon)));
  if (container != "video/mp4" && container != "audio/mp4" && container != "video/webm" &&
      container != "audio/webm") {
    return false;
  }
  if (semicolon == std::string_view::npos) {
    // No codec list. Accepted: it is a legal type string, and it means "whatever is inside", which the
    // demuxer will name for itself when the initialization segment arrives -- and `CodecId` refuses an
    // unsupported codec at *that* point too. Two chances to refuse, neither of them a guess.
    return true;
  }
  const std::string_view parameters = type.substr(semicolon + 1);
  const std::size_t codecs_at = util::AsciiLowerCase(std::string(parameters)).find("codecs");
  if (codecs_at == std::string_view::npos) {
    return true;
  }
  const std::size_t equals = parameters.find('=', codecs_at);
  if (equals == std::string_view::npos) {
    return false;  // `codecs` with no value is not a parameter anybody wrote on purpose.
  }
  std::string_view list = util::TrimAscii(parameters.substr(equals + 1));
  // The quotes are optional in the wild and always present in practice.
  if (list.size() >= 2 && (list.front() == '"' || list.front() == '\'') && list.back() == list.front()) {
    list = list.substr(1, list.size() - 2);
  }
  if (util::TrimAscii(list).empty()) {
    return false;
  }
  std::size_t at = 0;
  while (at <= list.size()) {
    const std::size_t comma = list.find(',', at);
    const std::string_view one =
        util::TrimAscii(list.substr(at, comma == std::string_view::npos ? comma : comma - at));
    if (one.empty() || !CodecFromContainerName(one).has_value()) {
      return false;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    at = comma + 1;
  }
  return true;
}

bool IsAudioCodec(CodecId codec) { return codec == CodecId::Aac || codec == CodecId::Opus; }

}  // namespace microbrowser::media
