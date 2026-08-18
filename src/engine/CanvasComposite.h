#pragma once

#include <cstdint>
#include <functional>
#include <optional>
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

// The canvas backing store holds **premultiplied** ARGB, and these two are the only places that
// matters.
//
// It is not a choice so much as a consequence: `gfx::BlendSrcOver` computes
// `out.rgb = src.rgb * src.a + dst.rgb * (1 - src.a)`, which is the premultiplied formula. On a page
// the destination is always opaque, so premultiplied and not are the same number and nobody ever had
// to say which this was. A canvas starts *transparent*, and there it is the difference between
// `fillStyle = 'rgba(0,255,0,0.5)'` reading back as green at half alpha and reading back as a dark
// green -- which is what every `2d.fillStyle.parse` test with an alpha was measuring.
std::uint32_t UnpremultiplyPixel(std::uint32_t argb);
std::uint32_t PremultiplyPixel(std::uint32_t argb);

// Whether this operator changes pixels the shape did not cover. Public because it is also the
// question "may I take the span-only fast path", and one answer is better than two.
bool CompositeAffectsUncovered(CompositeOp op);

// Composites a rasterized shape into `canvas` under `op`.
//
// `source` is asked for the colour at a device pixel and must return a *non*-premultiplied colour;
// the canvas holds premultiplied pixels, which is the asymmetry every line of the formula turns on.
// `alpha` is `globalAlpha`, folded in with the coverage.
// The shadow of a rasterized shape: its coverage, moved by the offset, blurred, and painted in one
// colour underneath it.
//
// The offset is in *canvas* space and is deliberately not transformed -- the specification says the
// shadow offsets are unaffected by the current transformation matrix, which is what makes a rotated
// shape's shadow fall in the same direction as every other shadow on the canvas.
//
// `sigma` is half the `shadowBlur`, per the specification. Zero means no blur and no buffer at all.
//
// **The shadow is composited before the shape and separately from it**, which is a stated
// approximation: the specification composites the two as one group, so under an operator that erases
// what it did not cover (`copy`, `source-in`) the shape's pass erases the shadow this one drew. Doing
// it properly needs an off-screen layer, which is the same machinery `filter` and `globalAlpha` on a
// group would want and is not built.
void PaintShadow(gfx::Canvas& canvas, const gfx::IntRect& clip,
                 const std::vector<gfx::CoverageSpan>& spans, double offset_x, double offset_y,
                 float sigma, gfx::Color color, float alpha, CompositeOp op);

void CompositeShape(gfx::Canvas& canvas, const gfx::IntRect& clip,
                    const std::vector<gfx::CoverageSpan>& spans,
                    const std::function<gfx::Color(int, int)>& source, float alpha, CompositeOp op);

}  // namespace microbrowser::engine
