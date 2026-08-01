#pragma once

#include <cstddef>
#include <cstdint>

#include "gfx/Color.h"

namespace microbrowser::gfx {

// Span blitters: the inner loop of every fill, every glyph, and every image.
//
// Measured before it was written, which is the only reason it exists in this
// form. A translucent fill of a 760px circle spent 5.2ms blending and 0.34ms
// filling the same pixels opaquely — the blend was fifteen times the cost of a
// memset over identical geometry, and eighty percent of the time a page of
// translucent boxes took to paint. Nothing else in the paint path was close.
//
// The scalar version stays, exported, and is not dead code: it is the
// definition the vector path is tested against. Deriving the vector formula
// independently and comparing the two against a third hand-written expectation
// is how a blend ends up off by one in a way that only shows on stacked
// translucent layers.

// Source-over blend of a single non-premultiplied `source` color across
// `length` destination pixels. Alpha 0 and alpha 255 are handled, though a
// caller that knows the color is opaque should be filling instead.
void BlendSpanSrcOver(std::uint32_t* destination, std::size_t length, Color source);

// Same operation, one pixel at a time, using Color.h's reference blend.
void BlendSpanSrcOverScalar(std::uint32_t* destination, std::size_t length, Color source);

// Whether this build selected a vector implementation.
//
// Only a test needs this, and it needs it badly: on a target with no vector
// path compiled in, a test comparing the two implementations would be comparing
// the scalar one against itself and would pass while checking nothing.
bool BlendSpanIsVectorized();

}  // namespace microbrowser::gfx
