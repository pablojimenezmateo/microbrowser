#pragma once

#include <optional>
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

}  // namespace microbrowser::gfx
