#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "gfx/Canvas.h"
#include "gfx/Color.h"
#include "gfx/Geometry.h"
#include "gfx/Rasterizer.h"

namespace microbrowser::engine {

// `globalCompositeOperation`, as the Porter-Duff algebra it actually is.
//
// Its own translation unit rather than a branch inside `CanvasSurfaces::Execute` because it is the
// one part of canvas drawing that is not "paint a shape": six of these twelve operators change
// pixels the shape never covered -- `copy` erases everything outside what was just drawn -- and a
// compositor written as a special case inside a fill would either miss that or have to reach out of
// the fill to fix it afterwards.
//
// It is deliberately *not* in `gfx`. `gfx::Painter` composites source-over and nothing else, which
// is right for a page: every layer under a page's paint is opaque, so the other eleven operators
// have no meaning there. They have meaning on a canvas because a canvas starts transparent.
enum class CompositeOp : std::uint8_t {
  SourceOver,
  SourceIn,
  SourceOut,
  SourceAtop,
  DestinationOver,
  DestinationIn,
  DestinationOut,
  DestinationAtop,
  Lighter,
  Copy,
  Xor,
  Clear,
};

// The operator a page named, or nothing when the name is not one of these. Nothing means *ignore the
// assignment*, which is the specification's rule -- and is why this returns an optional rather than
// defaulting to source-over: a typo that silently reset the operator would be a page compositing
// wrong with no way to find out.
std::optional<CompositeOp> ParseCompositeOp(std::string_view name);
std::string_view CompositeOpName(CompositeOp op);

// Whether this operator changes pixels the shape did not cover. Public because it is also the
// question "may I take the span-only fast path", and one answer is better than two.
bool CompositeAffectsUncovered(CompositeOp op);

// Composites a rasterized shape into `canvas` under `op`.
//
// `source` is asked for the colour at a device pixel and must return a *non*-premultiplied colour;
// the canvas holds premultiplied pixels, which is the asymmetry every line of the formula turns on.
// `alpha` is `globalAlpha`, folded in with the coverage.
void CompositeShape(gfx::Canvas& canvas, const gfx::IntRect& clip,
                    const std::vector<gfx::CoverageSpan>& spans,
                    const std::function<gfx::Color(int, int)>& source, float alpha, CompositeOp op);

}  // namespace microbrowser::engine
