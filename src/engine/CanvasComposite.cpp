#include "engine/CanvasComposite.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace microbrowser::engine {

namespace {

// The (Fa, Fb) pair of the Porter-Duff algebra, given the source and backdrop alphas.
//
// One table rather than twelve blend functions: every operator in this list is the *same* formula
// with a different pair of coefficients, and writing them out separately is how one of them ends up
// with the source and backdrop the wrong way round -- which looks correct on a symmetric test image
// and is wrong on every real one.
struct Coefficients {
  float fa = 0.0f;
  float fb = 0.0f;
};

Coefficients CoefficientsFor(CompositeOp op, float as, float ab) {
  switch (op) {
    case CompositeOp::SourceOver:
      return {1.0f, 1.0f - as};
    case CompositeOp::SourceIn:
      return {ab, 0.0f};
    case CompositeOp::SourceOut:
      return {1.0f - ab, 0.0f};
    case CompositeOp::SourceAtop:
      return {ab, 1.0f - as};
    case CompositeOp::DestinationOver:
      return {1.0f - ab, 1.0f};
    case CompositeOp::DestinationIn:
      return {0.0f, as};
    case CompositeOp::DestinationOut:
      return {0.0f, 1.0f - as};
    case CompositeOp::DestinationAtop:
      return {1.0f - ab, as};
    case CompositeOp::Lighter:
      return {1.0f, 1.0f};
    case CompositeOp::Copy:
      return {1.0f, 0.0f};
    case CompositeOp::Xor:
      return {1.0f - ab, 1.0f - as};
    case CompositeOp::Clear:
      return {0.0f, 0.0f};
  }
  return {1.0f, 1.0f - as};
}

std::uint8_t ToByte(float value) {
  return static_cast<std::uint8_t>(std::lround(std::clamp(value * 255.0f, 0.0f, 255.0f)));
}

// One pixel, in premultiplied space. `source` is non-premultiplied, `destination` is premultiplied,
// and the result is premultiplied -- which is the whole reason the source's colour is multiplied by
// `as` here and the backdrop's is not.
std::uint32_t Composite(std::uint32_t destination, gfx::Color source, float as, CompositeOp op) {
  const float ab = static_cast<float>((destination >> 24) & 0xFFu) / 255.0f;
  const Coefficients k = CoefficientsFor(op, as, ab);
  const float ao = as * k.fa + ab * k.fb;
  if (ao <= 0.0f) {
    return 0;
  }
  const auto channel = [&](float source_value, float backdrop_premultiplied) {
    return ToByte(as * k.fa * source_value + k.fb * backdrop_premultiplied);
  };
  const float sr = static_cast<float>(source.Red()) / 255.0f;
  const float sg = static_cast<float>(source.Green()) / 255.0f;
  const float sb = static_cast<float>(source.Blue()) / 255.0f;
  const float br = static_cast<float>((destination >> 16) & 0xFFu) / 255.0f;
  const float bg = static_cast<float>((destination >> 8) & 0xFFu) / 255.0f;
  const float bb = static_cast<float>(destination & 0xFFu) / 255.0f;
  // `lighter` is the one operator whose result can exceed the alpha it is premultiplied by, which is
  // what makes two half-opaque whites add to white rather than to grey. The clamp is in `ToByte`.
  return (static_cast<std::uint32_t>(ToByte(ao)) << 24) |
         (static_cast<std::uint32_t>(channel(sr, br)) << 16) |
         (static_cast<std::uint32_t>(channel(sg, bg)) << 8) |
         static_cast<std::uint32_t>(channel(sb, bb));
}

}  // namespace

std::optional<CompositeOp> ParseCompositeOp(std::string_view name) {
  static constexpr struct {
    std::string_view name;
    CompositeOp op;
  } kNames[] = {
      {"source-over", CompositeOp::SourceOver},
      {"source-in", CompositeOp::SourceIn},
      {"source-out", CompositeOp::SourceOut},
      {"source-atop", CompositeOp::SourceAtop},
      {"destination-over", CompositeOp::DestinationOver},
      {"destination-in", CompositeOp::DestinationIn},
      {"destination-out", CompositeOp::DestinationOut},
      {"destination-atop", CompositeOp::DestinationAtop},
      {"lighter", CompositeOp::Lighter},
      {"copy", CompositeOp::Copy},
      {"xor", CompositeOp::Xor},
  };
  for (const auto& entry : kNames) {
    if (entry.name == name) {
      return entry.op;
    }
  }
  // `clear` is deliberately absent from the parse table: it is in the algebra because `clearRect`
  // and `reset()` need it internally, and it is *not* a value a page may assign -- the specification
  // removed it years ago and a page that sets it must be ignored like any other unknown name.
  return std::nullopt;
}

std::string_view CompositeOpName(CompositeOp op) {
  switch (op) {
    case CompositeOp::SourceIn:
      return "source-in";
    case CompositeOp::SourceOut:
      return "source-out";
    case CompositeOp::SourceAtop:
      return "source-atop";
    case CompositeOp::DestinationOver:
      return "destination-over";
    case CompositeOp::DestinationIn:
      return "destination-in";
    case CompositeOp::DestinationOut:
      return "destination-out";
    case CompositeOp::DestinationAtop:
      return "destination-atop";
    case CompositeOp::Lighter:
      return "lighter";
    case CompositeOp::Copy:
      return "copy";
    case CompositeOp::Xor:
      return "xor";
    case CompositeOp::Clear:
    case CompositeOp::SourceOver:
      break;
  }
  return "source-over";
}

std::uint32_t UnpremultiplyPixel(std::uint32_t argb) {
  const std::uint32_t alpha = (argb >> 24) & 0xFFu;
  if (alpha == 0 || alpha == 255) {
    return alpha == 0 ? 0u : argb;
  }
  const auto channel = [alpha](std::uint32_t value) {
    return std::min(255u, (value * 255u + alpha / 2) / alpha);
  };
  return (alpha << 24) | (channel((argb >> 16) & 0xFFu) << 16) |
         (channel((argb >> 8) & 0xFFu) << 8) | channel(argb & 0xFFu);
}

std::uint32_t PremultiplyPixel(std::uint32_t argb) {
  const std::uint32_t alpha = (argb >> 24) & 0xFFu;
  if (alpha == 255) {
    return argb;
  }
  if (alpha == 0) {
    return 0;
  }
  return (alpha << 24) | (gfx::MulDiv255((argb >> 16) & 0xFFu, alpha) << 16) |
         (gfx::MulDiv255((argb >> 8) & 0xFFu, alpha) << 8) |
         gfx::MulDiv255(argb & 0xFFu, alpha);
}

bool CompositeAffectsUncovered(CompositeOp op) {
  // Derived rather than listed: with `as` zero the formula leaves the backdrop alone exactly when
  // `Fb` is one, and these six are the operators where it is not.
  return CoefficientsFor(op, 0.0f, 1.0f).fb != 1.0f;
}

void CompositeShape(gfx::Canvas& canvas, const gfx::IntRect& clip,
                    const std::vector<gfx::CoverageSpan>& spans,
                    const std::function<gfx::Color(int, int)>& source, float alpha,
                    CompositeOp op) {
  const gfx::IntRect region = clip.Intersected(canvas.Bounds());
  if (region.IsEmpty()) {
    return;
  }
  const auto apply = [&](int x, int y, std::uint8_t coverage) {
    std::uint32_t* row = canvas.Row(y);
    if (row == nullptr || x < 0 || x >= canvas.Width()) {
      return;
    }
    const gfx::Color colour = coverage == 0 ? gfx::Color::Rgba(0, 0, 0, 0) : source(x, y);
    const float as = static_cast<float>(colour.Alpha()) / 255.0f * alpha *
                     (static_cast<float>(coverage) / 255.0f);
    row[x] = Composite(row[x], colour, as, op);
  };
  if (!CompositeAffectsUncovered(op)) {
    for (const gfx::CoverageSpan& span : spans) {
      if (span.length <= 0 || span.y < region.Top() || span.y >= region.Bottom()) {
        continue;
      }
      for (std::int32_t i = 0; i < span.length; ++i) {
        const std::int32_t x = span.x + i;
        if (x >= region.Left() && x < region.Right()) {
          apply(x, span.y, span.coverage);
        }
      }
    }
    return;
  }
  // The operator erases what the shape did not cover, so every pixel of the clip is visited. One
  // coverage row at a time rather than a whole mask: a 4096x4096 canvas would be sixteen megabytes
  // of mask per draw, and a page chooses the size.
  std::vector<std::uint8_t> coverage(static_cast<std::size_t>(region.width), 0);
  std::size_t next = 0;
  // The rasterizer emits spans in raster order, which is what lets this be one pass rather than a
  // lookup per pixel.
  for (int y = region.Top(); y < region.Bottom(); ++y) {
    std::fill(coverage.begin(), coverage.end(), static_cast<std::uint8_t>(0));
    while (next < spans.size() && spans[next].y < y) {
      ++next;
    }
    for (std::size_t i = next; i < spans.size() && spans[i].y == y; ++i) {
      for (std::int32_t k = 0; k < spans[i].length; ++k) {
        const std::int32_t x = spans[i].x + k;
        if (x >= region.Left() && x < region.Right()) {
          coverage[static_cast<std::size_t>(x - region.Left())] = spans[i].coverage;
        }
      }
    }
    for (int x = region.Left(); x < region.Right(); ++x) {
      apply(x, y, coverage[static_cast<std::size_t>(x - region.Left())]);
    }
  }
}

}  // namespace microbrowser::engine
