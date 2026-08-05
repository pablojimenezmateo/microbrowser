#include "gfx/Woff2.h"

#include <algorithm>
#include <cstring>

#include "gfx/Woff2Internal.h"
#include "util/Brotli.h"
#include "util/PerformanceCounters.h"
#include "util/SaturatingMath.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;
using woff2::Reader;

constexpr std::uint32_t kWoff2Signature = 0x774F4632u;  // 'wOF2'

// How many tables one font may have. A real font has fewer than thirty; the field
// is 16 bits, and every entry costs a directory read and a bounds check, so this
// is the bound that stops a 12-byte header from describing 65,535 tables.
constexpr std::uint16_t kMaxTables = 512;

// The tag table WOFF2 uses to spell a known tag in five bits. Order is normative:
// the index *is* the tag, so a wrong entry silently renames a table -- which is why
// this is written out in full rather than derived.
constexpr const char* kKnownTags[] = {
    "cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm",
    "glyf", "loca", "prep", "CFF ", "VORG", "EBDT", "EBLC", "gasp", "hdmx", "kern",
    "LTSH", "PCLT", "VDMX", "vhea", "vmtx", "BASE", "GDEF", "GPOS", "GSUB", "EBSC",
    "JSTF", "MATH", "CBDT", "CBLC", "COLR", "CPAL", "SVG ", "sbix", "acnt", "avar",
    "bdat", "bloc", "bsln", "cvar", "fdsc", "feat", "fmtx", "fvar", "gvar", "hsty",
    "just", "lcar", "mort", "morx", "opbd", "prop", "trak", "Zapf", "Silf", "Glat",
    "Gloc", "Feat", "Sill",
};

struct TableEntry {
  std::uint32_t tag = 0;
  std::uint32_t length = 0;              // the table's length in the reassembled font
  std::size_t offset = 0;                // its offset in the decompressed stream
  std::uint32_t transformed_length = 0;  // how much of the stream it occupies
  bool transformed = false;
  // Set only for a table this decoder *produces* rather than copies: the
  // reconstructed `glyf`, the `loca` that is not in the file at all, and the `head`
  // whose `indexToLocFormat` had to be corrected to match them. An empty body means
  // "copy it out of the decompressed stream", which is every other table.
  std::vector<std::byte> body;

  std::uint32_t Length() const {
    return body.empty() ? length : static_cast<std::uint32_t>(body.size());
  }
};

std::uint32_t TagFromName(const char* name) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(name[0])) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(name[1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(name[2])) << 8) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(name[3]));
}

void AppendU16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::byte>(value & 0xFFu));
}

void AppendU32(std::vector<std::byte>& out, std::uint32_t value) {
  AppendU16(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
  AppendU16(out, static_cast<std::uint16_t>(value & 0xFFFFu));
}


// The `glyf` and `loca` a transformed font does not contain, plus the `head`
// fix-up that keeps them readable.
//
// Two tables are produced from one, which is why this cannot be done in the
// directory loop: `loca` is declared with a length of zero and *no* bytes in the
// stream, and every offset in it is a consequence of rebuilding the outlines. The
// third table touched is `head`, whose `indexToLocFormat` must agree with the
// `loca` actually written -- and does not always agree with the one the file asked
// for, because this decoder re-encodes outlines slightly larger than a hand-tuned
// font does.
bool ReconstructTransformedTables(const std::vector<std::byte>& decompressed,
                                  std::size_t max_output, std::vector<TableEntry>& tables) {
  TableEntry* glyf = nullptr;
  TableEntry* loca = nullptr;
  TableEntry* head = nullptr;
  TableEntry* maxp = nullptr;
  for (TableEntry& table : tables) {
    if (table.tag == TagFromName("glyf")) {
      glyf = &table;
    } else if (table.tag == TagFromName("loca")) {
      loca = &table;
    } else if (table.tag == TagFromName("head")) {
      head = &table;
    } else if (table.tag == TagFromName("maxp")) {
      maxp = &table;
    }
  }
  const bool glyf_transformed = glyf != nullptr && glyf->transformed;
  const bool loca_transformed = loca != nullptr && loca->transformed;
  if (!glyf_transformed && !loca_transformed) {
    return true;
  }
  // The two are transformed together or not at all, and a transformed `loca` holds
  // nothing: a file where one is and the other is not describes outlines with an
  // index into a different font.
  if (!glyf_transformed || !loca_transformed || loca->transformed_length != 0) {
    AddPerformanceCounter(PerfCounterId::GfxWoff2TransformedRefusals);
    return false;
  }
  if (glyf->offset + glyf->transformed_length > decompressed.size()) {
    AddPerformanceCounter(PerfCounterId::GfxWoff2TransformedRefusals);
    return false;
  }
  const std::optional<woff2::GlyfTables> rebuilt = woff2::ReconstructGlyf(
      std::span<const std::byte>(decompressed).subspan(glyf->offset, glyf->transformed_length),
      max_output);
  if (!rebuilt.has_value()) {
    return false;  // ReconstructGlyf counts its own refusals
  }

  // `maxp` decides how many `loca` entries a rasterizer will read. A transformed
  // `glyf` carries its own count, and the two disagreeing is a font whose last
  // glyph is read from whatever follows the table -- so it is a refusal rather
  // than a font with one wrong glyph.
  const std::size_t entry_size = rebuilt->index_format == 0 ? 2u : 4u;
  const std::size_t glyph_count = rebuilt->loca.size() / entry_size - 1u;
  if (maxp != nullptr && maxp->length >= 6u && maxp->offset + 6u <= decompressed.size()) {
    const std::size_t at = maxp->offset + 4u;
    const std::size_t declared =
        (static_cast<std::size_t>(static_cast<std::uint8_t>(decompressed[at])) << 8) |
        static_cast<std::uint8_t>(decompressed[at + 1u]);
    if (declared != glyph_count) {
      AddPerformanceCounter(PerfCounterId::GfxWoff2TransformedRefusals);
      return false;
    }
  }

  // `head` is where a rasterizer reads whether `loca` is short or long, and this
  // decoder decides that itself. A transformed font without a `head` long enough to
  // hold the field is refused rather than reassembled: the alternative is a `loca`
  // nothing can address correctly, which reads every glyph at double or half its
  // offset -- a font drawn out of the middle of other glyphs.
  if (head == nullptr || head->length < 54u || head->offset + head->length > decompressed.size()) {
    AddPerformanceCounter(PerfCounterId::GfxWoff2TransformedRefusals);
    return false;
  }
  glyf->body = rebuilt->glyf;
  loca->body = rebuilt->loca;
  head->body.assign(decompressed.begin() + static_cast<std::ptrdiff_t>(head->offset),
                    decompressed.begin() +
                        static_cast<std::ptrdiff_t>(head->offset + head->length));
  head->body[50] = std::byte{0};
  head->body[51] = static_cast<std::byte>(rebuilt->index_format & 0xFFu);
  AddPerformanceCounter(PerfCounterId::GfxWoff2GlyfReconstructed);
  return true;
}

}  // namespace

bool IsWoff2(std::span<const std::byte> input) {
  if (input.size() < 4) {
    return false;
  }
  Reader reader(input);
  return reader.U32() == kWoff2Signature && reader.Ok();
}

std::optional<Woff2Font> DecodeWoff2(std::span<const std::byte> input, std::size_t max_output) {
  Reader header(input);
  if (header.U32() != kWoff2Signature) {
    return std::nullopt;
  }
  const std::uint32_t flavor = header.U32();
  header.U32();  // length: the file's own size, which we already have
  const std::uint16_t table_count = header.U16();
  header.U16();  // reserved
  const std::uint32_t total_sfnt_size = header.U32();
  const std::uint32_t compressed_length = header.U32();
  header.U16();  // major version
  header.U16();  // minor version
  header.U32();  // meta offset
  header.U32();  // meta length
  header.U32();  // meta original length
  header.U32();  // private offset
  header.U32();  // private length
  if (!header.Ok() || table_count == 0 || table_count > kMaxTables) {
    return std::nullopt;
  }
  // The declared reassembled size, refused from its own claim before anything is
  // decompressed -- which is the check brotli itself cannot make, since a brotli
  // stream declares no output length. This is the one place a WOFF2 *does* say how
  // large it becomes, and taking the claim seriously is what makes the bound cheap.
  if (total_sfnt_size > max_output) {
    AddPerformanceCounter(PerfCounterId::GfxWoff2Refusals);
    return std::nullopt;
  }

  std::vector<TableEntry> tables;
  tables.reserve(table_count);
  std::size_t running = 0;
  for (std::uint16_t i = 0; i < table_count; ++i) {
    const std::uint8_t flags = header.U8();
    std::uint32_t tag = 0;
    if ((flags & 0x3Fu) == 0x3Fu) {
      tag = header.U32();  // an arbitrary tag, spelled out
    } else {
      const std::uint8_t index = flags & 0x3Fu;
      if (index >= std::size(kKnownTags)) {
        return std::nullopt;
      }
      tag = TagFromName(kKnownTags[index]);
    }
    std::uint32_t original_length = 0;
    if (!header.Base128(original_length)) {
      return std::nullopt;
    }
    const std::uint8_t transform = static_cast<std::uint8_t>((flags >> 6) & 0x03u);
    std::uint32_t transformed_length = original_length;
    const bool is_glyf_or_loca =
        tag == TagFromName("glyf") || tag == TagFromName("loca");
    // Transform 0 means *transformed* for glyf/loca and *untransformed* for
    // everything else, which is the one place this format's flag inverts its own
    // meaning. Getting it backwards would refuse every ordinary font and accept
    // every transformed one, so it is written as the specification states it.
    const bool transformed = is_glyf_or_loca ? transform == 0 : transform != 0;
    if (transformed) {
      if (!header.Base128(transformed_length)) {
        return std::nullopt;
      }
      if (!is_glyf_or_loca) {
        // The other transform this format defines is `hmtx`'s, which drops the
        // left-side-bearing arrays on the grounds that they repeat the glyph
        // bounding boxes. Reconstructing it means reading every glyph's box back
        // out of the `glyf` we just rebuilt, and no font measured here uses it --
        // so it is refused, and the counter says which refusal it was.
        AddPerformanceCounter(PerfCounterId::GfxWoff2TransformedRefusals);
        return std::nullopt;
      }
    }
    if (!header.Ok()) {
      return std::nullopt;
    }
    // A duplicate tag is refused rather than resolved. An sfnt has one table per
    // tag, so a directory with two `glyf` entries is asking which one every later
    // question means -- and "the last one wins" versus "the first one wins" is the
    // shape of half the CVEs this format has had. It is also free to check here,
    // where the directory is being built anyway.
    for (const TableEntry& seen : tables) {
      if (seen.tag == tag) {
        AddPerformanceCounter(PerfCounterId::GfxWoff2Refusals);
        return std::nullopt;
      }
    }
    tables.push_back(TableEntry{tag, original_length, running, transformed_length, transformed, {}});
    // Saturating, because these lengths are the file's: a directory whose sum
    // wraps would place a later table *before* an earlier one.
    running = util::SaturatingAdd(running, static_cast<std::size_t>(transformed_length));
    if (running > max_output) {
      AddPerformanceCounter(PerfCounterId::GfxWoff2Refusals);
      return std::nullopt;
    }
  }

  const std::size_t stream_start = header.Offset();
  if (stream_start > input.size() ||
      compressed_length > input.size() - stream_start) {
    return std::nullopt;
  }
  std::vector<std::byte> decompressed;
  if (!util::BrotliInflate(input.subspan(stream_start, compressed_length), max_output,
                           decompressed)) {
    AddPerformanceCounter(PerfCounterId::GfxWoff2Refusals);
    return std::nullopt;
  }
  // Every table has to be *inside* what came out. A directory that describes more
  // than the stream holds is the case that would otherwise read past the end, and
  // it is checked once here rather than per table copy.
  if (running > decompressed.size()) {
    AddPerformanceCounter(PerfCounterId::GfxWoff2Refusals);
    return std::nullopt;
  }

  if (!ReconstructTransformedTables(decompressed, max_output, tables)) {
    return std::nullopt;
  }

  // The reassembled file: an sfnt header, a real table directory with offsets and
  // lengths, then the tables, each padded to four bytes.
  const std::uint16_t entry_selector_count = table_count;
  std::uint16_t search_range = 16;
  std::uint16_t entry_selector = 0;
  while (static_cast<std::uint32_t>(search_range) * 2u <=
         static_cast<std::uint32_t>(entry_selector_count) * 16u) {
    search_range = static_cast<std::uint16_t>(search_range * 2);
    ++entry_selector;
  }

  std::vector<std::byte> sfnt;
  const std::size_t directory_size = 12u + static_cast<std::size_t>(table_count) * 16u;
  sfnt.reserve(std::min<std::size_t>(max_output, directory_size + running + running / 4u + 16u));
  AppendU32(sfnt, flavor);
  AppendU16(sfnt, table_count);
  AppendU16(sfnt, search_range);
  AppendU16(sfnt, entry_selector);
  AppendU16(sfnt,
            static_cast<std::uint16_t>(static_cast<std::uint32_t>(table_count) * 16u -
                                       static_cast<std::uint32_t>(search_range)));

  // The directory is written first and needs each table's final offset, so the
  // offsets are computed before any table body is copied. Padding is part of the
  // arithmetic rather than applied afterwards, because an offset that did not
  // account for it points into the previous table's tail.
  std::size_t table_offset = directory_size;
  std::vector<std::size_t> offsets;
  offsets.reserve(tables.size());
  for (const TableEntry& table : tables) {
    offsets.push_back(table_offset);
    const std::uint32_t length = table.Length();
    table_offset = util::SaturatingAdd(table_offset, static_cast<std::size_t>(length));
    table_offset = util::SaturatingAdd(table_offset,
                                      static_cast<std::size_t>((4u - (length % 4u)) % 4u));
    if (table_offset > max_output) {
      AddPerformanceCounter(PerfCounterId::GfxWoff2Refusals);
      return std::nullopt;
    }
  }
  for (std::size_t i = 0; i < tables.size(); ++i) {
    AppendU32(sfnt, tables[i].tag);
    // The checksum a real sfnt carries is deliberately zero: nothing in this
    // browser verifies one, FreeType does not require it, and computing a value we
    // never check would be work that only looks like a guarantee.
    AppendU32(sfnt, 0);
    AppendU32(sfnt, static_cast<std::uint32_t>(offsets[i]));
    AppendU32(sfnt, tables[i].Length());
  }
  for (const TableEntry& table : tables) {
    if (!table.body.empty()) {
      sfnt.insert(sfnt.end(), table.body.begin(), table.body.end());
    } else {
      if (table.offset + table.length > decompressed.size()) {
        return std::nullopt;
      }
      sfnt.insert(sfnt.end(), decompressed.begin() + static_cast<std::ptrdiff_t>(table.offset),
                  decompressed.begin() +
                      static_cast<std::ptrdiff_t>(table.offset + table.length));
    }
    for (std::uint32_t pad = (4u - (table.Length() % 4u)) % 4u; pad > 0; --pad) {
      sfnt.push_back(std::byte{0});
    }
  }
  if (sfnt.size() > max_output) {
    AddPerformanceCounter(PerfCounterId::GfxWoff2Refusals);
    return std::nullopt;
  }
  AddPerformanceCounter(PerfCounterId::GfxWoff2Decoded);
  Woff2Font font;
  font.sfnt = std::move(sfnt);
  return font;
}

}  // namespace microbrowser::gfx
