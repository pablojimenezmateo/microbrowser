#pragma once

#include <cstddef>

#include "gfx/DisplayList.h"
#include "gfx/Geometry.h"
#include "ipc/ByteStream.h"

namespace microbrowser::ipc {

// The display list on the wire.
//
// Private to this module -- it is not on `public:` in MODULE.deps, because
// nothing outside ipc has any business encoding a picture. It exists as a
// header at all because Message.cpp passed its translation-unit cap, and the
// cap means "a missing module or a missing file", not "raise the number".
// Framing a message and encoding a picture are different concerns, and the
// picture is both the larger of the two and the one every byte of which a
// compromised renderer chose.

// Four int32s that must describe a rectangle inside the device coordinate
// range. Shared with the framing side because a PaintFrame carries damage
// rectangles as well as a list.
void WriteRect(ByteWriter& writer, const gfx::IntRect& rect);
bool ReadRect(ByteReader& reader, gfx::IntRect& out);

// A rectangle costs sixteen bytes, which is what a count is checked against
// before anything is reserved for it.
inline constexpr std::size_t kBytesPerRect = 16;

void WriteDisplayList(ByteWriter& writer, const gfx::DisplayList& list);

// False for any frame this cannot decode into a well-formed list. `out` may
// have been partially built when it returns false, so callers discard it.
bool ReadDisplayList(ByteReader& reader, gfx::DisplayList& out);

}  // namespace microbrowser::ipc
