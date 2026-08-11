#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::text {

// Unicode Normalization Form C (UAX #15), and the character properties under it.
//
// It is here rather than beside its one caller because normalization is a property of *characters*,
// which is what this module owns. IDNA needs it (a host name is compared after normalizing, so
// `café` typed two ways must be one host), and `String.prototype.normalize` will need the same
// tables rather than a second copy of them — two answers to "are these two strings the same text"
// is the shape of a security bug, not a compatibility one.
//
// Only NFC is here. NFD, NFKC and NFKD are absent rather than approximate: nothing in this browser
// needs them yet, and the compatibility mappings are a second table twice the size of this one.
// ADR 0025 §1's rule about generated tables applies — see tools/unicode/generate_idna.py.

// Decoded text. Code points rather than bytes, because every algorithm below is defined over code
// points and a UTF-8 index would make "the previous character" a search.
using CodePoints = std::vector<std::uint32_t>;

// Decodes UTF-8, replacing every ill-formed sequence with U+FFFD. Never fails: the callers are
// parsers whose input is attacker-controlled, and a decoder that could refuse would give each of
// them a second failure path to get wrong.
CodePoints DecodeUtf8(std::string_view text);
std::string EncodeUtf8(const CodePoints& code_points);
void AppendUtf8(std::string& out, std::uint32_t code_point);

// Canonical_Combining_Class. Zero for a starter, which is the only distinction most callers want.
std::uint8_t CombiningClassOf(std::uint32_t code_point);

// General_Category is one of Mn, Mc or Me. UTS #46 forbids a label that begins with one.
bool IsCombiningMark(std::uint32_t code_point);

// Normalization Form C.
CodePoints NormalizeNfc(const CodePoints& input);

}  // namespace microbrowser::text
