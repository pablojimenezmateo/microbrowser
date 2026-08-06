#include "ipc/DisplayListCodec.h"

#include "util/PerformanceCounters.h"

#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// The display list on the wire.
//
// Split out of Message.cpp when that file passed its module cap, and the split
// is the one the cap was pointing at rather than an arbitrary halving: framing
// a message is a different concern from encoding a picture, and the picture is
// by far the larger and the more hostile of the two. Everything here reads
// bytes that a compromised renderer chose.
//
// The three rules this file is built on, each stated where it is applied:
//   - Geometry crosses inline, never as an index, so there is no index to be
//     out of range.
//   - Images cross as a table with indices, because a bitmap is megabytes and
//     a repeat is common -- and the index never escapes ReadDisplayList.
//   - A surface crosses as a *name*, because the compositor owns it and a name
//     that resolves to nothing composites nothing. ADR 0013.

namespace microbrowser::ipc {

namespace {

// Display-list command tags, same reasoning.
enum class CommandTag : std::uint8_t {
  FillRect = 1,
  PushClip = 2,
  PopClip = 3,
  FillPath = 4,
  StrokePath = 5,
  DrawText = 6,
  DrawImage = 7,
  DrawSurface = 8,
  PushTransform = 9,
  PopTransform = 10,
};

}  // namespace

void WriteRect(ByteWriter& writer, const gfx::IntRect& rect) {
  writer.WriteI32(rect.x);
  writer.WriteI32(rect.y);
  writer.WriteI32(rect.width);
  writer.WriteI32(rect.height);
}

// Four raw int32s become a rectangle only if they describe one.
//
// `IntRect::Right()` is `x + width`, so a frame naming a rect at x = 2e9 with
// width 2e9 turns the very next intersection into signed overflow — undefined
// behavior driven directly by a renderer that is assumed compromised. It was
// reachable: a single FillRect command with those values overflowed inside
// Execute, and UBSan caught it on the first hostile frame anyone wrote.
//
// The bound is not arbitrary. EnclosingIntRect, the only sanctioned way to
// produce a device rect from layout, already saturates to the same range, so
// this rejects exactly what the encoder cannot emit — and rejecting rather than
// clamping means a tampered frame is reported instead of quietly repainted
// somewhere else.
bool ReadRect(ByteReader& reader, gfx::IntRect& out) {
  gfx::IntRect rect;
  rect.x = reader.ReadI32();
  rect.y = reader.ReadI32();
  rect.width = reader.ReadI32();
  rect.height = reader.ReadI32();
  if (!reader.Ok() || !gfx::IsWithinDeviceRange(rect)) {
    return false;
  }
  out = rect;
  return true;
}

namespace {

constexpr std::size_t kMinBytesPerCommand = 1;
// Two int32s: the encoding of "no image", and the smallest any table entry can
// be. The count is checked against this before the table is reserved, so a
// frame claiming four billion images is refused rather than sized for.
constexpr std::size_t kMinBytesPerImage = 8;

// A font size beyond this is not a font size. FreeType converts to 26.6 fixed
// point internally, so a size near the float maximum overflows there rather
// than producing very large text, and a glyph outline scaled by it leaves the
// rasterizer's coordinate range anyway. The bound is generous enough that no
// legitimate page reaches it -- a 4096px heading is already absurd.
constexpr float kMaxFontSize = 16384.0f;

// A stroke width, and a miter limit. Twice the device range: a stroke wider
// than the coordinate space it lives in has no geometry left to express, and
// the bound keeps the outset that Bounds() derives from it inside an int.
constexpr float kMaxStrokeWidth = 2.0f * static_cast<float>(gfx::kMaxDeviceCoordinate);

// A viewport edge, and the physical-pixels-per-CSS-pixel scale. Both are far
// past anything real -- no display is 65536 pixels wide, and no device has a
// scale factor of 64 -- and both keep the products they feed inside their
// types.

// An image edge on the wire. The product of two of these is the pixel count,
// and 16384 * 16384 * 4 bytes is a gigabyte -- already far past anything a page
// can legitimately hand over, and small enough that the multiplication cannot
// overflow the size_t it is checked against.
constexpr std::int64_t kMaxImageEdge = 16384;
// A Close verb is one tag byte with no points; every other verb costs more.
constexpr std::size_t kMinBytesPerVerb = 1;

// Paths cross the seam as geometry, never as an index into a shared table.
//
// The display list addresses its paths by index internally, and it would be a
// shorter encoding to send the table and the indices. It would also mean a
// hostile renderer could name path 4000 in a table of three, and every consumer
// would have to remember to range-check it. Inlining the geometry deletes the
// bug class rather than guarding it: there is no index on the wire to be wrong.
void WritePath(ByteWriter& writer, const gfx::Path& path) {
  const std::span<const gfx::FloatPoint> points = path.Points();
  writer.WriteU32(static_cast<std::uint32_t>(path.Verbs().size()));
  std::size_t point_index = 0;
  for (const gfx::PathVerb verb : path.Verbs()) {
    writer.WriteU8(static_cast<std::uint8_t>(verb));
    for (std::size_t i = 0; i < gfx::PointsForVerb(verb); ++i) {
      writer.WriteF32(points[point_index].x);
      writer.WriteF32(points[point_index].y);
      ++point_index;
    }
  }
}

// Decoding replays through the Path builder, which is what makes the decoded
// path trustworthy without a separate validation pass: the builder is already
// the thing that rejects non-finite coordinates and refuses to draw an edge
// from the origin for a curve with no start point. A decoder that appended to
// the verb and point vectors directly would have to restate both rules, and
// would drift from them.
bool ReadPath(ByteReader& reader, gfx::Path& out) {
  out.Clear();
  const std::optional<std::uint32_t> verb_count = reader.ReadCount(kMinBytesPerVerb);
  if (!verb_count.has_value()) {
    return false;
  }

  const auto read_point = [&reader] {
    const float x = reader.ReadF32();
    const float y = reader.ReadF32();
    return gfx::FloatPoint{x, y};
  };

  for (std::uint32_t i = 0; i < *verb_count; ++i) {
    const auto verb = static_cast<gfx::PathVerb>(reader.ReadU8());
    if (!reader.Ok()) {
      return false;
    }
    switch (verb) {
      case gfx::PathVerb::Move: {
        const gfx::FloatPoint p = read_point();
        if (!reader.Ok()) {
          return false;
        }
        out.MoveTo(p);
        break;
      }
      case gfx::PathVerb::Line: {
        const gfx::FloatPoint p = read_point();
        if (!reader.Ok()) {
          return false;
        }
        out.LineTo(p);
        break;
      }
      case gfx::PathVerb::Quad: {
        const gfx::FloatPoint control = read_point();
        const gfx::FloatPoint end = read_point();
        if (!reader.Ok()) {
          return false;
        }
        out.QuadTo(control, end);
        break;
      }
      case gfx::PathVerb::Cubic: {
        const gfx::FloatPoint first = read_point();
        const gfx::FloatPoint second = read_point();
        const gfx::FloatPoint end = read_point();
        if (!reader.Ok()) {
          return false;
        }
        out.CubicTo(first, second, end);
        break;
      }
      case gfx::PathVerb::Close:
        out.Close();
        break;
      default:
        return false;
    }
  }
  return reader.Ok();
}

void WriteStrokeStyle(ByteWriter& writer, const gfx::StrokeStyle& style) {
  writer.WriteF32(style.width);
  writer.WriteF32(style.miter_limit);
  writer.WriteU8(static_cast<std::uint8_t>(style.cap));
  writer.WriteU8(static_cast<std::uint8_t>(style.join));
}

bool ReadStrokeStyle(ByteReader& reader, gfx::StrokeStyle& out) {
  out.width = reader.ReadF32();
  out.miter_limit = reader.ReadF32();
  const std::uint8_t cap = reader.ReadU8();
  const std::uint8_t join = reader.ReadU8();
  if (!reader.Ok()) {
    return false;
  }
  if (cap > static_cast<std::uint8_t>(gfx::LineCap::Square) ||
      join > static_cast<std::uint8_t>(gfx::LineJoin::Bevel)) {
    return false;
  }
  // A non-finite width or limit reaches float comparisons in the stroker that
  // silently answer "false" and produce a bevel. That is safe but it is not
  // decodable input, and accepting it would mean the wire format has values the
  // encoder can never produce.
  if (!std::isfinite(out.width) || !std::isfinite(out.miter_limit)) {
    return false;
  }
  // And bounded, not merely finite. A stroke width of 1e30 is finite, and
  // DisplayList::Bounds inflates a damage rect by half of it -- which the
  // fuzzer turned into a signed overflow before IntRect::Inflated was made
  // total. Both ends of that are fixed now; this one keeps the wire format
  // free of values no encoder produces.
  if (out.width < 0.0f || out.width > kMaxStrokeWidth || out.miter_limit < 0.0f ||
      out.miter_limit > kMaxStrokeWidth) {
    return false;
  }
  out.cap = static_cast<gfx::LineCap>(cap);
  out.join = static_cast<gfx::LineJoin>(join);
  return true;
}

// Text goes on the wire as the run and the font it asks for, inline, the same
// way a path does -- see the note in gfx/DisplayList.h about why the in-memory
// index must not cross a trust boundary. A renderer naming a run that is not
// there would be an out-of-bounds read waiting to happen.
void WriteFontRequest(ByteWriter& writer, const gfx::FontRequest& font) {
  // Count-prefixed rather than comma-joined: a family name may legitimately
  // contain a comma when the stylesheet quoted it, and re-splitting on the
  // reading side would be a second parser that disagrees with the first.
  writer.WriteU32(static_cast<std::uint32_t>(
      std::min<std::size_t>(font.families.size(), gfx::kMaxFontFamilies)));
  for (std::size_t i = 0; i < font.families.size() && i < gfx::kMaxFontFamilies; ++i) {
    writer.WriteString(font.families[i]);
  }
  writer.WriteF32(font.size);
  writer.WriteI32(font.weight);
  writer.WriteU8(font.italic ? 1 : 0);
}

bool ReadFontRequest(ByteReader& reader, gfx::FontRequest& out) {
  // Checked before the loop, not inside it: a count of four billion would
  // otherwise reserve nothing but run four billion failing reads.
  const std::uint32_t families = reader.ReadU32();
  if (!reader.Ok() || families > gfx::kMaxFontFamilies) {
    return false;
  }
  out.families.clear();
  out.families.reserve(families);
  for (std::uint32_t i = 0; i < families; ++i) {
    out.families.push_back(reader.ReadString());
    if (!reader.Ok()) {
      return false;
    }
  }
  out.size = reader.ReadF32();
  out.weight = reader.ReadI32();
  const std::uint8_t italic = reader.ReadU8();
  if (!reader.Ok() || italic > 1) {
    return false;
  }
  // A size that is not a positive finite number reaches FreeType's fixed-point
  // conversion, and a weight outside the CSS range makes the nearest-weight
  // search meaningless. Both are values the encoder cannot produce, so the
  // wire format does not have them either.
  if (!std::isfinite(out.size) || out.size <= 0.0f || out.size > kMaxFontSize) {
    return false;
  }
  if (out.weight < 1 || out.weight > 1000) {
    return false;
  }
  out.italic = italic != 0;
  return true;
}

// Pixels, once per distinct image per frame, in the frame's resource table.
//
// Images are the one resource that crosses this seam by index rather than
// inline, and the exception is measured rather than aesthetic. A path is tens
// of bytes, so inlining a repeat costs nothing and buys the argument above --
// there is no index on the wire to be wrong. A bitmap is megabytes, and a page
// that tiles one background image draws it once per element: on a real page
// that was the same decoded bitmap serialized dozens of times in a single
// frame. The table makes that one copy.
//
// The index is safe here for a reason the path comment's argument does not
// contradict: **it never escapes the decoder.** ReadDisplayList resolves every
// index into a shared_ptr against the table it has just read, in one place, and
// replays the result through DisplayList::DrawImage. An out-of-range index
// records nothing. No decoded structure holds a wire index, so no later
// consumer can get the range check wrong -- there is no later consumer that
// sees one.
//
// Still per frame, not across frames. A cache that survived a frame would need
// the receiver's memory to become part of the protocol -- what it evicts, how
// much it may hold, and what a renderer naming an id it never sent gets -- and
// that is a protocol design rather than an encoding change. ADR 0013's surface
// is the case that already needs it, and it is why a surface is named rather
// than sent at all.
void WriteImage(ByteWriter& writer, const gfx::Image& image) {
  // 0x0 is the encoding of "no image". The builder cannot put an invalid image
  // in the table -- DrawImage refuses one -- but a table slot is written before
  // anything reads it, and a size the decoder rejects outright would fail the
  // whole frame over a slot no command names.
  if (!image.IsValid()) {
    writer.WriteI32(0);
    writer.WriteI32(0);
    return;
  }
  writer.WriteI32(image.Width());
  writer.WriteI32(image.Height());
  for (const std::uint32_t pixel : image.Pixels()) {
    writer.WriteU32(pixel);
  }
}

bool ReadImage(ByteReader& reader, gfx::Image& out) {
  const std::int64_t width = reader.ReadI32();
  const std::int64_t height = reader.ReadI32();
  if (!reader.Ok()) {
    return false;
  }
  if (width == 0 && height == 0) {
    return true;  // "no image": `out` stays invalid, and naming it paints nothing
  }
  if (width <= 0 || height <= 0 || width > kMaxImageEdge || height > kMaxImageEdge) {
    return false;
  }
  const std::int64_t count = width * height;
  // Checked against the bytes that are actually left before the allocation, not
  // after: a frame claiming a 16384x16384 image must not cause a gigabyte
  // reserve on its way to being rejected.
  if (static_cast<std::uint64_t>(count) * sizeof(std::uint32_t) > reader.Remaining()) {
    return false;
  }
  std::vector<std::uint32_t> pixels(static_cast<std::size_t>(count));
  for (std::uint32_t& pixel : pixels) {
    pixel = reader.ReadU32();
  }
  if (!reader.Ok()) {
    return false;
  }
  return out.Adopt(static_cast<int>(width), static_cast<int>(height), std::move(pixels));
}

}  // namespace

void WriteDisplayList(ByteWriter& writer, const gfx::DisplayList& list) {
  const std::vector<gfx::Path>& paths = list.Paths();

  // The resource table comes first, so the decoder has every image in hand
  // before it meets an index naming one. A table that trailed the commands
  // would force the decoder either to buffer the commands or to make two
  // passes, and both are ways to be holding a wire index when something else
  // goes wrong.
  const std::vector<std::shared_ptr<const gfx::Image>>& images = list.Images();
  writer.WriteU32(static_cast<std::uint32_t>(images.size()));
  for (const std::shared_ptr<const gfx::Image>& image : images) {
    WriteImage(writer, image == nullptr ? gfx::Image{} : *image);
  }

  writer.WriteU32(static_cast<std::uint32_t>(list.Size()));
  for (const gfx::DisplayCommand& command : list.Commands()) {
    if (const auto* fill = std::get_if<gfx::FillRectCommand>(&command)) {
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::FillRect));
      WriteRect(writer, fill->rect);
      writer.WriteU32(fill->color.argb);
    } else if (const auto* push = std::get_if<gfx::PushClipCommand>(&command)) {
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::PushClip));
      WriteRect(writer, push->rect);
    } else if (const auto* transform = std::get_if<gfx::PushTransformCommand>(&command)) {
      // The matrix inline, not the index. An index is a *representation* of the
      // sending list, and a receiver that indexed a table with a number a hostile
      // renderer chose would be reading out of bounds on request -- the same
      // reasoning as paths, written where the decision is made.
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::PushTransform));
      const gfx::AffineTransform matrix = list.TransformAt(transform->matrix);
      writer.WriteF32(matrix.A());
      writer.WriteF32(matrix.B());
      writer.WriteF32(matrix.C());
      writer.WriteF32(matrix.D());
      writer.WriteF32(matrix.E());
      writer.WriteF32(matrix.F());
    } else if (std::holds_alternative<gfx::PopTransformCommand>(command)) {
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::PopTransform));
    } else if (const auto* fill_path = std::get_if<gfx::FillPathCommand>(&command)) {
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::FillPath));
      writer.WriteU32(fill_path->color.argb);
      writer.WriteU8(static_cast<std::uint8_t>(fill_path->rule));
      WritePath(writer, paths[fill_path->path]);
    } else if (const auto* stroke = std::get_if<gfx::StrokePathCommand>(&command)) {
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::StrokePath));
      writer.WriteU32(stroke->color.argb);
      WriteStrokeStyle(writer, stroke->style);
      WritePath(writer, paths[stroke->path]);
    } else if (const auto* text = std::get_if<gfx::DrawTextCommand>(&command)) {
      const gfx::DisplayList::TextRun* run = list.TextAt(text->text);
      const gfx::FontRequest* font = list.FontAt(text->font);
      if (run == nullptr || font == nullptr) {
        // Not reachable through the builder, which is why this writes a PopClip
        // rather than skipping: the command count was already written, and a
        // list that is one command short of its own header is a decode failure
        // on the other side.
        writer.WriteU8(static_cast<std::uint8_t>(CommandTag::PopClip));
        continue;
      }
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::DrawText));
      writer.WriteU32(text->color.argb);
      writer.WriteF32(text->origin.x);
      writer.WriteF32(text->origin.y);
      writer.WriteF32(run->advance);
      // The resolved direction, which the receiver has no way to recompute: bidi ran in the process
      // that built this list, and `unicode-bidi: bidi-override` means the text itself does not say.
      writer.WriteU8(run->right_to_left ? 1u : 0u);
      WriteFontRequest(writer, *font);
      writer.WriteString(run->text);
    } else if (const auto* image = std::get_if<gfx::DrawImageCommand>(&command)) {
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::DrawImage));
      WriteRect(writer, image->destination);
      writer.WriteU32(image->image);
    } else if (const auto* surface = std::get_if<gfx::DrawSurfaceCommand>(&command)) {
      // The one resource that crosses by name. There is no table to look it up
      // in on this side and none on the other: the id refers to a surface the
      // compositor owns, and an id naming one that does not exist composites
      // nothing. That is the property that makes it safe to put on the wire,
      // and it is why a video frame never appears here at all. ADR 0013.
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::DrawSurface));
      WriteRect(writer, surface->destination);
      writer.WriteU32(surface->surface);
    } else {
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::PopClip));
    }
  }
}

// Decoding replays the wire commands through the DisplayList builder rather
// than appending them raw. That normalizes as it decodes: a degenerate fill
// (empty rect, zero alpha) from a malformed or hostile frame is dropped instead
// of becoming a no-op command that every later frame diff has to carry. Round
// trips of lists built through the public API are exact, because the builder
// never emits a degenerate fill in the first place.
bool ReadDisplayList(ByteReader& reader, gfx::DisplayList& out) {
  // The resource table, bounded against the bytes actually remaining before
  // anything is reserved: eight bytes is the smallest an image can encode to,
  // so a frame claiming four billion of them is refused before it allocates.
  const std::optional<std::uint32_t> image_count = reader.ReadCount(kMinBytesPerImage);
  if (!image_count.has_value()) {
    return false;
  }
  std::vector<std::shared_ptr<const gfx::Image>> images;
  images.reserve(*image_count);
  for (std::uint32_t i = 0; i < *image_count; ++i) {
    auto image = std::make_shared<gfx::Image>();
    if (!ReadImage(reader, *image)) {
      return false;
    }
    // Invalid entries stay in the table so that later indices keep meaning what
    // the writer meant. DrawImage refuses a null or invalid image, so naming
    // one paints nothing.
    images.push_back(image->IsValid() ? std::move(image) : nullptr);
  }

  const std::optional<std::uint32_t> count = reader.ReadCount(kMinBytesPerCommand);
  if (!count.has_value()) {
    return false;
  }
  for (std::uint32_t i = 0; i < *count; ++i) {
    const auto tag = static_cast<CommandTag>(reader.ReadU8());
    if (!reader.Ok()) {
      return false;
    }
    switch (tag) {
      case CommandTag::FillRect: {
        gfx::IntRect rect;
        if (!ReadRect(reader, rect)) {
          return false;
        }
        const gfx::Color color{reader.ReadU32()};
        if (!reader.Ok()) {
          return false;
        }
        out.FillRect(rect, color);
        break;
      }
      case CommandTag::PushClip: {
        gfx::IntRect rect;
        if (!ReadRect(reader, rect)) {
          return false;
        }
        out.PushClip(rect);
        break;
      }
      case CommandTag::PopClip:
        out.PopClip();
        break;
      case CommandTag::FillPath: {
        const gfx::Color color{reader.ReadU32()};
        const std::uint8_t rule = reader.ReadU8();
        if (!reader.Ok() || rule > static_cast<std::uint8_t>(gfx::FillRule::EvenOdd)) {
          return false;
        }
        gfx::Path path;
        if (!ReadPath(reader, path)) {
          return false;
        }
        out.FillPath(path, color, static_cast<gfx::FillRule>(rule));
        break;
      }
      case CommandTag::StrokePath: {
        const gfx::Color color{reader.ReadU32()};
        gfx::StrokeStyle style;
        if (!ReadStrokeStyle(reader, style)) {
          return false;
        }
        gfx::Path path;
        if (!ReadPath(reader, path)) {
          return false;
        }
        out.StrokePath(path, style, color);
        break;
      }
      case CommandTag::DrawText: {
        const gfx::Color color{reader.ReadU32()};
        const float x = reader.ReadF32();
        const float y = reader.ReadF32();
        const float advance = reader.ReadF32();
        const std::uint8_t right_to_left = reader.ReadU8();
        if (!reader.Ok() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(advance) ||
            advance < 0.0f || right_to_left > 1) {
          return false;
        }
        gfx::FontRequest font;
        if (!ReadFontRequest(reader, font)) {
          return false;
        }
        const std::string run = reader.ReadString();
        if (!reader.Ok()) {
          return false;
        }
        out.DrawText(run, advance, font, gfx::FloatPoint{x, y}, color, right_to_left != 0);
        break;
      }
      case CommandTag::DrawImage: {
        gfx::IntRect destination;
        if (!ReadRect(reader, destination)) {
          return false;
        }
        const std::uint32_t index = reader.ReadU32();
        if (!reader.Ok()) {
          return false;
        }
        // **The one range check, and the only place a wire index exists.** An
        // index past the table is a malformed or hostile frame; it records
        // nothing rather than failing the frame, because a single bad index is
        // a missing picture and refusing the whole list is a blank window.
        if (index < images.size()) {
          out.DrawImage(images[index], destination);
        }
        break;
      }
      case CommandTag::DrawSurface: {
        gfx::IntRect destination;
        if (!ReadRect(reader, destination)) {
          return false;
        }
        const std::uint32_t id = reader.ReadU32();
        if (!reader.Ok()) {
          return false;
        }
        // Not validated against a registry, and deliberately not: this decoder
        // does not have one, and an id is not required to name a surface that
        // exists *yet*. Resolution happens at composite time, where a miss
        // draws nothing. Validating here would mean a frame that arrives one
        // turn before the surface it names is a decode failure.
        out.DrawSurface(id, destination);
        break;
      }
      case CommandTag::PushTransform: {
        const float a = reader.ReadF32();
        const float b = reader.ReadF32();
        const float c = reader.ReadF32();
        const float d = reader.ReadF32();
        const float e = reader.ReadF32();
        const float f = reader.ReadF32();
        if (!reader.Ok()) {
          return false;
        }
        // No bound on the values, and none is possible: every finite matrix is a
        // legal transform, and a degenerate one collapses the plane rather than
        // reading anything. What the *rasterizer* does with a huge coordinate is
        // its own saturation, which is where that bound belongs.
        out.PushTransform(gfx::AffineTransform{a, b, c, d, e, f});
        break;
      }
      case CommandTag::PopTransform:
        out.PopTransform();
        break;
      default:
        return false;
    }
  }
  return reader.Ok();
}

}  // namespace microbrowser::ipc
