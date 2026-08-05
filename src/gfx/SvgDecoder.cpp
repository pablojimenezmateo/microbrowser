#include "gfx/SvgDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/Canvas.h"
#include "gfx/ColorText.h"
#include "gfx/Painter.h"
#include "gfx/Path.h"
#include "gfx/Rasterizer.h"
#include "gfx/SvgPath.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microbrowser::gfx {

namespace {

bool IsXmlSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsXmlSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsXmlSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// The presentation state an element inherits from its ancestors.
//
// SVG has no cascade in this subset -- no stylesheets, no specificity -- so
// inheritance is the whole of it, and it is a value passed down the walk rather
// than a table looked up. That is also why `has_fill` is separate from the
// colour: `fill="none"` and "no fill stated" are different, and only the second
// one inherits.
struct Presentation {
  Color fill = Color::Rgb(0, 0, 0);
  bool has_fill = true;
  Color stroke;
  bool has_stroke = false;
  float stroke_width = 1.0f;
  float opacity = 1.0f;
  FillRule fill_rule = FillRule::NonZero;
  AffineTransform transform;
};

// One `name="value"` pair, as raw text.
struct Attribute {
  std::string_view name;
  std::string_view value;
};

std::optional<float> ParseNumber(std::string_view text) {
  text = Trim(text);
  // Units are stripped rather than converted: inside an SVG every length that
  // matters here is in user units, and `px` is the same thing spelled out. A
  // unit that is not (`em`, `%`) has no meaning without a context this decoder
  // does not have, so it produces nothing.
  if (text.ends_with("px")) {
    text.remove_suffix(2);
  }
  if (const std::optional<double> value = util::ParseDouble(Trim(text))) {
    if (std::isfinite(*value)) {
      return static_cast<float>(*value);
    }
  }
  return std::nullopt;
}

// The numbers in a list like `0 0 192 192` or `1,2 3,4`.
std::vector<float> ParseNumberList(std::string_view text, std::size_t limit) {
  std::vector<float> numbers;
  std::size_t at = 0;
  while (at < text.size() && numbers.size() < limit) {
    while (at < text.size() && (IsXmlSpace(text[at]) || text[at] == ',')) {
      ++at;
    }
    const std::size_t begin = at;
    while (at < text.size() && !IsXmlSpace(text[at]) && text[at] != ',') {
      ++at;
    }
    if (at == begin) {
      break;
    }
    if (const std::optional<float> value = ParseNumber(text.substr(begin, at - begin))) {
      numbers.push_back(*value);
    } else {
      break;
    }
  }
  return numbers;
}

// `translate(...)`, `scale(...)`, `matrix(...)`, `rotate(...)` in sequence.
// Skew is absent because it needs the same care as rotate for a case no logo
// uses; an unrecognised function ends the parse rather than being skipped,
// since a transform list applied minus one of its terms is not a near miss.
AffineTransform ParseTransform(std::string_view text) {
  AffineTransform result;
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && (IsXmlSpace(text[at]) || text[at] == ',')) {
      ++at;
    }
    const std::size_t name_begin = at;
    while (at < text.size() && text[at] != '(') {
      ++at;
    }
    if (at >= text.size()) {
      break;
    }
    const std::string_view name = Trim(text.substr(name_begin, at - name_begin));
    const std::size_t open = at + 1;
    const std::size_t close = text.find(')', open);
    if (close == std::string_view::npos) {
      break;
    }
    const std::vector<float> arguments = ParseNumberList(text.substr(open, close - open), 6);
    at = close + 1;

    AffineTransform step;
    if (name == "translate" && !arguments.empty()) {
      step = AffineTransform::Translation(arguments[0],
                                          arguments.size() > 1 ? arguments[1] : 0.0f);
    } else if (name == "scale" && !arguments.empty()) {
      step = AffineTransform::Scaling(arguments[0],
                                      arguments.size() > 1 ? arguments[1] : arguments[0]);
    } else if (name == "matrix" && arguments.size() == 6) {
      step = AffineTransform{arguments[0], arguments[1], arguments[2],
                             arguments[3], arguments[4], arguments[5]};
    } else if (name == "rotate" && !arguments.empty()) {
      const AffineTransform rotation =
          AffineTransform::Rotation(arguments[0] * 3.14159265358979323846f / 180.0f);
      if (arguments.size() >= 3) {
        // `rotate(a cx cy)` is a rotation about a point, which is three
        // transforms. Written out because composing them in the wrong order is
        // the classic way to get a shape that orbits instead of spinning.
        step = AffineTransform::Translation(-arguments[1], -arguments[2])
                   .Then(rotation)
                   .Then(AffineTransform::Translation(arguments[1], arguments[2]));
      } else {
        step = rotation;
      }
    } else {
      break;
    }
    result = step.Then(result);
  }
  return result;
}

// Scans `<name attr="value" ...>` bodies. Not a general XML parser: no
// entities, no namespaces, no DTD, and character data is skipped rather than
// kept, because nothing in the supported subset renders text. What it must get
// right is where a tag ends, which is the only thing that keeps the walk in
// step with the document.
class ElementScanner {
 public:
  explicit ElementScanner(std::string_view text) : text_(text) {}

  struct Element {
    std::string_view name;
    std::vector<Attribute> attributes;
    bool closing = false;
    bool self_closing = false;
  };

  // False at the end of the document, or when the input stops making sense.
  bool Next(Element& out);

 private:
  void SkipTo(std::string_view marker) {
    const std::size_t found = text_.find(marker, at_);
    at_ = found == std::string_view::npos ? text_.size() : found + marker.size();
  }

  std::string_view text_;
  std::size_t at_ = 0;
};

bool ElementScanner::Next(Element& out) {
  while (at_ < text_.size()) {
    const std::size_t open = text_.find('<', at_);
    if (open == std::string_view::npos) {
      return false;
    }
    at_ = open + 1;
    if (text_.compare(at_, 3, "!--") == 0) {
      SkipTo("-->");
      continue;
    }
    if (at_ < text_.size() && (text_[at_] == '?' || text_[at_] == '!')) {
      SkipTo(">");
      continue;
    }

    out = Element{};
    if (at_ < text_.size() && text_[at_] == '/') {
      out.closing = true;
      ++at_;
    }
    const std::size_t name_begin = at_;
    while (at_ < text_.size() && !IsXmlSpace(text_[at_]) && text_[at_] != '>' &&
           text_[at_] != '/') {
      ++at_;
    }
    out.name = text_.substr(name_begin, at_ - name_begin);
    if (out.name.empty()) {
      continue;
    }
    // A namespace prefix is dropped rather than matched: `<svg:path>` and
    // `<path>` are the same element, and this decoder has one namespace.
    if (const std::size_t colon = out.name.find(':'); colon != std::string_view::npos) {
      out.name.remove_prefix(colon + 1);
    }

    while (at_ < text_.size()) {
      while (at_ < text_.size() && IsXmlSpace(text_[at_])) {
        ++at_;
      }
      if (at_ >= text_.size()) {
        return false;
      }
      if (text_[at_] == '/') {
        out.self_closing = true;
        ++at_;
        continue;
      }
      if (text_[at_] == '>') {
        ++at_;
        return true;
      }
      const std::size_t attribute_begin = at_;
      while (at_ < text_.size() && text_[at_] != '=' && text_[at_] != '>' &&
             !IsXmlSpace(text_[at_])) {
        ++at_;
      }
      std::string_view name = text_.substr(attribute_begin, at_ - attribute_begin);
      while (at_ < text_.size() && IsXmlSpace(text_[at_])) {
        ++at_;
      }
      std::string_view value;
      if (at_ < text_.size() && text_[at_] == '=') {
        ++at_;
        while (at_ < text_.size() && IsXmlSpace(text_[at_])) {
          ++at_;
        }
        if (at_ < text_.size() && (text_[at_] == '"' || text_[at_] == '\'')) {
          const char quote = text_[at_++];
          const std::size_t value_begin = at_;
          while (at_ < text_.size() && text_[at_] != quote) {
            ++at_;
          }
          value = text_.substr(value_begin, at_ - value_begin);
          if (at_ < text_.size()) {
            ++at_;  // the closing quote
          }
        } else {
          const std::size_t value_begin = at_;
          while (at_ < text_.size() && !IsXmlSpace(text_[at_]) && text_[at_] != '>') {
            ++at_;
          }
          value = text_.substr(value_begin, at_ - value_begin);
        }
      }
      if (!name.empty()) {
        if (const std::size_t colon = name.find(':'); colon != std::string_view::npos) {
          name.remove_prefix(colon + 1);
        }
        out.attributes.push_back(Attribute{name, value});
      }
    }
    return false;
  }
  return false;
}

const std::string_view* Find(const std::vector<Attribute>& attributes, std::string_view name) {
  for (const Attribute& attribute : attributes) {
    if (attribute.name == name) {
      return &attribute.value;
    }
  }
  return nullptr;
}

// `style="fill:red;stroke-width:2"`. Read *after* the presentation attributes,
// because that is the precedence SVG gives it.
std::optional<std::string_view> FindInStyle(std::string_view style, std::string_view property) {
  std::size_t at = 0;
  while (at < style.size()) {
    const std::size_t end = std::min(style.find(';', at), style.size());
    const std::string_view declaration = style.substr(at, end - at);
    const std::size_t colon = declaration.find(':');
    if (colon != std::string_view::npos &&
        Trim(declaration.substr(0, colon)) == property) {
      return Trim(declaration.substr(colon + 1));
    }
    at = end + 1;
  }
  return std::nullopt;
}

// Reads one presentation property from wherever it may be written.
std::optional<std::string_view> Property(const std::vector<Attribute>& attributes,
                                         std::string_view name) {
  if (const std::string_view* style = Find(attributes, "style")) {
    if (const std::optional<std::string_view> found = FindInStyle(*style, name)) {
      return found;
    }
  }
  if (const std::string_view* attribute = Find(attributes, name)) {
    return *attribute;
  }
  return std::nullopt;
}

std::optional<Color> ParsePaint(std::string_view text) {
  const std::string_view trimmed = Trim(text);
  const std::string lowered = util::AsciiLowerCase(trimmed);
  if (lowered == "none" || lowered == "transparent") {
    return std::nullopt;
  }
  if (lowered.rfind("url(", 0) == 0) {
    // A gradient or pattern reference. Not supported, and a flat colour is not
    // an approximation of one -- so the shape is left unpainted rather than
    // filled with a guess.
    return std::nullopt;
  }
  return ParseColorText(lowered);
}

Presentation Inherit(const Presentation& parent, const std::vector<Attribute>& attributes) {
  Presentation state = parent;
  state.transform = AffineTransform{};  // a transform applies once, at its element

  if (const std::optional<std::string_view> fill = Property(attributes, "fill")) {
    const std::optional<Color> color = ParsePaint(*fill);
    state.has_fill = color.has_value();
    if (color.has_value()) {
      state.fill = *color;
    }
  }
  if (const std::optional<std::string_view> stroke = Property(attributes, "stroke")) {
    const std::optional<Color> color = ParsePaint(*stroke);
    state.has_stroke = color.has_value();
    if (color.has_value()) {
      state.stroke = *color;
    }
  }
  if (const std::optional<std::string_view> width = Property(attributes, "stroke-width")) {
    if (const std::optional<float> value = ParseNumber(*width)) {
      state.stroke_width = std::max(0.0f, value.value());
    }
  }
  if (const std::optional<std::string_view> rule = Property(attributes, "fill-rule")) {
    state.fill_rule = Trim(*rule) == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
  }
  // Group opacity is not the same as multiplying each child's alpha -- two
  // overlapping shapes in a half-transparent group show one colour, not two
  // blended. Compositing the group separately needs a scratch surface; until
  // there is one, the multiply is the honest approximation and is noted here
  // rather than left to be discovered.
  for (const char* name : {"opacity", "fill-opacity"}) {
    if (const std::optional<std::string_view> value = Property(attributes, name)) {
      if (const std::optional<float> parsed = ParseNumber(*value)) {
        state.opacity *= std::clamp(*parsed, 0.0f, 1.0f);
      }
    }
  }
  if (const std::optional<std::string_view> transform = Property(attributes, "transform")) {
    state.transform = ParseTransform(*transform);
  }
  return state;
}

Color WithOpacity(Color color, float opacity) {
  if (opacity >= 1.0f) {
    return color;
  }
  const float alpha = static_cast<float>(color.Alpha()) * std::max(0.0f, opacity);
  return color.WithAlpha(static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 255.0f) + 0.5f));
}

// The shape an element describes, in user units. Empty for an element that
// draws nothing.
Path ShapeFor(std::string_view name, const std::vector<Attribute>& attributes) {
  Path path;
  const auto number = [&attributes](std::string_view attribute, float fallback) {
    const std::string_view* text = Find(attributes, attribute);
    if (text == nullptr) {
      return fallback;
    }
    return ParseNumber(*text).value_or(fallback);
  };

  if (name == "path") {
    if (const std::string_view* data = Find(attributes, "d")) {
      ParseSvgPathData(*data, path, kMaxSvgPathCommands);
    }
  } else if (name == "rect") {
    const float width = number("width", 0.0f);
    const float height = number("height", 0.0f);
    if (width > 0.0f && height > 0.0f) {
      const FloatRect rect{number("x", 0.0f), number("y", 0.0f), width, height};
      // `ry` defaults to `rx` and vice versa, which is what makes
      // `<rect rx=4>` a rounded rectangle rather than an elliptical one.
      const float rx = std::max(0.0f, number("rx", number("ry", 0.0f)));
      const float ry = std::max(0.0f, number("ry", rx));
      if (rx > 0.0f || ry > 0.0f) {
        path.AddRoundedRect(rect, rx, rx, rx, ry > 0.0f && rx == 0.0f ? ry : rx);
      } else {
        path.AddRect(rect);
      }
    }
  } else if (name == "circle") {
    const float r = number("r", 0.0f);
    if (r > 0.0f) {
      const float cx = number("cx", 0.0f);
      const float cy = number("cy", 0.0f);
      path.AddEllipse(FloatRect{cx - r, cy - r, r * 2.0f, r * 2.0f});
    }
  } else if (name == "ellipse") {
    const float rx = number("rx", 0.0f);
    const float ry = number("ry", 0.0f);
    if (rx > 0.0f && ry > 0.0f) {
      const float cx = number("cx", 0.0f);
      const float cy = number("cy", 0.0f);
      path.AddEllipse(FloatRect{cx - rx, cy - ry, rx * 2.0f, ry * 2.0f});
    }
  } else if (name == "line") {
    path.MoveTo(FloatPoint{number("x1", 0.0f), number("y1", 0.0f)});
    path.LineTo(FloatPoint{number("x2", 0.0f), number("y2", 0.0f)});
  } else if (name == "polygon" || name == "polyline") {
    if (const std::string_view* points = Find(attributes, "points")) {
      const std::vector<float> numbers = ParseNumberList(*points, kMaxSvgPathCommands);
      for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
        const FloatPoint point{numbers[i], numbers[i + 1]};
        if (i == 0) {
          path.MoveTo(point);
        } else {
          path.LineTo(point);
        }
      }
      if (name == "polygon" && numbers.size() >= 4) {
        path.Close();
      }
    }
  }
  return path;
}

}  // namespace

bool LooksLikeSvg(std::span<const std::byte> bytes) {
  // Only the first kilobyte: a document that has not said it is SVG by then is
  // not one, and an unbounded search over a hostile blob is a denial of service
  // dressed as a content sniff.
  const std::size_t window = std::min<std::size_t>(bytes.size(), 1024);
  const std::string_view text(reinterpret_cast<const char*>(bytes.data()), window);
  const std::size_t open = text.find("<svg");
  if (open == std::string_view::npos) {
    return false;
  }
  const std::size_t after = open + 4;
  return after >= text.size() || IsXmlSpace(text[after]) || text[after] == '>' ||
         text[after] == ':';
}

SvgDecodeResult DecodeSvg(std::span<const std::byte> bytes, int width, int height) {
  SvgDecodeResult result;
  if (bytes.empty()) {
    result.error = "empty document";
    return result;
  }
  const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());

  // Two passes: the root element decides the surface size and the user-space
  // transform, and nothing can be rasterized before both are known.
  ElementScanner probe(text);
  ElementScanner::Element root;
  bool found_root = false;
  for (std::size_t seen = 0; seen < kMaxSvgElements && probe.Next(root); ++seen) {
    if (!root.closing && root.name == "svg") {
      found_root = true;
      break;
    }
  }
  if (!found_root) {
    result.error = "no svg element";
    return result;
  }

  FloatRect view_box{0.0f, 0.0f, 0.0f, 0.0f};
  if (const std::string_view* box = Find(root.attributes, "viewBox")) {
    const std::vector<float> numbers = ParseNumberList(*box, 4);
    if (numbers.size() == 4 && numbers[2] > 0.0f && numbers[3] > 0.0f) {
      view_box = FloatRect{numbers[0], numbers[1], numbers[2], numbers[3]};
    }
  }
  const std::string_view* declared_width = Find(root.attributes, "width");
  const std::string_view* declared_height = Find(root.attributes, "height");
  const float intrinsic_width =
      declared_width == nullptr ? 0.0f : ParseNumber(*declared_width).value_or(0.0f);
  const float intrinsic_height =
      declared_height == nullptr ? 0.0f : ParseNumber(*declared_height).value_or(0.0f);

  // The caller's size wins because it has already done layout; the document's
  // own size is the fallback, and its viewBox the fallback after that.
  float surface_width = static_cast<float>(width);
  float surface_height = static_cast<float>(height);
  if (!(surface_width > 0.0f)) {
    surface_width = intrinsic_width > 0.0f ? intrinsic_width : view_box.width;
  }
  if (!(surface_height > 0.0f)) {
    surface_height = intrinsic_height > 0.0f ? intrinsic_height : view_box.height;
  }
  if (!(surface_width > 0.0f) || !(surface_height > 0.0f)) {
    result.error = "no size";
    return result;
  }
  if (surface_width > static_cast<float>(kMaxSvgEdge) ||
      surface_height > static_cast<float>(kMaxSvgEdge)) {
    result.error = "size past the bound";
    return result;
  }

  // User space to device space. With no viewBox the document's own units are
  // device units, which is what an SVG with only width and height means.
  AffineTransform to_device;
  if (view_box.width > 0.0f && view_box.height > 0.0f) {
    float scale_x = surface_width / view_box.width;
    float scale_y = surface_height / view_box.height;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    // `preserveAspectRatio` defaults to `xMidYMid meet`: scale uniformly to fit
    // and centre what is left over. Only the explicit `none` stretches. Getting
    // this backwards distorts every icon whose viewBox is not the same shape as
    // the box it is drawn in, which is most of them -- and it distorts them
    // silently, since a stretched triangle is still a triangle.
    const std::string_view* preserve = Find(root.attributes, "preserveAspectRatio");
    const bool uniform = preserve == nullptr || Trim(*preserve).substr(0, 4) != "none";
    if (uniform) {
      const float scale = std::min(scale_x, scale_y);
      scale_x = scale;
      scale_y = scale;
      offset_x = (surface_width - view_box.width * scale) * 0.5f;
      offset_y = (surface_height - view_box.height * scale) * 0.5f;
    }
    to_device = AffineTransform::Translation(-view_box.x, -view_box.y)
                    .Then(AffineTransform::Scaling(scale_x, scale_y))
                    .Then(AffineTransform::Translation(offset_x, offset_y));
  }

  Canvas canvas{static_cast<int>(surface_width), static_cast<int>(surface_height)};
  if (canvas.IsEmpty()) {
    result.error = "no size";
    return result;
  }
  canvas.Clear(Color::Transparent());
  Painter painter{canvas};

  // The inherited state, one entry per open element. A vector rather than
  // recursion: the depth bound is then a length check rather than a stack the
  // document controls.
  std::vector<Presentation> stack;
  stack.push_back(Inherit(Presentation{}, root.attributes));

  ElementScanner scanner(text);
  ElementScanner::Element element;
  bool inside_root = false;
  std::size_t seen = 0;
  while (scanner.Next(element)) {
    if (++seen > kMaxSvgElements) {
      break;
    }
    if (!inside_root) {
      // Skip forward to the root the probe already found, so that the two
      // passes agree about where the document starts.
      if (!element.closing && element.name == "svg") {
        inside_root = true;
      }
      continue;
    }
    if (element.closing) {
      if (stack.size() > 1) {
        stack.pop_back();
      } else if (element.name == "svg") {
        break;
      }
      continue;
    }

    const Presentation state = Inherit(stack.back(), element.attributes);
    const AffineTransform transform = state.transform.Then(stack.back().transform);
    if (!element.self_closing) {
      if (stack.size() >= kMaxSvgDepth) {
        break;
      }
      Presentation pushed = state;
      pushed.transform = transform;
      stack.push_back(pushed);
    }

    const Path path = ShapeFor(element.name, element.attributes);
    if (path.IsEmpty()) {
      continue;
    }
    painter.SetTransform(transform.Then(to_device));
    if (state.has_fill) {
      painter.FillPath(path, WithOpacity(state.fill, state.opacity), state.fill_rule);
    }
    if (state.has_stroke && state.stroke_width > 0.0f) {
      StrokeStyle stroke;
      stroke.width = state.stroke_width;
      painter.StrokePath(path, stroke, WithOpacity(state.stroke, state.opacity));
    }
  }

  std::vector<std::uint32_t> pixels(canvas.Pixels().begin(), canvas.Pixels().end());
  if (!result.image.Adopt(canvas.Width(), canvas.Height(), std::move(pixels))) {
    result.error = "surface could not be adopted";
  }
  return result;
}

}  // namespace microbrowser::gfx
