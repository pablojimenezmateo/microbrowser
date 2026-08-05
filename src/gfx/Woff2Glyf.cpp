// The transformed `glyf`, rebuilt into the table a rasterizer reads.
//
// ADR 0024, and the half of WOFF2 that is not a container: every WOFF2 the web
// actually serves transforms `glyf` and drops `loca` entirely. A decoder that
// refuses a transformed `glyf` refuses Google Fonts, which is where measurement
// put this file -- the Inter face at fonts.gstatic.com has `glyf` transform 0,
// and so does every face produced by the reference compressor.
//
// The shape. A transformed `glyf` is **seven parallel substreams**, walked in
// lockstep, one glyph at a time: how many contours, how many points per contour,
// one flag byte per point, the point deltas, the composite components, the
// bounding boxes, and the instructions. Nothing in it is self-delimiting -- a
// glyph's position in every stream is the sum of what the glyphs before it
// consumed -- so a single wrong count does not corrupt one glyph, it shears every
// later glyph in the font. That is what every bound here is protecting, and it is
// also why the reader is sticky-failing: the first read past the end of a
// substream must keep saying so rather than answering about the next glyph.
//
// The outlines come back as *deltas*, which is what the standard `glyf` format
// stores too, so this re-encodes them rather than round-tripping through absolute
// coordinates. It does not attempt the repeat-flag compression a hand-built font
// uses: the result is a slightly larger `glyf` describing exactly the same
// outlines, and "slightly larger" is why `index_format` can come back as long
// where the file declared short.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

#include "gfx/Woff2Internal.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::gfx::woff2 {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Composite component flags, from the OpenType `glyf` table. Only the ones that
// change how many bytes a component occupies, plus the two that are questions
// about the glyph rather than about its bytes.
constexpr std::uint16_t kArgsAreWords = 0x0001;
constexpr std::uint16_t kHaveScale = 0x0008;
constexpr std::uint16_t kMoreComponents = 0x0020;
constexpr std::uint16_t kHaveXAndYScale = 0x0040;
constexpr std::uint16_t kHaveTwoByTwo = 0x0080;
constexpr std::uint16_t kHaveInstructions = 0x0100;

// Simple-glyph point flags.
constexpr std::uint8_t kOnCurve = 0x01;
constexpr std::uint8_t kXShort = 0x02;
constexpr std::uint8_t kYShort = 0x04;
constexpr std::uint8_t kXSameOrPositive = 0x10;
constexpr std::uint8_t kYSameOrPositive = 0x20;
constexpr std::uint8_t kOverlapSimple = 0x40;

// How many components one composite glyph may name, and how many points one glyph
// may have. Both are bounds on work rather than on validity: the format's own
// limit on points is 65,535 (the `loca`/`endPtsOfContours` fields are 16 bits) and
// a glyph at that limit is already absurd, but the *sum* over 65,535 glyphs is
// what decides how long a malformed font can keep this function busy.
constexpr std::size_t kMaxComponents = 4096;
constexpr std::size_t kMaxPointsPerGlyph = 65535;

void AppendU16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::byte>(value & 0xFFu));
}

void AppendU32(std::vector<std::byte>& out, std::uint32_t value) {
  AppendU16(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
  AppendU16(out, static_cast<std::uint16_t>(value & 0xFFFFu));
}

void AppendI16(std::vector<std::byte>& out, std::int32_t value) {
  AppendU16(out, static_cast<std::uint16_t>(static_cast<std::uint32_t>(value) & 0xFFFFu));
}

// WOFF2's `255UInt16`: one, two or three bytes, and deliberately not a canonical
// encoding -- 506 has three spellings. Nothing here depends on which was used, and
// nothing may: these are point counts and instruction lengths, never identities.
bool Read255UShort(Reader& reader, std::uint16_t& out) {
  const std::uint8_t code = reader.U8();
  if (!reader.Ok()) {
    return false;
  }
  std::uint32_t value = 0;
  switch (code) {
    case 253:
      value = reader.U16();
      break;
    case 254:
      value = static_cast<std::uint32_t>(reader.U8()) + 506u;
      break;
    case 255:
      value = static_cast<std::uint32_t>(reader.U8()) + 253u;
      break;
    default:
      value = code;
      break;
  }
  if (!reader.Ok() || value > 0xFFFFu) {
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
}

// The sign is the low bit of the flag, which is why the y sign is `flag >> 1`.
std::int32_t WithSign(std::uint8_t flag, std::int32_t magnitude) {
  return (flag & 1u) != 0 ? magnitude : -magnitude;
}

struct Point {
  std::int32_t dx = 0;
  std::int32_t dy = 0;
  bool on_curve = false;
};

// One point's flag byte and its 1-4 bytes of coordinate data, decoded into a
// delta. This is the format's whole compression idea: 128 encodings chosen so the
// common case -- a small move in both axes -- costs two bytes including the flag.
//
// Written as arithmetic rather than as a 128-row table on purpose. The table is
// generated by exactly this arithmetic, and a transcribed table is 896 numbers
// with no way to tell a typo from a specification detail; here a wrong constant
// shows up as every glyph in every font being wrong, which a fixture catches.
bool DecodePoint(std::uint8_t raw_flag, Reader& glyphs, Point& point) {
  point.on_curve = (raw_flag >> 7) == 0;
  const std::uint8_t flag = raw_flag & 0x7Fu;
  if (flag < 10) {
    point.dx = 0;
    point.dy = WithSign(flag, ((flag & 14) << 7) + glyphs.U8());
  } else if (flag < 20) {
    point.dx = WithSign(flag, (((flag - 10) & 14) << 7) + glyphs.U8());
    point.dy = 0;
  } else if (flag < 84) {
    const std::int32_t b0 = flag - 20;
    const std::int32_t b1 = glyphs.U8();
    point.dx = WithSign(flag, 1 + (b0 & 0x30) + (b1 >> 4));
    point.dy = WithSign(static_cast<std::uint8_t>(flag >> 1),
                        1 + ((b0 & 0x0C) << 2) + (b1 & 0x0F));
  } else if (flag < 120) {
    const std::int32_t b0 = flag - 84;
    const std::int32_t x = glyphs.U8();
    const std::int32_t y = glyphs.U8();
    point.dx = WithSign(flag, 1 + ((b0 / 12) << 8) + x);
    point.dy = WithSign(static_cast<std::uint8_t>(flag >> 1),
                        1 + (((b0 % 12) >> 2) << 8) + y);
  } else if (flag < 124) {
    const std::int32_t b0 = glyphs.U8();
    const std::int32_t b1 = glyphs.U8();
    const std::int32_t b2 = glyphs.U8();
    point.dx = WithSign(flag, (b0 << 4) + (b1 >> 4));
    point.dy = WithSign(static_cast<std::uint8_t>(flag >> 1), ((b1 & 0x0F) << 8) + b2);
  } else {
    const std::int32_t x0 = glyphs.U8();
    const std::int32_t x1 = glyphs.U8();
    const std::int32_t y0 = glyphs.U8();
    const std::int32_t y1 = glyphs.U8();
    point.dx = WithSign(flag, (x0 << 8) + x1);
    point.dy = WithSign(static_cast<std::uint8_t>(flag >> 1), (y0 << 8) + y1);
  }
  // A `glyf` stores each coordinate as a 16-bit delta, so a delta this format can
  // express but that format cannot is refused rather than truncated: a truncated
  // delta is a point in the wrong place, and every point after it inherits the
  // error because they are cumulative.
  return glyphs.Ok() && point.dx >= -32768 && point.dx <= 32767 && point.dy >= -32768 &&
         point.dy <= 32767;
}

// The point flags and coordinates, in the standard `glyf` encoding. No repeat
// compression: a repeat run saves bytes in a file we are about to hand to a
// rasterizer and then throw away, and the encoder for it is another place to be
// wrong about a font.
void AppendPoints(const std::vector<Point>& points, bool overlap,
                  std::vector<std::byte>& out) {
  std::vector<std::byte> xs;
  std::vector<std::byte> ys;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const Point& point = points[i];
    std::uint8_t flag = point.on_curve ? kOnCurve : 0u;
    if (i == 0 && overlap) {
      flag |= kOverlapSimple;
    }
    if (point.dx == 0) {
      flag |= kXSameOrPositive;
    } else if (point.dx >= -255 && point.dx <= 255) {
      flag |= kXShort;
      if (point.dx > 0) {
        flag |= kXSameOrPositive;
      }
      xs.push_back(static_cast<std::byte>(std::abs(point.dx)));
    } else {
      AppendI16(xs, point.dx);
    }
    if (point.dy == 0) {
      flag |= kYSameOrPositive;
    } else if (point.dy >= -255 && point.dy <= 255) {
      flag |= kYShort;
      if (point.dy > 0) {
        flag |= kYSameOrPositive;
      }
      ys.push_back(static_cast<std::byte>(std::abs(point.dy)));
    } else {
      AppendI16(ys, point.dy);
    }
    out.push_back(static_cast<std::byte>(flag));
  }
  out.insert(out.end(), xs.begin(), xs.end());
  out.insert(out.end(), ys.begin(), ys.end());
}

// The seven substreams, plus the bounding-box bitmap that sits in front of the
// bbox stream and is sized by glyph count rather than declared.
struct Substreams {
  Reader contours;
  Reader points;
  Reader flags;
  Reader glyphs;
  Reader composites;
  Reader bboxes;
  Reader instructions;
  std::span<const std::byte> bbox_bitmap;
  std::span<const std::byte> overlap_bitmap;
  std::uint16_t glyph_count = 0;
  std::uint16_t index_format = 0;
};

bool BitSet(std::span<const std::byte> bitmap, std::uint16_t index) {
  const std::size_t byte = index >> 3;
  if (byte >= bitmap.size()) {
    return false;
  }
  return (static_cast<std::uint8_t>(bitmap[byte]) & (0x80u >> (index & 7u))) != 0;
}

bool ReadHeader(std::span<const std::byte> transformed, Substreams& out) {
  Reader header(transformed);
  header.U16();  // version
  const std::uint16_t option_flags = header.U16();
  out.glyph_count = header.U16();
  out.index_format = header.U16();
  std::size_t sizes[7] = {};
  for (std::size_t& size : sizes) {
    size = header.U32();
  }
  if (!header.Ok() || out.glyph_count == 0) {
    return false;
  }
  // Every substream has to fit inside what is left, and the sum has to be checked
  // in the running total rather than at the end: seven 4GB claims sum to something
  // that fits in a size_t on a 64-bit machine and to nothing at all on 32 bits.
  std::size_t at = header.Offset();
  const bool has_overlap = (option_flags & 0x0001u) != 0;
  if (at > transformed.size()) {
    return false;
  }
  std::span<const std::byte> streams[7];
  for (std::size_t i = 0; i < 7; ++i) {
    if (sizes[i] > transformed.size() - at) {
      return false;
    }
    streams[i] = transformed.subspan(at, sizes[i]);
    at += sizes[i];
  }
  out.contours = Reader(streams[0]);
  out.points = Reader(streams[1]);
  out.flags = Reader(streams[2]);
  out.glyphs = Reader(streams[3]);
  out.composites = Reader(streams[4]);
  out.instructions = Reader(streams[6]);

  // The bbox bitmap is one bit per glyph rounded up to a *four-byte* boundary,
  // which is not the same as one byte per eight glyphs: a 33-glyph font has an
  // eight-byte bitmap. Getting that wrong shifts the whole bbox stream.
  const std::size_t bitmap_size = ((static_cast<std::size_t>(out.glyph_count) + 31u) >> 5) << 2;
  if (bitmap_size > streams[5].size()) {
    return false;
  }
  out.bbox_bitmap = streams[5].subspan(0, bitmap_size);
  out.bboxes = Reader(streams[5].subspan(bitmap_size));

  if (has_overlap) {
    const std::size_t overlap_size = (static_cast<std::size_t>(out.glyph_count) + 7u) >> 3;
    if (overlap_size > transformed.size() - at) {
      return false;
    }
    out.overlap_bitmap = transformed.subspan(at, overlap_size);
    at += overlap_size;
  }
  // The specification says the substreams account for the table exactly. Trailing
  // bytes mean a length was misread, and a misread length here is a sheared font
  // rather than a smaller one.
  return at == transformed.size();
}

bool ReadInstructions(Substreams& streams, std::span<const std::byte>& out) {
  std::uint16_t length = 0;
  if (!Read255UShort(streams.glyphs, length)) {
    return false;
  }
  out = streams.instructions.Take(length);
  return streams.instructions.Ok();
}

// A composite glyph, copied verbatim. The components are parsed only to find where
// they end -- re-serializing a component's transform would be a second encoder to
// disagree with the first, and a composite glyph whose transform is subtly
// different is a glyph drawn at the wrong scale.
bool ReconstructComposite(Substreams& streams, std::uint16_t glyph_index,
                          std::vector<std::byte>& out) {
  const std::size_t start = streams.composites.Offset();
  bool have_instructions = false;
  std::size_t components = 0;
  for (bool more = true; more;) {
    if (++components > kMaxComponents) {
      return false;
    }
    const std::uint16_t flags = streams.composites.U16();
    streams.composites.U16();  // the component's glyph index
    streams.composites.Take((flags & kArgsAreWords) != 0 ? 4u : 2u);
    if ((flags & kHaveTwoByTwo) != 0) {
      streams.composites.Take(8);
    } else if ((flags & kHaveXAndYScale) != 0) {
      streams.composites.Take(4);
    } else if ((flags & kHaveScale) != 0) {
      streams.composites.Take(2);
    }
    if (!streams.composites.Ok()) {
      return false;
    }
    have_instructions = have_instructions || (flags & kHaveInstructions) != 0;
    more = (flags & kMoreComponents) != 0;
  }
  const std::span<const std::byte> body =
      streams.composites.Between(start, streams.composites.Offset());

  // A composite glyph's bounding box is never computed: it would mean resolving
  // every component, and the format stores it for exactly that reason. A file that
  // omits it is refused rather than given a zero box, which would make the glyph
  // invisible in a way that looks like a layout bug.
  if (!BitSet(streams.bbox_bitmap, glyph_index)) {
    return false;
  }
  const std::int16_t x_min = streams.bboxes.I16();
  const std::int16_t y_min = streams.bboxes.I16();
  const std::int16_t x_max = streams.bboxes.I16();
  const std::int16_t y_max = streams.bboxes.I16();
  if (!streams.bboxes.Ok()) {
    return false;
  }
  std::span<const std::byte> instructions;
  if (have_instructions && !ReadInstructions(streams, instructions)) {
    return false;
  }

  AppendI16(out, -1);
  AppendI16(out, x_min);
  AppendI16(out, y_min);
  AppendI16(out, x_max);
  AppendI16(out, y_max);
  out.insert(out.end(), body.begin(), body.end());
  if (have_instructions) {
    AppendU16(out, static_cast<std::uint16_t>(instructions.size()));
    out.insert(out.end(), instructions.begin(), instructions.end());
  }
  return true;
}

bool ReconstructSimple(Substreams& streams, std::uint16_t glyph_index,
                       std::int16_t contour_count, std::vector<std::byte>& out) {
  std::vector<std::uint16_t> end_points;
  end_points.reserve(static_cast<std::size_t>(contour_count));
  std::size_t total_points = 0;
  for (std::int16_t i = 0; i < contour_count; ++i) {
    std::uint16_t in_contour = 0;
    if (!Read255UShort(streams.points, in_contour)) {
      return false;
    }
    total_points += in_contour;
    if (total_points > kMaxPointsPerGlyph) {
      return false;
    }
    // `endPtsOfContours` holds the *index* of each contour's last point, so an
    // empty contour would have to name the previous contour's last point -- which
    // is a contour two glyph parsers disagree about. Refused.
    if (in_contour == 0) {
      return false;
    }
    end_points.push_back(static_cast<std::uint16_t>(total_points - 1));
  }

  std::vector<Point> points;
  points.reserve(total_points);
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t x_min = 0;
  std::int32_t y_min = 0;
  std::int32_t x_max = 0;
  std::int32_t y_max = 0;
  for (std::size_t i = 0; i < total_points; ++i) {
    const std::uint8_t flag = streams.flags.U8();
    Point point;
    if (!streams.flags.Ok() || !DecodePoint(flag, streams.glyphs, point)) {
      return false;
    }
    x += point.dx;
    y += point.dy;
    // The absolute coordinates exist only for the bounding box: what a `glyf`
    // stores is the deltas, which is what came out of the stream. They are still
    // bounded, because a cumulative sum of 65,535 legal deltas is not a legal
    // coordinate and a font whose points leave the 16-bit grid is one FreeType
    // would read differently than this does.
    if (x < -32768 || x > 32767 || y < -32768 || y > 32767) {
      return false;
    }
    x_min = i == 0 ? x : std::min(x_min, x);
    y_min = i == 0 ? y : std::min(y_min, y);
    x_max = i == 0 ? x : std::max(x_max, x);
    y_max = i == 0 ? y : std::max(y_max, y);
    points.push_back(point);
  }

  std::span<const std::byte> instructions;
  if (!ReadInstructions(streams, instructions)) {
    return false;
  }
  // An explicit box overrides the computed one. Both spellings occur in real
  // fonts: the compressor drops the box when it is exactly the outline's extent,
  // which is the common case, and keeps it when the font's own box was larger.
  if (BitSet(streams.bbox_bitmap, glyph_index)) {
    x_min = streams.bboxes.I16();
    y_min = streams.bboxes.I16();
    x_max = streams.bboxes.I16();
    y_max = streams.bboxes.I16();
    if (!streams.bboxes.Ok()) {
      return false;
    }
  }

  AppendI16(out, contour_count);
  AppendI16(out, x_min);
  AppendI16(out, y_min);
  AppendI16(out, x_max);
  AppendI16(out, y_max);
  for (const std::uint16_t end : end_points) {
    AppendU16(out, end);
  }
  AppendU16(out, static_cast<std::uint16_t>(instructions.size()));
  out.insert(out.end(), instructions.begin(), instructions.end());
  AppendPoints(points, BitSet(streams.overlap_bitmap, glyph_index), out);
  return true;
}

}  // namespace

std::optional<GlyfTables> ReconstructGlyf(std::span<const std::byte> transformed,
                                          std::size_t max_output) {
  Substreams streams;
  if (!ReadHeader(transformed, streams)) {
    AddPerformanceCounter(PerfCounterId::GfxWoff2TransformedRefusals);
    return std::nullopt;
  }

  GlyfTables tables;
  std::vector<std::size_t> offsets;
  offsets.reserve(static_cast<std::size_t>(streams.glyph_count) + 1u);
  for (std::uint16_t i = 0; i < streams.glyph_count; ++i) {
    offsets.push_back(tables.glyf.size());
    const std::int16_t contour_count = streams.contours.I16();
    if (!streams.contours.Ok()) {
      AddPerformanceCounter(PerfCounterId::GfxWoff2TransformedRefusals);
      return std::nullopt;
    }
    bool ok = true;
    if (contour_count == 0) {
      // An empty glyph -- a space -- occupies no bytes at all, and its `loca`
      // entry equals the next one. Nothing is read from any substream for it, not
      // even a bounding box.
    } else if (contour_count == -1) {
      ok = ReconstructComposite(streams, i, tables.glyf);
    } else if (contour_count < 0) {
      ok = false;  // -2 and below have never meant anything
    } else {
      ok = ReconstructSimple(streams, i, contour_count, tables.glyf);
    }
    if (!ok) {
      AddPerformanceCounter(PerfCounterId::GfxWoff2TransformedRefusals);
      return std::nullopt;
    }
    // Padded so every offset is even, which a short `loca` requires because it
    // stores each offset halved. Padding to four rather than two costs at most
    // three bytes a glyph and keeps the table aligned for a reader that cares.
    while ((tables.glyf.size() % 4u) != 0) {
      tables.glyf.push_back(std::byte{0});
    }
    if (tables.glyf.size() > max_output) {
      AddPerformanceCounter(PerfCounterId::GfxWoff2Refusals);
      return std::nullopt;
    }
  }
  offsets.push_back(tables.glyf.size());

  // Short offsets are stored halved, so the largest a short `loca` can address is
  // 0x1FFFF * 2. This decoder re-encodes outlines slightly larger than a
  // hand-tuned font does, so a file that declared short can land above that -- and
  // the answer is a long `loca` plus the `head` fix-up, not a refusal.
  tables.index_format = streams.index_format == 0 && offsets.back() <= 0x1FFFEu ? 0u : 1u;
  tables.loca.reserve(offsets.size() * (tables.index_format == 0 ? 2u : 4u));
  for (const std::size_t offset : offsets) {
    if (tables.index_format == 0) {
      AppendU16(tables.loca, static_cast<std::uint16_t>(offset / 2u));
    } else {
      AppendU32(tables.loca, static_cast<std::uint32_t>(offset));
    }
  }
  return tables;
}

}  // namespace microbrowser::gfx::woff2
