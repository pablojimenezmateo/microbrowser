#include "support/SyntheticFont.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace microbrowser::tests {

namespace {

// --- Big-endian writing ------------------------------------------------------
// Every field in an sfnt is big-endian, so there is exactly one way to write a
// number here and the helpers make forgetting it impossible.

struct Writer {
  std::vector<std::byte> bytes;

  void U8(std::uint8_t v) { bytes.push_back(static_cast<std::byte>(v)); }
  void U16(std::uint16_t v) {
    U8(static_cast<std::uint8_t>(v >> 8));
    U8(static_cast<std::uint8_t>(v & 0xFFu));
  }
  void I16(std::int16_t v) { U16(static_cast<std::uint16_t>(v)); }
  void U32(std::uint32_t v) {
    U16(static_cast<std::uint16_t>(v >> 16));
    U16(static_cast<std::uint16_t>(v & 0xFFFFu));
  }
  void Tag(std::string_view tag) {
    for (const char c : tag) {
      U8(static_cast<std::uint8_t>(c));
    }
  }
  void Pad4() {
    while (bytes.size() % 4 != 0) {
      U8(0);
    }
  }
  std::size_t Size() const { return bytes.size(); }
};

// --- Glyph geometry ----------------------------------------------------------

struct GlyphPoint {
  std::int16_t x = 0;
  std::int16_t y = 0;
  bool on_curve = true;
};

struct GlyphOutline {
  std::vector<std::vector<GlyphPoint>> contours;
  std::int16_t advance = 0;
};

// TrueType convention: outer contours run clockwise in font space (y up), inner
// contours run the other way. Our rasterizer takes the absolute winding, so the
// direction only decides whether the inner square of 'C' is a hole — which is
// exactly why one glyph here has one.
GlyphOutline SquareGlyph() {
  GlyphOutline glyph;
  glyph.advance = 800;
  glyph.contours.push_back({{100, 0}, {100, 600}, {700, 600}, {700, 0}});
  return glyph;
}

GlyphOutline TriangleGlyph() {
  GlyphOutline glyph;
  glyph.advance = 700;
  glyph.contours.push_back({{0, 0}, {0, 600}, {600, 0}});
  return glyph;
}

GlyphOutline HoledSquareGlyph() {
  GlyphOutline glyph;
  glyph.advance = 900;
  glyph.contours.push_back({{0, 0}, {0, 800}, {800, 800}, {800, 0}});
  // Reversed, so the nonzero rule cancels it to a hole.
  glyph.contours.push_back({{200, 200}, {600, 200}, {600, 600}, {200, 600}});
  return glyph;
}

GlyphOutline CurvedGlyph() {
  GlyphOutline glyph;
  glyph.advance = 700;
  // A 600x600 square whose top edge bulges upward as one quadratic. The off
  // curve point is what makes FreeType emit a conic, which is the branch of the
  // outline decomposer a font of straight-edged glyphs would never reach.
  glyph.contours.push_back({{0, 0},
                            {0, 600},
                            {300, 900, false},  // control point, off curve
                            {600, 600},
                            {600, 0}});
  return glyph;
}

struct GlyphEntry {
  char32_t codepoint;
  GlyphOutline outline;
};

std::vector<GlyphEntry> Repertoire() {
  std::vector<GlyphEntry> glyphs;
  GlyphOutline space;
  space.advance = 500;
  glyphs.push_back({U' ', space});
  glyphs.push_back({U'A', SquareGlyph()});
  glyphs.push_back({U'B', TriangleGlyph()});
  glyphs.push_back({U'C', HoledSquareGlyph()});
  glyphs.push_back({U'D', CurvedGlyph()});
  return glyphs;
}

// --- Tables ------------------------------------------------------------------

std::vector<std::byte> BuildGlyphData(const GlyphOutline& glyph) {
  Writer w;
  if (glyph.contours.empty()) {
    return w.bytes;  // an empty glyf entry is how a space is spelled
  }

  std::int16_t min_x = 32767;
  std::int16_t min_y = 32767;
  std::int16_t max_x = -32768;
  std::int16_t max_y = -32768;
  std::size_t total_points = 0;
  for (const auto& contour : glyph.contours) {
    total_points += contour.size();
    for (const GlyphPoint& p : contour) {
      min_x = std::min(min_x, p.x);
      min_y = std::min(min_y, p.y);
      max_x = std::max(max_x, p.x);
      max_y = std::max(max_y, p.y);
    }
  }

  w.I16(static_cast<std::int16_t>(glyph.contours.size()));
  w.I16(min_x);
  w.I16(min_y);
  w.I16(max_x);
  w.I16(max_y);

  std::size_t index = 0;
  for (const auto& contour : glyph.contours) {
    index += contour.size();
    w.U16(static_cast<std::uint16_t>(index - 1));
  }
  w.U16(0);  // instructionLength

  // Flags: bit 0 is ON_CURVE_POINT. The short-vector and same-as-previous bits
  // stay clear, so every coordinate is a plain int16 delta. Larger than a real
  // font would be, and far easier to be certain about.
  for (const auto& contour : glyph.contours) {
    for (const GlyphPoint& p : contour) {
      w.U8(p.on_curve ? 0x01u : 0x00u);
    }
  }
  (void)total_points;

  std::int16_t previous = 0;
  for (const auto& contour : glyph.contours) {
    for (const GlyphPoint& p : contour) {
      w.I16(static_cast<std::int16_t>(p.x - previous));
      previous = p.x;
    }
  }
  previous = 0;
  for (const auto& contour : glyph.contours) {
    for (const GlyphPoint& p : contour) {
      w.I16(static_cast<std::int16_t>(p.y - previous));
      previous = p.y;
    }
  }
  w.Pad4();
  return w.bytes;
}

// Format 4 cmap over the repertoire. One segment for the space, one for the
// A-D run, and the mandatory 0xFFFF terminator.
std::vector<std::byte> BuildCmap() {
  struct Segment {
    std::uint16_t start;
    std::uint16_t end;
    std::uint16_t first_glyph;
  };
  const std::array<Segment, 3> segments{
      Segment{0x0020, 0x0020, 1},
      Segment{0x0041, 0x0044, 2},
      Segment{0xFFFF, 0xFFFF, 0},
  };
  const auto seg_count = static_cast<std::uint16_t>(segments.size());

  Writer sub;
  sub.U16(4);
  sub.U16(static_cast<std::uint16_t>(16 + 8 * seg_count));  // length
  sub.U16(0);                                               // language
  sub.U16(static_cast<std::uint16_t>(seg_count * 2));
  // searchRange = 2 * 2^floor(log2(segCount)); with three segments that is 4.
  sub.U16(4);
  sub.U16(1);                                                   // entrySelector
  sub.U16(static_cast<std::uint16_t>(seg_count * 2 - 4));        // rangeShift
  for (const Segment& s : segments) {
    sub.U16(s.end);
  }
  sub.U16(0);  // reservedPad
  for (const Segment& s : segments) {
    sub.U16(s.start);
  }
  for (const Segment& s : segments) {
    // The terminator maps 0xFFFF to glyph 0, which needs a delta of 1 and the
    // wrap it implies. Every other segment is glyph = code + delta.
    const std::uint16_t delta =
        s.start == 0xFFFF ? std::uint16_t{1}
                          : static_cast<std::uint16_t>(s.first_glyph - s.start);
    sub.U16(delta);
  }
  for (std::size_t i = 0; i < segments.size(); ++i) {
    sub.U16(0);  // idRangeOffset
  }

  Writer table;
  table.U16(0);  // version
  table.U16(1);  // numTables
  table.U16(3);  // platformID: Windows
  table.U16(1);  // encodingID: Unicode BMP
  table.U32(12);  // offset to the subtable
  table.bytes.insert(table.bytes.end(), sub.bytes.begin(), sub.bytes.end());
  table.Pad4();
  return table.bytes;
}

std::uint32_t Checksum(const std::vector<std::byte>& table) {
  std::uint32_t sum = 0;
  for (std::size_t i = 0; i + 3 < table.size(); i += 4) {
    std::uint32_t word = 0;
    for (std::size_t b = 0; b < 4; ++b) {
      word = (word << 8) | static_cast<std::uint32_t>(table[i + b]);
    }
    sum += word;
  }
  return sum;
}

struct Table {
  std::string_view tag;
  std::vector<std::byte> data;
};

std::vector<std::byte> Assemble(std::vector<Table> tables) {
  // The table directory is sorted by tag, which is what a reader binary-searches
  // over.
  std::sort(tables.begin(), tables.end(),
            [](const Table& a, const Table& b) { return a.tag < b.tag; });

  const auto count = static_cast<std::uint16_t>(tables.size());
  std::uint16_t entry_selector = 0;
  while ((1u << (entry_selector + 1)) <= count) {
    ++entry_selector;
  }
  const auto search_range = static_cast<std::uint16_t>((1u << entry_selector) * 16u);

  Writer out;
  out.U32(0x00010000);  // sfntVersion: TrueType outlines
  out.U16(count);
  out.U16(search_range);
  out.U16(entry_selector);
  out.U16(static_cast<std::uint16_t>(count * 16 - search_range));

  std::uint32_t offset = 12 + static_cast<std::uint32_t>(count) * 16;
  for (const Table& table : tables) {
    out.Tag(table.tag);
    out.U32(Checksum(table.data));
    out.U32(offset);
    out.U32(static_cast<std::uint32_t>(table.data.size()));
    offset += static_cast<std::uint32_t>(table.data.size());
  }
  for (const Table& table : tables) {
    out.bytes.insert(out.bytes.end(), table.data.begin(), table.data.end());
  }
  return out.bytes;
}

}  // namespace

double SyntheticGlyphArea(char32_t codepoint) {
  switch (codepoint) {
    case U'A':
      return 600.0 * 600.0;
    case U'B':
      return 600.0 * 600.0 / 2.0;
    case U'C':
      return 800.0 * 800.0 - 400.0 * 400.0;
    case U'D':
      // Square plus the region between the quadratic and its chord, which is
      // two thirds of the control triangle.
      return 600.0 * 600.0 + (2.0 / 3.0) * (600.0 * 300.0 / 2.0);
    default:
      return 0.0;
  }
}

int SyntheticGlyphAdvance(char32_t codepoint) {
  for (const GlyphEntry& entry : Repertoire()) {
    if (entry.codepoint == codepoint) {
      return entry.outline.advance;
    }
  }
  return 0;
}

std::vector<std::byte> BuildSyntheticFont(const SyntheticFontSpec& spec) {
  const std::vector<GlyphEntry> repertoire = Repertoire();
  const auto glyph_count = static_cast<std::uint16_t>(repertoire.size() + 1);  // +1 for .notdef

  // glyf and loca. Glyph 0 is .notdef and is empty, which is legal and keeps
  // the table honest about what is actually being tested.
  Writer glyf;
  std::vector<std::uint32_t> loca;
  loca.push_back(0);
  loca.push_back(0);  // .notdef occupies no bytes
  std::size_t max_points = 0;
  std::size_t max_contours = 0;
  for (const GlyphEntry& entry : repertoire) {
    const std::vector<std::byte> data = BuildGlyphData(entry.outline);
    glyf.bytes.insert(glyf.bytes.end(), data.begin(), data.end());
    loca.push_back(static_cast<std::uint32_t>(glyf.bytes.size()));
    max_contours = std::max(max_contours, entry.outline.contours.size());
    std::size_t points = 0;
    for (const auto& contour : entry.outline.contours) {
      points += contour.size();
    }
    max_points = std::max(max_points, points);
  }
  glyf.Pad4();

  Writer loca_table;
  for (const std::uint32_t value : loca) {
    loca_table.U32(value);  // long format, selected by indexToLocFormat below
  }
  loca_table.Pad4();

  Writer head;
  head.U32(0x00010000);  // version
  head.U32(0x00010000);  // fontRevision
  head.U32(0);           // checkSumAdjustment: readers tolerate zero
  head.U32(0x5F0F3CF5);  // magicNumber
  head.U16(0);           // flags
  head.U16(static_cast<std::uint16_t>(spec.units_per_em));
  for (int i = 0; i < 4; ++i) {
    head.U32(0);  // created and modified, eight bytes each
  }
  head.I16(0);                                            // xMin
  head.I16(0);                                            // yMin
  head.I16(static_cast<std::int16_t>(spec.units_per_em));  // xMax
  head.I16(static_cast<std::int16_t>(spec.units_per_em));  // yMax
  head.U16(0);  // macStyle
  head.U16(8);  // lowestRecPPEM
  head.I16(2);  // fontDirectionHint
  head.I16(1);  // indexToLocFormat: long
  head.I16(0);  // glyphDataFormat
  head.Pad4();

  Writer hhea;
  hhea.U32(0x00010000);
  hhea.I16(static_cast<std::int16_t>(spec.ascent));
  hhea.I16(static_cast<std::int16_t>(-spec.descent));  // negative, as the spec has it
  hhea.I16(static_cast<std::int16_t>(spec.line_gap));
  hhea.U16(1000);  // advanceWidthMax
  hhea.I16(0);     // minLeftSideBearing
  hhea.I16(0);     // minRightSideBearing
  hhea.I16(1000);  // xMaxExtent
  hhea.I16(1);     // caretSlopeRise
  hhea.I16(0);     // caretSlopeRun
  hhea.I16(0);     // caretOffset
  for (int i = 0; i < 4; ++i) {
    hhea.I16(0);  // reserved
  }
  hhea.I16(0);              // metricDataFormat
  hhea.U16(glyph_count);    // numberOfHMetrics: one per glyph, so hmtx is uniform
  hhea.Pad4();

  // The left side bearing must equal the glyph's own xMin. TrueType positions an
  // outline *by* its lsb, so a font that disagrees with itself here gets its
  // glyphs silently translated by the difference — which is exactly what
  // happened the first time this was written with a hardcoded zero.
  Writer hmtx;
  hmtx.U16(0);  // .notdef advance
  hmtx.I16(0);  // .notdef left side bearing
  for (const GlyphEntry& entry : repertoire) {
    std::int16_t left = 0;
    bool has_point = false;
    for (const auto& contour : entry.outline.contours) {
      for (const GlyphPoint& p : contour) {
        left = has_point ? std::min(left, p.x) : p.x;
        has_point = true;
      }
    }
    hmtx.U16(static_cast<std::uint16_t>(entry.outline.advance));
    hmtx.I16(has_point ? left : std::int16_t{0});
  }
  hmtx.Pad4();

  Writer maxp;
  maxp.U32(0x00010000);
  maxp.U16(glyph_count);
  maxp.U16(static_cast<std::uint16_t>(max_points));
  maxp.U16(static_cast<std::uint16_t>(max_contours));
  maxp.U16(0);  // maxCompositePoints
  maxp.U16(0);  // maxCompositeContours
  maxp.U16(2);  // maxZones
  maxp.U16(0);  // maxTwilightPoints
  maxp.U16(0);  // maxStorage
  maxp.U16(0);  // maxFunctionDefs
  maxp.U16(0);  // maxInstructionDefs
  maxp.U16(0);  // maxStackElements
  maxp.U16(0);  // maxSizeOfInstructions
  maxp.U16(0);  // maxComponentElements
  maxp.U16(0);  // maxComponentDepth
  maxp.Pad4();

  return Assemble({
      Table{"cmap", BuildCmap()},
      Table{"glyf", glyf.bytes},
      Table{"head", head.bytes},
      Table{"hhea", hhea.bytes},
      Table{"hmtx", hmtx.bytes},
      Table{"loca", loca_table.bytes},
      Table{"maxp", maxp.bytes},
  });
}

std::vector<std::byte> BuildCorruptFont() {
  Writer out;
  out.U32(0x00010000);
  out.U16(1);   // one table
  out.U16(16);
  out.U16(0);
  out.U16(0);
  out.Tag("head");
  out.U32(0);
  out.U32(0x7FFFFFFF);  // offset far past the end of the file
  out.U32(54);
  return out.bytes;
}

}  // namespace microbrowser::tests
