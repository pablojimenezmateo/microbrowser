#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace microbrowser::tests {

// A TrueType font built in memory, for tests.
//
// Text reference tests need a font, and there are three ways to get one. A
// system font makes the goldens depend on whichever version of DejaVu the
// machine happens to ship. A checked-in font adds a megabyte of binary to a
// repository that has none, and a license file to go with it. This is the
// third: a font small enough to write, whose glyphs are shapes whose areas are
// known in closed form, so the tests can assert *what was rendered* rather than
// only that something was.
//
// It also exercises the parts of the pipeline that matter — the cmap lookup,
// the y-axis flip out of font space, quadratic contours, and a contour wound
// backwards to cut a hole — which a font full of letters would exercise no
// better and verify far worse.
//
// The glyph repertoire, all on a 1000-unit em:
//
//   U+0020 space   no outline, advance 500
//   U+0041 'A'     square, (100,0)-(700,600)          area 360000 units^2
//   U+0042 'B'     triangle, (0,0),(600,0),(0,600)    area 180000
//   U+0043 'C'     square with a square hole          area 480000
//   U+0044 'D'     square with one quadratic edge     area 420000
//
// Areas are exact. The curved one is the square (600x600 = 360000) plus the
// region between a quadratic and its chord, which is two thirds of the triangle
// on its control polygon: (2/3) * (600 * 300 / 2) = 60000.

struct SyntheticFontSpec {
  int units_per_em = 1000;
  int ascent = 800;
  int descent = 200;  // positive, as a distance
  int line_gap = 0;
};

// The exact areas above, in square font units, indexed the same way the
// codepoints are. Exposed so a test states the expected value once.
double SyntheticGlyphArea(char32_t codepoint);

// Advance width in font units.
int SyntheticGlyphAdvance(char32_t codepoint);

std::vector<std::byte> BuildSyntheticFont(const SyntheticFontSpec& spec = {});

// A font file that parses far enough to be interesting and then is not a font:
// valid header, table directory pointing outside the file. Used to check that a
// bad font is a routine nullopt rather than a crash.
std::vector<std::byte> BuildCorruptFont();

}  // namespace microbrowser::tests
