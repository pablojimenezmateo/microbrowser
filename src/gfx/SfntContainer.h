#pragma once

#include <cstddef>
#include <span>

namespace microbrowser::gfx {

// Whether these bytes are an sfnt whose table directory describes itself.
//
// ADR 0024 §3: **every downloaded font is validated as a container before FreeType
// sees it.** This is that check, and the reason it is worth writing when FreeType
// bounds-checks its own reads is the shape of the risk rather than the presence of a
// second check: a font is the one attacker-controlled input this browser hands to a
// large C library with a bytecode interpreter in it, and the malformed-container
// class -- a directory that points outside the file, two tables claiming the same
// bytes -- is removable in eighty lines. What is left after this is the *decoders*,
// which is where a font parser's remaining risk actually lives.
//
// What it checks, and each one is a real file this rejects:
//
//   * A version this browser would want: TrueType, `true`, or `OTTO`. A collection
//     (`ttcf`) is refused because it has no single face -- FreeType would take
//     face 0, and "which face did the page get" would be decided by a header the
//     page's own server wrote.
//   * A directory that fits in the file, and a table count bounded well above what
//     any real font has.
//   * Every table inside the file, computed with saturating arithmetic, because
//     `offset + length` is two numbers from the file.
//   * **No two tables overlapping.** This is the one a naive validator misses. Two
//     entries pointing at the same bytes is how one table gets read as two -- a
//     `loca` that is also a `glyf`, and a glyph index that indexes the wrong array.
//
// It deliberately does *not* require particular tables or a sorted directory:
// FreeType decides what it needs, and WOFF2 preserves the original font's table
// order, which is not sorted. A validator stricter than the format refuses fonts
// that work everywhere else.
bool SfntContainerIsSane(std::span<const std::byte> input);

}  // namespace microbrowser::gfx
