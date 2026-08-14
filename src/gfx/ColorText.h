#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "gfx/Color.h"

namespace microbrowser::gfx {

// The textual form of a colour: `#abc`, `#aabbcc`, `#aabbccdd`, `rgb(...)`,
// `rgba(...)`, and the named colours a real page uses.
//
// Here rather than in css because two things parse it -- stylesheets and the
// SVG decoder -- and SVG is an image format, which puts its decoder in this
// module alongside PNG's. A second implementation would be a second set of
// answers to "is `#fff` white or 0xF0F0F0", which is exactly the sort of
// question that must have one.
//
// gfx owns Color, so gfx owning the way a Color is written down is the seam
// that keeps this out of the web layers rather than dragging web knowledge in:
// nothing here knows what a stylesheet or a document is.
//
// Nullopt for anything unrecognised, never a guess. An invalid colour makes a
// declaration invalid, and a declaration that silently became black would be
// worse than one that was dropped.
std::optional<Color> ParseColorText(std::string_view text);

// The other direction: `rgb(r, g, b)` when opaque, `rgba(r, g, b, a)` otherwise.
//
// This is what CSSOM calls a colour's serialization and what `getComputedStyle` reports, and every
// engine agrees on the form -- a page that compares against a hex string is comparing against
// something no browser returns. Here rather than beside either caller because there are two of them
// (the computed-style query and the inline-style setter) and two spellings of a colour would be two
// answers to the same question, which is the whole argument for this header.
std::string SerializeColorText(const Color& color);

}  // namespace microbrowser::gfx
